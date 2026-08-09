// ═══════════════════════════════════════════════════════════════════════════════
// symbol_metadata.cpp - Fetch, cache, and parse symbol metadata
//
// Uses XMLHttpRequest via EM_ASM - no -sFETCH needed, no SDL conflicts.
// XHR handles CORS correctly and works in all Emscripten environments.
//
// Flow:
//   1. Try localStorage cache (sync, instant)
//   2. If valid (< 1 hour old), parse immediately, fire on_ready
//   3. Always fire background XHR to refresh
//   4. On success, update localStorage + re-parse
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/symbol_metadata.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <algorithm>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

// ─── JSON Parsing ────────────────────────────────────────────────────────────

void SymbolRegistry::parse_json(const char* json_data, size_t len) {
    try {
        auto doc = json::parse(json_data, json_data + len);
        if (!doc.contains("symbols") || !doc["symbols"].is_array()) {
            return;
        }
        symbols_.clear();
        for (const auto& s : doc["symbols"]) {
            SymbolMetadata meta;
            meta.exchange     = s.value("exchange", "");
            meta.symbol       = s.value("symbol", "");

            // Exchange is always lowercase. The symbol is canonical-lowercase for
            // binancef (API returns "XPLUSDT", app uses "xplusdt"), but PRESERVED
            // for other venues: Hyperliquid coins are uppercase "BTC" end-to-end
            // (backend store rows + republish subjects), so lowercasing HL would
            // break subscription + historical-query matching.
            std::transform(meta.exchange.begin(), meta.exchange.end(), meta.exchange.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (meta.exchange == "binancef") {
                std::transform(meta.symbol.begin(), meta.symbol.end(), meta.symbol.begin(),
                               [](unsigned char c) { return std::tolower(c); });
            }

            meta.tick_size    = s.value("tick_size", 0.01);
            meta.step_size    = s.value("step_size", 1.0);
            meta.min_notional = s.value("min_notional", 5.0);
            meta.is_active    = s.value("is_active", true);

            // Base/quote asset for display name
            meta.base_asset   = s.value("base_asset", "");
            meta.quote_asset  = s.value("quote_asset", "");

            // Categories from underlying_sub_type array
            if (s.contains("underlying_sub_type") && s["underlying_sub_type"].is_array()) {
                for (const auto& cat : s["underlying_sub_type"]) {
                    if (cat.is_string()) {
                        meta.categories.push_back(cat.get<std::string>());
                    }
                }
            }

            if (meta.exchange.empty() || meta.symbol.empty()) continue;

            meta.pair_key = make_key(meta.exchange, meta.symbol);  // lowercase key; meta.symbol keeps venue case
            meta.fmt = PriceFormatter::from_tick_and_step(meta.tick_size, meta.step_size);
            symbols_[meta.pair_key] = std::move(meta);
        }
        loaded_ = true;
    } catch (const json::exception& e) {
        (void)e;
    }
}

// ─── Emscripten Implementation ───────────────────────────────────────────────

#ifdef __EMSCRIPTEN__

static constexpr int CACHE_MAX_MS = 3600 * 1000; // 1 hour

// C callback invoked from JS when XHR completes
extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void _symbol_metadata_on_fetch(const char* json_str, int len) {
        SymbolRegistry::instance().parse_json(json_str, static_cast<size_t>(len));
    }
}

void SymbolRegistry::fetch_metadata(std::function<void()> on_ready) {
    // 1. Try localStorage cache (synchronous)
    bool cache_hit = EM_ASM_INT({
        try {
            var tsStr = localStorage.getItem('edgedepth_symbol_metadata_ts');
            if (!tsStr) return 0;
            var age = Date.now() - parseInt(tsStr, 10);
            if (age > $0) return 0;

            var data = localStorage.getItem('edgedepth_symbol_metadata');
            if (!data) return 0;

            var len = lengthBytesUTF8(data);
            var buf = _malloc(len + 1);
            stringToUTF8(data, buf, len + 1);
            __symbol_metadata_on_fetch(buf, len);
            _free(buf);
            return 1;
        } catch(e) {
            console.warn('SymbolRegistry cache read failed:', e);
            return 0;
        }
    }, CACHE_MAX_MS);

    if (cache_hit) {
        if (on_ready) on_ready();
    }

    // 2. Fire background XHR to refresh (works regardless of CORS config)
    EM_ASM({
        var xhr = new XMLHttpRequest();
        xhr.open('GET', 'https://api.edgedepth.com/symbols/metadata', true);
        xhr.onload = function() {
            if (xhr.status === 200) {
                var text = xhr.responseText;
                try {
                    localStorage.setItem('edgedepth_symbol_metadata', text);
                    localStorage.setItem('edgedepth_symbol_metadata_ts', Date.now().toString());
                } catch(e) {
                    console.warn('SymbolRegistry cache write failed:', e);
                }

                var len = lengthBytesUTF8(text);
                var buf = _malloc(len + 1);
                stringToUTF8(text, buf, len + 1);
                __symbol_metadata_on_fetch(buf, len);
                _free(buf);
            } else {
                console.warn('SymbolRegistry: XHR failed with status', xhr.status);
            }
        };
        xhr.onerror = function() {
            console.warn('SymbolRegistry: XHR network error');
        };
        xhr.send();
    });
}

#else
// ── Non-Emscripten stub ──────────────────────────────────────────────────────
void SymbolRegistry::fetch_metadata(std::function<void()> on_ready) {
    loaded_ = true;
    if (on_ready) on_ready();
}
#endif