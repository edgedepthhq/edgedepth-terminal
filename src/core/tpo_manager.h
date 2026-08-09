#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// tpo_manager.h — TPO (Time Price Opportunity) / Market Profile
//
// Client-side computation from 30m candle data. No backend endpoint needed.
// Each session = 24h (00:00-00:00 UTC). Sessions are built by iterating
// 30m candles and recording which price rows each candle touches.
//
// Data flow:
//   ChartWidget visible range → build_sessions(candles_30m)
//   → compute POC, VAH/VAL, single prints, poor high/low, IB
//   → ChartWidget reads during render
// ═══════════════════════════════════════════════════════════════════════════════

#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <algorithm>

struct TPOBlock {
    int period_idx = 0;   // which 30m period (0 = first of session, 1 = second, ...)
};

struct TPORow {
    double price_lo = 0.0;
    double price_hi = 0.0;
    int    row_idx  = 0;         // index from bottom
    std::vector<TPOBlock> blocks;

    // Derived flags (set during session build)
    bool is_poc          = false;
    bool is_value_area   = false;
    bool is_single_print = false;
    bool is_initial_balance = false;
};

struct TPOSession {
    int64_t session_start_ms = 0;  // 00:00 UTC
    int64_t session_end_ms   = 0;  // next 00:00 UTC

    std::vector<TPORow> rows;      // sorted by price ascending
    int total_periods   = 0;       // number of 30m candles in this session
    int total_blocks    = 0;       // sum of all blocks across all rows
    int max_block_count = 0;       // widest row (for rendering normalization)

    // Key levels
    double poc_price    = 0.0;
    int    poc_row_idx  = -1;
    double vah          = 0.0;
    double val          = 0.0;
    double session_high = 0.0;
    double session_low  = 0.0;
    double ib_high      = 0.0;     // initial balance (first 2 periods)
    double ib_low       = 0.0;
    bool   has_poor_high = false;  // top row has ≥2 blocks
    bool   has_poor_low  = false;  // bottom row has ≥2 blocks
    double tick_per_row  = 0.0;

    // UI state
    bool expanded = false;         // show individual periods vs stacked
};

class TPOManager {
public:
    TPOManager() = default;

    // ── Core API ────────────────────────────────────────────────────────

    // Build sessions from 30m candle data covering visible range.
    // Candles must be sorted by timestamp ascending.
    // tick_per_row: price height of each row (e.g. 50.0 for $50/row on BTC).
    //              0 = auto-compute from session range.
    void build_sessions(const std::string& symbol,
                        const double* timestamps,   // candle timestamps (ms)
                        const double* highs,
                        const double* lows,
                        size_t candle_count,
                        int64_t timeframe_sec,       // should be 1800 for 30m
                        double tick_per_row);

    // Get computed sessions for rendering.
    const std::vector<TPOSession>* get_sessions(const std::string& symbol) const;

    // Toggle expand/collapse on a specific session.
    void toggle_expand(const std::string& symbol, size_t session_idx);

    // Clear data for a symbol.
    void clear(const std::string& symbol);

    // ── Config (public — read/written by settings UI) ───────────────────

    // Session period in hours (24 = daily, 168 = weekly)
    int   session_period_hours = 24;

    // Ticks per row (0 = auto)
    int   ticks_per_row_setting = 0;

    // Value area percentage (0.0 - 1.0, default 0.70)
    float value_area_pct = 0.70f;

    // Display toggles
    bool show_poc_ray       = true;
    bool show_vah_val_rays  = true;
    bool show_single_prints = true;
    bool show_poor_high_low = true;
    bool show_initial_balance = false;
    bool show_session_header = true;

    // Block coloring
    bool highlight_start_end = true;  // color first/last period differently
    int  profile_spacing     = 1;     // pixel gap between adjacent session profiles

    // State queries
    bool has_data(const std::string& symbol) const;

private:
    // symbol → sessions
    std::unordered_map<std::string, std::vector<TPOSession>> data_;

    // Debounce rebuild
    std::unordered_map<std::string, uint64_t> last_build_hash_;

    // Internal helpers
    void build_single_session(TPOSession& session,
                              const double* timestamps,
                              const double* highs,
                              const double* lows,
                              size_t start_idx, size_t end_idx,
                              int64_t session_start_ms,
                              int64_t session_end_ms,
                              int64_t timeframe_sec,
                              double tick_per_row);

    void compute_poc(TPOSession& session);
    void compute_value_area(TPOSession& session);
    void detect_single_prints(TPOSession& session);
    void detect_poor_high_low(TPOSession& session);
    void compute_initial_balance(TPOSession& session,
                                 const double* highs,
                                 const double* lows,
                                 size_t start_idx, size_t end_idx);
};
