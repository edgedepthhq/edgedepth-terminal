// ═══════════════════════════════════════════════════════════════════════════════
// indicator_series_test.cpp - native pin on the shared indicator series cache.
//
// Every stream-backed indicator (VPIN is the pathfinder) lands in this cache as
// a per-symbol series sorted by absolute ms. Three things arrive on that series
// from three different directions and have to converge on ONE answer:
//
//   the live stream        appends in ascending order
//   a history backfill     arrives in batches that overlap what is already there
//   a replay redispatch    re-delivers points the cache has already seen
//
// So the rule is insert-sorted with REPLACE on an equal timestamp. Idempotent
// re-delivery is the normal case, not the error case: scrubbing a replay back
// and forth re-sends the same volume-bucket prints, and a series that appended
// them instead of replacing would grow a duplicate for every scrub.
//
// The second thing pinned here is revision hygiene. Widgets rebuild their render
// caches when the revision moves, so a mutation that changes nothing must not
// move it - otherwise a replay rewind past the end of the data rebuilds every
// indicator panel on every frame.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/indicator_series.cpp"

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

void expect_i64(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what,
                     static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

Series::VPINPoint point(int64_t ts, float vpin) {
    Series::VPINPoint p;
    p.ts_ms = ts;
    p.vpin = vpin;
    return p;
}

// The series must be strictly ascending with no duplicate timestamps, always.
void expect_ascending(const IndicatorSeriesManager& mgr, const std::string& symbol,
                      const char* what) {
    const auto& v = mgr.vpin(symbol);
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i].ts_ms <= v[i - 1].ts_ms) {
            std::fprintf(stderr, "FAIL %s: ts %lld follows %lld at index %zu\n", what,
                         static_cast<long long>(v[i].ts_ms),
                         static_cast<long long>(v[i - 1].ts_ms), i);
            ++failures;
            return;
        }
    }
}

void test_regime_index_maps_the_wire_vocabulary() {
    // The backend's HMM labels and the threshold fallback share one vocabulary,
    // matched on the first letter in either case. Anything else is Normal, so an
    // unrecognised label degrades to the quietest strip rather than the loudest.
    expect_i64(Series::regime_index("Normal"), 0, "Normal");
    expect_i64(Series::regime_index("Elevated"), 1, "Elevated");
    expect_i64(Series::regime_index("High"), 2, "High");
    expect_i64(Series::regime_index("Critical"), 3, "Critical");

    expect_i64(Series::regime_index("elevated"), 1, "lowercase elevated");
    expect_i64(Series::regime_index("high"), 2, "lowercase high");
    expect_i64(Series::regime_index("critical"), 3, "lowercase critical");

    expect_i64(Series::regime_index(nullptr), 0, "a null label is Normal");
    expect_i64(Series::regime_index(""), 0, "an empty label is Normal");
    expect_i64(Series::regime_index("Xenomorph"), 0, "an unknown label is Normal");
    // An unknown label must never be mistaken for the loudest state: a garbled
    // frame should not paint the strip Critical.
    expect_true(Series::regime_index("unknown") != 3, "an unknown label is never Critical");
}

void test_ascending_appends_and_reads() {
    IndicatorSeriesManager mgr;

    // Unknown symbols read as empty, not as null. Render code dereferences this
    // every frame without a guard.
    expect_true(mgr.vpin("btcusdt").empty(), "an unseen symbol reads as an empty series");
    expect_i64(mgr.vpin_revision("btcusdt"), 0, "an unseen symbol has revision zero");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 0, "an unseen symbol has no oldest print");

    for (int64_t i = 1; i <= 5; ++i) mgr.add_vpin("btcusdt", point(i * 1000, 0.1f * i));
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 5, "five prints appended");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 1000, "the oldest print drives the history loop");
    expect_i64(mgr.vpin_revision("btcusdt"), 5, "each append moves the revision");
    expect_ascending(mgr, "btcusdt", "a live append stream stays ascending");

    // A print with no timestamp is dropped rather than sorted to the front,
    // where it would poison the oldest-print history loop.
    const uint64_t before = mgr.vpin_revision("btcusdt");
    mgr.add_vpin("btcusdt", point(0, 0.9f));
    mgr.add_vpin("btcusdt", point(-1, 0.9f));
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 5, "a timestampless print is dropped");
    expect_i64(mgr.vpin_revision("btcusdt"), static_cast<int64_t>(before),
               "and does not move the revision");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 1000, "nor the oldest print");
}

