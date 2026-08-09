#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// drawing_icons - ImDrawList vector glyphs for the drawing tools UI.
//
// No icon font exists in the app (theme.cpp loads Latin + a few extras only);
// chrome icons are drawn as vector primitives (precedent: the replay close-X,
// the chart-type glyphs). One glyph per tool + the rail's utility cluster,
// shared by the left rail, the topbar dropdown, and the style editor.
//
// All glyphs draw centered on `c` within a half-extent `s` (a 16px icon is
// s = 8) using the caller's color/thickness - logical pixels, DPI-agnostic.
// ═══════════════════════════════════════════════════════════════════════════════
#include "imgui.h"
#include "ui/drawing/drawing_types.h"

namespace drawing {

enum class UiIcon : uint8_t {
    Magnet,
    Eye,
    EyeOff,
    Trash,
    ChevronLeft,    // collapse the rail
    ChevronRight,   // expand the rail
    Pencil,         // topbar drawing-tools button
    Lock,
    Unlock,
    Gear,
    Close,
};

void draw_tool_icon(ImDrawList* dl, Tool t, ImVec2 c, float s, ImU32 col,
                    float th = 1.5f);
void draw_ui_icon(ImDrawList* dl, UiIcon icon, ImVec2 c, float s, ImU32 col,
                  float th = 1.5f);

}  // namespace drawing
