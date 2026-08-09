#include "ui/drawing/drawing_layer.h"

#include "core/app_context.h"
#include "core/candle_manager.h"
#include "core/drawing_manager.h"
#include "core/symbol_metadata.h"
#include "rendering/theme.h"

#include "imgui.h"
#include "imgui_internal.h"  // SetActiveID/ClearActiveID/KeepAliveID (precedent: layout.cpp)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace drawing {

namespace {

constexpr float kHitTolPx     = 6.0f;   // segment grab distance
constexpr float kHandleTolPx  = 8.0f;   // handle grab distance
constexpr float kHandleRadius = 4.5f;
constexpr int   kInvalidPart  = -999;   // never matches a handle index
constexpr float kBrushMinStepPx = 2.0f; // stroke point thinning

// The impossible 4-modifier chord: makes Pan/Select unreachable while keeping
// every button field in [0,5) (they index size-5 IO arrays — never use an
// out-of-range button for this).
constexpr int kImpossibleMod =
    ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super;

struct Wire { ImVec2 a, b; };

inline ImVec2 to_px(const Anchor& a) {
    return ImPlot::PlotToPixels(ImPlotPoint(static_cast<double>(a.t_ms), a.price));
}

inline int64_t snap_time(double t_ms, int64_t tf_ms) {
    if (tf_ms <= 0) return static_cast<int64_t>(t_ms);
    const double snapped = std::round(t_ms / static_cast<double>(tf_ms)) *
                           static_cast<double>(tf_ms);
    return static_cast<int64_t>(snapped);
}

float dist_seg(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float px = a.x + t * dx - p.x, py = a.y + t * dy - p.y;
    return std::sqrt(px * px + py * py);
}

// Liang-Barsky clip of segment a-b to rect. False = fully outside.
bool clip_line(ImVec2& a, ImVec2& b, ImVec2 rmin, ImVec2 rmax) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x - rmin.x, rmax.x - a.x, a.y - rmin.y, rmax.y - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;
        } else {
            const float r = q[i] / p[i];
            if (p[i] < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
            else             { if (r < t0) return false; if (r < t1) t1 = r; }
        }
    }
    const ImVec2 na(a.x + t0 * dx, a.y + t0 * dy);
    const ImVec2 nb(a.x + t1 * dx, a.y + t1 * dy);
    a = na; b = nb;
    return true;
}

// Far point along a->through, used to extend rays/lines before clipping.
ImVec2 extend_far(ImVec2 from, ImVec2 through) {
    ImVec2 d(through.x - from.x, through.y - from.y);
    const float mag = std::sqrt(d.x * d.x + d.y * d.y);
    if (mag < 1e-4f) return through;
    const float k = 100000.0f / mag;
    return ImVec2(from.x + d.x * k, from.y + d.y * k);
}

void line_styled(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float w,
                 LinePattern pat, ImVec2 rmin, ImVec2 rmax) {
    if (!clip_line(a, b, rmin, rmax)) return;
    if (pat == LinePattern::Solid) {
        dl->AddLine(a, b, col, w);
        return;
    }
    const float dash = pat == LinePattern::Dashed ? 8.0f : 2.0f;
    const float gap  = pat == LinePattern::Dashed ? 5.0f : 4.0f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    const float ux = dx / len, uy = dy / len;
    float at = 0.0f;
    int guard = 0;
    while (at < len && ++guard < 4096) {
        const float seg_end = std::min(at + dash, len);
        dl->AddLine(ImVec2(a.x + ux * at, a.y + uy * at),
                    ImVec2(a.x + ux * seg_end, a.y + uy * seg_end), col, w);
        at = seg_end + gap;
    }
}

void arrow_head_px(ImDrawList* dl, ImVec2 tip, ImVec2 from, ImU32 col, float len) {
    ImVec2 d(tip.x - from.x, tip.y - from.y);
    const float mag = std::sqrt(d.x * d.x + d.y * d.y);
    if (mag < 1e-3f) return;
    d.x /= mag; d.y /= mag;
    const ImVec2 n(-d.y, d.x);
    dl->AddTriangleFilled(
        tip,
        ImVec2(tip.x - d.x * len + n.x * len * 0.5f, tip.y - d.y * len + n.y * len * 0.5f),
        ImVec2(tip.x - d.x * len - n.x * len * 0.5f, tip.y - d.y * len - n.y * len * 0.5f),
        col);
}

void fmt_duration(char* buf, size_t n, int64_t ms) {
    int64_t s = ms / 1000;
    const int64_t days = s / 86400;
    s %= 86400;
    const int h = static_cast<int>(s / 3600);
    const int m = static_cast<int>((s % 3600) / 60);
    const int sec = static_cast<int>(s % 60);
    if (days > 0)
        snprintf(buf, n, "%lldd %02d:%02d", static_cast<long long>(days), h, m);
    else
        snprintf(buf, n, "%02d:%02d:%02d", h, m, sec);
}

// Price on the (a0 -> a1) line at time t (channel offset math).
double price_on_line(const Anchor& a0, const Anchor& a1, double t_ms) {
    if (a1.t_ms == a0.t_ms) return a0.price;
    const double f = (t_ms - static_cast<double>(a0.t_ms)) /
                     static_cast<double>(a1.t_ms - a0.t_ms);
    return a0.price + (a1.price - a0.price) * f;
}

// Magnet: snap price to the nearest O/H/L/C of the candle at t_ms when within
// 10px vertically. Caller gates on DrawingManager::magnet(); brush never snaps.
double magnet_price(const AppContext& ctx, int64_t t_ms, double price) {
    const auto& ts = ctx.candle_mgr().timestamps();
    if (ts.empty()) return price;
    const auto& os = ctx.candle_mgr().opens();
    const auto& hs = ctx.candle_mgr().highs();
    const auto& ls = ctx.candle_mgr().lows();
    const auto& cs = ctx.candle_mgr().closes();
    auto it = std::lower_bound(ts.begin(), ts.end(), static_cast<double>(t_ms));
    size_t i = it == ts.end() ? ts.size() - 1 : static_cast<size_t>(it - ts.begin());
    if (i > 0 && (it == ts.end() ||
                  std::fabs(ts[i - 1] - static_cast<double>(t_ms)) <
                      std::fabs(ts[i] - static_cast<double>(t_ms))))
        --i;
    if (i >= os.size() || i >= hs.size() || i >= ls.size() || i >= cs.size())
        return price;
    const float py =
        ImPlot::PlotToPixels(ImPlotPoint(static_cast<double>(t_ms), price)).y;
    const double cand[4] = {os[i], hs[i], ls[i], cs[i]};
    double best = price;
    float best_d = 10.0f;  // snap radius, px
    for (double c : cand) {
        const float cy =
            ImPlot::PlotToPixels(ImPlotPoint(static_cast<double>(t_ms), c)).y;
        const float dd = std::fabs(cy - py);
        if (dd < best_d) { best_d = dd; best = c; }
    }
    return best;
}

