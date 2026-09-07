#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
    static_assert(sizeof(ImDrawIdx) == 4);
    ImDrawListSharedData shared;
    ImDrawList draw(&shared);
    draw._ResetForNewFrame();
    draw.PushClipRectFullScreen();
    // The actual RT draw pattern at its 1,500-record cap, without VtxOffset.
    for (int i = 0; i < 1500; ++i) {
        const ImVec2 center(float(i % 100) * 10, float(i / 100) * 10);
        draw.AddCircleFilled(center, 28, IM_COL32(47, 214, 173, 184), 24);
        draw.AddCircle(center, 28, IM_COL32(47, 214, 173, 255), 24, 1);
    }
    assert(draw.VtxBuffer.Size > 65535);
    assert(*std::max_element(draw.IdxBuffer.begin(), draw.IdxBuffer.end()) > 65535);
    for (const auto& command : draw.CmdBuffer) assert(command.VtxOffset == 0);
    for (const auto index : draw.IdxBuffer) assert(index < unsigned(draw.VtxBuffer.Size));
    std::puts("PASS: dense RT bubbles retain 32-bit vertex indices without base-vertex support");
}
