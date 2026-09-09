#pragma once
#include "core/realtime_history.h"
#include <array>

// A dense view represents volume across the WHOLE visible time range. It never
// silently drops the oldest markers to make room for new ones. The archive
// keeps original records; zoom restores individual executions.
struct RealtimeTradeView {
    static constexpr size_t capacity = 1500;
    std::array<Terminal::Trade, capacity> records{};
    std::array<double, capacity> low{}, high{};
    size_t count = 0, source_count = 0;
    bool grouped = false;
    void build(const std::deque<Terminal::Trade>& trades, int64_t from, int64_t to,
               double minimum, bool source_grouped = false) {
        count = source_count = 0; grouped = source_grouped;
        for (const auto& t : trades) {
            if (t.timestamp_ms < from) continue;
            if (t.timestamp_ms > to) break;
            if (!source_grouped && t.price*t.qty < minimum) continue;
            if (count<capacity) { records[count]=t;low[count]=high[count]=t.price;++count; }
            ++source_count;
        }
        // A grouped archive plus its raw live tail must stay grouped even just
        // after refresh, when fewer than capacity records happen to be present.
        if (source_count<=capacity && !source_grouped) return;
        grouped = true;
        records.fill({});low.fill(0);high.fill(0);
        const int64_t step = std::max(int64_t(100), ((to-from+74899)/74900)*100);
        const int64_t origin = from/step;
        for (const auto& t : trades) {
            if (t.timestamp_ms < from) continue;
            if (t.timestamp_ms > to) break;
            const size_t i = size_t(t.timestamp_ms/step-origin)*2 + (t.is_buy?1:0);
            if (i>=capacity) continue;
            auto& b=records[i];
            b.price+=t.price*t.qty;b.qty+=t.qty;b.timestamp_ms=t.timestamp_ms;b.is_buy=t.is_buy;
            low[i]=low[i]>0?std::min(low[i],t.price):t.price;high[i]=std::max(high[i],t.price);
        }
        count=0;
        for(size_t i=0;i<capacity;++i)if(records[i].qty>0) {
            records[i].price/=records[i].qty;
            records[count]=records[i];low[count]=low[i];high[count]=high[i];++count;
        }
    }
};

// Bound a stalled archive view without discarding its observed volume. Only
// compact the prefix before the replaceable live tail, so grouped records can
// never overlap the next ring merge. Original executions remain in the archive.
inline bool bound_realtime_trade_tail(std::deque<Terminal::Trade>& displayed,
    const std::deque<Terminal::Trade>& recent, RealtimeTradeView& scratch) {
    if (displayed.size() <= RealtimeTradeHistory::max_trades * 2 || recent.empty()) return false;
    const int64_t seam = recent.front().timestamp_ms -
        (recent.size() < RealtimeTradeHistory::max_trades ? 1 : 0);
    scratch.build(displayed, displayed.front().timestamp_ms, seam, 0, true);
    while (!displayed.empty() && displayed.front().timestamp_ms <= seam) displayed.pop_front();
    std::sort(scratch.records.begin(), scratch.records.begin() + scratch.count,
        [](const auto& a, const auto& b) { return a.timestamp_ms < b.timestamp_ms; });
    displayed.insert(displayed.begin(), scratch.records.begin(), scratch.records.begin() + scratch.count);
    return true;
}