// ── Per-tool wire geometry + handles ────────────────────────────────────────
// One source of truth for hit-testing AND rendering. Handles are grab points
// (part index = position in `handles`); wires are grab/draw segments.
// Rect corner handles: 0 = a0, 1 = a1, 2 = (a0.t, a1.p), 3 = (a1.t, a0.p).
// Channel handles: 0 = a0, 1 = a1, 2 = parallel-line midpoint (drags chan_off).
void build_geometry(const Drawing& d, ImVec2 rmin, ImVec2 rmax,
                    std::vector<Wire>& wires, std::vector<ImVec2>& handles) {
    wires.clear();
    handles.clear();
    if (d.anchors.empty()) return;
    const ImVec2 p0 = to_px(d.anchors[0]);
    const ImVec2 p1 = d.anchors.size() > 1 ? to_px(d.anchors[1]) : p0;
    switch (d.tool) {
        case Tool::Trendline:
        case Tool::Arrow:
            wires.push_back({p0, p1});
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        case Tool::Ray:
            wires.push_back({p0, extend_far(p0, p1)});
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        case Tool::ExtendedLine:
            wires.push_back({extend_far(p1, p0), extend_far(p0, p1)});
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        case Tool::HLine:
            wires.push_back({ImVec2(rmin.x, p0.y), ImVec2(rmax.x, p0.y)});
            handles.push_back(p0);
            break;
        case Tool::HRay:
            wires.push_back({p0, ImVec2(rmax.x, p0.y)});
            handles.push_back(p0);
            break;
        case Tool::VLine:
            wires.push_back({ImVec2(p0.x, rmin.y), ImVec2(p0.x, rmax.y)});
            handles.push_back(p0);
            break;
        case Tool::CrossLine:
            wires.push_back({ImVec2(rmin.x, p0.y), ImVec2(rmax.x, p0.y)});
            wires.push_back({ImVec2(p0.x, rmin.y), ImVec2(p0.x, rmax.y)});
            handles.push_back(p0);
            break;
        case Tool::Rectangle: {
            const ImVec2 c2(p0.x, p1.y), c3(p1.x, p0.y);
            wires.push_back({p0, c3});
            wires.push_back({c3, p1});
            wires.push_back({p1, c2});
            wires.push_back({c2, p0});
            handles.push_back(p0);
            handles.push_back(p1);
            handles.push_back(c2);
            handles.push_back(c3);
            break;
        }
        case Tool::PriceRange:
            // Two horizontal boundaries + the center vertical arrow shaft.
            wires.push_back({p0, ImVec2(p1.x, p0.y)});
            wires.push_back({ImVec2(p0.x, p1.y), p1});
            wires.push_back({ImVec2((p0.x + p1.x) * 0.5f, p0.y),
                             ImVec2((p0.x + p1.x) * 0.5f, p1.y)});
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        case Tool::DateRange:
            wires.push_back({p0, ImVec2(p0.x, p1.y)});
            wires.push_back({ImVec2(p1.x, p0.y), p1});
            wires.push_back({ImVec2(p0.x, (p0.y + p1.y) * 0.5f),
                             ImVec2(p1.x, (p0.y + p1.y) * 0.5f)});
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        case Tool::Fib: {
            if (d.anchors.size() < 2) break;
            const float xa = std::min(p0.x, p1.x), xb = std::max(p0.x, p1.x);
            for (int i = 0; i < kFibLevelCount; ++i) {
                if (!(d.fib_mask & (1u << i))) continue;
                const double lp = d.anchors[0].price +
                    (d.anchors[1].price - d.anchors[0].price) * kFibLevels[i];
                const float y =
                    to_px(Anchor{d.anchors[0].t_ms, lp}).y;
                wires.push_back({ImVec2(xa, y), ImVec2(xb, y)});
            }
            wires.push_back({p0, p1});  // the dotted leg is grabbable too
            handles.push_back(p0);
            handles.push_back(p1);
            break;
        }
        case Tool::Channel: {
            if (d.anchors.size() < 2) break;
            const ImVec2 q0 = to_px(Anchor{d.anchors[0].t_ms,
                                           d.anchors[0].price + d.chan_off});
            const ImVec2 q1 = to_px(Anchor{d.anchors[1].t_ms,
                                           d.anchors[1].price + d.chan_off});
            wires.push_back({p0, p1});
            wires.push_back({q0, q1});
            handles.push_back(p0);
            handles.push_back(p1);
            handles.push_back(ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f));
            break;
        }
        case Tool::Brush:
        case Tool::Polyline: {
            for (size_t i = 1; i < d.anchors.size(); ++i)
                wires.push_back({to_px(d.anchors[i - 1]), to_px(d.anchors[i])});
            if (d.tool == Tool::Polyline)
                for (const Anchor& a : d.anchors) handles.push_back(to_px(a));
            break;
        }
        case Tool::Text: {
            ImFont* f = Theme::Fonts::ui();
            const char* txt = d.text.empty() ? "Text" : d.text.c_str();
            const ImVec2 ts = f->CalcTextSizeA(d.font_size, FLT_MAX, 0.0f, txt);
            const ImVec2 b0(p0.x - 3.0f, p0.y - 3.0f);
            const ImVec2 b1(p0.x + ts.x + 3.0f, p0.y + ts.y + 3.0f);
            wires.push_back({b0, ImVec2(b1.x, b0.y)});
            wires.push_back({ImVec2(b1.x, b0.y), b1});
            wires.push_back({b1, ImVec2(b0.x, b1.y)});
            wires.push_back({ImVec2(b0.x, b1.y), b0});
            handles.push_back(p0);
            break;
        }
        case Tool::LongPosition:
        case Tool::ShortPosition: {
            if (d.anchors.size() < 2) break;
            // anchors[0] = entry (t, price); anchors[1].t_ms = right edge.
            const float xr = p1.x;
            const float yt = to_px(Anchor{d.anchors[0].t_ms, d.target}).y;
            const float ys = to_px(Anchor{d.anchors[0].t_ms, d.stop}).y;
            const float cx = (p0.x + xr) * 0.5f;
            wires.push_back({p0, ImVec2(xr, p0.y)});                    // entry
            wires.push_back({ImVec2(p0.x, yt), ImVec2(xr, yt)});        // target
            wires.push_back({ImVec2(p0.x, ys), ImVec2(xr, ys)});        // stop
            handles.push_back(p0);                       // 0: entry (moves all)
            handles.push_back(ImVec2(xr, p0.y));         // 1: right edge (time)
            handles.push_back(ImVec2(cx, yt));           // 2: target price
            handles.push_back(ImVec2(cx, ys));           // 3: stop price
            break;
        }
        default:
            break;
    }
}

