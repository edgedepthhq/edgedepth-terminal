// ═══════════════════════════════════════════════════════════════════════════════
// research_url_test.cpp - native pin on the terminal → /research URL builder.
//
// Run through tests/native/CMakeLists.txt with CMake/CTest (no Emscripten).
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

static void expect_true(bool got, const char* what) {
    if (!got) {
        fprintf(stderr, "FAIL %s\n  got:  false\n  want: true\n", what);
        ++failures;
    }
}

static void expect_false(bool got, const char* what) {
    if (got) {
        fprintf(stderr, "FAIL %s\n  got:  true\n  want: false\n", what);
        ++failures;
    }
}

// Ladder rungs are exact literals on both sides, so this only guards against a
// future rung being computed rather than named.
static void expect_eq_rung(double got, double want, const char* what) {
    const double d = got - want;
    if (!(d > -1e-12 && d < 1e-12)) {
        fprintf(stderr, "FAIL %s\n  got:  %.17g\n  want: %.17g\n", what, got, want);
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
              "https://edgedepth.com/research/workbench?source=record&study=investigate"
              "&entry=terminal&moment=btcusdt,2026-08-01T13:57:00Z",
              "moment url");
    expect_eq(moment_url("blessusdt", t_minute, "feature.vpin,feature.taker_buy_ratio_15m"),
              "https://edgedepth.com/research/workbench?source=record&study=investigate"
              "&entry=terminal&moment=blessusdt,2026-08-01T13:57:00Z"
              "&mfields=feature.vpin,feature.taker_buy_ratio_15m",
              "moment url with mfields");
    expect_eq(moment_live_url("ETH/USDT"),
              "https://edgedepth.com/research/workbench?source=record&study=investigate"
              "&entry=terminal&moment=ethusdt,now",
              "live url");
    expect_eq(join_mfields({}), "", "join empty");
    expect_eq(join_mfields({"feature.vpin"}), "feature.vpin", "join one");

    // ─── snap_move: the shift-dragged range → a target the engine counts ─────
    // Magnitude snaps DOWN and the horizon snaps UP, so the dragged move is
    // always a member of the population it opens. kind is always "reached".
    const int64_t t_2258 = 1788303480000LL;         // 2026-09-01T22:58:00Z
    const int64_t t_2258_41 = 1788303521500LL;      // 2026-09-01T22:58:41.5Z
    const int64_t h3m10 = (3 * 60 + 10) * 60000LL;  // 3h10m
    const int64_t m50 = 50 * 60000LL;
    const int64_t d7 = 7 * 24 * 60 * 60000LL;

    // +11.8% over 3h10m: 0.118 snaps down to the 0.10 rung, 190 minutes snaps
    // up to the 4h horizon. The brief's worked example.
    {
        const MoveSnap s = snap_move(t_2258, t_2258 + h3m10, 100.0, 110.0, 111.8, 99.0);
        expect_true(s.ok, "snap +11.8%/3h10m ok");
        expect_eq(s.direction, "up", "snap +11.8%/3h10m direction");
        expect_eq_rung(s.magnitude, 0.10, "snap +11.8%/3h10m magnitude");
        expect_eq(s.horizon, "4h", "snap +11.8%/3h10m horizon");
        expect_eq_i64(s.start_minute_ms, t_2258, "snap +11.8%/3h10m anchor");
        expect_eq(move_label(s), "up 10% in 4h", "menu shortcut label");
    }

    // -3% over 50m: 0.03 snaps down to 0.02, 50 minutes snaps up to 1h. The
    // extreme is the low, and direction follows the last close, not the low.
    {
        const MoveSnap s = snap_move(t_2258, t_2258 + m50, 100.0, 98.0, 100.5, 97.0);
        expect_true(s.ok, "snap -3%/50m ok");
        expect_eq(s.direction, "down", "snap -3%/50m direction");
        expect_eq_rung(s.magnitude, 0.02, "snap -3%/50m magnitude");
        expect_eq(s.horizon, "1h", "snap -3%/50m horizon");
        expect_eq(move_label(s), "down 2% in 1h", "menu shortcut label, down");
    }

    // Too small to count: shorter than a minute, or under the smallest rung.
    expect_false(snap_move(t_2258, t_2258 + 30000, 100.0, 110.0, 111.8, 99.0).ok,
                 "snap rejects a sub-minute drag");
    expect_false(snap_move(t_2258, t_2258 + m50, 100.0, 100.05, 100.05, 99.99).ok,
                 "snap rejects a move under the 0.001 rung");
    expect_false(snap_move(t_2258, t_2258, 100.0, 110.0, 111.8, 99.0).ok,
                 "snap rejects an empty range");
    expect_false(snap_move(t_2258, t_2258 + m50, 0.0, 110.0, 111.8, 99.0).ok,
                 "snap rejects a missing anchor close");

    // The closed horizon list ends at 7d. Exactly 7d is inside it; a minute
    // more has no horizon and is refused rather than silently widened.
    expect_eq(snap_move(t_2258, t_2258 + d7, 100.0, 130.0, 130.0, 99.0).horizon, "7d",
              "snap 7d horizon");
    expect_false(snap_move(t_2258, t_2258 + d7 + 60000, 100.0, 130.0, 130.0, 99.0).ok,
                 "snap rejects longer than 7d");

    // The 60000 floor again, on the drag start this time: an unaligned drag
    // anchors on ITS minute. The span is then measured on the minute grid the
    // engine counts on (22:58 to 23:58 is 60 minutes, so 1h still covers it),
    // never in raw milliseconds from the unaligned start.
    {
        const MoveSnap s = snap_move(t_2258_41, t_2258_41 + 60 * 60000LL, 100.0, 103.0, 103.0, 99.5);
        expect_true(s.ok, "snap unaligned start ok");
        expect_eq_i64(s.start_minute_ms, t_2258, "snap floors the start to its minute");
        expect_eq(iso_utc(s.start_minute_ms), "2026-09-01T22:58:00Z", "snap anchor iso");
        expect_eq(s.horizon, "1h", "snap spans whole minutes, not raw ms");
    }

    // Ladder ends: an up move past the top rung stops at 4.00; a down move
    // stops at 1.00 because a return cannot fall past total loss.
    expect_eq_rung(snap_move(t_2258, t_2258 + m50, 100.0, 900.0, 1100.0, 99.0).magnitude, 4.00,
                   "snap caps up at the top rung");
    expect_eq_rung(snap_move(t_2258, t_2258 + m50, 100.0, 1.0, 100.0, 0.0).magnitude, 1.00,
                   "snap caps down at 1.00");

    // The handoff URL, byte for byte. This string is the contract shared with
    // the web door: magnitude is a fraction, the horizon a closed suffix, and
    // the kind is always "reached".
    {
        const MoveSnap s = snap_move(t_2258, t_2258 + h3m10, 100.0, 110.0, 111.8, 99.0);
        expect_eq(outcome_first_url("BTC/USDT", s),
                  "https://edgedepth.com/research/workbench?source=record&study=outcome-first"
                  "&entry=terminal&target=reached,up,0.1,4h&symbol=btcusdt"
                  "&at=2026-09-01T22:58:00Z&scope=sector",
                  "outcome-first url");
        expect_eq(outcome_first_url("BTC/USDT", MoveSnap{}), "",
                  "outcome-first url is empty without a snapped move");
    }

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("research_url_test: all assertions passed\n");
    return 0;
}
