#include "ui/drawing/drawing_icons.h"

#include <cmath>

namespace drawing {

namespace {

constexpr float kPi = 3.14159265358979323846f;  // IM_PI needs imgui_internal.h

inline ImVec2 at(ImVec2 c, float s, float x, float y) {
    // x/y in [-1, 1] glyph space -> pixels around the center.
    return ImVec2(c.x + x * s, c.y + y * s);
}

// Small anchor-point circle (the open handles TradingView shows on lines).
inline void dot(ImDrawList* dl, ImVec2 p, ImU32 col, float r, float th) {
    dl->AddCircle(p, r, col, 0, th);
}

void arrow_head(ImDrawList* dl, ImVec2 tip, ImVec2 from, ImU32 col, float len) {
    ImVec2 d(tip.x - from.x, tip.y - from.y);
    const float mag = std::sqrt(d.x * d.x + d.y * d.y);
    if (mag < 1e-3f) return;
    d.x /= mag; d.y /= mag;
    const ImVec2 n(-d.y, d.x);
    dl->AddTriangleFilled(
        tip,
        ImVec2(tip.x - d.x * len + n.x * len * 0.55f, tip.y - d.y * len + n.y * len * 0.55f),
        ImVec2(tip.x - d.x * len - n.x * len * 0.55f, tip.y - d.y * len - n.y * len * 0.55f),
        col);
}

}  // namespace

void draw_tool_icon(ImDrawList* dl, Tool t, ImVec2 c, float s, ImU32 col, float th) {
    const float r = s * 0.22f;  // handle-dot radius
    switch (t) {
        case Tool::Cursor: {
            // Mouse pointer: filled wedge + tail.
            const ImVec2 a = at(c, s, -0.35f, -0.80f);
            dl->AddTriangleFilled(a, at(c, s, -0.35f, 0.45f), at(c, s, 0.30f, 0.05f), col);
            dl->AddLine(at(c, s, 0.05f, 0.20f), at(c, s, 0.45f, 0.80f), col, th);
            break;
        }
        case Tool::Trendline:
            dl->AddLine(at(c, s, -0.55f, 0.55f), at(c, s, 0.55f, -0.55f), col, th);
            dot(dl, at(c, s, -0.75f, 0.75f), col, r, th);
            dot(dl, at(c, s, 0.75f, -0.75f), col, r, th);
            break;
        case Tool::Arrow: {
            const ImVec2 tip = at(c, s, 0.75f, -0.75f);
            dl->AddLine(at(c, s, -0.75f, 0.75f), tip, col, th);
            arrow_head(dl, tip, at(c, s, -0.75f, 0.75f), col, s * 0.55f);
            break;
        }
        case Tool::Ray:
            dot(dl, at(c, s, -0.65f, 0.65f), col, r, th);
            dl->AddLine(at(c, s, -0.45f, 0.45f), at(c, s, 0.95f, -0.95f), col, th);
            break;
        case Tool::ExtendedLine:
            dl->AddLine(at(c, s, -0.95f, 0.95f), at(c, s, 0.95f, -0.95f), col, th);
            dot(dl, at(c, s, -0.35f, 0.35f), col, r, th);
            dot(dl, at(c, s, 0.35f, -0.35f), col, r, th);
            break;
        case Tool::HLine:
            dl->AddLine(at(c, s, -0.95f, 0.0f), at(c, s, 0.95f, 0.0f), col, th);
            dot(dl, at(c, s, 0.0f, 0.0f), col, r, th);
            break;
        case Tool::HRay:
            dot(dl, at(c, s, -0.70f, 0.0f), col, r, th);
            dl->AddLine(at(c, s, -0.48f, 0.0f), at(c, s, 0.95f, 0.0f), col, th);
            break;
        case Tool::VLine:
            dl->AddLine(at(c, s, 0.0f, -0.95f), at(c, s, 0.0f, 0.95f), col, th);
            dot(dl, at(c, s, 0.0f, 0.0f), col, r, th);
            break;
        case Tool::CrossLine:
            dl->AddLine(at(c, s, -0.95f, 0.0f), at(c, s, 0.95f, 0.0f), col, th);
            dl->AddLine(at(c, s, 0.0f, -0.95f), at(c, s, 0.0f, 0.95f), col, th);
            break;
        case Tool::Rectangle:
            dl->AddRect(at(c, s, -0.75f, -0.55f), at(c, s, 0.75f, 0.55f), col, 0.0f, 0, th);
            break;
        case Tool::Brush: {
            // Freehand squiggle.
            const ImVec2 p0 = at(c, s, -0.85f, 0.55f);
            const ImVec2 p1 = at(c, s, -0.25f, -0.85f);
            const ImVec2 p2 = at(c, s, 0.25f, 0.85f);
            const ImVec2 p3 = at(c, s, 0.85f, -0.55f);
            dl->AddBezierCubic(p0, p1, p2, p3, col, th);
            break;
        }
        case Tool::Measure: {
            // Ruler: slanted bar with cross ticks.
            dl->AddLine(at(c, s, -0.80f, 0.60f), at(c, s, 0.80f, -0.60f), col, th);
            dl->AddLine(at(c, s, -0.55f, 0.15f), at(c, s, -0.30f, 0.50f), col, th * 0.8f);
            dl->AddLine(at(c, s, 0.00f, -0.28f), at(c, s, 0.25f, 0.08f), col, th * 0.8f);
            dl->AddLine(at(c, s, 0.52f, -0.68f), at(c, s, 0.78f, -0.32f), col, th * 0.8f);
            break;
        }
        case Tool::PriceRange: {
            // Vertical double-headed arrow between two levels.
            dl->AddLine(at(c, s, -0.85f, -0.80f), at(c, s, 0.85f, -0.80f), col, th);
            dl->AddLine(at(c, s, -0.85f, 0.80f), at(c, s, 0.85f, 0.80f), col, th);
            const ImVec2 top = at(c, s, 0.0f, -0.62f), bot = at(c, s, 0.0f, 0.62f);
            dl->AddLine(top, bot, col, th);
            arrow_head(dl, top, bot, col, s * 0.38f);
            arrow_head(dl, bot, top, col, s * 0.38f);
            break;
        }
        case Tool::DateRange: {
            dl->AddLine(at(c, s, -0.80f, -0.85f), at(c, s, -0.80f, 0.85f), col, th);
            dl->AddLine(at(c, s, 0.80f, -0.85f), at(c, s, 0.80f, 0.85f), col, th);
            const ImVec2 l = at(c, s, -0.62f, 0.0f), rr = at(c, s, 0.62f, 0.0f);
            dl->AddLine(l, rr, col, th);
            arrow_head(dl, l, rr, col, s * 0.38f);
            arrow_head(dl, rr, l, col, s * 0.38f);
            break;
        }
        case Tool::Fib:
            dl->AddLine(at(c, s, -0.85f, -0.65f), at(c, s, 0.85f, -0.65f), col, th);
            dl->AddLine(at(c, s, -0.85f, -0.10f), at(c, s, 0.30f, -0.10f), col, th);
            dl->AddLine(at(c, s, -0.85f, 0.35f), at(c, s, 0.60f, 0.35f), col, th);
            dl->AddLine(at(c, s, -0.85f, 0.80f), at(c, s, 0.85f, 0.80f), col, th);
            break;
        case Tool::LongPosition: {
            dl->AddRect(at(c, s, -0.80f, -0.85f), at(c, s, 0.80f, -0.05f), col, 0.0f, 0, th);
            dl->AddLine(at(c, s, -0.80f, 0.45f), at(c, s, 0.80f, 0.45f), col, th);
            const ImVec2 tip = at(c, s, 0.0f, -0.60f);
            arrow_head(dl, tip, at(c, s, 0.0f, 0.40f), col, s * 0.40f);
            break;
        }
        case Tool::ShortPosition: {
            dl->AddRect(at(c, s, -0.80f, 0.05f), at(c, s, 0.80f, 0.85f), col, 0.0f, 0, th);
            dl->AddLine(at(c, s, -0.80f, -0.45f), at(c, s, 0.80f, -0.45f), col, th);
            const ImVec2 tip = at(c, s, 0.0f, 0.60f);
            arrow_head(dl, tip, at(c, s, 0.0f, -0.40f), col, s * 0.40f);
            break;
        }
        case Tool::Text:
            dl->AddLine(at(c, s, -0.70f, -0.70f), at(c, s, 0.70f, -0.70f), col, th);
            dl->AddLine(at(c, s, -0.70f, -0.70f), at(c, s, -0.70f, -0.40f), col, th);
            dl->AddLine(at(c, s, 0.70f, -0.70f), at(c, s, 0.70f, -0.40f), col, th);
            dl->AddLine(at(c, s, 0.0f, -0.70f), at(c, s, 0.0f, 0.80f), col, th);
            dl->AddLine(at(c, s, -0.28f, 0.80f), at(c, s, 0.28f, 0.80f), col, th);
            break;
        case Tool::Channel:
            dl->AddLine(at(c, s, -0.85f, 0.35f), at(c, s, 0.55f, -0.85f), col, th);
            dl->AddLine(at(c, s, -0.55f, 0.85f), at(c, s, 0.85f, -0.35f), col, th);
            break;
        case Tool::Polyline: {
            const ImVec2 a = at(c, s, -0.85f, 0.55f);
            const ImVec2 b = at(c, s, -0.20f, -0.60f);
            const ImVec2 d = at(c, s, 0.30f, 0.30f);
            const ImVec2 e = at(c, s, 0.85f, -0.45f);
            dl->AddLine(a, b, col, th);
            dl->AddLine(b, d, col, th);
            dl->AddLine(d, e, col, th);
            dot(dl, b, col, r, th);
            dot(dl, d, col, r, th);
            break;
        }
        default:
            break;
    }
}

void draw_ui_icon(ImDrawList* dl, UiIcon icon, ImVec2 c, float s, ImU32 col, float th) {
    switch (icon) {
        case UiIcon::Magnet: {
            // Horseshoe: arc + two legs with pole ticks.
            dl->PathClear();
            dl->PathArcTo(at(c, s, 0.0f, -0.15f), s * 0.62f, kPi, kPi * 2.0f, 16);
            dl->PathStroke(col, 0, th);
            dl->AddLine(at(c, s, -0.62f, -0.15f), at(c, s, -0.62f, 0.70f), col, th);
            dl->AddLine(at(c, s, 0.62f, -0.15f), at(c, s, 0.62f, 0.70f), col, th);
            dl->AddLine(at(c, s, -0.80f, 0.42f), at(c, s, -0.44f, 0.42f), col, th);
            dl->AddLine(at(c, s, 0.44f, 0.42f), at(c, s, 0.80f, 0.42f), col, th);
            break;
        }
        case UiIcon::Eye:
        case UiIcon::EyeOff: {
            // Lens: two arcs + pupil.
            dl->PathClear();
            dl->PathArcTo(at(c, s, 0.0f, 0.55f), s * 1.05f, kPi * 1.25f, kPi * 1.75f, 12);
            dl->PathStroke(col, 0, th);
            dl->PathClear();
            dl->PathArcTo(at(c, s, 0.0f, -0.55f), s * 1.05f, kPi * 0.25f, kPi * 0.75f, 12);
            dl->PathStroke(col, 0, th);
            dl->AddCircleFilled(at(c, s, 0.0f, 0.0f), s * 0.22f, col);
            if (icon == UiIcon::EyeOff)
                dl->AddLine(at(c, s, -0.75f, 0.75f), at(c, s, 0.75f, -0.75f), col, th);
            break;
        }
        case UiIcon::Trash:
            dl->AddLine(at(c, s, -0.80f, -0.55f), at(c, s, 0.80f, -0.55f), col, th);
            dl->AddLine(at(c, s, -0.28f, -0.80f), at(c, s, 0.28f, -0.80f), col, th);
            dl->AddRect(at(c, s, -0.55f, -0.55f), at(c, s, 0.55f, 0.85f), col, 0.0f, 0, th);
            dl->AddLine(at(c, s, -0.18f, -0.25f), at(c, s, -0.18f, 0.55f), col, th * 0.8f);
            dl->AddLine(at(c, s, 0.18f, -0.25f), at(c, s, 0.18f, 0.55f), col, th * 0.8f);
            break;
        case UiIcon::ChevronLeft:
            dl->AddLine(at(c, s, 0.35f, -0.65f), at(c, s, -0.35f, 0.0f), col, th);
            dl->AddLine(at(c, s, -0.35f, 0.0f), at(c, s, 0.35f, 0.65f), col, th);
            break;
        case UiIcon::ChevronRight:
            dl->AddLine(at(c, s, -0.35f, -0.65f), at(c, s, 0.35f, 0.0f), col, th);
            dl->AddLine(at(c, s, 0.35f, 0.0f), at(c, s, -0.35f, 0.65f), col, th);
            break;
        case UiIcon::Pencil: {
            // Diagonal pencil body + tip.
            dl->AddLine(at(c, s, -0.70f, 0.45f), at(c, s, 0.45f, -0.70f), col, th);
            dl->AddLine(at(c, s, -0.45f, 0.70f), at(c, s, 0.70f, -0.45f), col, th);
            dl->AddLine(at(c, s, -0.70f, 0.45f), at(c, s, -0.85f, 0.85f), col, th);
            dl->AddLine(at(c, s, -0.85f, 0.85f), at(c, s, -0.45f, 0.70f), col, th);
            dl->AddLine(at(c, s, 0.45f, -0.70f), at(c, s, 0.70f, -0.45f), col, th);
            break;
        }
        case UiIcon::Lock:
        case UiIcon::Unlock: {
            dl->AddRect(at(c, s, -0.60f, -0.05f), at(c, s, 0.60f, 0.80f), col, s * 0.15f, 0, th);
            dl->PathClear();
            if (icon == UiIcon::Lock) {
                dl->PathArcTo(at(c, s, 0.0f, -0.05f), s * 0.38f, kPi, kPi * 2.0f, 12);
            } else {
                // Open shackle: arc swings up-left, free end raised.
                dl->PathArcTo(at(c, s, -0.22f, -0.35f), s * 0.38f, kPi * 0.85f, kPi * 1.95f, 12);
            }
            dl->PathStroke(col, 0, th);
            dl->AddCircleFilled(at(c, s, 0.0f, 0.38f), s * 0.14f, col);
            break;
        }
        case UiIcon::Gear: {
            dl->AddCircle(at(c, s, 0.0f, 0.0f), s * 0.32f, col, 0, th);
            for (int i = 0; i < 8; ++i) {
                const float a = (kPi * 2.0f / 8.0f) * static_cast<float>(i);
                const ImVec2 p0(c.x + std::cos(a) * s * 0.55f, c.y + std::sin(a) * s * 0.55f);
                const ImVec2 p1(c.x + std::cos(a) * s * 0.85f, c.y + std::sin(a) * s * 0.85f);
                dl->AddLine(p0, p1, col, th);
            }
            break;
        }
        case UiIcon::Close:
            dl->AddLine(at(c, s, -0.60f, -0.60f), at(c, s, 0.60f, 0.60f), col, th);
            dl->AddLine(at(c, s, -0.60f, 0.60f), at(c, s, 0.60f, -0.60f), col, th);
            break;
    }
}

}  // namespace drawing