// Right-edge price pill for horizontal levels (alert-pill pattern, no ✕).
void price_pill(ImDrawList* dl, const PriceFormatter& fmt, float y, ImU32 col,
                ImVec2 rmax, double price) {
    char buf[32];
    fmt.format_price(buf, sizeof(buf), price);
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const float pad = 5.0f;
    const ImVec2 p0(rmax.x - ts.x - pad * 2.0f, y - ts.y * 0.5f - 2.0f);
    const ImVec2 p1(rmax.x, y + ts.y * 0.5f + 2.0f);
    dl->AddRectFilled(p0, p1, col, Theme::Radius::R1);
    dl->AddText(ImVec2(p0.x + pad, p0.y + 2.0f),
                Theme::u32(Theme::Tokens::BRAND_INK), buf);
    ImGui::PopFont();
}

// Two-line readout chip (measure / price range / date range).
void readout_chip(ImDrawList* dl, ImVec2 center_top, const char* l1,
                  const char* l2, ImU32 col) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 s1 = ImGui::CalcTextSize(l1);
    const ImVec2 s2 = l2 ? ImGui::CalcTextSize(l2) : ImVec2(0, 0);
    const float w = std::max(s1.x, s2.x) + 16.0f;
    const float h = s1.y + (l2 ? s2.y + 3.0f : 0.0f) + 10.0f;
    const ImVec2 p0(center_top.x - w * 0.5f, center_top.y);
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, with_alpha(col, 235), Theme::Radius::R1);
    const ImU32 txt = Theme::u32(Theme::Tokens::BRAND_INK);
    dl->AddText(ImVec2(center_top.x - s1.x * 0.5f, p0.y + 5.0f), txt, l1);
    if (l2)
        dl->AddText(ImVec2(center_top.x - s2.x * 0.5f, p0.y + 5.0f + s1.y + 3.0f),
                    txt, l2);
    ImGui::PopFont();
}

void draw_handles(ImDrawList* dl, const std::vector<ImVec2>& handles,
                  int hot_part) {
    // Neutral white ring - no accent cyan in the drawing UI (2026-08-06).
    const ImU32 ring = Theme::u32(Theme::Tokens::TX1);
    const ImU32 fill = Theme::u32(Theme::Tokens::PANEL);
    for (size_t i = 0; i < handles.size(); ++i) {
        const float r = static_cast<int>(i) == hot_part ? kHandleRadius + 1.5f
                                                        : kHandleRadius;
        dl->AddCircleFilled(handles[i], r, fill);
        dl->AddCircle(handles[i], r, ring, 0, 1.6f);
    }
}

