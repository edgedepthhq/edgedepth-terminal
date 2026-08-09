#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// symbol_metadata.h - Symbol metadata registry + PriceFormatter
//
// Single source of truth for tick_size, step_size, and derived formatting.
// Fetched from https://api.edgedepth.com/symbols/metadata at startup.
// Cached in localStorage to avoid blocking on cold load.
// All widgets pull a PriceFormatter instead of hardcoding precision.
//
// IMPORTANT: All keys are lowercase. The API returns "XPLUSDT" but url_router
// lowercases to "xplusdt". make_key() normalizes both sides.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <cstdio>
#include <cmath>
#include <functional>
#include <algorithm>

// ─── PriceFormatter ──────────────────────────────────────────────────────────

struct PriceFormatter {
    int price_precision = 2;
    int qty_precision   = 3;
    char price_fmt[12]  = "%.2f";
    char qty_fmt[12]    = "%.3f";

    static int precision_from_step(double step) {
        if (step <= 0.0) return 8;
        int p = 0;
        double v = step;
        while (v < 0.9999999 && p < 10) {
            v *= 10.0;
            p++;
        }
        return p;
    }

    static PriceFormatter from_tick_and_step(double tick_size, double step_size) {
        PriceFormatter f;
        f.price_precision = precision_from_step(tick_size);
        f.qty_precision   = precision_from_step(step_size);
        snprintf(f.price_fmt, sizeof(f.price_fmt), "%%.%df", f.price_precision);
        snprintf(f.qty_fmt,   sizeof(f.qty_fmt),   "%%.%df", f.qty_precision);
        return f;
    }

    void format_price(char* buf, size_t buf_size, double price) const {
        snprintf(buf, buf_size, price_fmt, price);
    }
    void format_qty(char* buf, size_t buf_size, double qty) const {
        snprintf(buf, buf_size, qty_fmt, qty);
    }
};

// ─── SymbolMetadata ──────────────────────────────────────────────────────────

struct SymbolMetadata {
    std::string exchange;
    std::string symbol;
    std::string pair_key;       // "binancef-xplusdt" (always lowercase)
    std::string base_asset;     // "BTC", "ETH" (uppercase for display)
    std::string quote_asset;    // "USDT"
    std::vector<std::string> categories; // "Layer 1", "DeFi", "Meme", etc.
    double tick_size   = 0.01;
    double step_size   = 1.0;
    double min_notional = 5.0;
    bool   is_active   = true;
    PriceFormatter fmt;

    // Display name derived from base/quote: "BTC/USDT"
    std::string display_name() const {
        if (base_asset.empty()) return symbol;
        return base_asset + "/" + quote_asset;
    }
};

// ─── SymbolRegistry ──────────────────────────────────────────────────────────

class SymbolRegistry {
public:
    static SymbolRegistry& instance() {
        static SymbolRegistry s;
        return s;
    }

    void fetch_metadata(std::function<void()> on_ready);

    const SymbolMetadata* get(const std::string& exchange, const std::string& symbol) const {
        auto it = symbols_.find(make_key(exchange, symbol));
        return it != symbols_.end() ? &it->second : nullptr;
    }

    PriceFormatter get_formatter(const std::string& exchange, const std::string& symbol) const {
        const auto* meta = get(exchange, symbol);
        if (meta) return meta->fmt;
        return PriceFormatter::from_tick_and_step(0.01, 0.001);
    }

    void insert(SymbolMetadata meta) {
        meta.pair_key = make_key(meta.exchange, meta.symbol);
        meta.fmt = PriceFormatter::from_tick_and_step(meta.tick_size, meta.step_size);
        symbols_[meta.pair_key] = std::move(meta);
    }

    bool is_loaded() const { return loaded_; }
    size_t size() const { return symbols_.size(); }

    // Iterate all symbols (for symbol picker)
    const std::unordered_map<std::string, SymbolMetadata>& all() const { return symbols_; }

    // Get sorted list of all unique categories across all symbols
    std::vector<std::string> unique_categories() const {
        std::set<std::string> cats;
        for (const auto& [_, meta] : symbols_) {
            for (const auto& c : meta.categories) {
                cats.insert(c);
            }
        }
        return {cats.begin(), cats.end()};
    }

    // Public so the EMSCRIPTEN_KEEPALIVE callback can reach it
    void parse_json(const char* json_data, size_t len);

private:
    SymbolRegistry() = default;
    std::unordered_map<std::string, SymbolMetadata> symbols_;
    bool loaded_ = false;

    // Normalize to lowercase - API returns "XPLUSDT", url_router uses "xplusdt"
    static std::string make_key(const std::string& exchange, const std::string& symbol) {
        std::string key;
        key.reserve(exchange.size() + 1 + symbol.size());
        for (char c : exchange) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key += '-';
        for (char c : symbol) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }
};