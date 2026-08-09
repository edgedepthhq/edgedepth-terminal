// ═══════════════════════════════════════════════════════════════════════════════
// ticker_manager.cpp — Apply protobuf Ticker24hUpdate to ticker map
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/ticker_manager.h"
#include "pb/messages.pb.h"

void TickerManager::apply_update(const std::string& exchange, const pb::Ticker24hUpdate& update) {
    for (const auto& entry : update.entries()) {
        // Key by "{exchange}:{lowercased symbol}" (make_key) so venues never collide
        // and reads are case-insensitive on the symbol (HL is UPPERCASE).
        auto& t = tickers_[make_key(exchange, entry.symbol())];
        t.last_price     = entry.last_price();
        t.change_pct_24h = entry.change_pct();
        t.volume_quote   = entry.volume_quote();
        t.event_time_ms  = entry.event_time_ms();
    }

    last_update_ms_ = update.timestamp_ms();
}