// Renders one drawing (committed or placement preview).
void render_one(ImDrawList* dl, const Drawing& d, const PriceFormatter& fmt,
                int64_t tf_ms, ImVec2 rmin, ImVec2 rmax, bool selected,
                bool hovered, int hot_part, std::vector<Wire>& wires_scratch,
                std::vector<ImVec2>& handles_scratch) {
    build_geometry(d, rmin, rmax, wires_scratch, handles_scratch);
    if (wires_scratch.empty() && d.tool != Tool::Text) return;
    const ImU32 col = d.style.color;
    const float w = d.style.width + ((selected || hovered) ? 0.6f : 0.0f);

    // ── Area fills (under the wires) ────────────────────────────────────────
    if (d.anchors.size() > 1) {
        const ImVec2 p0 = to_px(d.anchors[0]);
        const ImVec2 p1 = to_px(d.anchors[1]);
        const ImVec2 f0(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
        const ImVec2 f1(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
        switch (d.tool) {
            case Tool::Rectangle:
                dl->AddRectFilled(f0, f1,
                                  d.style.fill ? d.style.fill : with_alpha(col, 30));
                break;
            case Tool::PriceRange:
            case Tool::DateRange: {
                const bool up = d.anchors[1].price >= d.anchors[0].price;
                const ImVec4& tone = d.tool == Tool::DateRange
                                         ? Theme::Tokens::BRAND
                                         : (up ? Theme::Tokens::UP : Theme::Tokens::DOWN);
                dl->AddRectFilled(f0, f1, Theme::u32(tone, 0.10f));
                break;
            }
            case Tool::Fib: {
                // Alternating translucent bands between consecutive on-levels.
                const float xa = std::min(p0.x, p1.x), xb = std::max(p0.x, p1.x);
                float prev_y = 0.0f;
                bool have_prev = false, shade = false;
                for (int i = 0; i < kFibLevelCount; ++i) {
                    if (!(d.fib_mask & (1u << i))) continue;
                    const double lp = d.anchors[0].price +
                        (d.anchors[1].price - d.anchors[0].price) * kFibLevels[i];
                    const float y = to_px(Anchor{d.anchors[0].t_ms, lp}).y;
                    if (have_prev && shade)
                        dl->AddRectFilled(ImVec2(xa, std::min(prev_y, y)),
                                          ImVec2(xb, std::max(prev_y, y)),
                                          with_alpha(col, 14));
                    shade = have_prev ? !shade : true;
                    prev_y = y;
                    have_prev = true;
                }
                break;
            }
            case Tool::Channel: {
                const ImVec2 q0 = to_px(Anchor{d.anchors[0].t_ms,
                                               d.anchors[0].price + d.chan_off});
                const ImVec2 q1 = to_px(Anchor{d.anchors[1].t_ms,
                                               d.anchors[1].price + d.chan_off});
                dl->AddQuadFilled(p0, p1, q1, q0, with_alpha(col, 18));
                break;
            }
            case Tool::LongPosition:
            case Tool::ShortPosition: {
                // Profit box (entry<->target) in UP, risk box (entry<->stop)
                // in DOWN — same tint both directions, TV convention.
                const float xl = p0.x, xr = p1.x;
                const float ye = p0.y;
                const float yt = to_px(Anchor{d.anchors[0].t_ms, d.target}).y;
                const float ys = to_px(Anchor{d.anchors[0].t_ms, d.stop}).y;
                dl->AddRectFilled(ImVec2(std::min(xl, xr), std::min(ye, yt)),
                                  ImVec2(std::max(xl, xr), std::max(ye, yt)),
                                  Theme::u32(Theme::Tokens::UP, 0.11f));
                dl->AddRectFilled(ImVec2(std::min(xl, xr), std::min(ye, ys)),
                                  ImVec2(std::max(xl, xr), std::max(ye, ys)),
                                  Theme::u32(Theme::Tokens::DOWN, 0.11f));
                break;
            }
            default:
                break;
        }
    }

    // ── Wires ───────────────────────────────────────────────────────────────
    // The fib leg (last wire) renders dotted; position tools tint the target/
    // stop lines UP/DOWN; everything else uses the style.
    const bool is_position =
        d.tool == Tool::LongPosition || d.tool == Tool::ShortPosition;
    const size_t leg_wire =
        d.tool == Tool::Fib && !wires_scratch.empty() ? wires_scratch.size() - 1
                                                      : static_cast<size_t>(-1);
    for (size_t i = 0; i < wires_scratch.size(); ++i) {
        const LinePattern pat =
            i == leg_wire ? LinePattern::Dotted : d.style.pattern;
        ImU32 wcol = i == leg_wire ? with_alpha(col, 110) : col;
        if (is_position) {
            if (i == 1) wcol = Theme::u32(Theme::Tokens::UP);
            else if (i == 2) wcol = Theme::u32(Theme::Tokens::DOWN);
        }
        line_styled(dl, wires_scratch[i].a, wires_scratch[i].b, wcol, w, pat,
                    rmin, rmax);
    }

    // ── Tool-specific decorations ───────────────────────────────────────────
    switch (d.tool) {
        case Tool::Arrow:
            if (d.anchors.size() > 1)
                arrow_head_px(dl, to_px(d.anchors[1]), to_px(d.anchors[0]), col,
                              std::max(9.0f, 4.5f * d.style.width));
            break;
        case Tool::HLine:
        case Tool::HRay: {
            const float y = to_px(d.anchors[0]).y;
            if (y >= rmin.y && y <= rmax.y)
                price_pill(dl, fmt, y, col, rmax, d.anchors[0].price);
            break;
        }
        case Tool::PriceRange:
        case Tool::DateRange: {
            if (d.anchors.size() < 2) break;
            const ImVec2 p0 = to_px(d.anchors[0]);
            const ImVec2 p1 = to_px(d.anchors[1]);
            if (d.tool == Tool::PriceRange) {
                const float cx = (p0.x + p1.x) * 0.5f;
                arrow_head_px(dl, ImVec2(cx, p1.y), ImVec2(cx, p0.y), col, 8.0f);
            } else {
                const float cy = (p0.y + p1.y) * 0.5f;
                arrow_head_px(dl, ImVec2(p1.x, cy), ImVec2(p0.x, cy), col, 8.0f);
            }
            const double dprice = d.anchors[1].price - d.anchors[0].price;
            const double pct = d.anchors[0].price != 0.0
                                   ? dprice / d.anchors[0].price * 100.0
                                   : 0.0;
            const int64_t dt = std::llabs(d.anchors[1].t_ms - d.anchors[0].t_ms);
            const long bars = tf_ms > 0 ? static_cast<long>(dt / tf_ms) : 0;
            char pbuf[32];
            fmt.format_price(pbuf, sizeof(pbuf), std::fabs(dprice));
            char l1[64], l2[64], dur[32];
            fmt_duration(dur, sizeof(dur), dt);
            const bool up = dprice >= 0.0;
            const ImU32 chip = d.tool == Tool::DateRange
                                   ? Theme::u32(Theme::Tokens::BRAND)
                                   : Theme::u32(up ? Theme::Tokens::UP
                                                   : Theme::Tokens::DOWN);
            if (d.tool == Tool::PriceRange) {
                snprintf(l1, sizeof(l1), "%s%s (%+.2f%%)", up ? "+" : "-", pbuf, pct);
                snprintf(l2, sizeof(l2), "%ld bars · %s", bars, dur);
            } else {
                snprintf(l1, sizeof(l1), "%ld bars · %s", bars, dur);
                snprintf(l2, sizeof(l2), "%s%s (%+.2f%%)", up ? "+" : "-", pbuf, pct);
            }
            const float by = std::max(p0.y, p1.y) + 8.0f;
            readout_chip(dl, ImVec2((p0.x + p1.x) * 0.5f, by), l1, l2, chip);
            break;
        }
        case Tool::Fib: {
            if (d.anchors.size() < 2) break;
            const ImVec2 p0 = to_px(d.anchors[0]);
            const ImVec2 p1 = to_px(d.anchors[1]);
            const float xa = std::min(p0.x, p1.x);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            for (int i = 0; i < kFibLevelCount; ++i) {
                if (!(d.fib_mask & (1u << i))) continue;
                const double lp = d.anchors[0].price +
                    (d.anchors[1].price - d.anchors[0].price) * kFibLevels[i];
                const float y = to_px(Anchor{d.anchors[0].t_ms, lp}).y;
                if (y < rmin.y || y > rmax.y) continue;
                char pbuf[32], lbuf[48];
                fmt.format_price(pbuf, sizeof(pbuf), lp);
                snprintf(lbuf, sizeof(lbuf), "%g  %s", kFibLevels[i], pbuf);
                const ImVec2 ts = ImGui::CalcTextSize(lbuf);
                dl->AddText(ImVec2(xa - ts.x - 6.0f, y - ts.y * 0.5f),
                            with_alpha(col, 210), lbuf);
            }
            ImGui::PopFont();
            break;
        }
        case Tool::Channel: {
            if (d.anchors.size() < 2) break;
            // Dashed midline at half the channel offset.
            const ImVec2 m0 = to_px(Anchor{d.anchors[0].t_ms,
                                           d.anchors[0].price + d.chan_off * 0.5});
            const ImVec2 m1 = to_px(Anchor{d.anchors[1].t_ms,
                                           d.anchors[1].price + d.chan_off * 0.5});
            line_styled(dl, m0, m1, with_alpha(col, 120), 1.0f,
                        LinePattern::Dashed, rmin, rmax);
            break;
        }
        case Tool::LongPosition:
        case Tool::ShortPosition: {
            if (d.anchors.size() < 2) break;
            const ImVec2 p0 = to_px(d.anchors[0]);
            const ImVec2 p1 = to_px(d.anchors[1]);
            const double entry = d.anchors[0].price;
            const double risk = std::fabs(entry - d.stop);
            const double reward = std::fabs(d.target - entry);
            const double rr = risk > 0.0 ? reward / risk : 0.0;
            const double tpct = entry != 0.0 ? (d.target - entry) / entry * 100.0 : 0.0;
            const double spct = entry != 0.0 ? (d.stop - entry) / entry * 100.0 : 0.0;
            char l1[64], l2[64];
            snprintf(l1, sizeof(l1), "RR %.2f", rr);
            snprintf(l2, sizeof(l2), "target %+.2f%% · stop %+.2f%%", tpct, spct);
            const float yt = to_px(Anchor{d.anchors[0].t_ms, d.target}).y;
            const float ys = to_px(Anchor{d.anchors[0].t_ms, d.stop}).y;
            const float top = std::min({p0.y, yt, ys});
            readout_chip(dl, ImVec2((p0.x + p1.x) * 0.5f, top - 42.0f), l1, l2,
                         Theme::u32(d.tool == Tool::LongPosition
                                        ? Theme::Tokens::UP
                                        : Theme::Tokens::DOWN));
            break;
        }
        case Tool::Text: {
            const ImVec2 p0 = to_px(d.anchors[0]);
            dl->AddText(Theme::Fonts::ui(), d.font_size, p0, col,
                        d.text.empty() ? "Text" : d.text.c_str());
            if (selected && !wires_scratch.empty()) {
                // Dashed bounds so the grab area reads.
                for (const Wire& wr : wires_scratch)
                    line_styled(dl, wr.a, wr.b, with_alpha(col, 130), 1.0f,
                                LinePattern::Dashed, rmin, rmax);
            }
            break;
        }
        default:
            break;
    }

    if (selected || hovered)
        draw_handles(dl, handles_scratch, selected ? hot_part : kInvalidPart);
}

// Tools with live placement/rendering.
constexpr bool tool_implemented(Tool t) {
    return t != Tool::Cursor && t < Tool::COUNT;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════

void DrawingLayer::render_in_plot(const AppContext& ctx, const PriceFormatter& fmt,
                                  bool overlays_allowed, int64_t tf_seconds) {
    DrawingManager& mgr = ctx.drawing_mgr();
    captures_ = false;

    if (!overlays_allowed) {
        // TPO/Renko: no time axis for overlays — park everything.
        cancel_placement();
        if (mgr.armed() != Tool::Cursor) mgr.arm(Tool::Cursor);
        hover_id_ = 0;
        text_editor_open_ = false;
        return;
    }

    const int64_t tf_ms = tf_seconds * 1000;

    handle_keys(ctx);

    if (mgr.armed() == Tool::Cursor) {
        update_cursor_mode(ctx, tf_ms);
    } else {
        hover_id_ = 0;
        hover_part_ = kPartNone;
        update_placement(ctx, tf_ms);
    }

    render_drawings(ctx, fmt, tf_ms);
    if (mgr.armed() != Tool::Cursor && mgr.armed() != Tool::Measure)
        render_placement_preview(ctx, fmt, tf_ms);
    render_measure(ctx, fmt, tf_ms);
    render_text_editor(ctx);

    const bool full = mgr.armed() != Tool::Cursor || !pending_.empty() ||
                      drag_id_ != 0;
    captures_ = full || hover_id_ != 0;
    if (full)
        apply_input_override(true);
    else if (hover_id_ != 0)
        apply_input_override(false);  // Fit only: double-click must not re-fit
}

void DrawingLayer::end_frame() {
    if (map_overridden_) {
        ImPlot::GetInputMap() = saved_map_;
        map_overridden_ = false;
    }
}

void DrawingLayer::apply_input_override(bool full) {
    ImPlotInputMap& map = ImPlot::GetInputMap();
    if (!map_overridden_) {
        saved_map_ = map;
        map_overridden_ = true;
    }
    map.Fit = ImGuiMouseButton_Middle;
    if (full) {
        map.PanMod    = kImpossibleMod;
        map.SelectMod = kImpossibleMod;
        map.Menu      = ImGuiMouseButton_Middle;
    }
}

void DrawingLayer::cancel_placement() {
    pending_.clear();
    measure_frozen_ = false;
    stroke_active_ = false;
}

void DrawingLayer::handle_keys(const AppContext& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    DrawingManager& mgr = ctx.drawing_mgr();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        bool consumed = true;
        if (text_editor_open_) {
            text_editor_open_ = false;
        } else if (drag_id_ != 0) {
            // Abort the drag: restore the snapshot, drop the ActiveID claim.
            if (Drawing* d = mgr.find(drag_id_)) *d = drag_orig_;
            ImGui::ClearActiveID();
            drag_id_ = 0;
            drag_part_ = kPartNone;
        } else if (!pending_.empty()) {
            cancel_placement();
        } else if (mgr.armed() != Tool::Cursor) {
            mgr.arm(Tool::Cursor);
        } else if (mgr.selected() != 0) {
            mgr.select(0);
        } else {
            consumed = false;
        }
        if (consumed) mgr.mark_escape_consumed(ImGui::GetFrameCount());
    }

    // Enter commits an in-progress polyline.
    if (mgr.armed() == Tool::Polyline && pending_.size() >= 2 &&
        ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        Drawing d;
        d.tool = Tool::Polyline;
        d.anchors = pending_;
        d.style = mgr.default_style();
        pending_.clear();
        if (mgr.hidden_all()) mgr.set_hidden_all(false);
        mgr.select(mgr.add(std::move(d)));
        mgr.arm(Tool::Cursor);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && drag_id_ == 0 &&
        mgr.selected() != 0) {
        mgr.remove(mgr.selected());
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && drag_id_ == 0) {
        mgr.undo();
    }
}

DrawingLayer::Hit DrawingLayer::hit_test(const AppContext& ctx) {
    Hit hit;
    if (!ImPlot::IsPlotHovered()) return hit;
    DrawingManager& mgr = ctx.drawing_mgr();
    if (mgr.hidden_all()) return hit;

    const ImVec2 rmin = ImPlot::GetPlotPos();
    const ImVec2 sz = ImPlot::GetPlotSize();
    const ImVec2 rmax(rmin.x + sz.x, rmin.y + sz.y);
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    static std::vector<Wire> wires;
    static std::vector<ImVec2> handles;

    // Latest-drawn wins: iterate newest-first.
    const auto& items = mgr.items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const Drawing& d = *it;
        if (d.hidden) continue;
        build_geometry(d, rmin, rmax, wires, handles);
        if (wires.empty()) continue;
        // Handles first (they sit on top of the wires) — grabbable when the
        // drawing is selected (TV behavior; body-hover shows them).
        if (d.id == mgr.selected()) {
            for (size_t i = 0; i < handles.size(); ++i) {
                const float dx = mouse.x - handles[i].x, dy = mouse.y - handles[i].y;
                if (std::sqrt(dx * dx + dy * dy) <= kHandleTolPx) {
                    hit.id = d.id;
                    hit.part = static_cast<int>(i);
                    return hit;
                }
            }
        }
        for (const Wire& wire : wires) {
            ImVec2 a = wire.a, b = wire.b;
            if (!clip_line(a, b, rmin, rmax)) continue;
            if (dist_seg(mouse, a, b) <= kHitTolPx) {
                hit.id = d.id;
                hit.part = kPartBody;
                return hit;
            }
        }
    }
    return hit;
}

void DrawingLayer::update_cursor_mode(const AppContext& ctx, int64_t tf_ms) {
    DrawingManager& mgr = ctx.drawing_mgr();
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID drag_gid = ImGui::GetID("##drawing_drag");

    if (drag_id_ != 0) {
        // Active drag: keep the ActiveID claim so the plot's ButtonBehavior
        // yields (no pan), re-apply from the snapshot each frame.
        ImGui::KeepAliveID(drag_gid);
        Drawing* d = mgr.find(drag_id_);
        if (!d) {
            ImGui::ClearActiveID();
            drag_id_ = 0;
            drag_part_ = kPartNone;
            return;
        }
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        const double dt = mouse.x - drag_start_.x;
        const double dp = mouse.y - drag_start_.y;
        d->anchors = drag_orig_.anchors;
        d->chan_off = drag_orig_.chan_off;
        d->stop = drag_orig_.stop;
        d->target = drag_orig_.target;
        const bool is_position =
            d->tool == Tool::LongPosition || d->tool == Tool::ShortPosition;

        auto move_anchor_time = [&](Anchor& a, const Anchor& orig) {
            a.t_ms = snap_time(static_cast<double>(orig.t_ms) + dt, tf_ms);
        };
        if (drag_part_ == kPartBody) {
            // Snap the time delta to whole candles so bodies stay aligned.
            const int64_t dt_snap =
                tf_ms > 0
                    ? static_cast<int64_t>(std::round(dt / static_cast<double>(tf_ms))) * tf_ms
                    : static_cast<int64_t>(dt);
            for (size_t i = 0; i < d->anchors.size(); ++i) {
                d->anchors[i].t_ms = drag_orig_.anchors[i].t_ms + dt_snap;
                d->anchors[i].price = drag_orig_.anchors[i].price + dp;
            }
            if (is_position) {
                d->stop = drag_orig_.stop + dp;
                d->target = drag_orig_.target + dp;
            }
        } else if (is_position) {
            // 0 = entry (moves the whole structure), 1 = right edge (time),
            // 2 = target price, 3 = stop price.
            switch (drag_part_) {
                case 0:
                    move_anchor_time(d->anchors[0], drag_orig_.anchors[0]);
                    d->anchors[0].price = drag_orig_.anchors[0].price + dp;
                    d->stop = drag_orig_.stop + dp;
                    d->target = drag_orig_.target + dp;
                    break;
                case 1:
                    move_anchor_time(d->anchors[1], drag_orig_.anchors[1]);
                    break;
                case 2:
                    d->target = drag_orig_.target + dp;
                    if (mgr.magnet())
                        d->target = magnet_price(ctx, snap_time(mouse.x, tf_ms),
                                                 d->target);
                    break;
                case 3:
                    d->stop = drag_orig_.stop + dp;
                    if (mgr.magnet())
                        d->stop = magnet_price(ctx, snap_time(mouse.x, tf_ms),
                                               d->stop);
                    break;
                default: break;
            }
        } else if (d->tool == Tool::Rectangle && d->anchors.size() > 1) {
            // Corner k: 0 = a0, 1 = a1, 2 = (a0.t, a1.p), 3 = (a1.t, a0.p).
            Anchor& a0 = d->anchors[0];
            Anchor& a1 = d->anchors[1];
            const Anchor& o0 = drag_orig_.anchors[0];
            const Anchor& o1 = drag_orig_.anchors[1];
            switch (drag_part_) {
                case 0: move_anchor_time(a0, o0); a0.price = o0.price + dp; break;
                case 1: move_anchor_time(a1, o1); a1.price = o1.price + dp; break;
                case 2: move_anchor_time(a0, o0); a1.price = o1.price + dp; break;
                case 3: move_anchor_time(a1, o1); a0.price = o0.price + dp; break;
                default: break;
            }
        } else if (d->tool == Tool::Channel && drag_part_ == 2) {
            d->chan_off = drag_orig_.chan_off + dp;
        } else if (drag_part_ >= 0 &&
                   static_cast<size_t>(drag_part_) < d->anchors.size()) {
            Anchor& a = d->anchors[static_cast<size_t>(drag_part_)];
            const Anchor& o = drag_orig_.anchors[static_cast<size_t>(drag_part_)];
            move_anchor_time(a, o);
            a.price = o.price + dp;
            if (mgr.magnet() && d->tool != Tool::Brush)
                a.price = magnet_price(ctx, a.t_ms, a.price);
        }
        mgr.mark_dirty();
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::ClearActiveID();
            mgr.end_modify(drag_id_);
            drag_id_ = 0;
            drag_part_ = kPartNone;
        }
        return;
    }

    const Hit hit = hit_test(ctx);
    hover_id_ = hit.id;
    hover_part_ = hit.part;
    if (hover_id_ != 0) {
        ImGui::SetMouseCursor(hit.part >= 0 ? ImGuiMouseCursor_Hand
                                            : ImGuiMouseCursor_ResizeAll);
        // Double-click on a Text drawing re-opens the inline editor.
        Drawing* hd = mgr.find(hover_id_);
        if (hd && hd->tool == Tool::Text &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            mgr.select(hover_id_);
            open_text_editor(hd->id, hd->anchors[0], hd->text.c_str());
            return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
            mgr.select(hover_id_);
            Drawing* d = mgr.find(hover_id_);
            if (d && !d->locked) {
                drag_id_ = hover_id_;
                drag_part_ = hit.part;
                drag_start_ = ImPlot::GetPlotMousePos();
                drag_orig_ = *d;
                mgr.begin_modify(drag_id_);
                ImGui::SetActiveID(drag_gid, ImGui::GetCurrentWindow());
            }
        }
    } else if (ImPlot::IsPlotHovered() &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift &&
               mgr.selected() != 0) {
        mgr.select(0);  // click on empty chart deselects
    }
}

