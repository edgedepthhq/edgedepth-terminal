#ifdef NDEBUG
#undef NDEBUG
#endif
#include "imgui.h"
#include "../../src/core/candle_bubble_display.h"
#include "../../src/rendering/realtime_bubble.h"
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
    assert(CandleBubbleDisplay::radius(100000,100)==24);
    ImDrawListSharedData shared;
    ImDrawList draw(&shared);
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(0,0), ImVec2(100,100));
    RealtimeBubble::draw(draw, ImVec2(-20,50), 3, ImVec4(1,0,0,1), IM_COL32(0,0,0,255));
    assert(draw.VtxBuffer.empty() && draw.IdxBuffer.empty());
    // A partially visible marker must keep its complete geometry for GPU clipping.
    RealtimeBubble::draw(draw, ImVec2(-1,50), 3, ImVec4(1,0,0,1), IM_COL32(0,0,0,255));
    assert(draw.VtxBuffer.Size == 37 && draw.IdxBuffer.Size == 180);
    assert(draw.VtxBuffer[0].col == IM_COL32(255,0,0,255));
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
        RealtimeBubble::draw(draw, center, 16, ImVec4(0.18f, 0.84f, 0.68f, 1), IM_COL32(10, 12, 16, 204));
    }
    assert(draw.VtxBuffer.Size > 65535);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto& command : draw.CmdBuffer) assert(command.VtxOffset == 0);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    draw._ResetForNewFrame();
    draw.PushClipRect(ImVec2(-100,-100), ImVec2(2000,1000));
    // Candle overlays reuse the opaque mesh at their larger radius limit.
    for (int i = 0; i < 1500; ++i)
        RealtimeBubble::draw(draw, ImVec2(float(i % 100) * 10, float(i / 100) * 10),
            CandleBubbleDisplay::radius(100000, 100), ImVec4(0.2f,0.6f,1,0.42f),
            IM_COL32(9,9,9,255));
    assert((draw.VtxBuffer[0].col & IM_COL32_A_MASK) == IM_COL32_A_MASK);
    assert(draw.VtxBuffer[25].col == IM_COL32(9,9,9,255));
    assert((draw.VtxBuffer[49].col & IM_COL32_A_MASK) == 0);
    assert(std::abs(draw.VtxBuffer[25].pos.x - 24) < 0.001f);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    std::puts("PASS: dense RT bubbles retain 32-bit vertex indices without base-vertex support");
}
