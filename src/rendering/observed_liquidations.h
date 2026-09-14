#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace observed_liquidations {
// Prices and quantity are reported fields, not an inferred match to tape prints.
template<class Event> double price(const Event& e) {
    return std::isfinite(e.avg_price) && e.avg_price > 0 ? e.avg_price : e.price;
}
template<class Event> double notional(const Event& e) {
    const double p = price(e), n = p * e.qty;
    return e.timestamp_ms > 0 && std::isfinite(p) && p > 0 &&
        std::isfinite(e.qty) && e.qty > 0 && std::isfinite(n) ? n : 0;
}
struct Bin { int64_t start_ms = 0; double longs = 0, shorts = 0; uint32_t count = 0; };
struct View {
    std::array<Bin, 256> bins{};
    size_t size = 0;
    int64_t step_ms = 1000;
    double longs = 0, shorts = 0, peak = 0;
    uint64_t count = 0;
};
template<class Events> View aggregate(const Events& events, int64_t from, int64_t to, int64_t cutoff) {
    View view;
    to = std::min(to, cutoff);
    if (from < 0 || to <= from) return view;
    while ((to - from) / view.step_ms > 254) view.step_ms *= 2;
    const int64_t origin = from / view.step_ms * view.step_ms;
    view.size = std::min<size_t>(256, size_t((to - origin) / view.step_ms + 1));
    for (size_t i = 0; i < view.size; ++i) view.bins[i].start_ms = origin + int64_t(i) * view.step_ms;
    const auto first = std::lower_bound(events.begin(), events.end(), from,
        [](const auto& e, int64_t t) { return e.timestamp_ms < t; });
    for (auto it = first; it != events.end() && it->timestamp_ms <= to; ++it) {
        const double n = notional(*it);
        if (n <= 0) continue;
        const size_t i = size_t((it->timestamp_ms - origin) / view.step_ms);
        if (i >= view.size) continue;
        auto& bin = view.bins[i];
        if (it->is_buy) { bin.shorts += n; view.shorts += n; }
        else { bin.longs += n; view.longs += n; }
        ++bin.count; ++view.count;
        view.peak = std::max({view.peak, bin.longs, bin.shorts});
    }
    return view;
}
} // namespace observed_liquidations
