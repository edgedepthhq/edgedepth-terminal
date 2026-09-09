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
    return failures ? 1 : 0;
}