void DrawingLayer::update_placement(const AppContext& ctx, int64_t tf_ms) {
    DrawingManager& mgr = ctx.drawing_mgr();
    const Tool tool = mgr.armed();
    ImGuiIO& io = ImGui::GetIO();

    if (!tool_implemented(tool)) return;

    // Right-click cancels the placement AND disarms (the context menu is
    // gated off by captures_mouse while armed).
    if (ImPlot::IsPlotHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        cancel_placement();
        mgr.arm(Tool::Cursor);
        return;
    }

    const bool hovered = ImPlot::IsPlotHovered();
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();

    // Candle-snapped time + (optional) OHLC-magnet price.
    auto place_anchor = [&](double tx, double py) {
        Anchor a{snap_time(tx, tf_ms), py};
        if (mgr.magnet()) a.price = magnet_price(ctx, a.t_ms, a.price);
        return a;
    };

    auto commit = [&](Drawing&& d, bool disarm) {
        pending_.clear();
        measure_frozen_ = false;
        stroke_active_ = false;
        if (mgr.hidden_all()) mgr.set_hidden_all(false);
        const uint64_t id = mgr.add(std::move(d));
        mgr.select(id);
        if (disarm) mgr.arm(Tool::Cursor);
    };

    // ── Brush: press-drag-release stroke ────────────────────────────────────
    if (tool == Tool::Brush) {
        if (stroke_active_) {
            if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                pending_.size() < kMaxBrushPoints) {
                const Anchor a{static_cast<int64_t>(mouse.x), mouse.y};
                const ImVec2 last = to_px(pending_.back());
                const ImVec2 cur = to_px(a);
                const float dx = cur.x - last.x, dy = cur.y - last.y;
                if (std::sqrt(dx * dx + dy * dy) >= kBrushMinStepPx)
                    pending_.push_back(a);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (pending_.size() >= 2) {
                    Drawing d;
                    d.tool = Tool::Brush;
                    d.anchors = pending_;
                    d.style = mgr.default_style();
                    commit(std::move(d), /*disarm=*/false);  // brush stays armed
                } else {
                    pending_.clear();
                    stroke_active_ = false;
                }
            }
        } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   !io.KeyShift) {
            stroke_active_ = true;
            pending_.clear();
            pending_.push_back(Anchor{static_cast<int64_t>(mouse.x), mouse.y});
        }
        return;
    }

    // ── Measure: 2 clicks, ephemeral; 3rd click / Esc dismisses ────────────
    if (tool == Tool::Measure) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            io.KeyShift)
            return;
        if (measure_frozen_ || pending_.size() >= 2) {
            cancel_placement();
            mgr.arm(Tool::Cursor);
        } else if (pending_.empty()) {
            pending_.push_back(place_anchor(mouse.x, mouse.y));
        } else {
            pending_.push_back(place_anchor(mouse.x, mouse.y));
            measure_frozen_ = true;
        }
        return;
    }

    // ── Long/Short position: single click places with sane defaults ────────
    if (tool == Tool::LongPosition || tool == Tool::ShortPosition) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            io.KeyShift)
            return;
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        const double r_unit = (lim.Y.Max - lim.Y.Min) * 0.10;  // 1R = 10% of view
        const double span_x = lim.X.Max - lim.X.Min;
        int64_t width_ms = static_cast<int64_t>(span_x * 0.25);
        if (tf_ms > 0) {
            width_ms = std::max<int64_t>(width_ms, 10 * tf_ms);
            width_ms = (width_ms / tf_ms) * tf_ms;
        }
        Drawing d;
        d.tool = tool;
        const Anchor entry = place_anchor(mouse.x, mouse.y);
        d.anchors = {entry, Anchor{entry.t_ms + width_ms, entry.price}};
        if (tool == Tool::LongPosition) {
            d.target = entry.price + 2.0 * r_unit;
            d.stop   = entry.price - r_unit;
        } else {
            d.target = entry.price - 2.0 * r_unit;
            d.stop   = entry.price + r_unit;
        }
        d.style = mgr.default_style();
        commit(std::move(d), true);
        return;
    }

    // ── Text: single click opens the inline editor ──────────────────────────
    if (tool == Tool::Text) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !io.KeyShift && !text_editor_open_) {
            open_text_editor(0, Anchor{snap_time(mouse.x, tf_ms), mouse.y}, "");
            mgr.arm(Tool::Cursor);
        }
        return;
    }

    // ── Polyline: click per vertex; double-click/Enter ends ────────────────
    if (tool == Tool::Polyline) {
        if (!hovered || io.KeyShift) return;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // The double-click's own clicks appended 1-2 duplicate vertices —
            // drop the trailing duplicate(s) within 3px of their predecessor.
            while (pending_.size() >= 2) {
                const ImVec2 a = to_px(pending_[pending_.size() - 2]);
                const ImVec2 b = to_px(pending_.back());
                const float dx = b.x - a.x, dy = b.y - a.y;
                if (std::sqrt(dx * dx + dy * dy) <= 3.0f) pending_.pop_back();
                else break;
            }
            if (pending_.size() >= 2) {
                Drawing d;
                d.tool = Tool::Polyline;
                d.anchors = pending_;
                d.style = mgr.default_style();
                commit(std::move(d), true);
            } else {
                cancel_placement();
                mgr.arm(Tool::Cursor);
            }
            return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            pending_.size() < kMaxPolylinePoints) {
            pending_.push_back(place_anchor(mouse.x, mouse.y));
        }
        return;
    }

    // ── Channel: A, B, then the parallel offset click ───────────────────────
    if (tool == Tool::Channel) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            io.KeyShift)
            return;
        if (pending_.size() < 2) {
            pending_.push_back(place_anchor(mouse.x, mouse.y));
        } else {
            Drawing d;
            d.tool = Tool::Channel;
            d.anchors = {pending_[0], pending_[1]};
            d.chan_off = mouse.y - price_on_line(pending_[0], pending_[1], mouse.x);
            d.style = mgr.default_style();
            commit(std::move(d), true);
        }
        return;
    }

    // ── Fixed-anchor tools ──────────────────────────────────────────────────
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.KeyShift)
        return;
    pending_.push_back(place_anchor(mouse.x, mouse.y));
    const int need = anchors_required(tool);
    if (need > 0 && static_cast<int>(pending_.size()) >= need) {
        Drawing d;
        d.tool = tool;
        d.anchors = pending_;
        d.style = mgr.default_style();
        commit(std::move(d), true);
    }
}

