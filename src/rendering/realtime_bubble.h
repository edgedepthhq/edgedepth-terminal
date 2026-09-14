#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <array>

namespace RealtimeBubble {
inline float radius(double notional, double minimum) {
    if (!(minimum > 0) || !std::isfinite(notional) || notional < minimum) return 0;
    return float(std::min(12.0, 3.0 * std::sqrt(notional / minimum)));
}

// Flat signed markers: stable area scaling, quiet edge, no lighting or rings.
inline void draw(ImDrawList& dl, ImVec2 center, float r, ImVec4 signed_color, ImU32 border) {
    if (!(r > 0) || !std::isfinite(r) || !std::isfinite(center.x) || !std::isfinite(center.y)) return;
    const ImVec4 clip = dl._CmdHeader.ClipRect;
    const float extent = r + 1;
    if (center.x + extent < clip.x || center.x - extent > clip.z ||
        center.y + extent < clip.y || center.y - extent > clip.w) return;
    signed_color.w = 1.0f;
    const int shape = r < 6 ? 0 : r < 10 ? 1 : 2;
    const int segments = shape == 0 ? 12 : shape == 1 ? 16 : 24;
    static const auto circles = [] {
        std::array<std::array<ImVec2, 24>, 3> result{};
        for (int shape = 0; shape < 3; ++shape) {
            const int count = shape == 0 ? 12 : shape == 1 ? 16 : 24;
            for (int i = 0; i < count; ++i) {
                const float angle = float(i) * (2 * IM_PI / count);
                result[shape][i] = ImVec2(std::cos(angle), std::sin(angle));
            }
        }
        return result;
    }();
    // One mesh shares the fill, narrow border and transparent outer fringe.
    // Two independently antialiased circles duplicate vertices and tessellation
    // for every bubble, every frame on busy markets.
    const ImU32 fill = ImGui::GetColorU32(signed_color);
    const ImU32 colors[] = {fill, border, border & ~IM_COL32_A_MASK};
    const float radii[] = {std::max(0.0f, r - 0.75f), r, r + 1.0f};
    dl.PrimReserve(segments * 15, 1 + segments * 3);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl._VtxCurrentIdx);
    const ImVec2 uv = dl._Data->TexUvWhitePixel;
    dl.PrimWriteVtx(center, uv, fill);
    for (int ring = 0; ring < 3; ++ring)
        for (int i = 0; i < segments; ++i) {
            const auto& direction = circles[shape][i];
            dl.PrimWriteVtx(ImVec2(center.x + direction.x * radii[ring],
                center.y + direction.y * radii[ring]), uv, colors[ring]);
        }
    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        dl.PrimWriteIdx(base); dl.PrimWriteIdx(base + 1 + i); dl.PrimWriteIdx(base + 1 + next);
        for (int ring = 0; ring < 2; ++ring) {
            const ImDrawIdx inner = base + 1 + ring * segments, outer = inner + segments;
            dl.PrimWriteIdx(inner + i); dl.PrimWriteIdx(outer + i); dl.PrimWriteIdx(outer + next);
            dl.PrimWriteIdx(inner + i); dl.PrimWriteIdx(outer + next); dl.PrimWriteIdx(inner + next);
        }
    }
}
}
