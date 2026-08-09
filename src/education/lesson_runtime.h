#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// lesson_runtime.h — Guided Replay engine (C++ / ImGui)
//
// Owns: parse LessonDoc JSON → Lesson; the gate loop (arm/fire steps as the replay
// playhead reaches them, clamped so a stutter can't skip the lesson); target→rect
// resolution; the spotlight overlay (dim + cut-out + ring) on the foreground draw
// list; a minimal debug coach card (text + Continue) until the React chrome lands.
//
// Drives off ReplayManager: progress = (interpolated_time - start) / (end - start).
// Reads the chart's per-frame ChartProjection to place the spotlight.
//
// V1 (this slice): self-contained, visible in the canvas. The C++↔JS control
// bridge + real React cards are the NEXT slice; this proves the loop + projection.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include "education/lesson_types.h"

class AppContext;       // fwd
class ReplayManager;    // fwd

namespace edu {

class LessonRuntime {
public:
    static LessonRuntime& instance() {
        static LessonRuntime inst;
        return inst;
    }

    // Parse the LessonDoc JSON (from EducationBoot). Returns false on parse error.
    bool load(const std::string& json);
    bool loaded() const { return lesson_.valid; }
    const Lesson& lesson() const { return lesson_; }

    // Per-frame. Reads replay progress, fires gates, advances tracking window.
    // Call after widgets update, before render_overlay. No-op until loaded.
    void update(const AppContext& ctx);

    // Per-frame. Draws spotlight + debug card on the foreground draw list.
    // Call late in the frame (after widgets render), before ImGui::Render.
    void render_overlay(const AppContext& ctx);

    // Continue from the current beat/quiz (resume replay → gate at next step).
    void continue_lesson(const AppContext& ctx);

    // ─── Deliberate navigation (driven by the React chrome via the JS bridge) ──
    // All three SEEK the replay (full re-buffer) and RE-ARM the gate so steps
    // fire correctly from the new position. shown_max_ ("furthest reached", for
    // the rail's completed ticks) is preserved by jump/back, reset by restart.
    void back(const AppContext& ctx);           // → previous step's anchor
    void restart(const AppContext& ctx);         // → step 1, clear progress
    void jump_to_step(int stepN, const AppContext& ctx);  // → step N (1-based)
    // Free scrub to an arbitrary fraction of the window (transport timeline drag/
    // click). Re-arms the step gate from the landing exactly like jump_to_step so
    // steps fire correctly as playback resumes. deliberate bypasses ReplayManager's
    // scrubber-drag debounce: false during a live drag, true on the drop/click.
    void scrub_to_progress(float progress, bool deliberate, const AppContext& ctx);

    // ─── C++ → JS state bridge ─────────────────────────────────────────────
    // Build the current lesson-state snapshot and, only when it has CHANGED in a
    // discrete field (phase / step / paused / speed), push it to JS:
    //   window.__EDGEDEPTH_LESSON_STATE__ = {...}; + CustomEvent('edgedepth:lesson').
    // The clock/progress ride along in each emit; React interpolates between
    // emits locally (paused+speed+clock+wall-time), so the scrubber stays smooth
    // without a per-frame boundary crossing. Call once per frame after update().
    void emit_state(const AppContext& ctx);

    // Accessors the bridge command callbacks need (they live at file scope).
    int  current_step() const { return cur_step_; }
    int  step_count()   const { return static_cast<int>(lesson_.steps.size()); }

    // ─── Export render mode (CLIP_FACTORY P3-v1) ──────────────────────────
    // While a Studio export records, the beat/quiz card must BURN INTO the
    // canvas (captureStream sees only the canvas), so render_overlay draws the
    // native ImGui card even in WASM — content only, NO Continue button (the
    // author advances from the React studio footer, which lives outside the
    // capture). Set by StudioRuntime for the export session's lifetime.
    void set_export_render(bool on) { export_render_ = on; }
    bool export_render() const { return export_render_; }
    // True while a beat/quiz card is up (drives the studio footer's Continue).
    bool card_up() const { return showing_card_; }
    // Tear the lesson down (export finished/aborted in studio): invalidates the
    // doc and clears gate/card/track/export state so the studio's normal free
    // scrub resumes without lesson gates firing.
    void unload();