void DrawingLayer::render_drawings(const AppContext& ctx, const PriceFormatter& fmt,
                                   int64_t tf_ms) {
    DrawingManager& mgr = ctx.drawing_mgr();
    if (mgr.hidden_all()) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 rmin = ImPlot::GetPlotPos();
    const ImVec2 sz = ImPlot::GetPlotSize();
    const ImVec2 rmax(rmin.x + sz.x, rmin.y + sz.y);

    static std::vector<Wire> wires;
    static std::vector<ImVec2> handles;

    const int hot_part = drag_id_ != 0 ? drag_part_ : hover_part_;
    const uint64_t hot_id = drag_id_ != 0 ? drag_id_ : hover_id_;
    for (const Drawing& d : mgr.items()) {
        if (d.hidden) continue;
        render_one(dl, d, fmt, tf_ms, rmin, rmax, d.id == mgr.selected(),
                   d.id == hot_id, d.id == hot_id ? hot_part : kInvalidPart,
                   wires, handles);
    }
}

void DrawingLayer::render_placement_preview(const AppContext& ctx,
                                            const PriceFormatter& fmt,
                                            int64_t tf_ms) {
    DrawingManager& mgr = ctx.drawing_mgr();
    const Tool tool = mgr.armed();
    if (!tool_implemented(tool)) return;
    if (pending_.empty() && !ImPlot::IsPlotHovered()) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 rmin = ImPlot::GetPlotPos();
    const ImVec2 sz = ImPlot::GetPlotSize();
    const ImVec2 rmax(rmin.x + sz.x, rmin.y + sz.y);

    Drawing preview;
    preview.tool = tool;
    preview.style = mgr.default_style();
    preview.anchors = pending_;

    const bool hovered = ImPlot::IsPlotHovered();
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();

    if (tool == Tool::Channel && pending_.size() >= 2) {
        // Stage 3: the parallel line tracks the cursor.
        preview.anchors = {pending_[0], pending_[1]};
        preview.chan_off =
            hovered ? mouse.y - price_on_line(pending_[0], pending_[1], mouse.x)
                    : 0.0;
    } else if (tool == Tool::Brush) {
        if (!stroke_active_) {
            if (hovered) {
                // Brush cursor dot at the pointer.
                const ImVec2 c = ImGui::GetIO().MousePos;
                dl->AddCircle(c, std::max(2.0f, preview.style.width * 1.5f),
                              preview.style.color, 0, 1.5f);
            }
            return;
        }
        // The in-progress stroke renders as-is.
    } else if (tool == Tool::Text) {
        if (hovered) {
            const ImVec2 c = ImGui::GetIO().MousePos;
            dl->AddText(Theme::Fonts::ui(), 14.0f, ImVec2(c.x + 8.0f, c.y - 7.0f),
                        with_alpha(preview.style.color, 170), "T");
        }
        return;
    } else if (hovered) {
        Anchor a{snap_time(mouse.x, tf_ms), mouse.y};
        if (mgr.magnet()) a.price = magnet_price(ctx, a.t_ms, a.price);
        preview.anchors.push_back(a);
    } else if (!pending_.empty() &&
               static_cast<int>(preview.anchors.size()) <
                   std::max(anchors_required(tool), 2)) {
        // Mid-placement but off-plot: freeze the preview at the last anchor.
        preview.anchors.push_back(preview.anchors.back());
    }

    if (preview.anchors.empty()) return;

    static std::vector<Wire> wires;
    static std::vector<ImVec2> handles;
    render_one(dl, preview, fmt, tf_ms, rmin, rmax, /*selected=*/false,
               /*hovered=*/true, kInvalidPart, wires, handles);
}

