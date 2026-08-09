#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// event_runtime.h — Archive-event replay runtime (C++ side).
//
// The archived market-event replay (public /events → /terminal?event=<id>) is the
// SAME embedded WASM terminal as the lesson/studio player, driven by a React chrome
// (EventReplayShell) that OWNS the shell + transport. Unlike the lesson there is no
// doc + no gate loop, and unlike studio no source picker — the event id + symbol +
// window arrive via window.__EDGEDEPTH_EVENT__ (read by EducationBoot), and the
// archive replay is auto-started by main.cpp maybe_start_event_replay().
//
// This runtime is the event analogue of StudioRuntime: a transport MIRROR. It
//   (1) EMITS the archive session's transport state (clock/progress/paused/speed/
//       window/loading) to the React chrome via CustomEvent('edgedepth:event'), and
//   (2) DRAINS the React transport commands (pause/play/speed/seek/skip) onto the
//       shared ReplayManager.
// No steps live here — the key_moments rail is built entirely on the web from the
// EventRecord. Mirrors transport_emit.h mechanics (discrete dirty-check + clock
// cadence + monotonic floor), exactly like lesson/studio.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>

class AppContext; // fwd

namespace edu {

class EventRuntime {
public:
    static EventRuntime& instance() {
        static EventRuntime inst;
        return inst;
    }

    // Per-frame, from main.cpp's event branch (after the terminal is initialized).
    // Drains a pending transport command onto the live archive replay. No-op until
    // a command is queued. Safe to call every frame.
    void update(const AppContext& ctx);

    // Per-frame, after update(). Pushes transport state to the React chrome via a
    // CustomEvent('edgedepth:event'). Dirty-checked on the DISCRETE fields (clock/
    // progress excluded) so it emits on transitions + a clock cadence, not every
    // frame — React interpolates the clock between emits, like the lesson player.
    void emit_state(const AppContext& ctx);

    // JS → C++ transport commands. Enqueued from the bridge exports (which have no
    // AppContext) and drained in update(ctx). Speed crosses as centi-speed (int,
    // 400 = 4.0×) and seek as milli-progress (int, 0..1000) to dodge the f64→f32
    // export mis-marshal that bit the lesson speed command.
    void cmd_set_paused(bool paused);
    void cmd_set_speed(int centi_speed);
    // deliberate bypasses ReplayManager's scrubber-drag debounce: false for a live
    // drag (let the debounce throttle), true for a discrete drop/click/step jump.
    void cmd_seek_progress(int milli_progress, bool deliberate);
    // Relative nudge from the current position, in seconds (signed: negative = back).
    void cmd_skip(int seconds);
    // "Watch again": if the session ENDED (Stopped — the replay data context is
    // torn down), pack mode re-requests the pack (fresh engine + context, the
    // same boot main.cpp did); a still-active session just seeks to the window
    // start. Box event mode falls back to the seek path either way.
    void cmd_restart();

private:
    EventRuntime() = default;

    // FNV-1a signature of the last emitted discrete state — skip re-emitting when
    // nothing discrete changed (clock/progress excluded; React interpolates).
    uint64_t emit_sig_ = 0;
    // Wall-clock (emscripten_get_now, ms) of the last clock-cadence emit, so a long
    // steady "playing" stretch still re-anchors React to interpolated_time_ms().
    double   last_clock_emit_ms_ = 0.0;
    // Monotonic floor for the REPORTED clock during a continuous playing stretch —
    // absorbs the small backward steps interpolated_time_ms() takes on catch-up.
    int64_t  clock_floor_ms_ = 0;

    // Pending transport command, drained in update(ctx). Last-write-wins.
    enum class TransportCmd { None, Pause, Play, SetSpeed };
    TransportCmd pending_transport_ = TransportCmd::None;
    float        pending_speed_ = 1.0f;

    // Seek/skip carry distinct payloads, so they get their own latches rather than
    // sharing the pause/speed slot (a seek must not clobber a queued pause).
    bool  pending_seek_ = false;
    float pending_seek_progress_ = 0.0f;   // 0..1
    bool  pending_seek_deliberate_ = false;
    bool  pending_skip_ = false;
    int   pending_skip_seconds_ = 0;       // signed: negative = backward
    bool  pending_restart_ = false;        // "watch again" (see cmd_restart)

    // One-shot deep-link seek (?event=…&t=…, EducationBoot::event_seek_to_ms):
    // applied in update() the first time the session is live (active && !loading),
    // so the seek lands on a primed/seeded book — then never again (the user owns
    // the transport from that point). See CLIP_FACTORY_PLAN Appendix A #5.
    bool  deep_link_seek_done_ = false;
};

} // namespace edu
