#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace RealtimeBubble {
inline float radius(double notional, double minimum) {
    if (!(minimum > 0) || !std::isfinite(notional) || notional < minimum) return 0;
    return float(std::min(12.0, 3.0 * std::sqrt(notional / minimum)));
}

// Flat signed markers: stable area scaling, quiet edge, no lighting or rings.
inline void draw(ImDrawList& dl, ImVec2 center, float r, ImVec4 signed_color, ImU32 border) {
    signed_color.w = 1.0f;
    const int segments = r < 6 ? 12 : r < 10 ? 16 : 24;
    dl.AddCircleFilled(center, r, ImGui::GetColorU32(signed_color), segments);
    dl.AddCircle(center, r, border, segments, 0.75f);
}
}
