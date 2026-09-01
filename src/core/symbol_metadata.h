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
#include <atomic>

// ─── PriceFormatter ──────────────────────────────────────────────────────────

struct PriceFormatter {
    int price_precision = 2;
    int qty_precision   = 3;
    char price_fmt[12]  = "%.2f";
    char qty_fmt[12]    = "%.3f";
    // False until a real tick/step built this. A default-constructed formatter
    // is a PLACEHOLDER, not a 2dp instrument: the metadata fetch can land after
    // the widgets are built, and rendering a sub-cent perp at 2dp (every price
    // reading "0.24") is how the demo shipped a broken-looking terminal.
    // Consumers that hold one must re-read it on Widget::refresh_instrument().
    bool resolved = false;

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
        f.resolved = (tick_size > 0.0);
        return f;
    }

    // The ONE stopgap for an instrument the registry has not answered for yet.
    // Precision comes off the price magnitude, which is never the exchange's
    // truth but is always the right ORDER of truth, so a chart axis stays
    // readable for the second or two before the real tick binds. Still
    // unresolved, so refresh_instrument() replaces it the moment it can.
    static PriceFormatter provisional_for_price(double px) {
        const double a = std::abs(px);
        PriceFormatter f;
        f.price_precision = (a >= 1000.0) ? 2
                          : (a >= 1.0)    ? 4
                          : (a >= 0.01)   ? 5
                          : (a >= 0.0001) ? 7
                                          : 8;
        snprintf(f.price_fmt, sizeof(f.price_fmt), "%%.%df", f.price_precision);
        snprintf(f.qty_fmt,   sizeof(f.qty_fmt),   "%%.%df", f.qty_precision);
        f.resolved = false;
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
        return PriceFormatter{};   // unresolved placeholder, NOT a 2dp instrument
    }

    bool has(const std::string& exchange, const std::string& symbol) const {
        return get(exchange, symbol) != nullptr;
    }

    // 0 = unknown. The only tick fallback in the app: there is no sane guess for
    // an instrument's grid (0.1 on a $0.24 perp draws a ladder with the whole
    // book in one row), so callers either wait or say so. Widgets rebind through
    // Widget::refresh_instrument() when the epoch moves.
    double tick_or_zero(const std::string& exchange, const std::string& symbol) const {
        const auto* meta = get(exchange, symbol);
        return meta ? meta->tick_size : 0.0;
    }

    // Seed one instrument from a source that is not the metadata API. A pack
    // carries its own tick in the header it already downloads (PackHeader
    // field 14), which makes a /demo session independent of a 500 KB fetch it
    // would otherwise be racing. Never overwrites an API entry.
    void seed_tick(const std::string& exchange, const std::string& symbol, double tick_size) {
        if (tick_size <= 0.0) return;
        const std::string key = make_key(exchange, symbol);
        seeded_.push_back({exchange, symbol, tick_size});
        if (symbols_.count(key)) return;      // API answer wins
        apply_seed(seeded_.back());
        bump_epoch();
    }

    // Bumped on every load (localStorage read, XHR refresh, pack-header seed).
    // Widgets rebind from the FRAME LOOP when this moves, never from inside the
    // fetch callback: that re-enters wasm from a browser event and would mutate
    // widget state mid-frame, between an ImGui Begin and its End.
    uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

    void insert(SymbolMetadata meta) {
        meta.pair_key = make_key(meta.exchange, meta.symbol);
        meta.fmt = PriceFormatter::from_tick_and_step(meta.tick_size, meta.step_size);
        symbols_[meta.pair_key] = std::move(meta);
        bump_epoch();
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

    // Re-apply pack-header seeds after a metadata payload replaced the map.
    // parse_json clears and rebuilds symbols_, so a symbol the API does not
    // carry (a delisted contract a recorded pack still replays) would otherwise
    // LOSE the tick it already had and send the DOM back to its waiting state.
    void reapply_seeds() {
        for (const auto& s : seeded_) {
            if (!symbols_.count(make_key(s.exchange, s.symbol))) apply_seed(s);
        }
    }

private:
    struct SeededTick {
        std::string exchange;
        std::string symbol;
        double tick_size = 0.0;
    };

    SymbolRegistry() = default;
    void bump_epoch() { epoch_.fetch_add(1, std::memory_order_release); }

    void apply_seed(const SeededTick& s) {
        SymbolMetadata meta;
        meta.exchange  = s.exchange;
        meta.symbol    = s.symbol;
        meta.tick_size = s.tick_size;
        meta.pair_key  = make_key(s.exchange, s.symbol);
        meta.fmt       = PriceFormatter::from_tick_and_step(meta.tick_size, meta.step_size);
        symbols_[meta.pair_key] = std::move(meta);
    }

    std::vector<SeededTick> seeded_;

    std::unordered_map<std::string, SymbolMetadata> symbols_;
    bool loaded_ = false;
    std::atomic<uint32_t> epoch_{0};

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