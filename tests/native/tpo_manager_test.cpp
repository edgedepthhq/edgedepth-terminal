// ═══════════════════════════════════════════════════════════════════════════════
// tpo_manager_test.cpp - native pin on the Market Profile (TPO) computation.
//
// This is the one chart study the terminal computes ENTIRELY on the client:
// there is no backend endpoint to compare against, so whatever this file says a
// profile is, is what the user sees. Market Profile is also a study traders read
// quantitatively rather than impressionistically - a POC one row off, or a value
// area that captures 60% instead of 70%, is a wrong number presented as a
// precise one, and nothing on screen says so.
//
// The structure under test: 30m candles are bucketed into UTC sessions, each
// candle prints a block into every price row it touches (once per row per 30m
// period), and the profile's key levels fall out of the resulting histogram:
//
//   POC             the widest row, tie-broken toward the session midpoint
//   Value area      expand out from the POC, always taking the fatter side,
//                   until 70% of all blocks are inside
//   Single prints   a one-block row with fatter rows either side
//   Poor high/low   the extreme row printed twice or more (price lingered)
//   Initial balance the rows printed by the first two periods (00:00-01:00 UTC)
//
// The fixtures below are small enough to work out by hand, and the expected
// histograms are written out in the comments so a failure says which step of the
// computation moved rather than just which number changed.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/tpo_manager.cpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_int(int got, int want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
        ++failures;
    }
}

void expect_double(double got, double want, const char* what) {
    if (std::fabs(got - want) > 1e-9) {
        std::fprintf(stderr, "FAIL %s: got %.9f, want %.9f\n", what, got, want);
        ++failures;
    }
}

constexpr int64_t kDay = 86400000LL;
constexpr int64_t kPeriod = 1800000LL;  // 30 minutes
// Any exact multiple of a day is a UTC midnight, which is where a daily session
// starts. 20320 days after the epoch.
constexpr int64_t kSession = 20320LL * kDay;

struct Candle {
    double ts;
    double high;
    double low;
};

// Build one session's worth of candles, one per consecutive 30m period.
std::vector<Candle> periods(std::initializer_list<std::pair<double, double>> high_low,
                            int64_t session_start = kSession) {
    std::vector<Candle> out;
    int idx = 0;
    for (const auto& [hi, lo] : high_low) {
        out.push_back({static_cast<double>(session_start + idx * kPeriod), hi, lo});
        ++idx;
    }
    return out;
}

const TPOSession* build(TPOManager& mgr, const std::string& symbol,
                        const std::vector<Candle>& candles, double tick_per_row) {
    std::vector<double> ts, highs, lows;
    for (const Candle& c : candles) {
        ts.push_back(c.ts);
        highs.push_back(c.high);
        lows.push_back(c.low);
    }
    mgr.build_sessions(symbol, ts.data(), highs.data(), lows.data(), ts.size(), 1800,
                       tick_per_row);
    const std::vector<TPOSession>* sessions = mgr.get_sessions(symbol);
    if (!sessions || sessions->empty()) return nullptr;
    return &sessions->front();
}

int blocks_in(const TPOSession& s, int row) {
    return static_cast<int>(s.rows[static_cast<std::size_t>(row)].blocks.size());
}

// ── Fixture A ────────────────────────────────────────────────────────────────
// tick_per_row 1.0, so row r covers [100+r, 101+r).
//
//   p0  101.5-102.5   ->  r1 r2
//   p1  100.0-102.5   ->  r0 r1 r2
//   p2  101.5-103.5   ->  r1 r2 r3
//   p3  102.0-102.9   ->  r2
//   p4  102.2-104.5   ->  r2 r3 r4
//
//   r4 (104-105)  #            1
//   r3 (103-104)  ##           2
//   r2 (102-103)  #####        5   <- POC, widest by a clear margin
//   r1 (101-102)  ###          3
//   r0 (100-101)  #            1
//                             12 blocks over 5 periods
std::vector<Candle> fixture_a() {
    return periods({{102.5, 101.5}, {102.5, 100.0}, {103.5, 101.5}, {102.9, 102.0},
                    {104.5, 102.2}});
}

