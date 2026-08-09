#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// studio_runtime.h - Course Authoring Studio runtime (C++ side)
//
// The studio is the SAME embedded WASM terminal as the Guided Replay player, in
// an AUTHOR mode: free scrub (no gate loop), no dim/card, and (next sub-step) a
// draw overlay that captures chart-space target geometry back to the React
// inspector. Unlike LessonRuntime there is NO lesson doc - the replay window is
// chosen in the React source picker and pushed in here as a JSON command.
//
// This file is the studio's analogue of LessonRuntime: a singleton driven once
// per frame from main.cpp's embedded branch (when EducationBoot::is_studio()).
//
// THIS SLICE: the source→replay path only. The author picks a symbol/window in
// React; React calls __studio_cmd_set_source(jsonPtr); we enqueue it; drain()
// (with a live AppContext) parses it and starts the replay via request_replay -
// the exact call LessonRuntime's maybe_start_lesson_replay uses, just sourced
// from the picker instead of a doc. The draw/capture overlay lands next.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <cstdint>

class AppContext; // fwd

namespace edu {

class StudioRuntime {
public:
    static StudioRuntime& instance() {
        static StudioRuntime inst;
        return inst;
    }

    // Per-frame, from main.cpp's studio branch (after the terminal is initialized).
    // Drains a pending source command into request_replay. No-op until a source is
    // set. Safe to call every frame.
    void update(const AppContext& ctx);

    // Per-frame, after update(). Pushes transport state (clock/progress/paused/
    // speed/window/loading) to the React chrome via a CustomEvent('edgedepth:studio').
    // Dirty-checked on the DISCRETE fields (clock/progress excluded) so it emits on
    // transitions, not every frame - React interpolates the clock between emits,
    // exactly like the lesson player. Mirrors LessonRuntime::emit_state but far
    // simpler: no steps/cards/spotlight, just transport.
    void emit_state(const AppContext& ctx);

    // JS → C++: the picker chose a source. The source JSON
    //   {"symbol":"btcusdt","startMs":<ms>,"endMs":<ms>,"tf":"5m"}
    // crosses via the window global __STUDIO_SOURCE__ (read inside the export's
    // EM_ASM, same seam as EducationBoot's __EDGEDEPTH_LESSON__) - so no malloc/
    // stringToUTF8 needs exporting to external JS. This setter is called from that
    // export with the parsed string. Enqueued (not applied here): update(ctx)
    // drains it with a live context, the same enqueue/drain split LessonRuntime
    // uses so the runtime never reaches into global app state from a bridge
    // callback. Last-write-wins (re-picking before the replay starts is fine).
    void set_source_json(const std::string& json);

    // JS → C++ transport commands. Like set_source, these are enqueued from the
    // bridge export (which has no AppContext) and drained in update(ctx). Speed
    // crosses as centi-speed (int, 400 = 4.0×) to dodge the float mis-marshal that
    // bit the lesson speed command.
    void cmd_set_paused(bool paused);
    void cmd_set_speed(int centi_speed);
    // Seek to a fraction of the window. Progress crosses as milli-progress (int,
    // 0..1000 = 0.0..1.0) - same int-not-float reasoning as speed. `deliberate`
    // bypasses ReplayManager's scrubber-drag debounce: pass false for a live drag
    // (let the debounce throttle the stream), true for a discrete drop/click.
    void cmd_seek_progress(int milli_progress, bool deliberate);
    // Relative nudge from the current position, in seconds (signed: negative =
    // backward). Always deliberate (discrete button press).
    void cmd_skip(int seconds);

    // True once a source has been applied (a replay has been requested). The draw
    // overlay (next slice) only arms once this is true.
    bool has_source() const { return has_source_; }

    // ─── Draw/capture overlay (studio phase 1) ──────────────────────────────
    // React arms ONE capture gesture (__studio_cmd_arm_capture): the author drags
    // a box on the chart; we inverse-project the pixel rect via chart_projection's
    // time_of_x / price_of_y and dispatch a one-shot
    // CustomEvent('edgedepth:capture', {mode,t0,t1,pTop,pBot}) back to the React
    // inspector, then disarm. mode: 0 = disarm, 1 = region box, 2 = price band.
    // Esc cancels (emits {cancelled:true} so the inspector's arm state resets).
    void cmd_arm_capture(int mode);
    // Per-frame, from main.cpp's studio branch (after update, skipped while an
    // export take runs - a capture overlay mid-take would burn into the video).
    void render_capture_overlay(const AppContext& ctx);
    bool capture_armed() const { return capture_mode_ != 0; }

