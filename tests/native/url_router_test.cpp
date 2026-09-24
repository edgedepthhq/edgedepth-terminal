// ═══════════════════════════════════════════════════════════════════════════════
// url_router_test.cpp - native pin on the terminal's own route parsing.
//
// The terminal is a single WASM page whose entire navigation model is the URL:
// /terminal/<venue>/<symbol> is canonical; the legacy /terminal/<symbol> and
// /terminal/<symbol>?exchange=<venue> still parse. Three rules in here are
// load bearing and none is obvious from the call sites.
//
//   1. Symbol case is venue dependent. Binance futures symbols are canonically
//      lowercase and get folded; Hyperliquid coins are uppercase "BTC"
//      end-to-end in the backend, so folding them would route to a symbol that
//      does not exist. Which means the exchange has to be resolved BEFORE the
//      symbol is cased, and the ordering is what this file pins.
//
//   2. Every legacy URL must keep resolving to the market it always did. The
//      venue-less alias is binancef, because every bookmark, share link,
//      marketing link and lesson deep link written before 2026-09-19 is one.
//
//   3. The builder emits ONLY the canonical shape. A first segment is a venue
//      iff it is a known exchange id, so no symbol can be mistaken for one.
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
    expect_route("/terminal/%E7%89%9B%E6%9D%A5USDT", "", "binancef", "牛来usdt", "encoded Chinese route");
    expect_route("/terminal/牛来USDT", "", "binancef", "牛来usdt", "literal Chinese route");
    expect_route("/terminal/%E7%89%9B", "?exchange=hl", "hl", "牛", "UTF-8 across venues");
    expect_route("/terminal/%2541", "", "binancef", "%41", "decode only once");
    expect_route("/terminal/%GG%", "", "binancef", "%gg%", "malformed escapes stay literal");
    expect_route("/terminal/%00BTC", "", "binancef", "%00btc", "do not introduce NUL");
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
    expect_route("/terminal/BTCUSDT", "?exchange=bybit", "bybit", "btcusdt", "a bybit contract is lowercased like binance");
    // The router does not judge symbols: a hyphenated name parses like any
    // other (dated contracts are no longer listed, but an old link must still
    // resolve to SOMETHING rather than crash the boot).
    expect_route("/terminal/BTCUSDT-02OCT26", "?exchange=bybit", "bybit", "btcusdt-02oct26", "a hyphenated symbol passes through");
    expect_route("/terminal/kPEPE", "?exchange=hl", "hl", "kPEPE",
                 "a hyperliquid coin keeps its inner case too");
    // Same path, no venue: now it IS a binancef symbol and does fold.
    expect_route("/terminal/BTC", "", "binancef", "btc", "the same path folds without a venue");
    // And an explicit binancef in the query behaves like the default.
    expect_route("/terminal/BTC", "?exchange=binancef", "binancef", "btc",
                 "an explicit binancef still folds");
}

