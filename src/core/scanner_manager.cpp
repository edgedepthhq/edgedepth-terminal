// ═══════════════════════════════════════════════════════════════════════════════
// scanner_manager.cpp — Apply protobuf MarketScannerUpdate to scanner map
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/scanner_manager.h"
#include "pb/messages.pb.h"

void ScannerManager::apply_update(const std::string& exchange, const pb::MarketScannerUpdate& update) {
    for (const auto& entry : update.entries()) {
        auto& s = entries_[make_key(exchange, entry.symbol())];
        s.edgedepth_score = entry.edgedepth_score();
        s.vpin            = entry.vpin();
        s.hawkes_br       = entry.hawkes_br();
        s.cascade_risk    = entry.cascade_risk();
        s.liq_hmm_state   = entry.liq_hmm_state();
        s.funding_rate    = entry.funding_rate();
        s.oi_change_1h    = entry.oi_change_1h_pct();
        s.buy_ratio       = entry.buy_ratio();
        s.flow_intensity  = entry.flow_intensity();
        s.open_interest   = entry.open_interest();
        s.price           = entry.price();
        s.vpin_regime     = entry.vpin_regime();
        s.ob_regime       = entry.ob_regime();
    }

    last_update_ms_ = update.timestamp_ms();
}
