// ═══════════════════════════════════════════════════════════════════════════════
// renko_builder_test.cpp - native pin on the Renko brick transform.
//
// Renko is the one chart type where the client, not the backend, decides what is
// drawn: bricks are derived from the close series on the fly. That makes two
// properties load bearing, and both are cheap to break by "tidying" the builder.
//
//   1. APPEND-ONLY. Replay mutates already-consumed candles all the time
//      (finalize-replace, late trades, 15s-tick merges). A brick that has been
//      printed must never un-print, or a lesson replayed twice shows a different
//      chart and the pack/replay determinism contract is gone.
//
//   2. ONE STEPPER. The provisional brick that forms under the live price and the
//      committed brick that lands when the candle closes must come out of the
//      same rules, or the rightmost brick visibly jumps at every candle close.
//
// The rest is the classic Renko boundary set: exact-threshold moves, the 2x
// reversal (and the 1x exception from a cold start), gaps that owe several
// bricks at once, and the rebuild triggers that a seek or a timeframe change
// depends on.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/renko_builder.cpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_size(std::size_t got, std::size_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %zu, want %zu\n", what, got, want);
        ++failures;
    }
}

void expect_near(double got, double want, double tolerance, const char* what) {
    if (!std::isfinite(got) || std::fabs(got - want) > tolerance) {
        std::fprintf(stderr, "FAIL %s: got %.9f, want %.9f\n", what, got, want);
        ++failures;
    }
}

void expect_brick(const RenkoBrick& got, bool up, double open, double close,
                  int64_t time_ms, const char* what) {
    if (got.up != up || std::fabs(got.open - open) > 1e-9 ||
        std::fabs(got.close - close) > 1e-9 || got.close_time_ms != time_ms) {
        std::fprintf(stderr,
                     "FAIL %s\n  got:  %s %.4f -> %.4f @ %lld\n"
                     "  want: %s %.4f -> %.4f @ %lld\n",
                     what, got.up ? "up" : "down", got.open, got.close,
                     static_cast<long long>(got.close_time_ms),
                     up ? "up" : "down", open, close, static_cast<long long>(time_ms));
        ++failures;
    }
}

// Every printed level must sit on the brick_size grid. This is what keeps a
// history prepend from shifting the whole chart sideways.
void expect_on_grid(const RenkoBuilder& b, double brick_size, const char* what) {
    for (std::size_t i = 0; i < b.total_count(); ++i) {
        const RenkoBrick& brick = b.at(i);
        for (const double level : {brick.open, brick.close}) {
            const double units = level / brick_size;
            if (std::fabs(units - std::round(units)) > 1e-6) {
                std::fprintf(stderr, "FAIL %s: level %.9f is off the %.9f grid\n",
                             what, level, brick_size);
                ++failures;
                return;
            }
        }
        // And a brick is always exactly one brick_size tall, in its own direction.
        const double expected = brick.up ? brick_size : -brick_size;
        if (std::fabs((brick.close - brick.open) - expected) > 1e-9) {
            std::fprintf(stderr, "FAIL %s: brick %zu is not one brick tall\n", what, i);
            ++failures;
            return;
        }
    }
}

void test_cold_start_and_grid_seed() {
    RenkoBuilder b;
    // The seed snaps to the grid, so the first brick prints from 100 even though
    // the first close was 103. Two different first candles that snap to the same
    // grid point must produce the same levels: that is the property a history
    // prepend relies on.
    b.build({103.0, 112.0}, {1, 2}, 10.0);
    expect_size(b.bricks().size(), 1, "one brick from the first qualifying move");
    expect_brick(b.bricks()[0], true, 100.0, 110.0, 2, "first brick prints off the grid seed");
    expect_on_grid(b, 10.0, "cold start levels");

    RenkoBuilder other;
    other.build({97.0, 112.0}, {1, 2}, 10.0);
    expect_size(other.bricks().size(), 1, "same brick count from a different first close");
    expect_brick(other.bricks()[0], true, 100.0, 110.0, 2,
                 "a different first close snapping to the same grid gives the same brick");

    // An empty series, a zero brick size and a negative brick size all produce
    // nothing rather than dividing by zero.
    RenkoBuilder empty;
    empty.build({}, {}, 10.0);
    expect_size(empty.bricks().size(), 0, "empty series prints nothing");
    empty.build({100.0, 200.0}, {1, 2}, 0.0);
    expect_size(empty.bricks().size(), 0, "zero brick size prints nothing");
    empty.build({100.0, 200.0}, {1, 2}, -5.0);
    expect_size(empty.bricks().size(), 0, "negative brick size prints nothing");
    // Mismatched parallel arrays are truncated to the shorter one, not read past.
    RenkoBuilder ragged;
    ragged.build({100.0, 110.0, 120.0}, {1, 2}, 10.0);
    expect_size(ragged.bricks().size(), 1, "ragged input stops at the shorter array");
}