void test_canonical_venue_path() {
    expect_route("/terminal/binancef/btcusdt", "", "binancef", "btcusdt", "canonical binancef path");
    expect_route("/terminal/bybit/btcusdt", "", "bybit", "btcusdt", "canonical bybit path");
    expect_route("/terminal/hl/BTC", "", "hl", "BTC", "canonical hyperliquid path keeps case");
    expect_route("/terminal/hl/kPEPE", "", "hl", "kPEPE", "canonical hyperliquid inner case kept");
    expect_route("/terminal/BYBIT/BTCUSDT", "", "bybit", "btcusdt", "the venue segment folds, then the symbol");
    expect_route("/terminal/bybit/btcusdt/", "", "bybit", "btcusdt", "a trailing slash is trimmed");
    expect_route("/terminal/bybit/btcusdt/extra", "", "bybit", "btcusdt", "deeper segments are ignored");
    expect_route("/terminal/bybit/%E7%89%9B", "", "bybit", "牛", "the symbol segment is decoded");
    // The path is the address; a stale query venue does not override it.
    expect_route("/terminal/bybit/btcusdt", "?exchange=hl", "bybit", "btcusdt",
                 "a path venue wins over a leftover query venue");
    // A first segment that is NOT a known venue is a symbol, as it always was.
    expect_route("/terminal/okx/btcusdt", "", "binancef", "okx",
                 "an unknown first segment is the legacy symbol, not a venue");
    expect_route("/terminal/hl", "", "binancef", "hl",
                 "a lone venue-looking segment is a symbol (nothing follows it)");
    // A venue segment with an empty symbol carries no symbol.
    expect_route("/terminal/bybit//", "", "bybit", "", "a venue with no symbol carries none");
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
    // The builder emits the canonical venue path and nothing else. The bare
    // /terminal/<symbol> is an ALIAS the parser accepts, never a shape we mint.
    expect_eq(build_terminal_path("binancef", "btcusdt"), "/terminal/binancef/btcusdt",
              "a binancef path names its venue");
    expect_eq(build_terminal_path("", "btcusdt"), "/terminal/binancef/btcusdt",
              "an unset venue is binancef");
    expect_eq(build_terminal_path("bybit", "btcusdt"), "/terminal/bybit/btcusdt",
              "a bybit path names its venue");
    expect_eq(build_terminal_path("hl", "BTC"), "/terminal/hl/BTC",
              "a hyperliquid path names its venue and keeps case");
    expect_true(build_terminal_path("bybit", "btcusdt").find('?') == std::string::npos,
                "the venue is in the path, not the query");

    // Round trip: whatever the builder emits, the parser must recover.
    struct Case {
        const char* exchange;
        const char* symbol;
    };
    const Case cases[] = {
        {"binancef", "btcusdt"}, {"binancef", "ethusdt"}, {"bybit", "btcusdt"},
        {"bybit", "ethusdt"},    {"hl", "BTC"},           {"hl", "kPEPE"},
        {"hl", "SOL"},
    };
    for (const Case& c : cases) {
        const std::string built = build_terminal_path(c.exchange, c.symbol);
        const Route back = parse_route(built, "");
        expect_eq(back.exchange, c.exchange, "the venue survives a build/parse round trip");
        expect_eq(back.symbol, c.symbol, "the symbol survives a build/parse round trip");
    }

    // And the legacy shapes resolve to the same market the canonical one does.
    for (const Case& c : cases) {
        const std::string canonical = build_terminal_path(c.exchange, c.symbol);
        const Route want = parse_route(canonical, "");
        const std::string legacy_path = std::string("/terminal/") + c.symbol;
        const std::string legacy_search =
            std::string(c.exchange) == "binancef" ? "" : std::string("?exchange=") + c.exchange;
        const Route legacy = parse_route(legacy_path, legacy_search);
        expect_eq(legacy.exchange, want.exchange, "a legacy link resolves to the canonical venue");
        expect_eq(legacy.symbol, want.symbol, "a legacy link resolves to the canonical symbol");
    }
}

void test_symbol_case_helper() {
    expect_eq(normalize_symbol_case("binancef", "BTCUSDT"), "btcusdt", "binancef folds");
    expect_eq(normalize_symbol_case("bybit", "BTCUSDT"), "btcusdt", "bybit folds");
    expect_eq(normalize_symbol_case("hl", "kPEPE"), "kPEPE", "hyperliquid keeps case");
    expect_true(is_known_exchange("binancef") && is_known_exchange("bybit") && is_known_exchange("hl"),
                "the three hub venues are known");
    expect_true(!is_known_exchange("okx") && !is_known_exchange("") && !is_known_exchange("HL"),
                "anything else, including a non-folded id, is not a venue");
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
    test_canonical_venue_path();
    test_path_shape();
    test_exchange_query_parsing();
    test_the_ws_override_survives_a_route();
    test_build_then_parse_round_trips();
    test_symbol_case_helper();
    test_non_emscripten_stubs_are_inert();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("url_router_test: all assertions passed\n");
    return 0;
}
