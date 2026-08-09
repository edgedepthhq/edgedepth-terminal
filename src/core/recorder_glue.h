#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// recorder_glue.h - P1 "Share clip": in-browser replay recording (CLIP_FACTORY P1)
//
// canvas.captureStream(30) + MediaRecorder on the live WebGL2 canvas, driven from
// the NATIVE replay transport bar (replay mode only in v1 - the lesson/studio
// React transports get their own path in P3-v1; they never render the native bar,
// so they don't inherit this and must NOT be special-cased here).
//
// ALL JS lives in recorder_glue.cpp as EM_JS (no --js-library plumbing).
// JS→C++ is ONE KEEPALIVE'd export, _recorder_on_state(int state, double bytes),
// listed in CMakeLists EXPORTED_FUNCTIONS as __recorder_on_state - the C++ state
// below mirrors what the browser ACTUALLY did, so the button reflects reality.
//
// Output: edgedepth_{symbol}_{yyyymmdd_hhmm}.{webm|mp4} - the timestamp is the
// REPLAY DATA position (UTC, matches the burned watermark), never wall clock;
// the extension matches the negotiated container from the codec fallback chain
// (vp9 → vp8 → webm → mp4). While recording, tick_and_render() burns the
// watermark badge into the capture via the ImGui FOREGROUND draw list.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>

