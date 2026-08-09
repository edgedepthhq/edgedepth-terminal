#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// paper_trading_types.h - Data structures for paper trading positions & account
//
// Lean C++ structs mirroring protobuf PaperPositionUpdate / PaperAccountState.
// No heap allocations in the hot path - strings are small (symbol, alert_type).
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <cstdint>

namespace Terminal {

struct PaperPosition {
    std::string position_id;
    std::string symbol;
    std::string side;           // "long" or "short"
    std::string alert_type;     // momentum, breakout, cap_recovery, etc.
    double entry_price    = 0.0;
    double mark_price     = 0.0;
    double quantity        = 0.0;
    double notional_usd   = 0.0;
    double leverage        = 0.0;
    double unrealized_pnl     = 0.0;
    double unrealized_pnl_pct = 0.0;
    double mae_pct            = 0.0;
    double mfe_pct            = 0.0;
    double stop_level         = 0.0;
    double tp1_level          = 0.0;
    double tp2_level          = 0.0;
    double trailing_stop_level = 0.0;
    double funding_costs      = 0.0;
    double entry_atr          = 0.0;
    double ml_score           = 0.0;
    int64_t opened_at         = 0;
    int64_t closed_at         = 0;   // 0 = still open
    std::string exit_reason;         // empty = still open
    double realized_pnl       = 0.0;
    double r_multiple         = 0.0;
    double slippage_entry     = 0.0;
    double slippage_exit      = 0.0;
    int64_t duration_seconds  = 0;

    // ── Partial-fill state (added 2026-04-16) ──
    // tp1_hit/tp2_hit: TP ladder rungs that have fired during the position
    // remaining_qty_pct: residual fraction (1.0 = full, 0.5 = TP1 fired @50%)
    // tp_strategy: disambiguates TP=0 cases. Values:
    //   "trail_only"  - no fixed TP, trail handles exits (momentum strong-trend)
    //   "tp1_only"    - single TP1 partial close
    //   "tp1_tp2"     - TP1 + TP2 ladder
    //   "fixed_tp"    - single TP closes 100%
    //   "unknown"     - pre-fix historical trades
    bool tp1_hit              = false;
    bool tp2_hit              = false;
    double remaining_qty_pct  = 1.0;
    std::string tp_strategy;

    [[nodiscard]] bool is_open() const { return closed_at == 0; }
    [[nodiscard]] bool is_long() const { return side == "long"; }
    [[nodiscard]] bool is_partially_closed() const { return remaining_qty_pct < 0.99 && remaining_qty_pct > 0.0; }
};

struct PaperAccount {
    double balance          = 1000.0;
    double equity           = 1000.0;
    double open_pnl         = 0.0;
    double initial_balance  = 1000.0;
    int32_t total_trades    = 0;
    int32_t winning_trades  = 0;
    int32_t losing_trades   = 0;
    double total_pnl        = 0.0;
    double total_funding    = 0.0;
    double peak_equity      = 1000.0;
    double max_drawdown_pct = 0.0;
    double profit_factor    = 0.0;
    int64_t timestamp_ms    = 0;

    [[nodiscard]] double win_rate() const {
        int closed = winning_trades + losing_trades;
        return closed > 0
            ? 100.0 * static_cast<double>(winning_trades) / static_cast<double>(closed)
            : 0.0;
    }
};

} // namespace Terminal
