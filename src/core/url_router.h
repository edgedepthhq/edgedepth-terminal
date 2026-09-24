#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Route shape: /terminal/<venue>/<symbol>, where venue is one of the hub's
// exchange ids (binancef | bybit | hl). That is the CANONICAL form and the
// only one the builder emits. Two legacy shapes still parse, because every
// bookmark, marketing link, lesson deep link and research report written
// before 2026-09-19 uses them, and none of those may break:
//
//   /terminal/<symbol>                 -> binancef (the venue-less alias)
//   /terminal/<symbol>?exchange=<ex>   -> the query-carried venue
//
// Boot normalizes an alias to the canonical path with url_push, so the
// address bar shows one shape. The venue moved INTO the path because a query
// param is route-owned state living outside the route: every builder that
// only knew a symbol silently produced a Binance link, and url_navigate
// deliberately strips ?exchange= on a symbol click. With the venue in the
// path a symbol alone is not an address, so that class of bug cannot recur.
//
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
    return "/terminal/binancef/btcusdt";
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
// wins per key). Boot uses this to normalize / and the legacy aliases into
// /terminal/<venue>/<symbol>. The legacy ?exchange= is dropped here: the path
// now carries the venue, and leaving the query copy behind would let the two
// disagree on the next navigation.
inline void url_push(const std::string& path) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var p = UTF8ToString($0);
        var qi = p.indexOf('?');
        var params = new URLSearchParams(window.location.search);
        params.delete('exchange');
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
#else
    (void)path;  // native builds (the unit tests) have no history to push
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
#else
    (void)path;  // native builds (the unit tests) have nowhere to navigate
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
//
// This walks the key=value pairs instead of searching for the literal
// "exchange=". A bare find() also matches a key that merely ENDS in exchange
// ("?myexchange=hl") and the same text sitting inside some other param's VALUE
// ("?note=exchange=hl"), and either one would silently route the terminal at a
// venue the user never asked for. Keys are matched whole, against a boundary.
inline std::string parse_exchange_query(const std::string& search) {
    using size_type = std::string::size_type;
    static const std::string key = "exchange";

    size_type pos = 0;
    while (pos < search.size()) {
        if (search[pos] == '?' || search[pos] == '&') { ++pos; continue; }

        const size_type amp = search.find('&', pos);
        const size_type end = (amp == std::string::npos) ? search.size() : amp;
        const size_type eq  = search.find('=', pos);

        if (eq != std::string::npos && eq < end &&
            search.compare(pos, eq - pos, key) == 0) {
            std::string ex = search.substr(eq + 1, end - eq - 1);
            std::transform(ex.begin(), ex.end(), ex.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ex;
        }
        pos = end + 1;
    }
    return "";
}

// The venues the hub serves, by their exchange id. This is the ONLY list the
// router consults: a first path segment is a venue iff it is in here, so a
// symbol can never be mistaken for a venue and a typo'd venue never routes
// anywhere. Mirrors markets.IsVenue in the Go backend.
inline bool is_known_exchange(const std::string& ex) {
    return ex == "binancef" || ex == "bybit" || ex == "hl";
}

// Symbol case is venue dependent: Binance and Bybit product symbols are
// lowercase end-to-end (the hub lowercases both in normalizePair); Hyperliquid
// coins keep native case ("BTC", "kPEPE") because their subjects and DB rows
// do. One helper so boot, the studio symbol and the router agree.
inline std::string normalize_symbol_case(const std::string& exchange, std::string symbol) {
    if (exchange == "binancef" || exchange == "bybit") {
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    return symbol;
}

// Decode one path segment: percent-escapes once, malformed escapes kept
// literally, a decoded slash never reinterpreted as routing.
inline std::string url_decode_segment(const std::string& seg) {
    std::string decoded;
    const auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < seg.size(); ++i) {
        if (seg[i] == '%' && i + 2 < seg.size()) {
            const int hi = hex(seg[i + 1]), lo = hex(seg[i + 2]);
            if (hi >= 0 && lo >= 0 && (hi || lo)) {
                decoded += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        decoded += seg[i];
    }
    return decoded;
}

// parse_route reads /terminal/<venue>/<symbol> (canonical), or the legacy
// /terminal/<symbol> with the venue defaulting to binancef unless an
// ?exchange=<ex> query names one. The exchange is resolved BEFORE the symbol
// is cased (see normalize_symbol_case). A path-carried venue wins over the
// query: the path is the address, the query is a leftover.
inline Route parse_route(const std::string& path, const std::string& search = "") {
    Route r;

    const std::string ex = parse_exchange_query(search);
    if (!ex.empty()) r.exchange = ex;

    const std::string prefix = "/terminal/";
    if (path.rfind(prefix, 0) != 0) return r;

    std::string rest = path.substr(prefix.size());
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) return r;

    // Split the path once; decode each segment after splitting.
    const auto slash = rest.find('/');
    std::string first = url_decode_segment(rest.substr(0, slash));
    std::string first_lower = first;
    std::transform(first_lower.begin(), first_lower.end(), first_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (slash != std::string::npos && is_known_exchange(first_lower)) {
        // Canonical: /terminal/<venue>/<symbol>[/ignored]
        r.exchange = first_lower;
        const std::string tail = rest.substr(slash + 1);
        const auto next = tail.find('/');
        r.symbol = url_decode_segment(tail.substr(0, next));
    } else {
        // Alias: /terminal/<symbol>[/ignored], venue from the query or default.
        r.symbol = std::move(first);
    }
    if (r.symbol.empty()) return r;

    r.symbol = normalize_symbol_case(r.exchange, r.symbol);
    return r;
}

// Canonical terminal path: /terminal/<venue>/<symbol>. An unset venue means
// binancef. There is deliberately no symbol-only overload any more: a symbol
// is not an address, and the overload was how venue-less links got minted.
inline std::string build_terminal_path(const std::string& exchange, const std::string& symbol) {
    return "/terminal/" + (exchange.empty() ? std::string("binancef") : exchange) + "/" + symbol;
}
