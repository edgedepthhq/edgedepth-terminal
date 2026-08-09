#pragma once
#include "types/types.h"
#include "stream_handler.h"
#include <array>
#include <string>
#include <string_view>
#include <cstdint>

// ──────────────────────────────────────────────────────────────────────────────
// Flat struct mirroring DebugEntry proto fields.
// All strings are short_string-friendly; no heap alloc in hot path.
// ──────────────────────────────────────────────────────────────────────────────
struct DebugEntry {
    int64_t     ts        = 0;
    // actor / category / detector / window are all short (<32 chars)
    char        actor[32] = {};
    char        category[32] = {};
    char        tf_label[16] = {};   // pre-formatted timeframe string e.g. "4h"
    char        detector[48] = {};
    char        window[16]   = {};
    float       score     = 0.0f;
    float       thresh    = 0.0f;
    double      price     = 0.0;
    // msg is the longest field — cap at 256 to stay on stack
    char        msg[256]  = {};
};

// ──────────────────────────────────────────────────────────────────────────────
// Ring buffer of DebugEntry.  MAX_ENTRIES = 1024 to match backend ring size.
// New entries overwrite the oldest when full.
// ──────────────────────────────────────────────────────────────────────────────
class DebugManager {
public:
    static constexpr size_t MAX_ENTRIES = 4096;

    explicit DebugManager(StreamManager& stream_mgr, const Terminal::Pair& pair);

    // ── Subscription lifecycle ────────────────────────────────────────────────
    // Subscribe to ALL debug logs for the given symbol (unsubscribes previous).
    void subscribe(const std::string& symbol);
    void unsubscribe();

    // ── Called by MessageHandler on STREAM_DEBUG (stream id 23) ───────────────
    // `data` is the raw inner bytes of WSPayload.data (DebugLogBatch proto).
    void handle_batch(const void* data, size_t size);

    void copy_all_to_clipboard() const;

    // ── Read interface for DebugLogWidget ─────────────────────────────────────
    // Returns entries in chronological order (oldest → newest).
    // Caller iterates [0, count()) and calls entry_at(i).
    size_t                count()         const { return entry_count_; }
    const DebugEntry&     entry_at(size_t i) const;
    bool                  has_new_entries() const { return new_entries_since_render_; }
    void                  mark_rendered()  { new_entries_since_render_ = false; }
    const std::string&    subscribed_symbol() const { return subscribed_symbol_; }

    // Clear all entries (used on replay rewind)
    void clear_entries() {
        write_head_ = 0;
        entry_count_ = 0;
        new_entries_since_render_ = true;
    }

    // ── Range loading ─────────────────────────────────────────────────────
    void load_range(int64_t start_ms, int64_t end_ms);
    void handle_range_start(int64_t start_ms, int64_t end_ms);
    void handle_range_complete(int64_t start_ms, int64_t end_ms, int msg_count, int entry_count);

    // When range is active, the widget reads from range_entries_ instead of ring_
    bool              is_range_active()    const { return range_active_; }
    bool              is_range_loading()   const { return range_loading_; }
    size_t            range_count()        const { return range_entries_.size(); }
    const DebugEntry& range_entry_at(size_t i) const { return range_entries_[i]; }
    void              clear_range();

private:
    StreamManager&  stream_mgr_;
    Terminal::Pair  pair_;
    std::string     subscribed_symbol_;

    // Ring buffer — fixed allocation, no heap in hot path
    std::array<DebugEntry, MAX_ENTRIES> ring_{};
    size_t write_head_  = 0;   // next write position (mod MAX_ENTRIES)
    size_t entry_count_ = 0;   // total entries ever written (not capped)
    bool   new_entries_since_render_ = false;

    // ── Range state ───────────────────────────────────────────────────────
    bool range_active_  = false;   // true = widget should display range_entries_
    bool range_loading_ = false;   // true = waiting for server batches
    int64_t range_start_ms_ = 0;
    int64_t range_end_ms_   = 0;
    std::vector<DebugEntry> range_entries_;  // flat vector, not ring — range is bounded

    void push_entry(const DebugEntry& e);

    // tf label helper: int64 ms → "1m", "4h", "1d", etc.
    static void fmt_tf(int64_t tf_ms, char* out, size_t out_sz);
};