    // ─── Export session (CLIP_FACTORY P3-v1 - Studio "Export video") ───────
    // A live-take export: the selected lesson's doc is loaded into LessonRuntime
    // (export-render mode: gates + spotlight + BURNED-IN cards, no button), the
    // replay is scrubbed to the lesson start, and once the window is primed and
    // rolling the recorder starts (canvas 30fps + the narration mic through the
    // WebAudio graph + the cam bubble composited in-render). The author narrates
    // live and advances beats from the React footer (__edu_cmd_continue - outside
    // the capture). Ends on: lesson end (no card up) · footer Stop · 15:00 cap ·
    // session death. StudioRuntime owns WHEN; ClipRecorder owns HOW.
    enum class ExportPhase : uint8_t { Idle, Arming, Recording, Saving, Error };

    // JS → C++ (KEEPALIVE'd at file scope): lesson JSON + slug cross via the
    // window globals __STUDIO_EXPORT_LESSON__ / __STUDIO_EXPORT_SLUG__ (same
    // seam as __STUDIO_SOURCE__). Enqueued; update(ctx) drains with a live ctx.
    void set_export_json(std::string lesson_json, std::string slug);
    void cmd_export_stop();

    bool export_session_active() const { return export_phase_ != ExportPhase::Idle; }

private:
    StudioRuntime() = default;

    // Export drain/tick helpers (called from update(ctx)).
    void drain_export_start(const AppContext& ctx);
    void tick_export(const AppContext& ctx);
    void end_export_session();   // unload lesson + drop export-render, phase→Idle

    std::string pending_source_;   // raw JSON, empty = nothing pending
    bool        has_pending_ = false;
    bool        has_source_  = false;
    // Guard against re-requesting the identical window every frame; only re-replay
    // when the source actually changes.
    std::string applied_key_;      // "sym|start|end" of the last applied source

    // The applied source, kept for emit_state so it can report the window/symbol/tf
    // to React even before SessionInfo is fully populated (during buffering).
    std::string src_symbol_;
    std::string src_tf_;
    int64_t     src_start_ms_ = 0;
    int64_t     src_end_ms_   = 0;

    // FNV-1a signature of the last emitted discrete state - skip re-emitting when
    // nothing discrete changed (clock/progress excluded; React interpolates).
    uint64_t    emit_sig_ = 0;

    // Wall-clock (emscripten_get_now, ms) of the last clock-cadence emit. The
    // discrete-field gate above would emit ONCE when playback starts and then never
    // again during a steady "playing" stretch - leaving the React transport to
    // dead-reckon the clock open-loop (clockMs + wall×speed) with no correction,
    // which let the studio UTC readout drift ahead of the trade tape. emit_state
    // re-emits the clock on a fixed cadence while playing so React re-anchors to
    // interpolated_time_ms() - the self-correcting value the live control bar shows.
    double      last_clock_emit_ms_ = 0.0;

    // Monotonic floor for the REPORTED replay clock during a continuous playing
    // stretch. interpolated_time_ms() can step backward when the backend status
    // anchor momentarily lags the wall-time extrapolation (notably the initial
    // catch-up), and the 250ms re-emit turns that into a visible 00:01→00:00
    // snap-back loop on the React clock. We hold the max across emits, but allow a
    // LARGE backward jump (a real seek/rewind) through - see emit_state.
    int64_t     clock_floor_ms_ = 0;

    // ── Draw/capture state ──────────────────────────────────────────────────
    // 0 = disarmed, 1 = region, 2 = band. Set directly from the bridge export
    // (single-threaded main loop; no ctx needed, unlike the transport commands).
    int   capture_mode_ = 0;
    bool  capture_dragging_ = false;
    float capture_x0_ = 0.0f;   // drag anchor, screen px
    float capture_y0_ = 0.0f;

    // Pending transport command, drained in update(ctx). Only the latest matters
    // (last-write-wins): pausing then immediately playing should land on play.
    enum class TransportCmd { None, Pause, Play, SetSpeed };
    TransportCmd pending_transport_ = TransportCmd::None;
    float        pending_speed_ = 1.0f;

    // Seek/skip carry distinct payloads, so they get their own latches rather than
    // sharing the pause/speed slot (a seek must not clobber a queued pause).
    bool         pending_seek_ = false;
    float        pending_seek_progress_ = 0.0f;   // 0..1
    bool         pending_seek_deliberate_ = false;
    bool         pending_skip_ = false;
    int          pending_skip_seconds_ = 0;       // signed: negative = backward

    // ── Export session state ────────────────────────────────────────────────
    ExportPhase  export_phase_ = ExportPhase::Idle;
    // Pending export command (raw doc JSON + slug), drained in update(ctx).
    std::string  pending_export_json_;
    std::string  pending_export_slug_;
    bool         has_pending_export_ = false;
    bool         pending_export_stop_ = false;
    // The running session's identity (filename slug, badge symbol, window).
    std::string  export_slug_;
    std::string  export_sym_upper_;
    int64_t      export_start_ms_ = 0;
    int64_t      export_end_ms_   = 0;
    // Wall clock (emscripten_get_now) when Arming began - buffer-stall bailout.
    double       arming_since_ms_ = 0.0;
    // Wall clock when Error was entered (decay back to Idle so the UI recovers).
    double       export_error_ms_ = 0.0;
};

} // namespace edu
