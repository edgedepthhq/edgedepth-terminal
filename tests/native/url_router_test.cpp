// ═══════════════════════════════════════════════════════════════════════════════
// url_router_test.cpp - native pin on the terminal's own route parsing.
//
// The terminal is a single WASM page whose entire navigation model is the URL:
// /terminal/<symbol> plus an optional ?exchange=<venue>. Two rules in here are
// load bearing and neither is obvious from the call sites.
//
//   1. Symbol case is venue dependent. Binance futures symbols are canonically
//      lowercase and get folded; Hyperliquid coins are uppercase "BTC"
//      end-to-end in the backend, so folding them would route to a symbol that
//      does not exist. Which means the exchange has to be resolved BEFORE the
//      symbol is cased, and the ordering is what this file pins.
//
//   2. A binancef URL must stay byte-identical to what it was before venues
//      existed. Appending ?exchange=binancef "for consistency" would invalidate
//      every bookmark, share link and lesson deep link in the product.
//
// The companion file research_url_test.cpp covers the outbound terminal ->
// /research link; this one covers the inbound URL -> app-state direction.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/url_router.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got:  '%s'\n  want: '%s'\n", what, got.c_str(),
                     want.c_str());
        ++failures;
    }
}

void expect_route(const std::string& path, const std::string& search,
                  const std::string& want_exchange, const std::string& want_symbol,
                  const char* what) {
    const Route r = parse_route(path, search);
    if (r.exchange != want_exchange || r.symbol != want_symbol) {
        std::fprintf(stderr,
                     "FAIL %s\n  path '%s' search '%s'\n"
                     "  got:  exchange '%s' symbol '%s'\n"
                     "  want: exchange '%s' symbol '%s'\n",
                     what, path.c_str(), search.c_str(), r.exchange.c_str(),
                     r.symbol.c_str(), want_exchange.c_str(), want_symbol.c_str());
        ++failures;
    }
}

void test_default_route() {
    const Route fresh;
    expect_eq(fresh.exchange, "binancef", "a fresh Route defaults to binance futures");
    expect_eq(fresh.symbol, "", "a fresh Route has no symbol");

    // Anything that is not a /terminal/ path yields no symbol, so the caller
    // falls back to its default rather than routing to a garbage pair.
    expect_route("/", "", "binancef", "", "the site root carries no symbol");
    expect_route("/pricing", "", "binancef", "", "a non-terminal path carries no symbol");
    expect_route("/terminal", "", "binancef", "", "the bare /terminal path carries no symbol");
    expect_route("/terminal/", "", "binancef", "", "a trailing slash with no symbol carries none");
    expect_route("/TERMINAL/btcusdt", "", "binancef", "",
                 "the route prefix itself is case sensitive");
    // ...but an explicit venue still survives a path that carries no symbol.
    expect_route("/pricing", "?exchange=hl", "hl", "",
                 "the venue is read even when the path has no symbol");
}

void test_symbol_case_is_venue_dependent() {
    // Binance futures: canonical lowercase, however the link was written.
    expect_route("/terminal/BTCUSDT", "", "binancef", "btcusdt", "an uppercase binancef symbol folds");
    expect_route("/terminal/BtcUsdt", "", "binancef", "btcusdt", "a mixed-case binancef symbol folds");
    expect_route("/terminal/btcusdt", "", "binancef", "btcusdt", "an already-lowercase symbol is unchanged");

    // Hyperliquid: case is preserved, because the backend stores and routes the
    // uppercase coin. This is the assertion that fails if the fold is hoisted
    // above the exchange lookup.
    expect_route("/terminal/BTC", "?exchange=hl", "hl", "BTC", "a hyperliquid coin keeps its case");
    expect_route("/terminal/kPEPE", "?exchange=hl", "hl", "kPEPE",
                 "a hyperliquid coin keeps its inner case too");
    // Same path, no venue: now it IS a binancef symbol and does fold.
    expect_route("/terminal/BTC", "", "binancef", "btc", "the same path folds without a venue");
    // And an explicit binancef in the query behaves like the default.
    expect_route("/terminal/BTC", "?exchange=binancef", "binancef", "btc",
                 "an explicit binancef still folds");
}

void test_path_shape() {
    expect_route("/terminal/btcusdt/", "", "binancef", "btcusdt", "a trailing slash is trimmed");
    expect_route("/terminal/btcusdt/extra", "", "binancef", "btcusdt",
                 "only the first path segment is the symbol");
    expect_route("/terminal/btcusdt/extra/more", "", "binancef", "btcusdt",
                 "deeper segments are ignored too");
    expect_route("/terminal/ethusdt", "", "binancef", "ethusdt", "a second symbol parses the same way");
}