void test_exact_threshold_is_inclusive() {
    // A move of exactly one brick prints; one tick short of it does not. This is
    // the boundary the whole chart hangs off.
    RenkoBuilder exact;
    exact.build({100.0, 110.0}, {1, 2}, 10.0);
    expect_size(exact.bricks().size(), 1, "a move of exactly one brick prints");

    RenkoBuilder shy;
    shy.build({100.0, 109.999999}, {1, 2}, 10.0);
    expect_size(shy.bricks().size(), 0, "a move one tick short of a brick prints nothing");

    RenkoBuilder down_exact;
    down_exact.build({100.0, 90.0}, {1, 2}, 10.0);
    expect_size(down_exact.bricks().size(), 1, "an exact one-brick fall prints");

    RenkoBuilder down_shy;
    down_shy.build({100.0, 90.000001}, {1, 2}, 10.0);
    expect_size(down_shy.bricks().size(), 0, "a fall one tick short prints nothing");
}

void test_gap_owes_several_bricks_at_one_timestamp() {
    // One candle can owe many bricks. They all carry that candle's timestamp,
    // which is exactly why the Renko x-axis is non-uniform.
    RenkoBuilder b;
    b.build({100.0, 135.0}, {1, 2}, 10.0);
    expect_size(b.bricks().size(), 3, "a 3.5-brick gap prints three bricks");
    expect_brick(b.bricks()[0], true, 100.0, 110.0, 2, "gap brick 1");
    expect_brick(b.bricks()[1], true, 110.0, 120.0, 2, "gap brick 2");
    expect_brick(b.bricks()[2], true, 120.0, 130.0, 2, "gap brick 3");
    expect_on_grid(b, 10.0, "gap levels");
    // The 5 left over do not print: the remainder is carried in the state, not
    // rounded away.
    b.build({100.0, 135.0, 140.0}, {1, 2, 3}, 10.0);
    expect_size(b.bricks().size(), 4, "the carried remainder completes the next brick");
    expect_brick(b.bricks()[3], true, 130.0, 140.0, 3, "brick completed by the carried remainder");
}

void test_reversal_needs_two_bricks() {
    RenkoBuilder b;
    // Up to 130 (dir = up, last brick 120 -> 130).
    b.build({100.0, 135.0}, {1, 2}, 10.0);

    // A 1.5-brick pullback against the trend prints NOTHING. Traditional Renko
    // needs 2x to turn, and this is the assertion that fails if someone
    // "simplifies" the reversal threshold to 1x.
    b.build({100.0, 135.0, 115.0}, {1, 2, 3}, 10.0);
    expect_size(b.bricks().size(), 3, "a 1.5-brick pullback does not turn the trend");

    // 2x does turn it, and the turning brick opens at the PRIOR brick's open,
    // which is the characteristic overlapping 2-brick turn.
    b.build({100.0, 135.0, 115.0, 110.0}, {1, 2, 3, 4}, 10.0);
    expect_size(b.bricks().size(), 4, "a 2x move against the trend turns it");
    expect_brick(b.bricks()[3], false, 120.0, 110.0, 4,
                 "the turning brick opens at the prior brick's open");
    expect_on_grid(b, 10.0, "reversal levels");

    // Turning back up now needs 2x again, from the new down state.
    RenkoBuilder c;
    c.build({100.0, 135.0, 110.0, 125.0}, {1, 2, 3, 4}, 10.0);
    expect_size(c.bricks().size(), 4, "a 1.5-brick bounce does not turn a down trend");
    RenkoBuilder d;
    d.build({100.0, 135.0, 110.0, 130.0}, {1, 2, 3, 4}, 10.0);
    expect_size(d.bricks().size(), 5, "a 2x bounce turns the down trend");
    expect_brick(d.bricks()[4], true, 120.0, 130.0, 4,
                 "the up turn opens at the prior down brick's open");
}

