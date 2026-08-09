#pragma once

#include "types/types.h"
#include "stream_handler.h"
#include <deque>
#include <vector>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// CandleManager — owns all candle state for one symbol+timeframe
//
// Extracted from ChartWidget. Follows the manager ownership pattern:
//   MessageHandler dispatches → CandleManager updates state
//   ChartWidget reads CandleManager during render (const accessors)
//
// Multiple widgets sharing the same symbol+timeframe can read one CandleManager.
// ═══════════════════════════════════════════════════════════════════════════════

class CandleManager {
public:
    CandleManager(const Terminal::Pair& pair,
                  int64_t timeframe_seconds,
                  StreamManager& stream_mgr);
    ~CandleManager();

    static constexpr size_t MAX_CANDLES = 200000;
    static constexpr int LOAD_COOLDOWN_MS = 500;
    static constexpr size_t INITIAL_PRELOAD_CANDLES = 2880;

    // Recent-tick ring buffer caps (Line chart). Bounded by BOTH count and age;
    // whichever bites first drops the oldest. ~50k ticks / ~5 min of fresh trade
    // data. Sized for the Line real-time view (RT rolling window): bounds memory
    // (<1MB) while covering the ~5 min the live frame ever shows.
    static constexpr size_t  MAX_TICKS       = 50000;
    static constexpr int64_t MAX_TICK_AGE_MS = 5 * 60 * 1000;

    // ─── Data Input ──────────────────────────────────────────────────────

    void handle_trade(const Terminal::Trade& trade);
    void handle_candle(const Terminal::Candle& candle);
    void handle_candle_batch(std::span<const Terminal::Candle> batch);

    // ─── Lifecycle ───────────────────────────────────────────────────────

    // Call once per frame. Handles time-based finalization + auto-scroll-load.
    void update(double visible_x_min, double visible_x_max);

    void change_timeframe(int64_t new_timeframe_seconds);
    void initial_load();
    void request_historical(size_t count, int64_t end_time_ms = 0);

    // ─── SoA Read Accessors (for rendering) ─────────────────────────────

    const std::vector<double>& timestamps()  const { ensure_cache(); return cached_timestamps_; }
    const std::vector<double>& opens()       const { ensure_cache(); return cached_opens_; }
    const std::vector<double>& highs()       const { ensure_cache(); return cached_highs_; }
    const std::vector<double>& lows()        const { ensure_cache(); return cached_lows_; }
    const std::vector<double>& closes()      const { ensure_cache(); return cached_closes_; }
    const std::vector<double>& volumes()     const { ensure_cache(); return cached_volumes_; }
    const std::vector<double>& vbuys()       const { ensure_cache(); return cached_vbuys_; }
    const std::vector<double>& vsells()      const { ensure_cache(); return cached_vsells_; }

    // ─── Heikin Ashi derived SoA (parallel to the real cache) ──────────────
    // Populated in rebuild_cache() right after the real cache under the same
    // cache_dirty_ invalidation, so scroll-load prepends rebuild it for free.
    const std::vector<double>& ha_opens()    const { ensure_cache(); return cached_ha_opens_; }
    const std::vector<double>& ha_highs()    const { ensure_cache(); return cached_ha_highs_; }
    const std::vector<double>& ha_lows()     const { ensure_cache(); return cached_ha_lows_; }
    const std::vector<double>& ha_closes()   const { ensure_cache(); return cached_ha_closes_; }

    // Building (live) candle
    bool has_building_candle() const { return has_current_candle_; }
    const Terminal::Candle& building_candle() const { return current_candle_; }

    // ─── Recent-tick ring buffer (Line chart real-time resolution) ─────────
    // Parallel SoA of the most recent raw trades (timeframe-independent), so the
    // Line chart can move tick-by-tick instead of once per candle close. Bounded
    // by BOTH count and age; older points fall back to candle closes. See §5.1.
    const std::vector<int64_t>& tick_times()  const { return tick_times_; }
    const std::vector<double>&  tick_prices() const { return tick_prices_; }

    // State queries
    bool empty() const { return candles_.empty() && !has_current_candle_; }
    size_t count() const { return candles_.size(); }
    bool is_loading() const { return is_loading_; }
    bool is_initial_load_complete() const { return initial_load_complete_; }
    double last_close_price() const { return last_close_price_; }
    int64_t oldest_timestamp() const { return oldest_candle_timestamp_; }
    const std::deque<Terminal::Candle>& candles() const { return candles_; }

    const Terminal::Pair& pair() const { return pair_; }
    int64_t timeframe_seconds() const { return timeframe_seconds_; }

    bool follow_live() const { return follow_live_; }
    void set_follow_live(bool v) { follow_live_ = v; }

    size_t visible_candles_for_timeframe() const;

