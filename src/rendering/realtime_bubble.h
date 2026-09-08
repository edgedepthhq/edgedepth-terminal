#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace RealtimeBubble {
inline float radius(double notional, double minimum) {
    if (!(minimum > 0) || !std::isfinite(notional) || notional < minimum) return 0;
    return float(std::min(16.0, 4.0 * std::sqrt(notional / minimum)));
}

// Opaque signed fill prevents pale additive clusters. The subtle diagonal shade
// changes color only: every vertex retains the true circular radius and center.
inline void draw(ImDrawList& dl, ImVec2 center, float r, ImVec4 signed_color, ImU32 border) {
    const int first = dl.VtxBuffer.Size;
    signed_color.w = 1.0f;
    dl.AddCircleFilled(center, r, ImGui::GetColorU32(signed_color), 24);
    ImVec4 shade = signed_color;
    shade.x *= 0.62f; shade.y *= 0.62f; shade.z *= 0.62f;
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(&dl, first, dl.VtxBuffer.Size,
        ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r),
        ImGui::GetColorU32(signed_color), ImGui::GetColorU32(shade));
    dl.AddCircle(center, r, border, 24, 1.75f);
}
}