void test_first_move_from_cold_start_is_one_brick_not_two() {
    // The 2x rule needs a trend to reverse. From an unseeded state the first
    // move in EITHER direction is a plain 1x step that sets the direction.
    RenkoBuilder down;
    down.build({100.0, 90.0}, {1, 2}, 10.0);
    expect_size(down.bricks().size(), 1, "the first move down is a 1x step");
    expect_brick(down.bricks()[0], false, 100.0, 90.0, 2, "cold-start down brick");

    RenkoBuilder up;
    up.build({100.0, 110.0}, {1, 2}, 10.0);
    expect_size(up.bricks().size(), 1, "the first move up is a 1x step");
    expect_brick(up.bricks()[0], true, 100.0, 110.0, 2, "cold-start up brick");
}

void test_append_only_survives_a_mutated_tail() {
    // The invariant the replay determinism contract rests on.
    RenkoBuilder b;
    std::vector<double> closes{100.0, 110.0, 120.0};
    const std::vector<int64_t> times{1, 2, 3};
    b.build(closes, times, 10.0);
    expect_size(b.bricks().size(), 2, "two bricks from the initial series");
    const std::vector<RenkoBrick> before = b.bricks();

    // Replay rewrites an already-consumed candle's close. Its TIMESTAMP is
    // unchanged, so the builder recognises the tail and leaves the printed
    // bricks exactly as they were.
    closes[1] = 100.0;
    closes[2] = 105.0;
    b.build(closes, times, 10.0);
    expect_size(b.bricks().size(), 2, "a mutated tail never un-prints a brick");
    for (std::size_t i = 0; i < before.size(); ++i) {
        expect_brick(b.bricks()[i], before[i].up, before[i].open, before[i].close,
                     before[i].close_time_ms, "printed brick survives the mutation");
    }

    // Rebuilding with the identical input is a no-op, not a duplicate append.
    b.build(closes, times, 10.0);
    expect_size(b.bricks().size(), 2, "an identical rebuild appends nothing");

    // A genuinely NEW candle folds on top of the untouched history.
    closes.push_back(130.0);
    std::vector<int64_t> times4 = times;
    times4.push_back(4);
    b.build(closes, times4, 10.0);
    expect_size(b.bricks().size(), 3, "a new candle appends one brick");
    expect_brick(b.bricks()[0], before[0].up, before[0].open, before[0].close,
                 before[0].close_time_ms, "history is still untouched after the append");
    expect_brick(b.bricks()[2], true, 120.0, 130.0, 4, "the appended brick continues from the old state");
}

void test_rebuild_triggers() {
    // A brick size change must rebuild from scratch, not append at the old size.
    RenkoBuilder b;
    b.build({100.0, 140.0}, {1, 2}, 10.0);
    expect_size(b.bricks().size(), 4, "four 10-wide bricks");
    b.build({100.0, 140.0}, {1, 2}, 20.0);
    expect_size(b.bricks().size(), 2, "the same move rebuilds as two 20-wide bricks");
    expect_near(b.brick_size(), 20.0, 0.0, "brick size follows the rebuild");
    expect_on_grid(b, 20.0, "levels after a brick size change");

    // A seek or timeframe reload hands over a series whose timestamps no longer
    // line up. That must rebuild, or bricks from the old window survive the seek.
    RenkoBuilder seek;
    seek.build({100.0, 140.0}, {1, 2}, 10.0);
    seek.build({100.0, 140.0}, {1001, 1002}, 10.0);
    expect_size(seek.bricks().size(), 4, "a timestamp change rebuilds rather than appending");
    expect_brick(seek.bricks()[0], true, 100.0, 110.0, 1002, "rebuilt brick carries the new timestamp");

    // A history prepend shifts every index. The last-consumed candle is no
    // longer where it was, so this rebuilds too.
    RenkoBuilder prepend;
    prepend.build({110.0, 120.0}, {2, 3}, 10.0);
    const std::size_t after_first = prepend.bricks().size();
    prepend.build({100.0, 110.0, 120.0}, {1, 2, 3}, 10.0);
    expect_true(prepend.bricks().size() >= after_first,
                "a prepend rebuilds and keeps at least the moves it already had");
    expect_brick(prepend.bricks().front(), true, 100.0, 110.0, 2,
                 "the rebuilt series starts from the prepended history");

    // A series that got SHORTER (fewer candles than already consumed) rebuilds.
    RenkoBuilder shrink;
    shrink.build({100.0, 110.0, 120.0}, {1, 2, 3}, 10.0);
    shrink.build({100.0, 110.0}, {1, 2}, 10.0);
    expect_size(shrink.bricks().size(), 1, "a shortened series rebuilds to match");
}

