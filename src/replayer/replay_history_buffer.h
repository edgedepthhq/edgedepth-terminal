#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// ReplayHistoryBuffer - Client-side message history for instant backward skip
//
// Stores the last N minutes of delivered replay messages so that backward
// skip (<<) can replay them locally instead of waiting for the backend.
//
// Design:
//   - Single std::deque<HistoryEntry>, roughly timestamp-ordered
//   - Entries captured at the binary frame routing point in main.cpp
//   - On <<, find nearest OB snapshot before target, replay forward
//   - Periodic OB snapshots (~30s) avoid replaying from session start
//   - Heatmaps, VPVR, footprint are rewind-exempt (slow-moving, not reset)
//
// Memory budget: ~6MB per symbol for 5 minutes (depth-dominated)
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#include <functional>
#include "types/types.h"

class OrderbookManager;

class ReplayHistoryBuffer {
public:
    // ─── Entry Types ─────────────────────────────────────────────────────
    struct HistoryEntry {
        int64_t timestamp_ms;
        std::vector<uint8_t> data;      // Raw protobuf bytes (normal entry)
        Terminal::Stream stream;
        int64_t timeframe;
        bool is_ob_snapshot;            // If true, data is serialized OB state
    };
    // Serialized OB snapshot - stored as HistoryEntry with is_ob_snapshot=true.
    // Uses plain C++ structs (not protobuf) since this is client-internal.
    struct OBLevel {
        double price;
        double size;
    };
    struct OBSnapshot {
        std::vector<OBLevel> bids;
        std::vector<OBLevel> asks;
        double last_price;
        int64_t last_update_id;
        int64_t timestamp_ms;
    };

    // ─── Configuration ───────────────────────────────────────────────────
    static constexpr int64_t DEFAULT_WINDOW_MS   = 10 * 60 * 1000;   // 10 minutes
    static constexpr int64_t OB_SNAPSHOT_INTERVAL_MS = 10 * 1000;    // 10 seconds
    static constexpr size_t  MAX_BYTES           = 100 * 1024 * 1024; // 100MB hard cap

    explicit ReplayHistoryBuffer(int64_t window_ms = DEFAULT_WINDOW_MS);

    // ─── Capture ─────────────────────────────────────────────────────────
    // Called from main.cpp binary frame routing - stores a copy of raw bytes.
    // timestamp_ms: message timestamp (from protobuf or current replay clock)
    // stream/timeframe: routing metadata for replay dispatch
    void push(int64_t timestamp_ms,
              Terminal::Stream stream,
              int64_t timeframe,
              const uint8_t* data, size_t len);

    // Take an OB snapshot. Call periodically (~30s) from the replay loop.
    // Reads the current OB state from the manager and serializes it.
    void snapshot_orderbook(int64_t timestamp_ms,
                            const OrderbookManager& ob_mgr,
                            const Terminal::Pair& pair);

    // ─── Rewind Query ────────────────────────────────────────────────────
    // Can we rewind to this timestamp from the buffer?
    bool contains(int64_t timestamp_ms) const;

    // Earliest buffered timestamp
    int64_t earliest_ms() const;

    // Latest buffered timestamp
    int64_t latest_ms() const;

    // Find the latest OB snapshot at or before target_time.
    // Returns nullptr if none exists (caller should fall back to full seek).
    const HistoryEntry* find_ob_snapshot_before(int64_t target_time_ms) const;

    // Replay callback: called for each entry from snapshot_time to end.
    // The callback receives raw protobuf bytes to dispatch through the
    // normal MessageHandler pipeline.
    using ReplayFn = std::function<void(const uint8_t* data, size_t len)>;

    // OB restore callback: called once with the snapshot to apply before
    // replaying deltas.
    using OBRestoreFn = std::function<void(const OBSnapshot& snapshot)>;

    // Execute a rewind: find OB snapshot, restore it, replay messages.
    // Returns false if target_time is outside buffer range.
    // target_time_ms: where we're rewinding TO (e.g., 04:33:00)
    // current_time_ms: where we were BEFORE the rewind (e.g., 04:33:45)
    // Replays all messages from target_time to current_time to rebuild state.
    // ob_restore: applies the OB snapshot to the orderbook manager
    // replay_msg: dispatches each message through the normal pipeline
    bool replay_from(int64_t target_time_ms,
                     int64_t current_time_ms,
                     OBRestoreFn ob_restore,
                     ReplayFn replay_msg);

    // ─── Maintenance ─────────────────────────────────────────────────────
    void clear();

    // Stats for debugging
    size_t entry_count() const { return entries_.size(); }
    size_t total_bytes() const { return total_bytes_; }
    size_t ob_snapshot_count() const;

    // Number of entries trimmed since last call to consume_trimmed_count().
    // Used by ReplayManager to adjust drip_feed_cursor_ after trim.
    size_t consume_trimmed_count() {
        size_t n = trimmed_count_;
        trimmed_count_ = 0;
        return n;
    }

    // Direct access to entries for dispatch/replay
    const std::deque<HistoryEntry>& entries() const { return entries_; }

private:
    void trim();

    std::deque<HistoryEntry> entries_;
    int64_t window_ms_;
    size_t total_bytes_ = 0;
    size_t trimmed_count_ = 0;             // Entries trimmed since last consume
    int64_t last_ob_snapshot_ms_ = 0;  // Tracks interval for auto-snapshot
    int trim_counter_ = 0;             // Trim every N pushes, not every push
};
