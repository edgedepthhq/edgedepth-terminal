// ═══════════════════════════════════════════════════════════════════════════════
// lead_lag_tracker_test.cpp - native pin on the compare replay's lead/lag read.
//
// Two synthetic tapes on one clock: B repeats A's path 800 ms later. The
// tracker must say A moved first by 800 ms, and must find the +800 ms lag by
// cross-correlation with a strong positive rho. Then the ways it must stay
// quiet: one leg silent, a tape too thin to correlate, a flat tape.
// ═══════════════════════════════════════════════════════════════════════════════

#include "replayer/lead_lag_tracker.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool v, const char* what) {
    if (!v) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

// A path: a random-walk-ish sequence of prices, one trade per 100 ms, that
// jumps +25 bps at step 10 (t = anchor + 1000 ms).
std::vector<double> path(double base) {
    std::vector<double> p;
    double px = base;
    const double wiggle[] = {0.0, 1.0, -0.6, 0.8, -1.2, 0.4, 0.9, -0.3, 1.1, -2.1};  // zero-sum: no drift
    for (int i = 0; i < 400; ++i) {
        if (i == 10) px *= 1.0025;                          // the move
        px *= 1.0 + wiggle[i % 10] * 0.00003;               // texture, well under 10 bps
        p.push_back(px);
    }
    return p;
}

void test_b_follows_a_by_800ms() {
    leadlag::Tracker t;
    t.reset(1'000'000, {"btcusdt", "ethusdt"});
    expect_true(t.armed(), "two legs and an anchor arm the tracker");
    const auto pa = path(100'000.0);
    const auto pb = path(4'000.0);
    for (size_t i = 0; i < pa.size(); ++i) {
        const int64_t ts = 1'000'000 + static_cast<int64_t>(i) * 100;
        t.on_trade("btcusdt", ts, pa[i]);
        t.on_trade("ethusdt", ts + 800, pb[i]);
    }
    const auto r = t.compute(1'000'000 + 400 * 100 + 800);
    expect_true(r.legs.size() == 2, "both legs reported");
    expect_true(r.legs[0].first_move_ts == 1'001'000, "A's first move is the +25 bps jump at +1000 ms");
    expect_true(r.legs[1].first_move_ts == 1'001'800, "B's first move is the same jump 800 ms later");
    expect_true(r.both_moved && r.first_move_lead_ms == 800, "the first-move lead is +800 ms (A first)");
    expect_true(r.legs[0].first_move_dir == 1 && r.legs[1].first_move_dir == 1, "both moved up");
    expect_true(r.lag.ok, "enough overlap to estimate a lag");
    expect_true(r.lag.lag_ms() == 800, "cross-correlation peaks at +800 ms (A leads)");
    expect_true(r.lag.rho > 0.9, "rho at the peak is strong");
    expect_true(std::fabs(r.legs[0].change_bps() - 25.0) < 3.0, "A's net change reads about +25 bps");
}

void test_a_follows_b_reads_negative() {
    leadlag::Tracker t;
    t.reset(5'000'000, {"btcusdt", "ethusdt"});
    const auto pa = path(100'000.0);
    const auto pb = path(4'000.0);
    for (size_t i = 0; i < pa.size(); ++i) {
        const int64_t ts = 5'000'000 + static_cast<int64_t>(i) * 100;
        t.on_trade("btcusdt", ts + 600, pa[i]);   // primary is the FOLLOWER here
        t.on_trade("ethusdt", ts, pb[i]);
    }
    const auto r = t.compute(5'000'000 + 400 * 100 + 600);
    expect_true(r.both_moved && r.first_move_lead_ms == -600, "B first: the lead is negative");
    expect_true(r.lag.ok && r.lag.lag_ms() == -600, "cross-correlation peaks at -600 ms (B leads)");
}

void test_silent_leg_and_thin_tape_stay_quiet() {
    leadlag::Tracker t;
    t.reset(1'000'000, {"btcusdt", "ethusdt"});
    const auto pa = path(100'000.0);
    for (size_t i = 0; i < pa.size(); ++i)
        t.on_trade("btcusdt", 1'000'000 + static_cast<int64_t>(i) * 100, pa[i]);
    const auto r = t.compute(1'040'000);
    expect_true(!r.both_moved, "one silent leg: no first-move lead");
    expect_true(!r.lag.ok, "one silent leg: no lag estimate");

    leadlag::Tracker thin;
    thin.reset(1'000'000, {"btcusdt", "ethusdt"});
    for (int i = 0; i < 5; ++i) {
        thin.on_trade("btcusdt", 1'000'000 + i * 100, 100'000.0 + i);
        thin.on_trade("ethusdt", 1'000'000 + i * 100, 4'000.0 + i);
    }
    expect_true(!thin.compute(1'000'500).lag.ok, "five trades are too thin to correlate");

    leadlag::Tracker flat;
    flat.reset(1'000'000, {"btcusdt", "ethusdt"});
    for (int i = 0; i < 400; ++i) {
        flat.on_trade("btcusdt", 1'000'000 + i * 100, 100'000.0);
        flat.on_trade("ethusdt", 1'000'000 + i * 100, 4'000.0);
    }
    const auto fr = flat.compute(1'040'000);
    expect_true(!fr.both_moved && !fr.lag.ok, "a flat tape moves nothing and correlates nothing");
}

void test_trades_before_anchor_and_unknown_symbols_are_ignored() {
    leadlag::Tracker t;
    t.reset(1'000'000, {"btcusdt", "ethusdt"});
    t.on_trade("btcusdt", 999'999, 1.0);         // before the anchor
    t.on_trade("solusdt", 1'000'000, 1.0);       // not a session leg
    t.on_trade("btcusdt", 1'000'000, 0.0);       // not a price
    const auto r = t.compute(1'000'000);
    expect_true(r.legs[0].anchor_px == 0.0 && r.legs[1].anchor_px == 0.0,
                "nothing before the anchor, off-session or priceless fixes an anchor price");
    leadlag::Tracker one;
    one.reset(1'000'000, {"btcusdt"});
    expect_true(!one.armed(), "a single market is not a compare");
}

}  // namespace

int main() {
    test_b_follows_a_by_800ms();
    test_a_follows_b_reads_negative();
    test_silent_leg_and_thin_tape_stay_quiet();
    test_trades_before_anchor_and_unknown_symbols_are_ignored();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("lead_lag_tracker_test: all assertions passed\n");
    return 0;
}