void DrawingLayer::render_measure(const AppContext& ctx, const PriceFormatter& fmt,
                                  int64_t tf_ms) {
    DrawingManager& mgr = ctx.drawing_mgr();
    if (mgr.armed() != Tool::Measure || pending_.empty()) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    Anchor a = pending_[0];
    Anchor b;
    if (measure_frozen_ && pending_.size() >= 2) {
        b = pending_[1];
    } else if (ImPlot::IsPlotHovered()) {
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        b = Anchor{snap_time(mouse.x, tf_ms), mouse.y};
        if (mgr.magnet()) b.price = magnet_price(ctx, b.t_ms, b.price);
    } else {
        return;
    }

    const ImVec2 pa = to_px(a), pb = to_px(b);
    const bool up = b.price >= a.price;
    const ImVec4& tone = up ? Theme::Tokens::UP : Theme::Tokens::DOWN;
    const ImVec2 r0(std::min(pa.x, pb.x), std::min(pa.y, pb.y));
    const ImVec2 r1(std::max(pa.x, pb.x), std::max(pa.y, pb.y));
    dl->AddRectFilled(r0, r1, Theme::u32(tone, 0.13f));
    dl->AddRect(r0, r1, Theme::u32(tone), 0.0f, 0, 1.0f);
    // Direction arrows through the middle.
    const float cx = (r0.x + r1.x) * 0.5f, cy = (r0.y + r1.y) * 0.5f;
    dl->AddLine(ImVec2(cx, pa.y), ImVec2(cx, pb.y), Theme::u32(tone), 1.2f);
    arrow_head_px(dl, ImVec2(cx, pb.y), ImVec2(cx, pa.y), Theme::u32(tone), 8.0f);
    dl->AddLine(ImVec2(pa.x, cy), ImVec2(pb.x, cy), Theme::u32(tone), 1.2f);
    arrow_head_px(dl, ImVec2(pb.x, cy), ImVec2(pa.x, cy), Theme::u32(tone), 8.0f);

    const double dprice = b.price - a.price;
    const double pct = a.price != 0.0 ? dprice / a.price * 100.0 : 0.0;
    const int64_t dt = std::llabs(b.t_ms - a.t_ms);
    const long bars = tf_ms > 0 ? static_cast<long>(dt / tf_ms) : 0;
    char pbuf[32], dur[32], l1[64], l2[64];
    fmt.format_price(pbuf, sizeof(pbuf), std::fabs(dprice));
    fmt_duration(dur, sizeof(dur), dt);
    snprintf(l1, sizeof(l1), "%s%s (%+.2f%%)", up ? "+" : "-", pbuf, pct);
    snprintf(l2, sizeof(l2), "%ld bars · %s", bars, dur);
    readout_chip(dl, ImVec2(cx, r1.y + 8.0f), l1, l2, Theme::u32(tone));
}