void test_building_bricks_never_touch_the_committed_sequence() {
    RenkoBuilder b;
    b.build({100.0, 110.0}, {1, 2}, 10.0);
    expect_size(b.bricks().size(), 1, "one committed brick");

    // The live price implies two more bricks. They appear as provisional only.
    b.fold_building(130.0, 99);
    expect_size(b.bricks().size(), 1, "committed bricks are untouched by a fold");
    expect_size(b.building().size(), 2, "the live price owes two provisional bricks");
    expect_size(b.total_count(), 3, "total spans committed then provisional");
    expect_brick(b.at(0), true, 100.0, 110.0, 2, "at() reads the committed brick first");
    expect_brick(b.at(1), true, 110.0, 120.0, 99, "at() then reads provisional bricks");
    expect_brick(b.at(2), true, 120.0, 130.0, 99, "at() reaches the last provisional brick");

    // Folding is idempotent: it always runs from a COPY of the committed state,
    // so calling it every frame cannot ratchet the chart forward.
    b.fold_building(130.0, 99);
    expect_size(b.building().size(), 2, "a repeated fold at the same price is idempotent");
    b.fold_building(130.0, 99);
    expect_size(b.building().size(), 2, "and stays idempotent on the third frame");

    // A retrace simply drops the provisional bricks next frame.
    b.fold_building(112.0, 100);
    expect_size(b.building().size(), 0, "a retrace drops the provisional bricks");
    expect_size(b.bricks().size(), 1, "and still leaves the committed sequence alone");

    // THE contract: a provisional brick is what the committed brick will be.
    // Fold the live price, then close a candle at that same price, and the
    // committed result must match what was already on screen.
    RenkoBuilder folded;
    folded.build({100.0, 110.0}, {1, 2}, 10.0);
    folded.fold_building(135.0, 3);
    const std::vector<RenkoBrick> provisional = folded.building();
    RenkoBuilder closed;
    closed.build({100.0, 110.0}, {1, 2}, 10.0);
    closed.build({100.0, 110.0, 135.0}, {1, 2, 3}, 10.0);
    expect_size(closed.bricks().size(), 1 + provisional.size(),
                "closing the candle commits exactly the bricks that were provisional");
    for (std::size_t i = 0; i < provisional.size(); ++i) {
        expect_brick(closed.bricks()[1 + i], provisional[i].up, provisional[i].open,
                     provisional[i].close, provisional[i].close_time_ms,
                     "the committed brick matches the provisional brick it replaced");
    }

    // Folding before anything is seeded is a no-op, not a brick off a zero level.
    RenkoBuilder unseeded;
    unseeded.fold_building(1000.0, 1);
    expect_size(unseeded.building().size(), 0, "an unseeded builder folds nothing");
    unseeded.build({}, {}, 10.0);
    unseeded.fold_building(1000.0, 1);
    expect_size(unseeded.building().size(), 0, "an empty series still folds nothing");
}

