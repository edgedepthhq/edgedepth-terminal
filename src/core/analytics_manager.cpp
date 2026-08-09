// ═══════════════════════════════════════════════════════════════════════════════
// analytics_manager.cpp - apply protobuf analytics-state updates to latest maps
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/analytics_manager.h"
#include "pb/messages.pb.h"

void AnalyticsManager::apply_positioning(const std::string& symbol,
                                         const pb::PositioningStateUpdate& u) {
    SymbolState& st = symbols_[lower(symbol)];
    PositioningState& p = st.latest;
    p.global_long_account     = u.global_long_account();
    p.top_trader_long_account = u.top_trader_long_account();
    p.funding_rate            = u.funding_rate();
    p.open_interest           = u.open_interest();
    p.oi_change_24h           = u.oi_change_24h();
    p.long_liq_usd            = u.long_liq_usd();
    p.short_liq_usd           = u.short_liq_usd();
    p.liq_imbalance           = u.liq_imbalance();
    p.smart_money_bias        = u.smart_money_bias();
    p.retail_sentiment        = u.retail_sentiment();
    p.flow_pressure           = u.flow_pressure();
    p.cascade_risk            = u.cascade_risk();
    p.taker_buy_ratio         = u.taker_buy_ratio();
    p.taker_sell_ratio        = u.taker_sell_ratio();
    p.cvd                     = u.cvd();
    p.timestamp_ms            = u.timestamp_ms();
    p.valid                   = true;

    // OI history ring - backs client-derived velocity + 1h/4h deltas (there is no
    // proto field for those; see the OI-velocity open decision in the plan doc).
    if (u.open_interest() > 0.0 && u.timestamp_ms() > 0) {
        std::deque<OISample>& h = st.oi_history;
        if (h.empty() || u.timestamp_ms() >= h.back().ts_ms) {
            h.push_back({u.timestamp_ms(), u.open_interest()});
        }
        const int64_t min_ts = u.timestamp_ms() - OI_HISTORY_MAX_AGE_MS;
        while (!h.empty() && h.front().ts_ms < min_ts) h.pop_front();
        while (h.size() > OI_HISTORY_MAX_SAMPLES) h.pop_front();
    }

    if (p.timestamp_ms > last_update_ms_) last_update_ms_ = p.timestamp_ms;
}

const PositioningState* AnalyticsManager::get_positioning(const std::string& symbol) const {
    auto it = symbols_.find(lower(symbol));
    return (it != symbols_.end() && it->second.latest.valid) ? &it->second.latest : nullptr;
}

double AnalyticsManager::oi_change_pct(const std::string& symbol, int64_t window_ms, bool* has) const {
    if (has) *has = false;
    auto it = symbols_.find(lower(symbol));
    if (it == symbols_.end()) return 0.0;
    const std::deque<OISample>& h = it->second.oi_history;
    if (h.size() < 2) return 0.0;
    const int64_t now_ts = h.back().ts_ms;
    const double  now_oi = h.back().oi;
    if (now_oi <= 0.0) return 0.0;
    const int64_t target = now_ts - window_ms;
    if (h.front().ts_ms > target) return 0.0;  // history does not yet span the window
    double ref_oi = h.front().oi;
    for (const OISample& s : h) {
        if (s.ts_ms <= target) ref_oi = s.oi; else break;
    }
    if (ref_oi <= 0.0) return 0.0;
    if (has) *has = true;
    return (now_oi - ref_oi) / ref_oi;
}

double AnalyticsManager::oi_velocity_per_min(const std::string& symbol, bool* has) const {
    if (has) *has = false;
    auto it = symbols_.find(lower(symbol));
    if (it == symbols_.end()) return 0.0;
    const std::deque<OISample>& h = it->second.oi_history;
    if (h.size() < 2) return 0.0;
    const int64_t now_ts = h.back().ts_ms;
    const double  now_oi = h.back().oi;
    const int64_t target = now_ts - 60000;  // ~60s window
    const OISample* ref = &h.front();
    for (auto rit = h.rbegin(); rit != h.rend(); ++rit) {
        ref = &(*rit);
        if (rit->ts_ms <= target) break;
    }
    const int64_t dt = now_ts - ref->ts_ms;
    if (dt <= 0 || ref->oi <= 0.0) return 0.0;
    if (has) *has = true;
    return ((now_oi - ref->oi) / ref->oi) * (60000.0 / static_cast<double>(dt));
}

void AnalyticsManager::apply_contagion(const pb::ContagionSnapshot& u) {
    contagion_.market_stress = u.market_stress();
    contagion_.stress_regime = u.stress_regime();
    contagion_.timestamp_ms  = u.timestamp_ms();
    contagion_.valid         = true;
    if (contagion_.timestamp_ms > last_update_ms_) last_update_ms_ = contagion_.timestamp_ms;
}

void AnalyticsManager::trim_after(int64_t cutoff_ms) {
    // Replay rewind: points past the cutoff are the replay's future. Drop future
    // OI samples and invalidate a future 'latest' - the next drip refills it.
    for (auto& kv : symbols_) {
        std::deque<OISample>& h = kv.second.oi_history;
        while (!h.empty() && h.back().ts_ms > cutoff_ms) h.pop_back();
        if (kv.second.latest.timestamp_ms > cutoff_ms) kv.second.latest.valid = false;
    }
    if (contagion_.valid && contagion_.timestamp_ms > cutoff_ms) contagion_ = ContagionState{};
}