void test_the_histogram_is_built_row_by_row() {
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    if (!s) {
        std::fprintf(stderr, "FAIL fixture A produced no session\n");
        ++failures;
        return;
    }

    expect_double(s->session_low, 100.0, "session low is the lowest candle low");
    expect_double(s->session_high, 104.5, "session high is the highest candle high");
    expect_double(s->tick_per_row, 1.0, "the requested row height is used");

    // The grid spans every row the session touched, and no more.
    expect_int(static_cast<int>(s->rows.size()), 5, "five rows cover 100 to 104");
    expect_double(s->rows.front().price_lo, 100.0, "the bottom row starts at the session low");
    expect_double(s->rows.back().price_hi, 105.0, "the top row ends one row above the high");
    for (std::size_t r = 1; r < s->rows.size(); ++r) {
        expect_double(s->rows[r].price_lo, s->rows[r - 1].price_hi,
                      "rows tile the range with no gap or overlap");
    }

    expect_int(blocks_in(*s, 0), 1, "row 0 count");
    expect_int(blocks_in(*s, 1), 3, "row 1 count");
    expect_int(blocks_in(*s, 2), 5, "row 2 count");
    expect_int(blocks_in(*s, 3), 2, "row 3 count");
    expect_int(blocks_in(*s, 4), 1, "row 4 count");

    expect_int(s->total_periods, 5, "five 30m periods printed");
    expect_int(s->total_blocks, 12, "twelve blocks in total");
    expect_int(s->max_block_count, 5, "the widest row sets the render normalisation");

    // total_blocks must equal what is actually in the rows, or every percentage
    // derived from it (the value area target above all) is computed off a lie.
    int summed = 0;
    for (const TPORow& row : s->rows) summed += static_cast<int>(row.blocks.size());
    expect_int(summed, s->total_blocks, "the block total matches the rows");

    // A period prints into a row at most once, however many candles of that
    // period touched it. Two candles in one 30m slot are one TPO, not two.
    for (const TPORow& row : s->rows) {
        for (std::size_t i = 1; i < row.blocks.size(); ++i) {
            expect_true(row.blocks[i].period_idx != row.blocks[i - 1].period_idx,
                        "a period never prints twice into the same row");
        }
    }
}

void test_poc_is_the_widest_row() {
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    if (!s) return;

    expect_int(s->poc_row_idx, 2, "the POC is the widest row");
    expect_double(s->poc_price, 102.5, "the POC price is that row's midpoint");
    expect_true(s->rows[2].is_poc, "the POC row is flagged");

    int flagged = 0;
    for (const TPORow& row : s->rows) {
        if (row.is_poc) ++flagged;
    }
    expect_int(flagged, 1, "exactly one row is the POC");

    // No row may be wider than the POC.
    for (const TPORow& row : s->rows) {
        expect_true(static_cast<int>(row.blocks.size()) <=
                        static_cast<int>(s->rows[static_cast<std::size_t>(s->poc_row_idx)]
                                             .blocks.size()),
                    "no row out-widths the POC");
    }
}

void test_poc_ties_break_toward_the_session_midpoint() {
    // Two rows tie at 2 blocks: r0 (100-101) and r5 (105-106). The session
    // midpoint is 105.45, so the POC is r5 even though r0 is found first.
    //
    //   p0  100.0-110.9  -> every row r0..r10
    //   p1  105.0-105.9  -> r5
    //   p2  100.0-100.9  -> r0
    TPOManager mgr;
    const TPOSession* s = build(
        mgr, "btcusdt", periods({{110.9, 100.0}, {105.9, 105.0}, {100.9, 100.0}}), 1.0);
    if (!s) {
        std::fprintf(stderr, "FAIL tie fixture produced no session\n");
        ++failures;
        return;
    }

    expect_int(static_cast<int>(s->rows.size()), 11, "eleven rows cover 100 to 110");
    expect_int(blocks_in(*s, 0), 2, "the low row ties at two blocks");
    expect_int(blocks_in(*s, 5), 2, "the middle row ties at two blocks");
    expect_int(s->max_block_count, 2, "nothing is wider than the tie");

    expect_int(s->poc_row_idx, 5, "the tie breaks toward the session midpoint");
    expect_double(s->poc_price, 105.5, "and the POC price is that row");
}

