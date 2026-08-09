#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, js_push_url, (const char* path), {
    var p = UTF8ToString(path);
    if (window.location.pathname !== p) {
        window.history.pushState({}, "", p);
    }
});

EM_JS(char*, js_get_path, (), {
    var path = window.location.pathname;
    var len = lengthBytesUTF8(path) + 1;
    var buf = _malloc(len);
    stringToUTF8(path, buf, len);
    return buf;
});

EM_JS(char*, js_get_search, (), {
    var s = window.location.search;
    var len = lengthBytesUTF8(s) + 1;
    var buf = _malloc(len);
    stringToUTF8(s, buf, len);
    return buf;
});

EM_JS(void, js_register_popstate, (void), {
    window.addEventListener("popstate", function() {
        if (Module._on_popstate) {
            Module._on_popstate();
        }
    });
});

#endif // __EMSCRIPTEN__

struct Route {
    std::string symbol;
    std::string exchange;

    Route() : exchange("binancef") {}
};

inline std::string url_get_current_path() {
#ifdef __EMSCRIPTEN__
    char* raw = js_get_path();
    std::string path(raw);
    free(raw);
    return path;
#else
    return "/terminal/btcusdt";
#endif
}

inline std::string url_get_current_search() {
#ifdef __EMSCRIPTEN__
    char* raw = js_get_search();
    std::string s(raw);
    free(raw);
    return s;
#else
    return "";
#endif
}

inline void url_push(const std::string& path) {
#ifdef __EMSCRIPTEN__
    js_push_url(path.c_str());
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