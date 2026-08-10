#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Query-string discipline: neither the boot normalization (url_push) nor a
// symbol navigation (url_navigate) may eat the user's query params. ?ws= is
// the load-bearing one: a self-hoster pointing the terminal at their own
// gateway would otherwise lose the override on boot or on the first watchlist
// click and silently reconnect to the production feed. Route-owned keys
// (exchange) and one-shot deep links (pack/packsym/packt/t/lesson/event) are
// dropped on NAVIGATION, because a symbol click is an intent to leave those
// modes; everything else is carried over. Everything here is EM_ASM, not
// EM_JS, so this header is safe to include from any translation unit.

struct Route {
    std::string symbol;
    std::string exchange;

    Route() : exchange("binancef") {}
};

inline std::string url_get_current_path() {
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        var path = window.location.pathname;
        var len = lengthBytesUTF8(path) + 1;
        var buf = _malloc(len);
        stringToUTF8(path, buf, len);
        return buf;
    }));
    std::string path(raw);
    free(raw);
    return path;
#else
    return "/terminal/btcusdt";
#endif
}

inline std::string url_get_current_search() {
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        var s = window.location.search;
        var len = lengthBytesUTF8(s) + 1;
        var buf = _malloc(len);
        stringToUTF8(s, buf, len);
        return buf;
    }));
    std::string s(raw);
    free(raw);
    return s;
#else
    return "";
#endif
}

// pushState to path, merging the current query string (the path's own query
// wins per key). Boot uses this to normalize / into /terminal/<symbol>.
inline void url_push(const std::string& path) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var p = UTF8ToString($0);
        var qi = p.indexOf('?');
        var params = new URLSearchParams(window.location.search);
        if (qi >= 0) {
            new URLSearchParams(p.slice(qi + 1)).forEach(function(v, k) {
                params.set(k, v);
            });
            p = p.slice(0, qi);
        }
        var q = params.toString();
        var url = q ? p + '?' + q : p;
        if (window.location.pathname + window.location.search !== url) {
            window.history.pushState({}, "", url);
        }
    }, path.c_str());
#endif
}

// Full navigation to path (reloads the app). Carries the query string over,
// minus route-owned and one-shot deep-link keys; the path's own query wins.
inline void url_navigate(const std::string& path) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var p = UTF8ToString($0);
        var qi = p.indexOf('?');
        var params = new URLSearchParams(window.location.search);
        // split(' ') instead of an array literal: EM_ASM is a C macro and the
        // preprocessor would treat the literal's commas as argument breaks.
        'exchange pack packsym packt t lesson event'.split(' ')
            .forEach(function(k) { params.delete(k); });
        if (qi >= 0) {
            new URLSearchParams(p.slice(qi + 1)).forEach(function(v, k) {
                params.set(k, v);
            });
            p = p.slice(0, qi);
        }
        var q = params.toString();
        window.location.href = q ? p + '?' + q : p;
    }, path.c_str());
#endif
}

inline void url_register_popstate() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        window.addEventListener("popstate", function() {
            if (Module._on_popstate) {
                Module._on_popstate();
            }
        });
    });
#endif
}

// Extract a lowercased ?exchange=<ex> value from a query string (e.g.
// "?exchange=hl&foo=1"); returns "" when absent.
inline std::string parse_exchange_query(const std::string& search) {
    const std::string key = "exchange=";
    auto pos = search.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = search.find('&', pos);
    std::string ex = (end == std::string::npos) ? search.substr(pos)
                                                : search.substr(pos, end - pos);
    std::transform(ex.begin(), ex.end(), ex.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ex;
}

// parse_route reads the symbol from /terminal/<symbol> and the venue from an
// optional ?exchange=<ex> query. binancef symbols are canonical lowercase; the
// symbol case is PRESERVED for other venues (Hyperliquid coins are uppercase
// "BTC" end-to-end), matching how the backend stores + routes them.
inline Route parse_route(const std::string& path, const std::string& search = "") {
    Route r;

    const std::string ex = parse_exchange_query(search);
    if (!ex.empty()) r.exchange = ex;

    const std::string prefix = "/terminal/";
    if (path.rfind(prefix, 0) != 0) return r;

    std::string rest = path.substr(prefix.size());
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) return r;

    auto slash = rest.find('/');
    r.symbol = (slash == std::string::npos) ? rest : rest.substr(0, slash);

    if (r.exchange == "binancef") {
        std::transform(r.symbol.begin(), r.symbol.end(), r.symbol.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    }

    return r;
}

inline std::string build_terminal_path(const std::string& symbol) {
    return "/terminal/" + symbol;
}

// Exchange-aware path: appends ?exchange=<ex> only for non-binancef venues, so
// every existing binancef URL stays byte-identical.
inline std::string build_terminal_path(const std::string& exchange, const std::string& symbol) {
    std::string p = "/terminal/" + symbol;
    if (!exchange.empty() && exchange != "binancef") p += "?exchange=" + exchange;
    return p;
}
