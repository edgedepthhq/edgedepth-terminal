#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// scanner_manager.h — Market scanner data for symbol picker
//
// Receives MarketScannerUpdate protobufs from the DataThread.
// Provides per-symbol microstructure scores (EdgeDepth Score, VPIN,
// Hawkes BR, cascade risk, funding rate) for display in the symbol picker.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <unordered_map>
#include <cstdint>
#include <cctype>

namespace pb { class MarketScannerUpdate; }

struct ScannerEntry {
    double edgedepth_score = 0.0;  // 0-100 composite
    double vpin            = 0.0;  // 0-1
    double hawkes_br       = 0.0;  // 0-1+
    double cascade_risk    = 0.0;  // 0-1
    int    liq_hmm_state   = 0;    // 0=Normal,1=Building,2=Active,3=Extreme
    double funding_rate    = 0.0;
    double oi_change_1h    = 0.0;
    double buy_ratio       = 0.0;
    double flow_intensity  = 0.0;
    double open_interest   = 0.0;
    double price           = 0.0;
    std::string vpin_regime;       // Normal/Elevated/High/Critical
    std::string ob_regime;
};

class ScannerManager {
public:
    static ScannerManager& instance() {
        static ScannerManager s;
        return s;
    }

    // Apply a protobuf MarketScannerUpdate for one exchange (the venue is carried by
    // the feed subject scanner.{exchange}.global, not the payload).
    void apply_update(const std::string& exchange, const pb::MarketScannerUpdate& update);

    // Read a single entry keyed by (exchange, symbol). Case-insensitive on the
    // symbol (HL is UPPERCASE "ACE", binancef lowercase); the map keys by lowercased
    // symbol via make_key, so callers can pass either casing.
    const ScannerEntry* get(const std::string& exchange, const std::string& symbol) const {
        auto it = entries_.find(make_key(exchange, symbol));
        return it != entries_.end() ? &it->second : nullptr;
    }

    bool has_data() const { return !entries_.empty(); }
    int64_t last_update_ms() const { return last_update_ms_; }
    size_t count() const { return entries_.size(); }

    // make_key builds "{exchange}:{lowercased symbol}" — the single normalization
    // point for apply_update (write) and get (read).
    static std::string make_key(const std::string& exchange, const std::string& symbol) {
        std::string k;
        k.reserve(exchange.size() + 1 + symbol.size());
        k += exchange;
        k += ':';
        for (char c : symbol) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    }

private:
    ScannerManager() = default;
    std::unordered_map<std::string, ScannerEntry> entries_;
    int64_t last_update_ms_ = 0;
};
