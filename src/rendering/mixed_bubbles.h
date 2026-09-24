#pragma once
#include "types/types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Mixed trade bubbles, the Bookmap idiom: the received records that share a
// time bin and a price merge into ONE marker sized by their total quote value
// and carrying both aggressor sides, so a bubble that had takers on both sides
// draws as a disc of the larger side with an inner disc for the smaller one
// (RealtimeBubble::draw with `minor`). Real-time bins are one pixel column,
// candle charts bin by the bar. Over capacity, only the largest markers are
// displayed; source records remain intact. Never widen bins to meet the cap:
// averaging separate visits to a price invents a time/price path on reversals.
namespace MixedBubbles {

struct Marker {
    int64_t timestamp_ms = 0;   // mean execution time of the merged records
    double price = 0;           // value-weighted price of the merged records
    double low = 0, high = 0;   // price extremes (archive summaries carry theirs)
    double buy = 0, sell = 0;   // quote value per aggressor side
    uint32_t count = 0;         // received records (a summary counts its own)
    double value() const { return buy + sell; }
    bool buy_led() const { return buy >= sell; }
    // Share of value on the smaller side: 0 = one-sided, 0.5 = even.
    double minor() const { const double v = value(); return v > 0 ? std::min(buy, sell) / v : 0; }
};

inline const Terminal::Trade& as_trade(const Terminal::Trade& t) { return t; }
inline const Terminal::Trade& as_trade(const Terminal::Trade* t) { return *t; }

struct View {
    static constexpr size_t capacity = 1500;
    std::array<Marker, capacity> markers{};
    size_t count = 0;
    size_t record_count = 0;   // records that went into the markers
    size_t qualifying_count = 0; // markers over the minimum, before the display cap
    int64_t bin_ms = 0;        // requested time resolution, never widened
    double price_step = 0;     // requested price grouping, never widened

    // Records in [first, last) (Trade values or pointers, any order) merge by
    // (timestamp / bin_ms, round(price / price_step)). Markers below `minimum`
    // total value are not kept; the total is what the filter sees, so a column
    // of small prints at one price can show where each print alone would not.
    template <class Iterator>
    void build(Iterator first, Iterator last, int64_t bin, double step, double minimum) {
        count = 0; record_count = 0; qualifying_count = 0;
        bin_ms = std::max<int64_t>(1, bin);
        price_step = step > 0 ? step : 1e-9;
        source_.clear();
        for (auto it = first; it != last; ++it) {
            const Terminal::Trade& t = as_trade(*it);
            const double value = t.price * t.qty;
            if (!(value > 0) || !std::isfinite(value)) continue;
            Marker m;
            m.timestamp_ms = t.timestamp_ms; m.price = t.price;
            m.low = t.summary_count ? t.summary_low : t.price;
            m.high = t.summary_count ? t.summary_high : t.price;
            (t.is_buy ? m.buy : m.sell) = value;
            m.count = t.summary_count ? uint32_t(std::min<uint64_t>(t.summary_count, UINT32_MAX)) : 1;
            source_.push_back(m);
        }
        record_count = source_.size();
        if (source_.empty()) return;
        std::sort(source_.begin(), source_.end(),
            [](const Marker& a, const Marker& b) { return a.timestamp_ms < b.timestamp_ms; });
        merge();
        merged_.erase(std::remove_if(merged_.begin(), merged_.end(),
            [&](const Marker& m) { return m.value() < minimum; }), merged_.end());
        qualifying_count = merged_.size();
        if (qualifying_count > capacity) {
            // Stable tie-breaks prevent equal-value bubbles changing selection
            // just because input order changed. This selects, never reaggregates.
            std::nth_element(merged_.begin(), merged_.begin() + capacity, merged_.end(),
                [](const Marker& a, const Marker& b) {
                    if (a.value() != b.value()) return a.value() > b.value();
                    if (a.timestamp_ms != b.timestamp_ms) return a.timestamp_ms < b.timestamp_ms;
                    return a.price < b.price;
                });
            merged_.resize(capacity);
        }
        for (const Marker& m : merged_) markers[count++] = m;
        // nth_element selects a set but leaves its order unstable. Keep painter
        // order consistent as live records enter/leave the viewport, including
        // transitions across the cap, so overlapping discs cannot flash.
        std::sort(markers.begin(), markers.begin() + count,
            [](const Marker& a, const Marker& b) {
                if (a.timestamp_ms != b.timestamp_ms) return a.timestamp_ms < b.timestamp_ms;
                return a.price < b.price;
            });
    }

private:
    std::vector<Marker> source_, merged_;
    std::vector<size_t> run_;

    // One pass over the time-sorted records: each bin is a contiguous run, and
    // inside a run the records sort by price key so equal keys are adjacent.
    void merge() {
        merged_.clear();
        const auto key = [&](const Marker& m) { return std::llround(m.price / price_step); };
        size_t i = 0;
        while (i < source_.size()) {
            const int64_t bin = source_[i].timestamp_ms / bin_ms;
            run_.clear();
            for (; i < source_.size() && source_[i].timestamp_ms / bin_ms == bin; ++i) run_.push_back(i);
            std::sort(run_.begin(), run_.end(), [&](size_t a, size_t b) {
                const int64_t ka = key(source_[a]), kb = key(source_[b]);
                return ka != kb ? ka < kb : source_[a].timestamp_ms < source_[b].timestamp_ms;
            });
            for (size_t j = 0; j < run_.size();) {
                const int64_t k = key(source_[run_[j]]);
                Marker out{};
                // Times are summed relative to the group's first record: epoch
                // milliseconds summed over thousands of records leave double's
                // exact range.
                const int64_t base = source_[run_[j]].timestamp_ms;
                double time_sum = 0, price_sum = 0;
                uint64_t n = 0;
                for (; j < run_.size() && key(source_[run_[j]]) == k; ++j) {
                    const Marker& m = source_[run_[j]];
                    const double v = m.value();
                    out.buy += m.buy; out.sell += m.sell; out.count += m.count;
                    out.low = n ? std::min(out.low, m.low) : m.low;
                    out.high = n ? std::max(out.high, m.high) : m.high;
                    time_sum += double(m.timestamp_ms - base); price_sum += m.price * v; ++n;
                }
                out.timestamp_ms = base + std::llround(time_sum / double(n));
                out.price = price_sum / out.value();
                merged_.push_back(out);
            }
        }
    }
};

}  // namespace MixedBubbles