void test_value_area_captures_seventy_percent_around_the_poc() {
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    if (!s) return;

    // 12 blocks, 70% target = ceil(8.4) = 9. Expanding from r2 (5): the lower
    // side is fatter (r1 has 3 vs r3's 2) so r1 joins first for 8, then r3 for
    // 10, which clears the target. r0 and r4 stay outside.
    expect_true(!s->rows[0].is_value_area, "the thin bottom row is outside value");
    expect_true(s->rows[1].is_value_area, "the fatter lower row is inside value");
    expect_true(s->rows[2].is_value_area, "the POC row is always inside value");
    expect_true(s->rows[3].is_value_area, "the upper row joins to clear the target");
    expect_true(!s->rows[4].is_value_area, "the thin top row is outside value");

    expect_double(s->val, 101.0, "VAL is the bottom of the lowest value row");
    expect_double(s->vah, 104.0, "VAH is the top of the highest value row");

    // The three properties that make a value area a value area, checked from the
    // flags rather than restated from the walk above.
    int inside = 0, first = -1, last = -1;
    for (int r = 0; r < static_cast<int>(s->rows.size()); ++r) {
        if (!s->rows[static_cast<std::size_t>(r)].is_value_area) continue;
        inside += blocks_in(*s, r);
        if (first < 0) first = r;
        last = r;
    }
    const int target = static_cast<int>(std::ceil(s->total_blocks * 0.70));
    expect_true(inside >= target, "the value area holds at least the target share");
    expect_int(last - first + 1, 3, "the value area is one contiguous run");
    expect_true(first <= s->poc_row_idx && s->poc_row_idx <= last,
                "and the run contains the POC");
    expect_true(s->val <= s->poc_price && s->poc_price <= s->vah,
                "the POC sits between VAL and VAH");
    expect_true(s->val >= s->rows.front().price_lo && s->vah <= s->rows.back().price_hi,
                "the value area stays inside the session range");
}

void test_value_area_expands_one_sided_when_a_side_runs_out() {
    // The POC is the bottom row, so the walk can only go up. It must keep going
    // rather than stopping at the exhausted side.
    //
    //   p0  100.0-100.9  -> r0
    //   p1  100.0-100.9  -> r0
    //   p2  100.5-102.5  -> r0 r1 r2
    //   p3  102.0-102.9  -> r2
    //   p4  102.0-102.9  -> r2
    //
    //   r2  ###   3
    //   r1  #     1   <- single print: one block, fatter rows either side
    //   r0  ###   3   <- POC (ties with r2, closer to the 101.45 midpoint)
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt",
                                periods({{100.9, 100.0},
                                         {100.9, 100.0},
                                         {102.5, 100.5},
                                         {102.9, 102.0},
                                         {102.9, 102.0}}),
                                1.0);
    if (!s) {
        std::fprintf(stderr, "FAIL one-sided fixture produced no session\n");
        ++failures;
        return;
    }

    expect_int(static_cast<int>(s->rows.size()), 3, "three rows");
    expect_int(blocks_in(*s, 0), 3, "bottom row count");
    expect_int(blocks_in(*s, 1), 1, "middle row count");
    expect_int(blocks_in(*s, 2), 3, "top row count");
    expect_int(s->total_blocks, 7, "seven blocks");
    expect_int(s->poc_row_idx, 0, "the POC is the lower of the two tied rows");

    // Target ceil(7 * 0.7) = 5. From r0 (3) the only direction is up: r1 makes
    // 4, still short, so r2 joins for 7.
    expect_true(s->rows[0].is_value_area, "the POC row is inside value");
    expect_true(s->rows[1].is_value_area, "the thin row is absorbed on the way up");
    expect_true(s->rows[2].is_value_area, "and the walk continues past it to the target");
    expect_double(s->val, 100.0, "VAL is the session floor here");
    expect_double(s->vah, 103.0, "VAH is the session ceiling here");

    // Single print: exactly one block, with fatter rows above and below.
    expect_true(s->rows[1].is_single_print, "the one-block row between two fat rows is a single print");
    expect_true(!s->rows[0].is_single_print, "the bottom row is never a single print");
    expect_true(!s->rows[2].is_single_print, "the top row is never a single print");

    // Poor high and poor low: price lingered at both extremes.
    expect_true(s->has_poor_high, "a top row with two or more blocks is a poor high");
    expect_true(s->has_poor_low, "a bottom row with two or more blocks is a poor low");
}