void test_clear_resets_every_field() {
    RenkoBuilder b;
    b.build({100.0, 140.0}, {1, 2}, 10.0);
    b.fold_building(150.0, 3);
    expect_true(b.total_count() > 0, "builder has state to clear");

    b.clear();
    expect_size(b.bricks().size(), 0, "clear drops committed bricks");
    expect_size(b.building().size(), 0, "clear drops provisional bricks");
    expect_size(b.total_count(), 0, "clear zeroes the total");
    expect_near(b.brick_size(), 0.0, 0.0, "clear forgets the brick size");
    // And the append bookkeeping is reset too: a build after a clear must not
    // think it can append onto bricks that no longer exist.
    b.fold_building(150.0, 3);
    expect_size(b.building().size(), 0, "a cleared builder is unseeded again");
    b.build({100.0, 110.0}, {1, 2}, 10.0);
    expect_size(b.bricks().size(), 1, "a cleared builder rebuilds from scratch");
    expect_brick(b.bricks()[0], true, 100.0, 110.0, 2, "and starts from a fresh grid seed");
}

void test_pathological_brick_size_is_capped() {
    // A tiny brick size on a huge move must not allocate without bound or stall
    // the frame. The cap is the guard; this is the assertion that it is wired in.
    RenkoBuilder b;
    b.build({0.0, 1.0e7}, {1, 2}, 1.0);
    expect_size(b.bricks().size(), RenkoBuilder::MAX_BRICKS, "runaway brick count is capped");
    // Still capped after another call: the builder does not resume past the cap.
    b.build({0.0, 1.0e7, 2.0e7}, {1, 2, 3}, 1.0);
    expect_size(b.bricks().size(), RenkoBuilder::MAX_BRICKS, "the cap holds across calls");
}

void test_auto_brick_ticks() {
    using RB = RenkoBuilder;
    // No price or no tick size yet: 0 tells the caller to defer, rather than
    // resolving to a bogus brick that the chart would then have to rebuild.
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(0.0, 0.1)), 0, "no price defers");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(-1.0, 0.1)), 0, "negative price defers");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(60000.0, 0.0)), 0, "no tick size defers");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(60000.0, -0.1)), 0, "negative tick size defers");

    // Worked examples across the venue's real price scales.
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(60000.0, 0.1)), 1000, "BTC at 60k, 0.1 tick");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(3000.0, 0.01)), 500, "ETH at 3k, 0.01 tick");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(1.0, 0.0001)), 20, "a $1 alt, 0.0001 tick");

    // A tick coarser than the target still yields a usable brick, never 0.
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(0.5, 0.001)), 1, "a coarse tick floors at one");
    expect_size(static_cast<std::size_t>(RB::auto_brick_ticks(1.0, 100.0)), 1, "an absurdly coarse tick still floors at one");

    // The two properties the heuristic actually promises, checked across four
    // decades of price rather than at the three points above.
    int previous = 0;
    for (double price = 0.05; price < 200000.0; price *= 1.07) {
        const int ticks = RB::auto_brick_ticks(price, 0.01);
        expect_true(ticks >= 1, "a resolved brick is always at least one tick");
        expect_true(ticks >= previous, "a higher price never resolves to a smaller brick");
        previous = ticks;

        // "A nice 1/2/5 x 10^n count", which is what makes the default legible.
        int mantissa = ticks;
        while (mantissa % 10 == 0) mantissa /= 10;
        expect_true(mantissa == 1 || mantissa == 2 || mantissa == 5,
                    "the tick count rounds to a 1/2/5 x 10^n figure");

        // And it stays near the 0.15%-of-price target it aims at, once the tick
        // size is fine enough for the target to be expressible at all.
        const double raw = (price * 0.0015) / 0.01;
        if (raw > 1.0) {
            expect_true(ticks >= raw * 0.5 && ticks <= raw * 1.5,
                        "the resolved brick stays within rounding distance of the target");
        }
    }
}

}  // namespace

int main() {
    test_cold_start_and_grid_seed();
    test_exact_threshold_is_inclusive();
    test_gap_owes_several_bricks_at_one_timestamp();
    test_reversal_needs_two_bricks();
    test_first_move_from_cold_start_is_one_brick_not_two();
    test_append_only_survives_a_mutated_tail();
    test_rebuild_triggers();
    test_building_bricks_never_touch_the_committed_sequence();
    test_clear_resets_every_field();
    test_pathological_brick_size_is_capped();
    test_auto_brick_ticks();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("renko_builder_test: all assertions passed\n");
    return 0;
}