void DrawingLayer::open_text_editor(uint64_t id, const Anchor& anchor,
                                    const char* initial) {
    text_editor_open_ = true;
    text_edit_focus_ = true;
    text_edit_id_ = id;
    text_edit_anchor_ = anchor;
    snprintf(text_edit_buf_, sizeof(text_edit_buf_), "%s", initial ? initial : "");
}

void DrawingLayer::render_text_editor(const AppContext& ctx) {
    if (!text_editor_open_) return;
    DrawingManager& mgr = ctx.drawing_mgr();

    const ImVec2 anchor_px = to_px(text_edit_anchor_);
    ImGui::SetNextWindowPos(ImVec2(anchor_px.x + 12.0f, anchor_px.y + 12.0f),
                            ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;
    bool commit = false, cancel = false;
    if (ImGui::Begin("##drawing_text_editor", nullptr, flags)) {
        ImGui::PushFont(Theme::Fonts::ui());
        if (text_edit_focus_) {
            ImGui::SetKeyboardFocusHere();
            text_edit_focus_ = false;
        }
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::InputText("##dtext", text_edit_buf_, sizeof(text_edit_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            commit = true;
        if (ImGui::Button("Save", ImVec2(70.0f, 0.0f))) commit = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(70.0f, 0.0f))) cancel = true;
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (cancel) {
        text_editor_open_ = false;
        return;
    }
    if (!commit) return;
    text_editor_open_ = false;
    if (text_edit_buf_[0] == '\0') return;  // empty text: nothing to keep
    if (text_edit_id_ == 0) {
        Drawing d;
        d.tool = Tool::Text;
        d.anchors = {text_edit_anchor_};
        d.style = mgr.default_style();
        d.text = text_edit_buf_;
        if (mgr.hidden_all()) mgr.set_hidden_all(false);
        mgr.select(mgr.add(std::move(d)));
    } else if (Drawing* d = mgr.find(text_edit_id_)) {
        mgr.begin_modify(text_edit_id_);
        d->text = text_edit_buf_;
        mgr.end_modify(text_edit_id_);
        mgr.mark_dirty();
    }
}

}  // namespace drawing
