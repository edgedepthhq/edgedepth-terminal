// ═══════════════════════════════════════════════════════════════════════════════
// research_url_test.cpp - native pin on the terminal → /research URL builder.
//
// Run:  bash tests/native/run_tests.sh   (plain host g++, no Emscripten)
//
// The one bug class this exists to catch forever: reusing the chart's
// timeframe-floored context timestamp for research. A 4H chart floors the
// right-click to a 4-hour boundary; research reads a MINUTE. The assertions
// below fail if the 60000 floor ever drifts.
// ═══════════════════════════════════════════════════════════════════════════════

#include "../../src/core/research_url.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

static void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}

static void expect_eq_i64(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s\n  got:  %lld\n  want: %lld\n", what,
                static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

int main() {
    using namespace research_url;

    // Symbol normalization: the display pair becomes the web's lowercase form.
    expect_eq(normalize_symbol("BTC/USDT"), "btcusdt", "normalize BTC/USDT");
    expect_eq(normalize_symbol("btcusdt"), "btcusdt", "normalize already-normal");
    expect_eq(normalize_symbol("1000PEPE/USDT"), "1000pepeusdt", "normalize numeric prefix");
    expect_eq(normalize_symbol(""), "", "normalize empty");

    // The 60000 floor - the trap. 2026-08-01T13:57:41.500Z lives in minute 13:57.
    const int64_t t_mid_minute = 1785592661500LL;  // 2026-08-01T13:57:41.5Z
    const int64_t t_minute = 1785592620000LL;      // 2026-08-01T13:57:00Z
    expect_eq_i64(floor_minute_ms(t_mid_minute), t_minute, "floor mid-minute");
    expect_eq_i64(floor_minute_ms(t_minute), t_minute, "floor exact minute");
    expect_eq_i64(floor_minute_ms(0), 0, "floor zero");
    expect_eq_i64(floor_minute_ms(-5), 0, "floor negative");

    // A 4H-chart-floored timestamp and the minute floor of the SAME click
    // differ: this is the silent wrong-minute failure the spec names.
    const int64_t four_h_ms = 4LL * 3600 * 1000;
    const int64_t tf_floored = (t_mid_minute / four_h_ms) * four_h_ms;
    if (tf_floored == floor_minute_ms(t_mid_minute)) {
        fprintf(stderr, "FAIL tf-floor vs minute-floor must differ for a mid-window click\n");
        ++failures;
    }

    // ISO formatting, seconds precision, UTC.
    expect_eq(iso_utc(t_minute), "2026-08-01T13:57:00Z", "iso minute");
    expect_eq(minute_label_utc(t_mid_minute), "13:57", "menu shortcut label");
    expect_eq(minute_header_utc(t_mid_minute), "2026-08-01 13:57 UTC", "panel header label");

    // The full handoff URL: floors internally, normalizes internally.
    expect_eq(moment_url("BTC/USDT", t_mid_minute),
              "https://edgedepth.com/research?source=record&study=investigate"
              "&entry=terminal&moment=btcusdt,2026-08-01T13:57:00Z",
              "moment url");
    expect_eq(moment_url("blessusdt", t_minute, "feature.vpin,feature.taker_buy_ratio_15m"),
              "https://edgedepth.com/research?source=record&study=investigate"
              "&entry=terminal&moment=blessusdt,2026-08-01T13:57:00Z"
              "&mfields=feature.vpin,feature.taker_buy_ratio_15m",
              "moment url with mfields");
    expect_eq(moment_live_url("ETH/USDT"),
              "https://edgedepth.com/research?source=record&study=investigate"
              "&entry=terminal&moment=ethusdt,now",
              "live url");
    expect_eq(join_mfields({}), "", "join empty");
    expect_eq(join_mfields({"feature.vpin"}), "feature.vpin", "join one");

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("research_url_test: all assertions passed\n");
    return 0;
}