void test_out_of_order_and_repeated_delivery() {
    IndicatorSeriesManager mgr;
    // Arrive deliberately scrambled: a history backfill interleaved with live.
    for (const int64_t ts : {5000, 1000, 3000, 2000, 4000}) {
        mgr.add_vpin("btcusdt", point(ts, static_cast<float>(ts) / 10000.0f));
    }
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 5, "every print is kept once");
    expect_ascending(mgr, "btcusdt", "out-of-order delivery still sorts");
    expect_i64(mgr.vpin("btcusdt").front().ts_ms, 1000, "the oldest print sorts to the front");
    expect_i64(mgr.vpin("btcusdt").back().ts_ms, 5000, "the newest sorts to the back");

    // THE idempotency rule. A replay scrub re-delivers prints the cache already
    // holds; they must REPLACE, not accumulate.
    const uint64_t before = mgr.vpin_revision("btcusdt");
    for (const int64_t ts : {5000, 1000, 3000, 2000, 4000}) {
        mgr.add_vpin("btcusdt", point(ts, 0.99f));
    }
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 5,
               "re-delivering the whole series does not duplicate it");
    expect_ascending(mgr, "btcusdt", "re-delivery preserves the ordering");
    for (const auto& p : mgr.vpin("btcusdt")) {
        expect_true(p.vpin == 0.99f, "the re-delivered value replaces the old one");
    }
    expect_true(mgr.vpin_revision("btcusdt") > before,
                "a replacement is a real change and moves the revision");

    // Scrubbing the same session ten more times must not grow the series.
    for (int pass = 0; pass < 10; ++pass) {
        for (const int64_t ts : {1000, 2000, 3000, 4000, 5000}) {
            mgr.add_vpin("btcusdt", point(ts, 0.5f));
        }
    }
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 5,
               "ten scrubs of the same window leave five prints");
}

void test_batches_merge_into_an_existing_series() {
    IndicatorSeriesManager mgr;
    // Live has run for a while.
    for (const int64_t ts : {3000, 4000, 5000}) mgr.add_vpin("btcusdt", point(ts, 0.3f));

    // A history batch arrives covering earlier prints AND overlapping the live
    // ones. The overlap replaces; the rest inserts before.
    const std::vector<Series::VPINPoint> batch{point(1000, 0.1f), point(2000, 0.2f),
                                               point(3000, 0.9f), point(4000, 0.9f)};
    mgr.add_vpin_batch("btcusdt", batch.data(), batch.size());

    const auto& v = mgr.vpin("btcusdt");
    expect_i64(static_cast<int64_t>(v.size()), 5, "the overlap merges rather than duplicating");
    expect_ascending(mgr, "btcusdt", "a merged batch stays ascending");
    expect_i64(v[0].ts_ms, 1000, "the batch's earlier prints land at the front");
    expect_true(v[2].vpin == 0.9f, "the overlapping print took the batch's value");
    expect_true(v[4].vpin == 0.3f, "the non-overlapping live print kept its own");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 1000, "the oldest print moves back with the batch");

    // Batches tolerate disorder and garbage the same way single inserts do.
    const std::vector<Series::VPINPoint> messy{point(9000, 0.1f), point(0, 0.1f),
                                               point(7000, 0.1f), point(-5, 0.1f),
                                               point(8000, 0.1f)};
    mgr.add_vpin_batch("btcusdt", messy.data(), messy.size());
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 8,
               "three valid prints of five are kept");
    expect_ascending(mgr, "btcusdt", "a disordered batch still sorts");

    // Degenerate batch arguments are refused without touching the series.
    const std::size_t before = mgr.vpin("btcusdt").size();
    mgr.add_vpin_batch("btcusdt", nullptr, 4);
    mgr.add_vpin_batch("btcusdt", batch.data(), 0);
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), static_cast<int64_t>(before),
               "a null or empty batch changes nothing");
}

void test_symbols_do_not_leak_into_each_other() {
    IndicatorSeriesManager mgr;
    for (const int64_t ts : {1000, 2000, 3000}) mgr.add_vpin("btcusdt", point(ts, 0.1f));
    for (const int64_t ts : {7000, 8000}) mgr.add_vpin("ethusdt", point(ts, 0.5f));

    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 3, "btc keeps its own series");
    expect_i64(static_cast<int64_t>(mgr.vpin("ethusdt").size()), 2, "eth keeps its own series");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 1000, "btc has its own oldest print");
    expect_i64(mgr.vpin_oldest_ts("ethusdt"), 7000, "eth has its own oldest print");
    expect_true(mgr.vpin("solusdt").empty(), "a third symbol is still empty");

    // Adding to one must not move the other's revision, or every symbol's panel
    // rebuilds whenever any symbol ticks.
    const uint64_t eth_before = mgr.vpin_revision("ethusdt");
    mgr.add_vpin("btcusdt", point(4000, 0.1f));
    expect_i64(mgr.vpin_revision("ethusdt"), static_cast<int64_t>(eth_before),
               "one symbol's tick does not invalidate another's cache");
}

