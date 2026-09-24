#include "core/realtime_bubbles.h"
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    using History = RealtimeBubbleHistory;
    auto snapshot = [](const History& history, int64_t from, int64_t to, int64_t as_of) {
        std::vector<History::Bubble> result;
        history.for_each(from, to, as_of, [&](const auto& bubble) { result.push_back(bubble); });
        return result;
    };
    auto equal = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].timestamp_ms != b[i].timestamp_ms || a[i].price != b[i].price ||
                a[i].buy != b[i].buy || a[i].sell != b[i].sell || a[i].count != b[i].count) return false;
        return true;
    };
    auto trade = [](double price, double qty, int64_t timestamp, bool buy,
                    int64_t id = 0, double low = 0, double high = 0, uint64_t count = 0) {
        Terminal::Trade t{};
        t.price = price; t.qty = qty; t.timestamp_ms = timestamp; t.is_buy = buy;
        t.agg_trade_id = id; t.summary_low = low; t.summary_high = high; t.summary_count = count;
        return t;
    };
    History history;
    history.append(trade(100, 2, 1001, true), 1050);
    history.append(trade(100, 1, 1099, false), 1090);
    history.append(trade(101, 1, 1099, true), 1090);
    history.advance(1189);
    expect(history.size() == 0, "wait for the complete window and delivery settling before first display");
    history.advance(1190);
    const auto original = snapshot(history, 0, 2000, 1190);
    expect(original.size() == 2 && original[0].timestamp_ms == 1050 && original[0].price == 100 &&
        original[0].buy == 200 && original[0].sell == 100 && original[0].count == 2 &&
        original[1].price == 101 && original[1].timestamp_ms == 1050,
        "fixed midpoint, exact separate prices and conserved side totals");
    expect(snapshot(history, 0, 2000, 1189).empty(), "paused as-of view cannot expose later publications");
    history.append(trade(100, 100000, 1099, false), 1191);
    history.append(trade(110, 1, 1100, true), 1191);
    history.advance(1291);
    expect(history.late_records == 1 && equal(original, snapshot(history, 0, 1099, 1291)),
        "late larger trades cannot move, resize, recolour or replace completed bubbles");
    expect(history.size() == 3, "adjacent window closes independently");
    expect(equal(original, snapshot(history, 0, 2000, 1190)), "paused chart stays identical while capture continues");
    history.advance(1100);
    history.advance(1291);
    expect(equal(original, snapshot(history, 0, 1099, 1291)), "clock corrections do not destroy frozen groups");

    History dense;
    for (int bin = 0; bin < 600; ++bin) {
        const int64_t start = 10000 + bin * 100;
        // A rapid reversal path with ten prices and both aggressors at each price.
        for (int price = 0; price < 10; ++price) {
            const double p = 100 + (bin % 20 < 10 ? bin % 10 : 19 - bin % 20) * 10 + price;
            dense.append(trade(p, 2, start + price, true), start + 20);
            dense.append(trade(p, 1, start + 99 - price, false), start + 20);
        }
        dense.advance(start + 120);
    }
    const auto before = snapshot(dense, 0, 100000, 100000);
    expect(before.size() == 4800 && dense.filtered_groups == 1200,
        "dense history keeps a fixed per-window budget beyond the former global 1500 cap");
    for (const auto& bubble : before) {
        const int bin = int((bubble.timestamp_ms - 10050) / 100);
        const double base = 100 + (bin % 20 < 10 ? bin % 10 : 19 - bin % 20) * 10;
        expect(bubble.price >= base + 2 && bubble.price <= base + 9 && bubble.buy == 2 * bubble.price &&
            bubble.sell == bubble.price && bubble.count == 2 && bubble.timestamp_ms % 100 == 50,
            "selection is local to each window and never averages across a reversal");
    }
    for (int i = 0; i < 2000; ++i) dense.append(trade(1000.0 + i, 100000, 80000 + i * 100, true), 280000);
    dense.advance(280100);
    expect(equal(before, snapshot(dense, 0, 70000, 280100)), "thousands of new larger bubbles cannot evict old ones");
    const auto zoomed = snapshot(dense, 25000, 29999, 280100);
    std::vector<History::Bubble> expected_zoom;
    for (const auto& bubble : before) if (bubble.timestamp_ms >= 25000 && bubble.timestamp_ms < 30000) expected_zoom.push_back(bubble);
    expect(equal(zoomed, expected_zoom) && equal(before, snapshot(dense, 0, 70000, 280100)),
        "zoom and pan are read-only queries of the identical publications");
    dense.advance(10000 + History::retention_ms + 100);
    expect(snapshot(dense, 0, 10099, 2000000).empty() && !snapshot(dense, 10100, 10199, 2000000).empty(),
        "only the explicit 30 minute time retention expires old publications");

    History summaries;
    summaries.append(trade(100, 20, 1001, true, 0, 98, 103, 10), 1000);
    summaries.append(trade(100, 20, 1001, true, 0, 100, 100, 10), 1000);
    summaries.append(trade(std::numeric_limits<double>::infinity(), 1, 1001, true), 1000);
    summaries.advance(1100);
    const auto exact = snapshot(summaries, 0, 2000, 2000);
    expect(summaries.approximate_records == 10 && exact.size() == 1 && exact[0].price == 100 &&
        exact[0].buy == 2000 && exact[0].count == 10,
        "multi-price summaries cannot create imaginary exact-price bubbles; exact summaries remain valid");
    History bounded;
    for (size_t i = 0; i <= History::max_pending_groups; ++i)
        bounded.append(trade(100.0 + i, 1, 1001, true), 1000);
    expect(bounded.overflow_records == 1, "pending capture has a hard bounded budget");
    bounded.advance(1100);
    expect(bounded.size() == 8 && bounded.filtered_groups == History::max_pending_groups - 8,
        "a pathological burst publishes only its stable local selection");
    bounded.append(trade(100, 1, 1101, true), 1100);
    bounded.advance(1200);
    expect(bounded.size() == 9 && bounded.overflow_records == 1, "publication releases pending capacity");
    bounded.reset();
    bounded.append(trade(100, 1, 1001, true), 1000);
    bounded.advance(1100);
    expect(bounded.size() == 1 && bounded.filtered_groups == 0 && bounded.overflow_records == 0,
        "explicit reset starts a clean traversal, including old timestamps");
    if (!failures) std::puts("PASS: immutable fixed-window bubbles, burst selection, pause, zoom, late data and retention");
    return failures ? 1 : 0;
}
