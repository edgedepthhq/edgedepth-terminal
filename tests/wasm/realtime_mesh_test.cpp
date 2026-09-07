#include "imgui.h"
#include "../../src/rendering/realtime_bubble.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    static_assert(sizeof(ImDrawIdx) == 4);
    ImGui::CreateContext();
    assert(RealtimeBubble::radius(99, 100) == 0);
    assert(RealtimeBubble::radius(100, 100) == 4);
    assert(RealtimeBubble::radius(400, 100) == 8);
    assert(RealtimeBubble::radius(1600, 100) == 16);
    assert(RealtimeBubble::radius(160000, 100) == 16);
    ImDrawListSharedData shared;
    ImDrawList draw(&shared);
    draw._ResetForNewFrame();
    draw.PushClipRectFullScreen();
    // The actual RT draw pattern at its 1,500-record cap, without VtxOffset.
    for (int i = 0; i < 1500; ++i) {
        const ImVec2 center(float(i % 100) * 10, float(i / 100) * 10);
        RealtimeBubble::draw(draw, center, 16, ImVec4(0.18f, 0.84f, 0.68f, 1), IM_COL32(10, 12, 16, 204));
    }
    assert(draw.VtxBuffer.Size > 65535);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto& command : draw.CmdBuffer) assert(command.VtxOffset == 0);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    std::puts("PASS: dense RT bubbles retain 32-bit vertex indices without base-vertex support");
}