    // ─── JS → C++ command queue ────────────────────────────────────────────
    // The KEEPALIVE'd bridge callbacks run from JS with no AppContext in hand,
    // but nav commands need one (seek/set_speed/resume). So they ENQUEUE a
    // command here; update(ctx) — which already has the ctx — drains it. This
    // keeps the education module from reaching into global app state.
    enum class Cmd : uint8_t { None, Continue, Back, Restart, Jump, Play, Pause, SetSpeed, Seek };
    void enqueue_cmd(Cmd c, int arg_i = 0, float arg_f = 0.0f);

private:
    LessonRuntime() = default;

    // gate machinery
    void open_step(int idx, const AppContext& ctx, bool pause);
    void start_track(const Step& s, const AppContext& ctx);
    void end_track();

    // target → screen rect (uses chart_projection + candle data via ctx).
    // `now_ms` is the current replay clock — used to grow track-follow boxes.
    struct Rect { float x, y, w, h; bool valid = false; };
    Rect resolve_target(const Step& s, const AppContext& ctx, int64_t now_ms) const;

    // current replay clock (unix ms) and helpers
    int64_t replay_now_ms(const AppContext& ctx) const;

    Lesson lesson_;
    int    armed_   = 0;          // next step index to fire
    int    cur_step_ = -1;        // currently shown step
    int    shown_max_ = -1;       // furthest step reached
    bool   showing_card_ = false; // a beat/quiz card is up
    // A beat/quiz wanted to pause but the replay wasn't Playing yet (e.g. the
    // intro beat anchored at the window start fires DURING Buffering, where
    // ReplayManager::pause() is a no-op). Latch the intent; update() enforces it
    // once the replay reaches Playing while the card is still up — so the intro
    // card opens paused even though it armed before the buffering gate promoted.
    bool   want_pause_ = false;
    int    tracking_ = -1;        // index of active track step, or -1
    float  prev_speed_ = 0.0f;    // restore speed after a slowTo track

    // Last spotlight rect (CSS px, viewport-relative) computed in render_overlay,
    // published in emit_state so the React floating card can pin itself beside the
    // spotlight. valid=false when no spotlight is up (or projection not ready).
    struct Box { float x = 0, y = 0, w = 0, h = 0; bool valid = false; } last_box_;

    // C++→JS emit dirty-check: a hash of the DISCRETE state fields (phase, step,
    // paused, speed). Clock/progress are excluded so we don't emit every frame.
    uint64_t emit_sig_ = 0;       // 0 = nothing emitted yet
    bool     steps_emitted_ = false;  // rail list resent on first emit / after load
    // Wall-clock (emscripten_get_now, ms) of the last clock-cadence emit. The
    // discrete gate alone emits once per "playing" stretch; without periodic re-
    // emits the React scrubber dead-reckons the clock open-loop and drifts ahead of
    // the trade tape on heavier windows. emit_state re-emits on a cadence while
    // playing so React re-anchors to interpolated_time_ms().
    double   last_clock_emit_ms_ = 0.0;

    // Monotonic floor for the REPORTED replay clock during a continuous playing
    // stretch — absorbs the small backward steps interpolated_time_ms() takes when
    // the backend status anchor lags the wall-time extrapolation (initial catch-up),
    // which otherwise snap the React clock 00:01→00:00. Large backward jumps (real
    // seeks/rewinds) pass through. See emit_state.
    int64_t  clock_floor_ms_ = 0;

    // Export render mode (see set_export_render). Never persists past an export.
    bool  export_render_ = false;

    // JS→C++ pending command (drained by update(ctx)). Single-slot: commands are
    // discrete user clicks, never bursty; last-write-wins is correct UX.
    Cmd   pending_cmd_   = Cmd::None;
    int   pending_arg_i_ = 0;
    float pending_arg_f_ = 0.0f;
    void  drain_cmd(const AppContext& ctx);
};

} // namespace edu