void test_extremes_printed_once_are_not_poor() {
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    if (!s) return;
    // Fixture A's extremes were each touched by a single period, which is a
    // clean rejection rather than a poor one.
    expect_int(blocks_in(*s, 0), 1, "the bottom row was printed once");
    expect_int(blocks_in(*s, 4), 1, "the top row was printed once");
    expect_true(!s->has_poor_low, "a bottom row printed once is not a poor low");
    expect_true(!s->has_poor_high, "a top row printed once is not a poor high");
    // And nothing in this profile qualifies as a single print.
    for (const TPORow& row : s->rows) {
        expect_true(!row.is_single_print, "no single prints in a filled profile");
    }
}

void test_initial_balance_is_the_first_hour() {
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    if (!s) return;

    // IB is periods 0 and 1 only, i.e. 00:00-01:00 UTC. In fixture A those are
    // p0 (r1,r2) and p1 (r0,r1,r2), so rows 0-2 are IB and rows 3-4 are not.
    expect_true(s->rows[0].is_initial_balance, "row 0 was printed in the first hour");
    expect_true(s->rows[1].is_initial_balance, "row 1 was printed in the first hour");
    expect_true(s->rows[2].is_initial_balance, "row 2 was printed in the first hour");
    expect_true(!s->rows[3].is_initial_balance, "row 3 was only printed later");
    expect_true(!s->rows[4].is_initial_balance, "row 4 was only printed later");

    expect_double(s->ib_low, 100.0, "IB low is the bottom of the lowest first-hour row");
    expect_double(s->ib_high, 103.0, "IB high is the top of the highest first-hour row");
    expect_true(s->ib_low >= s->session_low, "the IB sits inside the session range");
    expect_true(s->ib_high <= s->rows.back().price_hi, "the IB cannot exceed the grid");

    // A session shorter than the first hour is all IB; a session with no rows
    // at all reports a zeroed IB rather than the sentinel it starts from.
    TPOManager brief;
    const TPOSession* b = build(brief, "ethusdt", periods({{101.5, 100.0}}), 1.0);
    if (b) {
        expect_int(b->total_periods, 1, "a lone candle is one period");
        for (const TPORow& row : b->rows) {
            expect_true(row.is_initial_balance, "every row of a first-hour-only session is IB");
        }
        expect_double(b->ib_low, 100.0, "the brief session's IB low");
        expect_double(b->ib_high, 102.0, "the brief session's IB high");
    } else {
        std::fprintf(stderr, "FAIL brief fixture produced no session\n");
        ++failures;
    }
}

void test_candles_are_split_into_utc_day_sessions() {
    // Two candles a day apart belong to two different profiles. Merging them
    // would silently blend yesterday's structure into today's.
    TPOManager mgr;
    std::vector<double> ts{static_cast<double>(kSession),
                           static_cast<double>(kSession + 2 * kPeriod),
                           static_cast<double>(kSession + kDay),
                           static_cast<double>(kSession + kDay + 2 * kPeriod)};
    std::vector<double> highs{101.0, 102.0, 201.0, 202.0};
    std::vector<double> lows{100.0, 101.0, 200.0, 201.0};
    mgr.build_sessions("btcusdt", ts.data(), highs.data(), lows.data(), ts.size(), 1800, 1.0);

    const std::vector<TPOSession>* sessions = mgr.get_sessions("btcusdt");
    expect_true(sessions != nullptr, "sessions were built");
    if (!sessions) return;
    expect_int(static_cast<int>(sessions->size()), 2, "two UTC days are two sessions");

    const TPOSession& day1 = (*sessions)[0];
    const TPOSession& day2 = (*sessions)[1];
    expect_true(day1.session_start_ms == kSession, "the first session starts at UTC midnight");
    expect_true(day1.session_end_ms == kSession + kDay, "and runs a full day");
    expect_true(day2.session_start_ms == kSession + kDay, "the second starts the next midnight");
    expect_true(day1.session_end_ms == day2.session_start_ms, "the sessions abut exactly");

    // Neither session sees the other's prices.
    expect_double(day1.session_high, 102.0, "day one keeps its own high");
    expect_double(day2.session_low, 200.0, "day two keeps its own low");
    expect_true(day1.session_high < day2.session_low, "the two profiles do not overlap");

    // A candle landing exactly on midnight belongs to the NEW session, not the
    // old one: the session range is half open, [start, end).
    expect_true(day2.session_start_ms == kSession + kDay,
                "a candle at the boundary opens the next session");
}