namespace ClipRecorder {

// Mirrors the JS-side ints reported through _recorder_on_state().
enum class State : int {
    Idle        = 0,   // ready to record (probe passed)
    Recording   = 1,   // MediaRecorder running, chunks accumulating
    Stopped     = 2,   // transient: stop fired, blob assembly/download in flight
    Error       = 3,   // MediaRecorder error - decays back to Idle after ~4s
    Unsupported = 4,   // probe failed: no MediaRecorder/captureStream/codec
};

// Hard clip cap. The C++ timer in tick_and_render() is AUTHORITATIVE (drives the
// UI and calls stop()); the JS glue arms a belt-and-braces safety timer slightly
// past this in case the wasm side ever stalls. Memory is bounded by the cap
// (~180MB worst case at 8Mbps).
inline constexpr double kMaxClipMs = 180000.0;  // 3:00

void   probe_support();   // once at boot - MediaRecorder.isTypeSupported chain
bool   supported();       // false → render the record button disabled
State  state();
bool   is_recording();
double elapsed_ms();      // monotonic (emscripten_get_now) since start; 0 idle
double bytes();           // bytes delivered so far (ondataavailable counter)

// ── Focus layout ─────────────────────────────────────────────────────────────
// While recording (default ON) the terminal hides the app shell (topbar/statsbar/
// status bar), the watchlist and the perf overlay, so clips frame chart+DOM+tape
// only; everything restores when the recording ends. Toggled from the FOCUS pill
// on the transport; locked while a recording is running (no mid-clip layout jump).
bool focus_enabled();
void set_focus_enabled(bool on);
// True when the focus layout must be applied THIS frame. Includes the short
// pending window between ⏺ and the deferred JS start (see start()) so the first
// captured composite is already chrome-free - no one-frame topbar flash on clips.
bool focus_active();

// Start a clip. symbol_upper = display symbol (e.g. "BTCUSDT"); replay_data_ms =
// current replay DATA time (UTC epoch ms) - stamps the watermark line AND the
// download filename. No-op while a recording or download is in flight.
// With focus enabled, the actual MediaRecorder start is DEFERRED two frames:
// the click lands after this frame's chrome already rendered, so we hide the
// chrome first (focus_active() flips immediately) and start capturing only once
// a clean frame has been composited.
void start(const char* symbol_upper, int64_t replay_data_ms);

// Stop + download. The 3:00 auto-stop takes EXACTLY this path.
void stop();

// Per-frame, between NewFrame and Render: enforces the cap, ends the clip if the
// replay session died, decays Error→Idle, and draws the REC watermark badge on
// the foreground draw list (always-on-top → burned into the capture).
// NO-OP while an export session runs (export_tick_and_render owns that mode).
void tick_and_render(bool replay_active);

// ═══════════════════════════════════════════════════════════════════════════
// Export mode - CLIP_FACTORY P3-v1: Studio "Export video" (lesson → mp4/webm).
//
// SAME recorder, second entry point: realtime capture of lesson playback in
// /studio, driven by StudioRuntime (which owns WHEN to start/stop - this module
// owns HOW). Differences from the P1 share-clip path, all deliberate:
//   · AUDIO - a live narration take: React puts {ctx: AudioContext, stream:
//     getUserMedia(mic+cam)} on window.__EDGEDEPTH_EXPORT_MEDIA__ before the
//     export command; the JS glue wires mic → MediaStreamAudioDestinationNode →
//     addTrack() onto the canvas captureStream, and re-negotiates the container
//     WITH audio codecs (vp9,opus → vp8,opus → webm → mp4, still via
//     isTypeSupported, probed per-export into its own slot - the P1 boot probe
//     is untouched). Missing/denied media ⇒ silent video export, still valid.
//   · CAM BUBBLE - the cam track feeds a hidden <video>, uploaded per-frame into
//     a GL texture and drawn as a circle bottom-right on the FOREGROUND draw
//     list, so captureStream sees it (anything React-side is invisible to the
//     recording - in-render compositing is the whole point).
//   · CAP - kMaxExportMs (15:00) instead of 3:00; a lesson runs its length.
//   · BADGE - the EDGEDEPTH · SYM · REPLAY date line stays (brand watermark),
//     the REC dot + elapsed timer are DROPPED: this is a produced video, not a
//     live share-clip.
//   · FILENAME - edgedepth_{lesson_slug}_{yyyymmdd_hhmm}.{ext}; timestamp is the
//     lesson window START (replay DATA date, UTC) - wall clock never appears.
//
// P1 interlock: one MediaRecorder slot. export_start() no-ops while a P1 clip
// records and vice versa (shared JS st.rec guard + shared State). tick_and_render
// (P1) returns immediately in export mode.
// ═══════════════════════════════════════════════════════════════════════════

inline constexpr double kMaxExportMs = 900000.0;  // 15:00 ceiling (sane, not 3:00)

// True from export_start() until the download/error settles back to Idle.
bool export_active();

// Begin the export capture NOW (StudioRuntime calls this once the lesson replay
// is primed and playing/on its intro card - there is no chrome to hide in the
// embedded studio, so no deferred-start dance is needed).
//   slug          - lesson slug for the filename (sanitized here to [a-z0-9-])
//   symbol_upper  - display symbol for the badge line (e.g. "BTCUSDT")
//   lesson_start_ms - lesson window start (unix ms, UTC) → filename + badge date
void export_start(const char* slug, const char* symbol_upper, int64_t lesson_start_ms);

// Stop + download (identical blob path as P1's ⏹). The 15:00 cap and a dead
// replay session take exactly this path too.
void export_stop();

// Release a stashed-but-unconsumed window.__EDGEDEPTH_EXPORT_MEDIA__ (mic/cam
// tracks + AudioContext) - the export was cancelled/refused BEFORE the capture
// consumed it, and without this the tab's mic indicator stays lit. Safe no-op
// while a capture is running (the glue's own cleanup owns it from then on).
void export_release_media();

// Elapsed wall ms of the running export capture (0 when not recording).
double export_ms();

// True when the negotiated recording actually carries an audio track.
bool export_has_audio();

// Per-frame in studio mode, between NewFrame and Render: enforces the 15:00 cap,
// stops on a dead session, decays Error→Idle, draws the badge line (no REC dot /
// timer) and composites the cam bubble into the canvas. No-op unless in export
// mode. session_alive = replay session still active.
void export_tick_and_render(bool session_alive);

}  // namespace ClipRecorder
