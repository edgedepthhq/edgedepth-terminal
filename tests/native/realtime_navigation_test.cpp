#include "ui/realtime_navigation.h"
#include <cstdio>
#include <cmath>
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    check(!realtime_price_pan_detaches(0, 4), "click jitter preserves price follow");
    check(!realtime_price_pan_detaches(100, 13), "horizontal pan with vertical jitter preserves price follow");
    check(!realtime_price_pan_detaches(20, 20), "diagonal gesture preserves price follow");
    check(realtime_price_pan_detaches(4, 20), "deliberate upward price pan detaches");
    check(realtime_price_pan_detaches(-4, -20), "deliberate downward price pan detaches");
    for (double tick : {0.00001, 0.01, 0.1}) for (int fidelity : {1, 2, 5, 10, 20}) {
        const double step = tick * fidelity;
        RealtimePriceWindow window;
        auto r = window.update(78140, 78165, step, 800, 16, 78148, true);
        const double span = r.high - r.low;
        for (double price : {78183.0, 77900.0, 78500.0, 78160.5}) {
            r = window.update(r.low, r.high, step, 800, 16, price, true);
            check(price > r.low && price < r.high, "BTC jump stays inside followed price range");
            check(std::abs(r.high-r.low-span) < 1e-8, "following preserves zoom and fidelity");
        }
        const auto centered = r;
        const double inside = (r.low + r.high) * 0.5 + span * 0.2;
        r = window.update(r.low, r.high, step, 800, 16, inside, true);
        check(r.low == centered.low && r.high == centered.high, "safe-zone movement does not jitter the ladder");
        const auto manual = window.update(r.low, r.high, step, 800, 16, 80000, false);
        check(manual.low == r.low && manual.high == r.high, "manual inspection is not recentered");
        const auto resumed = window.update(r.low, r.high, step, 800, 16, 80000, true);
        check(resumed.low < 80000 && resumed.high > 80000, "resume catches up immediately");
    }
    for (double tick : {0.00001, 0.01, 0.1}) for (int minimum : {1, 2, 5, 10, 20}) {
        RealtimeAutoFit fit;
        const double base = tick < 0.001 ? 0.18 : 78106;
        fit.update(base, base + tick * 20, tick, minimum, 800, 16, 1000);
        const auto first = fit.range;
        const int first_group = fit.grouping;
        fit.update(base, base + tick * 1000, tick, minimum, 800, 16, 2000);
        check(fit.range.low <= base && fit.range.high >= base + tick * 1000,
              "expanding range preserves earlier prices and newest market");
        check(fit.grouping > first_group && fit.grouping % minimum == 0,
              "coarser grouping aligns with retained minimum buckets");
        check(tick * fit.grouping * 800 / (fit.range.high-fit.range.low) >= 16 - 1e-6,
              "automatic grouping keeps every displayed row readable");
        const auto wide = fit.range;
        fit.update(base, base + tick * 20, tick, minimum, 800, 16, 3000);
        fit.update(base, base + tick * 20, tick, minimum, 800, 16, 7999);
        check(fit.range.low == wide.low && fit.range.high == wide.high,
              "transient contraction does not pulse range or grouping");
        fit.update(base, base + tick * 20, tick, minimum, 800, 16, 8000);
        check(fit.grouping == first_group && fit.range.high-fit.range.low <= first.high-first.low+tick,
              "sustained spare room restores finer detail");
        fit.update(base, base + tick * 1000, tick, minimum, 400, 16, 9000);
        check(fit.range.low <= base && fit.range.high >= base+tick*1000,
              "resize keeps full observed range");
        fit.update(base, base + tick * 20, tick, minimum, 800, 16, 500);
        check(fit.grouping == first_group, "rewind clears future grouping and shrink timer");
        const auto valid = fit.range;
        fit.update(NAN, base, tick, minimum, 800, 16, 600);
        check(fit.range.low == valid.low && fit.range.high == valid.high, "invalid observations leave range intact");
    }
    // A 20-minute overview survives returning from a five-minute working ring.
    std::deque<RealtimeDepthHistory::SamplePtr> cached, recent;
    for (int64_t t = 1000; t < 1201000; t += 1000) {
        auto s = std::make_shared<RealtimeDepthHistory::Sample>();
        s->timestamp_ms = t; cached.push_back(s);
    }
    for (int64_t t = 1201000; t < 1501000; t += 100) {
        auto s = std::make_shared<RealtimeDepthHistory::Sample>();
        s->timestamp_ms = t; s->serial = uint64_t(t / 100);
        recent.push_back(s);
    }
    append_realtime_depth_tail(cached, recent, 1201000, 1501000, 1000);
    check(cached.front()->timestamp_ms == 1000 && cached.size() == 1500,
          "resuming a cached overview keeps its prefix and bins the raw tail");
    check(!cached[1200]->segment_start, "overlapping source proves a continuous tail");
    const auto count = cached.size();
    append_realtime_depth_tail(cached, recent, 1201000, 1501000, 1000);
    check(cached.size() == count, "repeated resume does not duplicate depth bins");
    for (int64_t t = 1501000; t < 2101000; t += 100) {
        auto s = std::make_shared<RealtimeDepthHistory::Sample>();
        s->timestamp_ms = t;
        append_realtime_depth_sample(cached, s, 1000);
    }
    check(cached.front()->timestamp_ms == 1000 && cached.size() == 2100,
          "ten minutes of ongoing raw arrivals cannot evict the cached overview");
    auto reseed = std::make_shared<RealtimeDepthHistory::Sample>();
    reseed->timestamp_ms = 2100999; reseed->segment_start = true;
    append_realtime_depth_sample(cached, reseed, 1000);
    check(cached.size() == 2100 && cached.back()->segment_start,
          "a same-bin reseed preserves the boundary within the bounded tail");
    cached.resize(1200);
    append_realtime_depth_tail(cached, recent, 1100000, 1501000, 1000);
    check(cached[1200]->segment_start && !recent.front()->segment_start,
          "retired source interval becomes a gap without mutating shared samples");
    cached.resize(1200);
    append_realtime_depth_tail(cached, recent, 1201000, 1300000, 1000);
    check(cached.back()->timestamp_ms <= 1300000, "paused tail excludes future depth");
    return failures ? 1 : 0;
}