    // Register callbacks on the StreamManager. Called from initial_load()
    // for live mode, or from DataContext factory for replay mode.
    void subscribe();

    // Mark as ready to accept candles without requesting historical data.
    // Used by replay: the backend streams candles chronologically,
    // so there's no "initial batch" to wait for.
    void mark_ready_for_replay() { initial_load_complete_ = true; }

    // Set the replay start time. When non-zero, change_timeframe() requests
    // historical candles ending at this time instead of the latest data.
    void set_replay_time(int64_t start_time_ms) { replay_start_time_ms_ = start_time_ms; }
    int64_t replay_start_time_ms() const { return replay_start_time_ms_; }

    // Clear all candle data and re-request historical candles up to seek_time_ms.
    // Used when the replay seeks to a new position — clears stale data and
    // re-requests the historical context for the new position.
    void reset_for_seek(int64_t seek_time_ms);

    // Remove candles with timestamps after cutoff_ms and reset building candle.
    // Used by << (backward skip) to trim "future" candles from the display
    // without doing a full data reload.
    void trim_candles_after(int64_t cutoff_ms);

    // Suppress server candle messages (trades still build candles).
    // Used during drip-feed after rewind — server candle messages in the
    // buffer carry the FULL 15s tick OHLCV which includes "future" data.
    void set_suppress_candle_messages(bool suppress) { suppress_candle_messages_ = suppress; }

private:
    void unsubscribe();
    void build_candle_from_trade(const Terminal::Trade& trade);
    void finalize_current_candle();
    int64_t get_candle_timestamp(int64_t trade_timestamp_ms) const;

    bool should_load_more(double visible_x_min) const;
    void check_and_load_more(double visible_x_min);

    void rebuild_cache() const;
    void ensure_cache() const { if (cache_dirty_) rebuild_cache(); }
    void mark_dirty() { cache_dirty_ = true; }

    Terminal::Pair pair_;
    int64_t timeframe_seconds_;
    StreamManager& stream_mgr_;

    std::deque<Terminal::Candle> candles_;
    Terminal::Candle current_candle_{};
    bool has_current_candle_ = false;
    double last_close_price_ = 0.0;

    bool is_loading_ = false;
    bool initial_load_complete_ = false;
    int64_t oldest_candle_timestamp_ = 0;
    // Flat bar-count paging (Binance klines / TradingView datafeed convention:
    // page by a fixed number of bars, not a calendar span). 10080 is chosen to
    // equal exactly one 7-day TimescaleDB chunk on the 1m base table
    // (7d * 1440 = 10080), so the hot 1m path reads a single chunk-skip-aligned
    // The initial 1m preload is 2 days, comfortably beyond the Field's 16h
    // decay horizon. Scroll-back pages stay at one week for efficient history.
    size_t preload_count_ = INITIAL_PRELOAD_CANDLES;
    size_t scroll_load_count_ = 10080;
    bool follow_live_ = true;
    int64_t replay_start_time_ms_ = 0;  // 0 = live mode, >0 = replay mode
    int64_t replay_latest_time_ms_ = 0;
    bool suppress_candle_messages_ = false;  // True during drip-feed after rewind

    std::chrono::steady_clock::time_point last_load_time_;

    StreamKey candle_stream_key_;
    StreamKey trade_stream_key_;

    // Recent-tick ring buffer (Line chart). Parallel SoA appended in
    // handle_trade(); a BATCHED front-drop keeps it bounded by count AND age.
    // Timeframe-independent (kept across TF changes; cleared on seek / symbol
    // change). The front trim only fires once a slack accumulates, so the common
    // append path is push_back (amortized O(1), capacity reused) with no
    // per-trade whole-vector shift.
    std::vector<int64_t> tick_times_;
    std::vector<double>  tick_prices_;
    static constexpr size_t kTickTrimSlack = 512;  // batch size for front drops
    void append_tick(int64_t time_ms, double price);
    void clear_ticks();

    // SoA cache — mutable for lazy const rebuild
    mutable bool cache_dirty_ = true;
    mutable std::vector<double> cached_timestamps_;
    mutable std::vector<double> cached_opens_;
    mutable std::vector<double> cached_highs_;
    mutable std::vector<double> cached_lows_;
    mutable std::vector<double> cached_closes_;
    mutable std::vector<double> cached_volumes_;
    mutable std::vector<double> cached_vbuys_;
    mutable std::vector<double> cached_vsells_;

    // Heikin Ashi derived SoA — rebuilt inside rebuild_cache() from the real
    // cache (recursive, path-dependent from the oldest loaded candle).
    mutable std::vector<double> cached_ha_opens_;
    mutable std::vector<double> cached_ha_highs_;
    mutable std::vector<double> cached_ha_lows_;
    mutable std::vector<double> cached_ha_closes_;
};