void test_exchange_query_parsing() {
    expect_eq(parse_exchange_query(""), "", "an empty search has no venue");
    expect_eq(parse_exchange_query("?foo=1"), "", "an unrelated param has no venue");
    expect_eq(parse_exchange_query("?exchange=hl"), "hl", "the only param");
    expect_eq(parse_exchange_query("?exchange=hl&foo=1"), "hl", "the first of several params");
    expect_eq(parse_exchange_query("?foo=1&exchange=hl"), "hl", "a later param");
    expect_eq(parse_exchange_query("?foo=1&exchange=hl&bar=2"), "hl", "a param in the middle");
    expect_eq(parse_exchange_query("?exchange=HL"), "hl", "the venue is folded to lowercase");
    expect_eq(parse_exchange_query("?exchange="), "", "an empty venue value reads as absent");

    // An empty venue must not clobber the default. A link written with a
    // dangling ?exchange= should still land on binance futures.
    expect_route("/terminal/btcusdt", "?exchange=", "binancef", "btcusdt",
                 "a dangling exchange param leaves the default intact");

    // A key that merely ENDS in "exchange" is a different key. Matching it would
    // silently route the user to another venue's feed.
    expect_eq(parse_exchange_query("?myexchange=hl"), "",
              "a key ending in 'exchange' is not the exchange param");
    expect_eq(parse_exchange_query("?foo=1&not_exchange=hl"), "",
              "a suffixed key is not the exchange param");
    // Nor is it a match inside another param's VALUE.
    expect_eq(parse_exchange_query("?note=exchange=hl"), "",
              "the exchange param is not matched inside another value");
}

void test_the_ws_override_survives_a_route() {
    // ?ws= is the self-hoster's gateway override. It is not route-owned, so
    // parsing a route must never consume or invalidate it, and the venue must
    // still be found alongside it.
    expect_route("/terminal/btcusdt", "?ws=wss://my.box:8080", "binancef", "btcusdt",
                 "a ws override does not disturb the route");
    expect_route("/terminal/BTC", "?ws=wss://my.box:8080&exchange=hl", "hl", "BTC",
                 "a ws override sits alongside the venue");
    expect_eq(parse_exchange_query("?ws=wss://my.box:8080"), "",
              "a ws override is not mistaken for a venue");
}

void test_build_then_parse_round_trips() {
    // The single-argument form is the legacy shape and must stay bare.
    expect_eq(build_terminal_path("btcusdt"), "/terminal/btcusdt", "the legacy builder is bare");

    // The venue-aware form must produce the IDENTICAL string for binancef, so
    // every existing link keeps working byte for byte.
    expect_eq(build_terminal_path("binancef", "btcusdt"), build_terminal_path("btcusdt"),
              "a binancef path is byte-identical to the legacy path");
    expect_eq(build_terminal_path("", "btcusdt"), "/terminal/btcusdt",
              "an unset venue also produces the bare path");
    expect_eq(build_terminal_path("hl", "BTC"), "/terminal/BTC?exchange=hl",
              "a non-binancef venue is carried in the query");

    // Round trip: whatever the builder emits, the parser must recover.
    struct Case {
        const char* exchange;
        const char* symbol;
    };
    const Case cases[] = {
        {"binancef", "btcusdt"}, {"binancef", "ethusdt"}, {"hl", "BTC"},
        {"hl", "kPEPE"},         {"hl", "SOL"},
    };
    for (const Case& c : cases) {
        const std::string built = build_terminal_path(c.exchange, c.symbol);
        const auto qi = built.find('?');
        const std::string path = (qi == std::string::npos) ? built : built.substr(0, qi);
        const std::string search = (qi == std::string::npos) ? "" : built.substr(qi);
        const Route back = parse_route(path, search);
        expect_eq(back.exchange, c.exchange, "the venue survives a build/parse round trip");
        expect_eq(back.symbol, c.symbol, "the symbol survives a build/parse round trip");
    }
}

void test_non_emscripten_stubs_are_inert() {
    // Built natively these are no-ops, which is what lets this file exist at
    // all. Calling them must not crash or corrupt anything the parser reads.
    url_push("/terminal/btcusdt");
    url_navigate("/terminal/btcusdt");
    url_register_popstate();
    expect_eq(url_get_current_search(), "", "the native search stub is empty");
    expect_true(url_get_current_path().rfind("/terminal/", 0) == 0,
                "the native path stub is still a terminal route");
    const Route r = parse_route(url_get_current_path(), url_get_current_search());
    expect_eq(r.exchange, "binancef", "the native stub route resolves to the default venue");
    expect_true(!r.symbol.empty(), "the native stub route resolves to a symbol");
}

}  // namespace

int main() {
    test_default_route();
    test_symbol_case_is_venue_dependent();
    test_path_shape();
    test_exchange_query_parsing();
    test_the_ws_override_survives_a_route();
    test_build_then_parse_round_trips();
    test_non_emscripten_stubs_are_inert();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("url_router_test: all assertions passed\n");
    return 0;
}
