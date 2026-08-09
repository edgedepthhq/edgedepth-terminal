#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// drawing::DrawingLayer — chart-side engine for the drawing tools.
//
// Owns the placement state machine (armed tool -> anchor clicks -> commit),
// hit-testing/selection/drags, the ImPlot input-map arbitration, and all
// in-plot rendering. The store + armed tool live on DrawingManager (via
// AppContext); this class is a ChartWidget member and holds only transient
// interaction state.
//
// Contract with ChartWidget (kept to ~10 lines there — God-file rule):
//   * render_in_plot() runs INSIDE the ##Price BeginPlot scope, immediately
//     before handle_plot_interaction(), every frame the plot is live.
//   * end_frame() runs right after render_chart() returns (after EndPlot,
//     before render_indicators()) — restores the ImPlot input map so the
//     indicator subplots never see the overrides. Idempotent.
//   * captures_mouse() gates the chart's own click/drag handlers (follow-live
//     clear, Shift+drag replay selection, right-click context menu).
//
// Input arbitration (verified against the pinned ImPlot d65a2be):
//   * While a tool is armed / placing: ImPlot::GetInputMap() is overridden —
//     PanMod/SelectMod get an impossible 4-modifier chord, Fit and Menu move
//     to the middle button. GetInputMap() returns a mutable ref that EndPlot
//     reads LIVE, so a mid-frame override applies the same frame. Button
//     fields must stay in [0,5) — they index size-5 IO arrays; the chord
//     trick is what makes Pan unreachable, never an out-of-range button.
//     Crosshair, hover, and wheel zoom stay fully alive (the TV feel).
//   * Cursor-mode drags do NOT touch the map: the drag claims the ImGui
//     ActiveID (DragPoint pattern), and the plot's own ButtonBehavior yields.
// ═══════════════════════════════════════════════════════════════════════════════
#include "implot.h"
#include "ui/drawing/drawing_types.h"

#include <cstdint>
#include <vector>

struct AppContext;
struct PriceFormatter;

namespace drawing {

class DrawingLayer {
public:
    void render_in_plot(const AppContext& ctx, const PriceFormatter& fmt,
                        bool overlays_allowed, int64_t tf_seconds);
    void end_frame();

    [[nodiscard]] bool captures_mouse() const { return captures_; }

private:
    // Hit "parts": >= 0 is a handle index (per-tool meaning; rect corners are
    // 0..3), kPartBody drags the whole drawing.
    static constexpr int kPartNone = -1;
    static constexpr int kPartBody = -2;

    struct Hit { uint64_t id = 0; int part = kPartNone; };

    // Frame stages (all called from render_in_plot).
    void handle_keys(const AppContext& ctx);
    void update_cursor_mode(const AppContext& ctx, int64_t tf_ms);
    void update_placement(const AppContext& ctx, int64_t tf_ms);
    void render_drawings(const AppContext& ctx, const PriceFormatter& fmt,
                         int64_t tf_ms);
    void render_placement_preview(const AppContext& ctx, const PriceFormatter& fmt,
                                  int64_t tf_ms);
    void render_measure(const AppContext& ctx, const PriceFormatter& fmt,
                        int64_t tf_ms);
    void render_text_editor(const AppContext& ctx);
    void open_text_editor(uint64_t id, const Anchor& anchor, const char* initial);
    void apply_input_override(bool full);

    Hit  hit_test(const AppContext& ctx);
    void cancel_placement();

    // ── Transient interaction state ──────────────────────────────────────────
    std::vector<Anchor> pending_;        // anchors placed for the armed tool
    uint64_t    drag_id_    = 0;
    int         drag_part_  = kPartNone;
    ImPlotPoint drag_start_{};           // plot-space mouse at drag start
    Drawing     drag_orig_{};            // snapshot the drag mutates from
    uint64_t    hover_id_   = 0;
    int         hover_part_ = kPartNone;

    // Measure (ephemeral — never stored): pending_[0..1] + frozen flag.
    bool measure_frozen_ = false;
    // Brush: a stroke is being captured into pending_.
    bool stroke_active_ = false;
    // Text: floating inline editor (placing new when text_edit_id_ == 0).
    bool     text_editor_open_ = false;
    bool     text_edit_focus_  = false;
    uint64_t text_edit_id_     = 0;
    Anchor   text_edit_anchor_{};
    char     text_edit_buf_[256] = {};

    ImPlotInputMap saved_map_{};
    bool map_overridden_ = false;
    bool captures_       = false;
};

}  // namespace drawing
