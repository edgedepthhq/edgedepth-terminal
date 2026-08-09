#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// ticker_manager.h — Live 24h ticker data for all symbols
//
// Receives protobuf Ticker24hUpdate from the DataThread.
// Used by the symbol picker for live price + 24h% display and sorting.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#include <cctype>

namespace pb { class Ticker24hUpdate; }

struct TickerEntry {
    double last_price      = 0.0;
    double change_pct_24h  = 0.0;
    double volume_quote    = 0.0;  // quote asset volume (USDT)
    int64_t event_time_ms  = 0;
};

class TickerManager {
public:
    static TickerManager& instance() {
        static TickerManager t;
        return t;
    }

    // Apply a protobuf Ticker24hUpdate for one exchange (the venue is carried by the
    // feed subject ticker24h.{exchange}.global, not the payload).
    void apply_update(const std::string& exchange, const pb::Ticker24hUpdate& update);

    // Read a single ticker keyed by (exchange, symbol). Case-insensitive on the
    // symbol: HL symbols are UPPERCASE ("ACE"), binancef lowercase ("btcusdt"), and
    // the map is keyed by lowercased symbol (make_key) — so callers can pass either.
    const TickerEntry* get(const std::string& exchange, const std::string& symbol) const {
        auto it = tickers_.find(make_key(exchange, symbol));
        return it != tickers_.end() ? &it->second : nullptr;
    }

    bool has_data() const { return !tickers_.empty(); }
    int64_t last_update_ms() const { return last_update_ms_; }
    size_t count() const { return tickers_.size(); }

    // make_key builds the map key "{exchange}:{lowercased symbol}". The single
    // normalization point for both apply_update (write) and get (read).
    static std::string make_key(const std::string& exchange, const std::string& symbol) {
        std::string k;
        k.reserve(exchange.size() + 1 + symbol.size());
        k += exchange;
        k += ':';
        for (char c : symbol) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    }

private:
    TickerManager() = default;
    std::unordered_map<std::string, TickerEntry> tickers_;
    int64_t last_update_ms_ = 0;
};