void test_replay_rewind_trims_the_future() {
    IndicatorSeriesManager mgr;
    for (const int64_t ts : {1000, 2000, 3000, 4000, 5000}) {
        mgr.add_vpin("btcusdt", point(ts, 0.1f));
    }
    for (const int64_t ts : {1000, 5000}) mgr.add_vpin("ethusdt", point(ts, 0.1f));

    // Rewinding to 3000 makes 4000 and 5000 the replay's future. The print AT
    // the cutoff is the present and stays: the scrubber sits on a bucket that
    // has already completed.
    mgr.trim_after(3000);
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 3, "prints past the cutoff are dropped");
    expect_i64(mgr.vpin("btcusdt").back().ts_ms, 3000, "the print at the cutoff survives");
    expect_ascending(mgr, "btcusdt", "trimming preserves the ordering");

    // trim_after is session-wide: a rewind rewinds every symbol, not just the
    // one on the chart.
    expect_i64(static_cast<int64_t>(mgr.vpin("ethusdt").size()), 1, "every symbol is trimmed");
    expect_i64(mgr.vpin("ethusdt").back().ts_ms, 1000, "eth keeps only its past");

    // A trim that removes nothing must not move the revision, or a replay
    // sitting past the end of the data rebuilds every panel every frame.
    const uint64_t before = mgr.vpin_revision("btcusdt");
    mgr.trim_after(3000);
    expect_i64(mgr.vpin_revision("btcusdt"), static_cast<int64_t>(before),
               "a no-op trim does not invalidate the render cache");
    mgr.trim_after(999999);
    expect_i64(mgr.vpin_revision("btcusdt"), static_cast<int64_t>(before),
               "nor does a trim past the end of the series");
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 3, "and it changes nothing");

    // Trimming before everything empties the series without destroying it.
    mgr.trim_after(0);
    expect_true(mgr.vpin("btcusdt").empty(), "trimming to zero empties the series");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 0, "an emptied series has no oldest print");
    expect_true(mgr.vpin_revision("btcusdt") > before, "and that emptying does move the revision");
}

void test_seek_clears_every_series() {
    IndicatorSeriesManager mgr;
    for (const int64_t ts : {1000, 2000}) mgr.add_vpin("btcusdt", point(ts, 0.1f));
    for (const int64_t ts : {3000, 4000}) mgr.add_vpin("ethusdt", point(ts, 0.1f));

    mgr.clear_all();
    expect_true(mgr.vpin("btcusdt").empty(), "a seek clears btc");
    expect_true(mgr.vpin("ethusdt").empty(), "a seek clears eth");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 0, "and leaves no oldest print behind");

    // Clearing an already-clear cache changes nothing, revision included.
    const uint64_t btc = mgr.vpin_revision("btcusdt");
    const uint64_t eth = mgr.vpin_revision("ethusdt");
    mgr.clear_all();
    expect_i64(mgr.vpin_revision("btcusdt"), static_cast<int64_t>(btc),
               "a redundant clear does not invalidate btc");
    expect_i64(mgr.vpin_revision("ethusdt"), static_cast<int64_t>(eth),
               "a redundant clear does not invalidate eth");

    // And the cache is reusable: the backend re-seeds the window after a seek.
    mgr.add_vpin("btcusdt", point(9000, 0.4f));
    expect_i64(static_cast<int64_t>(mgr.vpin("btcusdt").size()), 1, "the re-seed lands");
    expect_i64(mgr.vpin_oldest_ts("btcusdt"), 9000, "on a fresh window");
    expect_true(mgr.vpin_revision("btcusdt") > btc, "and moves the revision again");
}

}  // namespace

int main() {
    test_regime_index_maps_the_wire_vocabulary();
    test_ascending_appends_and_reads();
    test_out_of_order_and_repeated_delivery();
    test_batches_merge_into_an_existing_series();
    test_symbols_do_not_leak_into_each_other();
    test_replay_rewind_trims_the_future();
    test_seek_clears_every_series();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("indicator_series_test: all assertions passed\n");
    return 0;
}