void test_auto_row_height_and_the_grid_sanity_bail() {
    // tick_per_row 0 asks the manager to size the grid itself, targeting roughly
    // fifty rows so the profile is readable at any price scale.
    TPOManager mgr;
    const TPOSession* s = build(mgr, "btcusdt",
                               periods({{60500.0, 60000.0}, {60400.0, 60100.0}}), 0.0);
    if (!s) {
        std::fprintf(stderr, "FAIL auto-height fixture produced no session\n");
        ++failures;
        return;
    }
    expect_double(s->tick_per_row, 10.0, "a 500-wide range auto-sizes to 10 per row");
    expect_true(s->rows.size() >= 40 && s->rows.size() <= 60,
                "the auto grid lands near fifty rows");
    expect_true(s->total_blocks > 0, "the auto grid still prints blocks");

    // A flat session (no range at all) must still produce a usable grid rather
    // than dividing by zero.
    TPOManager flat;
    const TPOSession* f = build(flat, "btcusdt", periods({{100.0, 100.0}}), 0.0);
    expect_true(f != nullptr, "a zero-range session still builds");
    if (f) {
        expect_true(f->tick_per_row > 0.0, "a zero-range session gets a positive row height");
        expect_true(!f->rows.empty(), "and at least one row");
    }

    // A row height fine enough to need more than 10000 rows is refused outright:
    // the grid would cost more to build and draw than the study is worth.
    TPOManager huge;
    std::vector<double> ts{static_cast<double>(kSession)};
    std::vector<double> highs{100000.0};
    std::vector<double> lows{0.0};
    huge.build_sessions("btcusdt", ts.data(), highs.data(), lows.data(), 1, 1800, 0.001);
    const std::vector<TPOSession>* sessions = huge.get_sessions("btcusdt");
    expect_true(sessions != nullptr && sessions->size() == 1, "the session still exists");
    if (sessions && !sessions->empty()) {
        expect_true(sessions->front().rows.empty(), "but an oversized grid is left unbuilt");
    }
}

