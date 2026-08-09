#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// footprint_manager.h — Per-candle tick volume data for footprint chart overlay
//
// Stores raw 1-tick-per-row data fetched from the backend. The client regroups
// levels into configurable tick_per_row buckets on the fly, so a re-request is
// only needed when the visible time range changes — not when the user adjusts
// granularity.
//
// Data flow:
//   Historical: ChartWidget zoom-in → request_history() → WS → backend DB →
//               batch of TickVolumeUpdate → on_tick_volume_update() → store
//   Live:       STREAM_TICK_VOLUME from NATS → on_tick_volume_update()
//   Render:     get_footprint() per visible candle → group_levels() → draw grid
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

#include <pb/messages.pb.h>

class StreamManager;

namespace Terminal { struct Pair; }

class FootprintManager {
public:
    // ── Per-level raw data (1 tick per row from DB) ──────────────────────
    struct Level {
        double price        = 0.0;
        double buy_volume   = 0.0;
        double sell_volume  = 0.0;
        double total_volume = 0.0;
        int64_t trade_count = 0;
        double delta        = 0.0; // buy - sell
    };

    // ── Aggregated candle footprint (all levels for one 1m candle) ───────
    struct CandleFootprint {
        int64_t start_time  = 0;   // candle start (ms)
        int64_t end_time    = 0;
        std::vector<Level> levels; // sorted by price ascending
        double total_volume = 0.0;
        double total_buy    = 0.0;
        double total_sell   = 0.0;
        double delta        = 0.0; // candle-level net delta
        double poc          = 0.0; // point of control price
        double high_price   = 0.0;
        double low_price    = 0.0;
        bool valid          = false;
        uint64_t version    = 0;   // per-candle version (for cache invalidation)
    };

    // ── Display modes ────────────────────────────────────────────────────
    enum class Mode {
        SellsBuys,  // Two columns: sells | buys (raw volume)  ("Cluster")
        Delta,      // Single column: delta per level
        Volume,     // Single column: total volume per level
        Profile,    // Aggregated sidebar histogram (all visible candles)
    };

    // ── Grouped level (after client-side tick regrouping) ────────────────
    struct GroupedLevel {
        double price_mid    = 0.0;
        double price_lo     = 0.0;
        double price_hi     = 0.0;
        double buy_volume   = 0.0;
        double sell_volume  = 0.0;
        double total_volume = 0.0;
        double delta        = 0.0;
        bool is_poc         = false;
        bool is_imbalance   = false; // buy:sell > ratio or sell:buy > ratio
        bool buy_dominant   = false; // which side has the imbalance
    };

    // ── Public API ───────────────────────────────────────────────────────

    // Request historical footprint data for a time range.
    void request_history(const Terminal::Pair& pair, int64_t start_ms, int64_t end_ms,
                         StreamManager* sm);

    // Called by message handler when a TickVolumeUpdate arrives (live or historical).
    void on_tick_volume_update(const std::string& symbol, const pb::TickVolumeUpdate& update);

    // Look up the footprint for a specific candle by its start_time.
    const CandleFootprint* get_footprint(const std::string& symbol, int64_t start_time) const;

    // Client-side regrouping — no re-request needed when user changes granularity.
    std::vector<GroupedLevel> group_levels(const CandleFootprint& fp, double tick_per_row) const;

    // Clear all data for a symbol (e.g., on symbol switch).
    void clear(const std::string& symbol);

    // ── Cached group_levels — avoids per-frame rebuild ──────────────────
    struct MergedCache {
        std::vector<GroupedLevel> levels;
        double tick_per_row       = 0.0;
        uint64_t composite_ver    = 0;    // sum of constituent bucket versions
        double total_volume       = 0.0;
        double total_buy          = 0.0;
        double total_sell         = 0.0;
        double delta              = 0.0;
        double high_price         = 0.0;
        double low_price          = 0.0;
    };

    // Get merged+grouped levels for a candle (handles multi-minute merging + grouping + caching).
    // Returns nullptr if no data. All heavy work is cached — safe to call every frame.
    const MergedCache* get_merged_grouped(
        const std::string& symbol, int64_t candle_ts,
        int64_t tf_sec, double tick_per_row);

    // Fast-path: check if any data has changed since last frame.
    // If false, all existing cache entries are guaranteed valid.
    bool has_new_data_since(uint64_t version) const { return data_version_ != version; }

    // Current data version (increments on any data update).
    uint64_t data_version() const { return data_version_; }

    // ── Config (public — read/written by ChartWidget UI) ─────────────────
    Mode  mode             = Mode::SellsBuys;
    int   ticks_per_row    = 0;       // 0 = auto
    bool  enabled          = false;
    bool  show_imbalances  = true;
    bool  show_poc         = true;
    bool  show_summary     = true;    // V: / D: footer per candle
    float imbalance_ratio  = 3.0f;    // threshold for imbalance detection

    // ── State queries ────────────────────────────────────────────────────
    bool is_loading() const { return loading_; }
    int  candle_count(const std::string& symbol) const;

private:
    // symbol → (start_time → CandleFootprint)
    std::unordered_map<std::string,
        std::unordered_map<int64_t, CandleFootprint>> data_;

    bool loading_ = false;

    // Data version — incremented on every on_tick_volume_update
    uint64_t data_version_ = 0;

    // Cache for merged+grouped results: symbol → (candle_ts → MergedCache)
    std::unordered_map<std::string,
        std::unordered_map<int64_t, MergedCache>> merged_cache_;

    // Track requested range to avoid duplicate requests
    std::string last_symbol_;
    int64_t last_start_ = 0;
    int64_t last_end_   = 0;
};
