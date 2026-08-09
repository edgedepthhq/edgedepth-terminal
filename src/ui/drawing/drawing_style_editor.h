#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// drawing_style_editor - floating mini-toolbar for the selected drawing.
//
// A floating ImGui window (NOT a popup - sidesteps OpenPopup scope rules and
// survives clicks into the chart), rendered late in the frame from main.cpp.
// Shown while a drawing is selected: color swatches, width, line pattern,
// hide/lock/delete, plus per-tool extras (fib level toggles, text size).
// Style changes route through begin_modify/end_modify so Ctrl+Z covers them.
// Also owns the default style applied to NEW drawings when nothing is selected
// while a tool is armed.
// ═══════════════════════════════════════════════════════════════════════════════

struct AppContext;

namespace drawing {

void render_style_editor(const AppContext& ctx);

}  // namespace drawing