void test_rebuild_is_debounced_and_state_is_per_symbol() {
    TPOManager mgr;
    // Two symbols must not share a profile.
    build(mgr, "btcusdt", fixture_a(), 1.0);
    build(mgr, "ethusdt", periods({{201.0, 200.0}, {202.0, 201.0}}), 1.0);

    const TPOSession* btc = &mgr.get_sessions("btcusdt")->front();
    const TPOSession* eth = &mgr.get_sessions("ethusdt")->front();
    expect_double(btc->session_low, 100.0, "btc keeps its own profile");
    expect_double(eth->session_low, 200.0, "eth keeps its own profile");
    expect_true(mgr.has_data("btcusdt") && mgr.has_data("ethusdt"), "both symbols report data");
    expect_true(!mgr.has_data("solusdt"), "an unseen symbol reports no data");
    expect_true(mgr.get_sessions("solusdt") == nullptr, "and returns no sessions");

    // Rebuilding with the same candles and the same row height is skipped: the
    // chart calls this every frame it scrolls, and rebuilding a whole profile
    // per frame is what the debounce exists to avoid.
    build(mgr, "btcusdt", fixture_a(), 1.0);
    expect_int(static_cast<int>(mgr.get_sessions("btcusdt")->size()), 1,
               "an identical rebuild does not duplicate the session");

    // Changing the row height DOES rebuild, because the grid is different.
    const TPOSession* rebuilt = build(mgr, "btcusdt", fixture_a(), 0.5);
    expect_true(rebuilt != nullptr, "a new row height rebuilds");
    if (rebuilt) {
        expect_double(rebuilt->tick_per_row, 0.5, "and adopts the new row height");
        expect_true(rebuilt->rows.size() > 5, "which gives a finer grid");
    }

    // Expand is per session and toggles rather than latching.
    expect_true(!mgr.get_sessions("btcusdt")->front().expanded, "sessions start collapsed");
    mgr.toggle_expand("btcusdt", 0);
    expect_true(mgr.get_sessions("btcusdt")->front().expanded, "toggling expands");
    mgr.toggle_expand("btcusdt", 0);
    expect_true(!mgr.get_sessions("btcusdt")->front().expanded, "toggling again collapses");
    mgr.toggle_expand("btcusdt", 99);  // out of range: must not corrupt anything
    expect_true(!mgr.get_sessions("btcusdt")->front().expanded,
                "an out-of-range toggle is ignored");
    mgr.toggle_expand("solusdt", 0);  // unknown symbol: must not create state
    expect_true(!mgr.has_data("solusdt"), "toggling an unknown symbol creates nothing");

    // Clearing drops the symbol's profile and leaves the other alone.
    mgr.clear("btcusdt");
    expect_true(!mgr.has_data("btcusdt"), "a cleared symbol reports no data");
    expect_true(mgr.has_data("ethusdt"), "and its neighbour is untouched");
}

void test_changing_a_setting_rebuilds_the_profile() {
    // Regression. The rebuild debounce used to key on the candle window and the
    // row height ONLY, so changing a setting on a chart that was not moving
    // recomputed nothing and the profile silently stayed at the old setting.
    // These are the settings that change the COMPUTATION, so each has to be part
    // of what the debounce compares.
    TPOManager mgr;
    const std::vector<Candle> candles = fixture_a();

    const TPOSession* s = build(mgr, "btcusdt", candles, 1.0);
    if (!s) {
        std::fprintf(stderr, "FAIL settings fixture produced no session\n");
        ++failures;
        return;
    }
    // 12 blocks, 70% target = 9, reached by rows 1 to 3.
    expect_true(!s->rows[0].is_value_area, "the default value area excludes row 0");
    expect_true(!s->rows[4].is_value_area, "the default value area excludes row 4");
    expect_double(s->val, 101.0, "the default VAL");
    expect_double(s->vah, 104.0, "the default VAH");

    // Widening the value area to 100% must take effect on the SAME candles.
    mgr.value_area_pct = 1.0f;
    s = build(mgr, "btcusdt", candles, 1.0);
    if (!s) return;
    for (const TPORow& row : s->rows) {
        expect_true(row.is_value_area, "a 100% value area covers every row");
    }
    expect_double(s->val, 100.0, "VAL follows the widened value area");
    expect_double(s->vah, 105.0, "VAH follows the widened value area");

    // Narrowing it must take effect too, in the other direction.
    mgr.value_area_pct = 0.30f;
    s = build(mgr, "btcusdt", candles, 1.0);
    if (!s) return;
    int inside = 0;
    for (const TPORow& row : s->rows) {
        if (row.is_value_area) ++inside;
    }
    expect_int(inside, 1, "a 30% value area is satisfied by the POC row alone");
    expect_true(s->rows[2].is_value_area, "and that row is the POC");

    // The session period is the other computed setting. Switching to weekly
    // must regroup the candles rather than reuse the daily grouping.
    //
    // NOTE: a non-daily period aligns to the EPOCH, not to a weekday the market
    // cares about, and the epoch was a Thursday. So a weekly TPO session runs
    // Thursday 00:00 UTC to Thursday 00:00 UTC. The fixture anchors to a real
    // epoch-week boundary rather than assuming any three days share a week.
    constexpr int64_t kWeek = 7 * kDay;
    constexpr int64_t kWeekStart = (kSession / kWeek) * kWeek;
    TPOManager weekly;
    std::vector<double> ts, highs, lows;
    for (int day = 0; day < 3; ++day) {
        ts.push_back(static_cast<double>(kWeekStart + day * kDay));
        highs.push_back(101.0 + day);
        lows.push_back(100.0 + day);
    }
    weekly.build_sessions("btcusdt", ts.data(), highs.data(), lows.data(), ts.size(), 1800, 1.0);
    expect_int(static_cast<int>(weekly.get_sessions("btcusdt")->size()), 3,
               "three days are three daily sessions");

    weekly.session_period_hours = 168;
    weekly.build_sessions("btcusdt", ts.data(), highs.data(), lows.data(), ts.size(), 1800, 1.0);
    const std::vector<TPOSession>* regrouped = weekly.get_sessions("btcusdt");
    expect_true(regrouped != nullptr, "the weekly rebuild produced sessions");
    if (regrouped) {
        expect_int(static_cast<int>(regrouped->size()), 1,
                   "the same three days regroup into one weekly session");
        expect_true(regrouped->front().session_start_ms == kWeekStart,
                    "starting on the epoch-week boundary");
        expect_true(regrouped->front().session_end_ms - regrouped->front().session_start_ms ==
                        kWeek,
                    "and that session spans a week");
        expect_double(regrouped->front().session_low, 100.0, "spanning every day's low");
        expect_double(regrouped->front().session_high, 103.0, "and every day's high");
    }

    // Display-only settings must NOT rebuild: a rebuild discards the per-session
    // expanded flag, so toggling a ray would collapse the user's open profile.
    TPOManager display;
    build(display, "btcusdt", candles, 1.0);
    display.toggle_expand("btcusdt", 0);
    expect_true(display.get_sessions("btcusdt")->front().expanded, "the session is expanded");
    display.show_poc_ray = !display.show_poc_ray;
    display.show_single_prints = !display.show_single_prints;
    display.show_initial_balance = !display.show_initial_balance;
    display.profile_spacing = 7;
    build(display, "btcusdt", candles, 1.0);
    expect_true(display.get_sessions("btcusdt")->front().expanded,
                "toggling a display setting does not rebuild and does not collapse it");
}

