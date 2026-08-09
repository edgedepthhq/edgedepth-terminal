#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// analytics_manager.h - Latest-per-symbol analytics state for the v2 Stats panel
//
// Holds the "state" analytics streams the Stats panel + market header read:
//   - PositioningStateUpdate (STREAM_POSITIONING_STATE=16): a clean single-type
//     stream carrying CVD, taker ratios, global/top-trader long account, open
//     interest + 24h change, long/short liq USD, smart-money bias, cascade risk.
//   - ContagionSnapshot (STREAM_CONTAGION=19; replay-only today): market stress
//     + stress regime (a market-wide value, not per-symbol).
//
// VPIN (STREAM_VPIN_STATE=15) is NOT stored here - its latest point is read from
// the IndicatorSeriesManager SeriesCache via ctx.series_mgr().
//
// The multiplexed STREAM_ANALYTICS=14 sub-types (orderbook regime / smart money /
// liquidation-cascade windows) are Tier 2 - deferred until that stream carries a
// message-type discriminator.
//
// Threading: written main-thread-only (via the DispatchQueue in live mode, inline
// on the main thread during replay) and read lock-free at render - the same rule
// as the VPIN SeriesCache. No mutex.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <unordered_map>
#include <deque>
#include <cstdint>
#include <cctype>

namespace pb { class PositioningStateUpdate; class ContagionSnapshot; }

// Latest positioning snapshot for one symbol (the fields the panel/header read).
struct PositioningState {
    double  global_long_account     = 0.0;  // retail crowd long fraction (0..1)
    double  top_trader_long_account = 0.0;  // smart-money long fraction (0..1)
    double  funding_rate            = 0.0;
    double  open_interest           = 0.0;  // contracts (notional = x mark_price)
    double  oi_change_24h           = 0.0;  // signed fraction
    double  long_liq_usd            = 0.0;
    double  short_liq_usd           = 0.0;
    double  liq_imbalance           = 0.0;
    double  smart_money_bias        = 0.0;  // -1..+1
    double  retail_sentiment        = 0.0;
    double  flow_pressure           = 0.0;
    double  cascade_risk            = 0.0;  // 0..1
    double  taker_buy_ratio         = 0.0;
    double  taker_sell_ratio        = 0.0;
    double  cvd                     = 0.0;  // session cumulative volume delta
    int64_t timestamp_ms            = 0;
    bool    valid                   = false;
};

// Market-wide contagion (single latest; not keyed per symbol).
struct ContagionState {
    double      market_stress = 0.0;   // 0..1
    std::string stress_regime;         // "calm" / "elevated" / "stress" / "crisis"
    int64_t     timestamp_ms  = 0;
    bool        valid         = false;
};

class AnalyticsManager {
public:
    // -- Positioning (STREAM_POSITIONING_STATE=16) --
    void apply_positioning(const std::string& symbol, const pb::PositioningStateUpdate& u);
    const PositioningState* get_positioning(const std::string& symbol) const;

    // OI derived metrics (client-side, from the per-symbol OI history ring).
    // window_ms e.g. 3600000 (1h) / 14400000 (4h). Returns a signed fraction;
    // *has (optional) reports whether enough history exists to be meaningful.
    double oi_change_pct(const std::string& symbol, int64_t window_ms, bool* has = nullptr) const;
    double oi_velocity_per_min(const std::string& symbol, bool* has = nullptr) const;

    // -- Contagion (STREAM_CONTAGION=19; replay-only today) --
    void apply_contagion(const pb::ContagionSnapshot& u);
    const ContagionState* get_contagion() const { return contagion_.valid ? &contagion_ : nullptr; }

    // Replay rewind: drop state/samples newer than cutoff (mirrors SeriesCache::trim_after).
    void trim_after(int64_t cutoff_ms);

    int64_t last_update_ms() const { return last_update_ms_; }
    bool    has_data()       const { return !symbols_.empty() || contagion_.valid; }

private:
    struct OISample { int64_t ts_ms; double oi; };
    struct SymbolState {
        PositioningState     latest;
        std::deque<OISample> oi_history;  // capped by age (4h) + a hard sample cap
    };

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::unordered_map<std::string, SymbolState> symbols_;
    ContagionState contagion_;
    int64_t last_update_ms_ = 0;

    static constexpr int64_t OI_HISTORY_MAX_AGE_MS  = 4LL * 60 * 60 * 1000;  // 4h
    static constexpr size_t  OI_HISTORY_MAX_SAMPLES = 5000;
};
