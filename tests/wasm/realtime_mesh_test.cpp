#ifdef NDEBUG
#undef NDEBUG
#endif
#include "imgui.h"
#include "../../src/core/candle_bubble_display.h"
#include "../../src/rendering/realtime_bubble.h"
#include "../../src/rendering/mixed_bubbles.h"
#include <deque>
#include <map>
#include "imgui_internal.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    static_assert(sizeof(ImDrawIdx) == 4);
    ImGui::CreateContext();
    assert(RealtimeBubble::radius(99, 100) == 0);
    assert(RealtimeBubble::radius(100, 100) == 3);
    assert(RealtimeBubble::radius(400, 100) == 6);
    assert(RealtimeBubble::radius(1600, 100) == 12);
    assert(RealtimeBubble::radius(160000, 100) == 12);
    assert(CandleBubbleDisplay::radius(0,100)==0);
    assert(CandleBubbleDisplay::radius(100,100)<CandleBubbleDisplay::radius(1000,100));
    assert(CandleBubbleDisplay::radius(100000,100)==18);
    ImDrawListSharedData shared;
    ImDrawList draw(&shared);
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(0,0), ImVec2(100,100));
    RealtimeBubble::draw(draw, ImVec2(-20,50), 3, ImVec4(1,0,0,1), IM_COL32(0,0,0,255), RealtimeBubble::Fill::Opaque);
    assert(draw.VtxBuffer.empty() && draw.IdxBuffer.empty());
    // A partially visible marker must keep its complete geometry for GPU clipping.
    // Real-time markers are flat opaque fills with the dark separator edge.
    RealtimeBubble::draw(draw, ImVec2(-1,50), 3, ImVec4(1,0,0,1), IM_COL32(0,0,0,255), RealtimeBubble::Fill::Opaque);
    assert(draw.VtxBuffer.Size == 37 && draw.IdxBuffer.Size == 180);
    assert(draw.VtxBuffer[0].col == IM_COL32(255,0,0,255));
    assert(draw.VtxBuffer[13].col == IM_COL32(0,0,0,255));
    for (int i=25;i<37;++i) assert((draw.VtxBuffer[i].col & IM_COL32_A_MASK) == 0);
    for (const auto& vertex : draw.VtxBuffer) {
        assert(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y));
        assert(std::abs(vertex.pos.x + 1) <= 4.001f && std::abs(vertex.pos.y - 50) <= 4.001f);
    }
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(-100,-100), ImVec2(2000,1000));
    // The actual RT draw pattern at its 1,500-record cap, without VtxOffset.
    for (int i = 0; i < 1500; ++i) {
        const ImVec2 center(float(i % 100) * 10, float(i / 100) * 10);
        RealtimeBubble::draw(draw, center, 16, ImVec4(0.18f, 0.84f, 0.68f, 1), IM_COL32(10, 12, 16, 204),
            RealtimeBubble::Fill::Opaque);
    }
    assert(draw.VtxBuffer.Size > 65535);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto& command : draw.CmdBuffer) assert(command.VtxOffset == 0);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(-100,-100), ImVec2(2000,1000));
    // Candle overlays reuse the mesh at their larger radius limit as translucent
    // discs: see-through fill, signed-colour edge, no dark separator.
    for (int i = 0; i < 1500; ++i)
        RealtimeBubble::draw(draw, ImVec2(float(i % 100) * 10, float(i / 100) * 10),
            CandleBubbleDisplay::radius(100000, 100), ImVec4(0.2f,0.6f,1,0.42f),
            IM_COL32(9,9,9,255), RealtimeBubble::Fill::Translucent);
    assert((draw.VtxBuffer[0].col & IM_COL32_A_MASK) < IM_COL32_A_MASK);
    assert(draw.VtxBuffer[25].col == ImGui::GetColorU32(ImVec4(0.2f,0.6f,1,1)));
    assert((draw.VtxBuffer[49].col & IM_COL32_A_MASK) == 0);
    assert(std::abs(draw.VtxBuffer[25].pos.x - 18) < 0.001f);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    std::puts("PASS: dense RT bubbles retain 32-bit vertex indices without base-vertex support");

    // Mixed marker: the smaller side is an inner disc, drawn as separate
    // triangles (two extra rings at the seam), never blended over the major fill.
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(0,0), ImVec2(100,100));
    RealtimeBubble::draw(draw, ImVec2(50,50), 12, ImVec4(1,0,0,1), IM_COL32(0,0,0,255),
        RealtimeBubble::Fill::Opaque, 0.25f, ImVec4(0,1,0,1));
    assert(draw.VtxBuffer.Size == 1 + 24 * 5 && draw.IdxBuffer.Size == 24 * 27);
    assert(draw.VtxBuffer[0].col == IM_COL32(0,255,0,255));    // centre: minor side
    assert(draw.VtxBuffer[1].col == IM_COL32(0,255,0,255));    // inner disc rim
    assert(draw.VtxBuffer[25].col == IM_COL32(255,0,0,255));   // ring start: major side
    assert(draw.VtxBuffer[73].col == IM_COL32(0,0,0,255));     // separator edge
    // Inner area is the minor share of the fill disc: r_in = r_fill * sqrt(0.25).
    const float r_fill = 12 - 0.75f, r_in = r_fill * 0.5f;
    assert(std::abs(draw.VtxBuffer[1].pos.x - (50 + r_in - 0.5f)) < 0.01f);
    assert(std::abs(draw.VtxBuffer[25].pos.x - (50 + r_in + 0.5f)) < 0.01f);
    // A minor share too small for a pixel keeps the plain mesh.
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(0,0), ImVec2(100,100));
    RealtimeBubble::draw(draw, ImVec2(50,50), 3, ImVec4(1,0,0,1), IM_COL32(0,0,0,255),
        RealtimeBubble::Fill::Translucent, 0.05f, ImVec4(0,1,0,1));
    assert(draw.VtxBuffer.Size == 37 && draw.IdxBuffer.Size == 180);
    std::puts("PASS: mixed bubbles draw the smaller side as an inner disc");

    // Mixed view: records sharing a bin and a price merge with exact sums; the
    // filter sees the merged total; over capacity the sums are conserved.
    std::deque<Terminal::Trade> trades;
    auto trade = [&](int64_t ts, double price, double qty, bool buy) {
        Terminal::Trade t{}; t.timestamp_ms = ts; t.price = price; t.qty = qty; t.is_buy = buy; trades.push_back(t);
    };
    trade(1000, 100.0, 2, true); trade(1004, 100.0, 1, false); trade(1006, 100.0, 1, true);  // one column, one price
    trade(1008, 100.5, 5, false);                                                            // same column, next price
    trade(1200, 100.0, 4, true);                                                             // next column
    static MixedBubbles::View view;
    view.build(trades.begin(), trades.end(), 10, 0.5, 0.0);
    assert(view.count == 3 && view.record_count == 5);
    const auto* mixed = &view.markers[0];
    assert(mixed->buy == 300 && mixed->sell == 100 && mixed->count == 3 && mixed->buy_led());
    assert(std::abs(mixed->minor() - 0.25) < 1e-9 && mixed->timestamp_ms == 1003 && mixed->price == 100.0);
    assert(view.markers[1].sell == 502.5 && view.markers[1].buy == 0 && view.markers[1].minor() == 0);
    assert(view.markers[2].buy == 400 && view.markers[2].timestamp_ms == 1200);
    // The minimum applies to the merged total: three 100-value prints pass a 250 floor together.
    view.build(trades.begin(), trades.end(), 10, 0.5, 250.0);
    assert(view.count == 3);
    view.build(trades.begin(), trades.end(), 1, 0.5, 250.0);
    assert(view.count == 2);   // per-millisecond bins: only the 502.5 and 400 prints qualify
    // Pointer ranges and unsorted input retain the largest local markers.
    trades.clear();
    std::vector<const Terminal::Trade*> ptrs;
    for (int i = 0; i < 4000; ++i) trade(1000 + (i * 7919) % 4000, 100.0 + (i % 50) * 0.5, 1, i % 3 == 0);
    for (const auto& t : trades) ptrs.push_back(&t);
    view.build(ptrs.begin(), ptrs.end(), 1, 0.5, 0.0);
    assert(view.count == MixedBubbles::View::capacity && view.qualifying_count == 4000);
    assert(view.bin_ms == 1 && view.price_step == 0.5);
    double smallest = 1e9;
    for (size_t i = 0; i < view.count; ++i) smallest = std::min(smallest, view.markers[i].value());
    size_t larger = 0;
    for (const auto& t : trades) larger += t.price * t.qty > smallest;
    assert(larger <= view.count);

    // Volatile ONE-like path: revisiting a price must never merge separate
    // swings. Compare EVERY selected marker with its original column/price
    // group, including exact side totals, timestamp and source multiplicity.
    trades.clear();
    constexpr int64_t start = 1790028670000LL, bin = 34;
    constexpr double tick = 1e-7;
    struct Expected { double buy = 0, sell = 0; int64_t offsets = 0; uint32_t count = 0; };
    std::map<std::pair<int64_t, int64_t>, Expected> expected;
    for (int ms = 0; ms < 60000; ms += 2) {
        const int phase = (ms * 4) % 60000;
        const int64_t price_ticks = 50000 + (phase < 30000 ? phase : 60000 - phase) / 10;
        trade(start + ms, price_ticks * tick, 100000, (ms / 2) % 2);
        const auto& t = trades.back();
        auto& e = expected[{t.timestamp_ms / bin, price_ticks}];
        (t.is_buy ? e.buy : e.sell) += t.price * t.qty;
        e.offsets += t.timestamp_ms % bin; ++e.count;
    }
    view.build(trades.begin(), trades.end(), bin, tick, 100);
    assert(view.count == MixedBubbles::View::capacity && view.qualifying_count == expected.size());
    assert(view.bin_ms == bin && view.price_step == tick && trades.size() == 30000);
    for (size_t i = 0; i < view.count; ++i) {
        const auto& m = view.markers[i];
        const auto key = std::make_pair(m.timestamp_ms / bin, std::llround(m.price / tick));
        const auto it = expected.find(key);
        assert(it != expected.end());
        const auto& e = it->second;
        assert(m.timestamp_ms == key.first * bin + std::llround(double(e.offsets) / e.count));
        assert(m.count == e.count && std::abs(m.buy - e.buy) < 1e-6 && std::abs(m.sell - e.sell) < 1e-6);
        assert(std::abs(m.price - key.second * tick) < 1e-12);
    }
    // Zooming into an uncapped interval reveals all its original groups again.
    const auto end = trades.begin() + 250;
    view.build(trades.begin(), end, 1, tick, 100);
    assert(view.count == 250 && view.qualifying_count == 250 && view.record_count == 250);

    // Equal-value overflow has deterministic selection even with reversed input.
    trades.clear();
    for (int i = 0; i < 2000; ++i) trade(start + i, 1, 100, i % 2);
    view.build(trades.begin(), trades.end(), 1, 0.1, 0);
    std::vector<int64_t> selected;
    for (size_t i = 0; i < view.count; ++i) selected.push_back(view.markers[i].timestamp_ms);
    std::reverse(trades.begin(), trades.end());
    view.build(trades.begin(), trades.end(), 1, 0.1, 0);
    std::vector<int64_t> reversed;
    for (size_t i = 0; i < view.count; ++i) reversed.push_back(view.markers[i].timestamp_ms);
    std::sort(selected.begin(), selected.end()); std::sort(reversed.begin(), reversed.end());
    assert(selected == reversed && selected.size() == 1500);
    // Selection must also have stable painter order. Adding live prints or
    // retiring the left edge must not flip overlapping historical discs.
    for (int initial : {1490, 2000}) {
        trades.clear();
        auto append = [&](int i) {
            trade(start + i * 10, .005 + (i % 5) * 1e-7,
                  1000 + (i * 7919) % 1000, i % 2);
        };
        for (int i = 0; i < initial; ++i) append(i);
        view.build(trades.begin(), trades.end(), 10, 1e-7, 0);
        std::map<int64_t, size_t> previous_rank;
        for (size_t i = 0; i < view.count; ++i)
            previous_rank[view.markers[i].timestamp_ms] = i;
        for (int i = initial; i < initial + 40; ++i) append(i);
        for (int i = 0; i < 10; ++i) trades.pop_front();
        view.build(trades.begin(), trades.end(), 10, 1e-7, 0);
        size_t common = 0, previous = 0;
        for (size_t i = 0; i < view.count; ++i) {
            const auto it = previous_rank.find(view.markers[i].timestamp_ms);
            if (it == previous_rank.end()) continue;
            assert(common == 0 || it->second > previous);
            previous = it->second; ++common;
        }
        assert(common > 1400);
    }
    trades.clear();
    view.build(trades.begin(), trades.end(), 1, tick, 0);
    assert(view.count == 0 && view.qualifying_count == 0 && view.record_count == 0);
    std::puts("PASS: mixed bubbles preserve local time/price groups through volatile overflow and zoom");
}