void test_degenerate_inputs_are_refused() {
    TPOManager mgr;
    const double ts[] = {static_cast<double>(kSession)};
    const double px[] = {100.0};

    mgr.build_sessions("btcusdt", nullptr, px, px, 1, 1800, 1.0);
    expect_true(!mgr.has_data("btcusdt"), "a null timestamp array builds nothing");
    mgr.build_sessions("btcusdt", ts, nullptr, px, 1, 1800, 1.0);
    expect_true(!mgr.has_data("btcusdt"), "a null high array builds nothing");
    mgr.build_sessions("btcusdt", ts, px, nullptr, 1, 1800, 1.0);
    expect_true(!mgr.has_data("btcusdt"), "a null low array builds nothing");
    mgr.build_sessions("btcusdt", ts, px, px, 0, 1800, 1.0);
    expect_true(!mgr.has_data("btcusdt"), "an empty candle array builds nothing");

    // And after all that, a real call still works: the refusals left no state
    // behind that would poison the next build.
    const TPOSession* s = build(mgr, "btcusdt", fixture_a(), 1.0);
    expect_true(s != nullptr, "a valid build still succeeds afterwards");
    if (s) expect_int(s->total_blocks, 12, "and produces the expected profile");
}

}  // namespace

int main() {
    test_the_histogram_is_built_row_by_row();
    test_poc_is_the_widest_row();
    test_poc_ties_break_toward_the_session_midpoint();
    test_value_area_captures_seventy_percent_around_the_poc();
    test_value_area_expands_one_sided_when_a_side_runs_out();
    test_extremes_printed_once_are_not_poor();
    test_initial_balance_is_the_first_hour();
    test_candles_are_split_into_utc_day_sessions();
    test_auto_row_height_and_the_grid_sanity_bail();
    test_rebuild_is_debounced_and_state_is_per_symbol();
    test_changing_a_setting_rebuilds_the_profile();
    test_degenerate_inputs_are_refused();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tpo_manager_test: all assertions passed\n");
    return 0;
}
