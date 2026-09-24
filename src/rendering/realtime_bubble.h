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

// Fill treatment. Candle charts draw large prints as translucent discs with a
// signed-colour edge: the print's size stays readable, the bar underneath stays
// visible, and overlapping prints read as overlap rather than one blob. Real-time
// draws every trade over the sampled depth field, where a translucent disc sinks
// into the cells: those are flat opaque fills with a thin dark separator edge
// (`border`), which the translucent style does not use.
enum class Fill { Translucent, Opaque };

// A mixed marker (MixedBubbles::Marker with takers on both sides) draws the
// larger side as the disc and the smaller side as an inner disc whose AREA is
// its share of the value: `minor` is that share (0 = one-sided, 0.5 = even),
// `minor_color` its signed colour. The inner disc and the ring around it are
// separate triangles, so a translucent fill is one colour in every pixel and a
// 1px gradient at the seam antialiases it.
inline void draw(ImDrawList& dl, ImVec2 center, float r, ImVec4 signed_color, ImU32 border, Fill fill_style,
                 float minor = 0.0f, ImVec4 minor_color = ImVec4()) {
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
    const bool opaque = fill_style == Fill::Opaque;
    const ImU32 edge = opaque ? border : ImGui::GetColorU32(signed_color);
    // Translucent: small marks stay visible; large ones stay see-through.
    const float fill_alpha = opaque ? 1.0f : r < 5.0f ? 0.55f : 0.30f;
    ImVec4 fill_color = signed_color;
    fill_color.w = fill_alpha;
    const ImU32 fill = ImGui::GetColorU32(fill_color);
    const float r_fill = std::max(0.0f, r - (opaque ? 0.75f : 1.25f));
    // Inner disc for the smaller side; below a pixel it would only dirty the centre.
    const float r_minor = minor > 0.0f ? r_fill * std::sqrt(std::min(minor, 0.5f)) : 0.0f;
    const bool mixed = r_minor >= 1.0f;
    ImVec4 minor_fill_color = minor_color;
    minor_fill_color.w = fill_alpha;
    const ImU32 minor_fill = ImGui::GetColorU32(minor_fill_color);
    // Rings out from the centre. Mixed adds two at the seam: the inner disc's
    // rim in the minor colour and the ring's start in the major colour.
    ImU32 colors[5];
    float radii[5];
    int rings = 0;
    if (mixed) {
        colors[rings] = minor_fill; radii[rings++] = std::max(0.0f, r_minor - 0.5f);
        colors[rings] = fill;       radii[rings++] = r_minor + 0.5f;
    }
    colors[rings] = fill;                    radii[rings++] = r_fill;
    colors[rings] = edge;                    radii[rings++] = r;
    colors[rings] = edge & ~IM_COL32_A_MASK; radii[rings++] = r + 1.0f;
    dl.PrimReserve(segments * 3 * (2 * rings - 1), 1 + segments * rings);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl._VtxCurrentIdx);
    const ImVec2 uv = dl._Data->TexUvWhitePixel;
    dl.PrimWriteVtx(center, uv, mixed ? minor_fill : fill);
    for (int ring = 0; ring < rings; ++ring)
        for (int i = 0; i < segments; ++i) {
            const auto& direction = circles[shape][i];
            dl.PrimWriteVtx(ImVec2(center.x + direction.x * radii[ring],
                center.y + direction.y * radii[ring]), uv, colors[ring]);
        }
    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        dl.PrimWriteIdx(base); dl.PrimWriteIdx(base + 1 + i); dl.PrimWriteIdx(base + 1 + next);
        for (int ring = 0; ring + 1 < rings; ++ring) {
            const ImDrawIdx inner = base + 1 + ring * segments, outer = inner + segments;
            dl.PrimWriteIdx(inner + i); dl.PrimWriteIdx(outer + i); dl.PrimWriteIdx(outer + next);
            dl.PrimWriteIdx(inner + i); dl.PrimWriteIdx(outer + next); dl.PrimWriteIdx(inner + next);
        }
    }
}
}
