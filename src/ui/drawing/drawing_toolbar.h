#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// drawing_toolbar - the in-chart drawing-tools rail + the tool dropdown.
//
// The rail renders INSIDE the chart widget as a child region at the chart
// window's left edge (2026-08-06: moved off the viewport edge so it sits
// directly against the chart, not beside the watchlist). ChartWidget reserves
// its width from the plot area and gates it like full_shell chrome (hidden in
// embedded education chromes, pack mode and clip-recorder focus); the
// drawings themselves still render in every chrome.
//
// The tool dropdown body (icon + label rows) is shared between the AppShell
// topbar pencil and the chart-toolbar Draw button.
// ═══════════════════════════════════════════════════════════════════════════════

class DrawingManager;

namespace drawing {

// Current rail reserve width in logical px (collapsed rail keeps a slim
// expander strip so the tools stay one click away).
float rail_width(const DrawingManager& mgr);

// In-chart rail: a `rail_width()`-wide, `height`-tall child at the current
// cursor position. The caller follows with SameLine(0,0) + the plot region.
void render_chart_rail(DrawingManager& mgr, float height);

// Tool dropdown rows (grouped icon + label list). Call between BeginPopup/
// EndPopup; arms the clicked tool and closes the popup.
void render_tool_menu_rows(DrawingManager& mgr);

// Topbar pencil button + tool dropdown (rendered by AppShell).
void render_topbar_button(DrawingManager& mgr);

}  // namespace drawing
