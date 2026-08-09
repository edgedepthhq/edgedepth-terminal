#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// recorder_runtime.h — CLIP_FACTORY P2: scripted clip-recorder driver.
//
// A clip is a headless lesson. The render harness (Xvfb + Chromium + ffmpeg
// x11grab) boots the terminal in an EXISTING replay mode — archived event
// (window.__EDGEDEPTH_EVENT__) or CDN pack (window.__EDGEDEPTH_PACK__ with
// embedded:true) — and ADDITIONALLY injects window.__EDGEDEPTH_RECORDER__ =
// <RecorderScript> before the glue loads. The recorder is NOT a fourth session
// mode: it is an orthogonal DRIVER layered over whichever session booted. It
//   (1) walks the script's shots[] against the shared ReplayManager
//       (skip/seek + set_speed + timeframe + pause/hold),
//   (2) EMITS its progress to the harness via CustomEvent('edgedepth:recorder')
//       (+ window.__EDGEDEPTH_RECORDER_STATE__), transport_emit.h mechanics, and
//   (3) burns the EDGEDEPTH watermark badge into the canvas (export-badge
//       style: line only, no REC dot — this is a produced video).
//
// Session boot, symbol routing, the focused widget set and chrome suppression
// all reuse the event/pack paths; main.cpp folds active() into its rec_focus
// const so the app shell / status bar / watchlist / perf overlay stay out of
// frame. No new EXPORTED_FUNCTIONS: the script rides a window global (read by
// EducationBoot::detect), state rides a CustomEvent, and the runtime never
// needs a JS→C++ command (the script IS the transport).
//
// ── RecorderScript v1 (wire shape — script-gen emits this) ────────────────────
// Times are epoch ms ints (never ISO strings), tf is chart timeframe SECONDS
// (the __set_chart_timeframe unit). Each shot has an EXPLICIT end (untilMs).
// {
//   "v": 1,
//   "symbol":  "taikousdt",       // badge text (session symbol comes from the
//                                 // event/pack global, not from here)
//   "eventId": "taikousdt-short-squeeze--20260701_183655",   // echoed in state
//   "speedScale": 1.0,            // determinism trick: 0.25–0.5 captures slow
//                                 // under SwiftShader, post retimes to 60fps.
//                                 // Multiplies every shot speed; effective
//                                 // speed clamps to ReplayManager's [0.1, 10].
//   "shots": [
//     { "fromMs":  1751352300000, // shot start, replay-DATA unix ms
//       "untilMs": 1751358600000, // shot end: clock >= untilMs → next shot
//       "speed":   10.0,          // scripted playback speed (pre-scale)
//       "tf":      300,           // chart timeframe seconds (0 = keep current)
//       "holdMs":  0,             // freeze-frame at shot end (wall ms, 0 = no)
//       "focus":   "dom",         // ""|"dom"|"tape"|"chart" — lightbox the
//                                 // panel for this shot (lesson-style dim +
//                                 // accent ring, burned in-render); the panel
//                                 // rect also rides state as focusRect so post
//                                 // can crop-zoom deterministically instead
//       "caption": "…" }, …       // echoed in state; post burns the .ass
//   ]
// }
//
// Shot transitions: forward jumps use ReplayManager::skip_forward_to (clock-
// only, instant, formed candles stay on the chart — visual continuity);
// backward jumps fall back to ONE deliberate seek (full re-prime; well-formed
// scripts never need it). After the last shot the replay is PAUSED and phase
// "done" is emitted — the harness stops capture; the end card is a post step.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

class AppContext;  // fwd

namespace edu {

class RecorderRuntime {
public:
    static RecorderRuntime& instance() {
        static RecorderRuntime inst;
        return inst;
    }

    // Optional script-level view overrides: layer state IS the clip's visual
    // grammar, so scripts pin it explicitly instead of inheriting whatever
    // the build's defaults are. -1 = leave alone, 0/1 = force off/on.
    // Wire shape (all keys optional):
    //   "view": { "liqField": true, "liqLevels": false, "liqProfile": true,
    //             "liqObserved": true, "obDepth": true, "vpvr": false }
    struct ViewOverrides {
        int liq_field = -1, liq_levels = -1, liq_profile = -1,
            liq_observed = -1, ob_depth = -1, vpvr = -1;
        // "viewSpanMinutes": pin the chart's visible X span to the last N
        // minutes behind the playhead, every frame while the recorder runs.
        // Deterministic framing for produced clips: the post-seek batch fit
        // depends on how much history the seed served, which is exactly what
        // a scripted shot must not inherit. <=0 = leave the chart alone.
        int view_span_min = -1;
    };
    // Non-null once a script with a "view" object has loaded.
    const ViewOverrides* view() const { return (valid_ && has_view_) ? &view_ : nullptr; }

    // Parse the RecorderScript JSON (stashed by EducationBoot::detect from the
    // window global). Returns false on parse/validation error — the runtime
    // then stays inert (active() == false) and the terminal behaves as a plain
    // event/pack replay.
    bool load(const std::string& json);

    // True once a script has loaded — main.cpp gates the per-frame calls AND
    // folds this into rec_focus (chrome suppression) on it.
    bool active() const { return valid_; }

    // True only while the capture-relevant frames run (shot 0 onward, incl.
    // holds + the done freeze). Chart framing pins gate on THIS, not active():
    // during boot/seek/prime the chart must behave exactly like a plain pack
    // replay or the initial candle batch races the pinned window.
    bool in_shot() const {
        return valid_ && (phase_ == Phase::Shot || phase_ == Phase::Hold ||
                          phase_ == Phase::Done);
    }

    // Per-frame, from the main loop (after the widgets update, alongside the
    // education runtimes). Drives the shot state machine against ReplayManager.
    void update(const AppContext& ctx);

    // Per-frame, after update(). Pushes recorder state to the harness via
    // CustomEvent('edgedepth:recorder') — transport_emit.h mechanics (discrete
    // dirty-check + clock cadence while a shot plays).
    void emit_state(const AppContext& ctx);

    // Per-frame, late (foreground draw list). Burns the EDGEDEPTH badge from
    // the first shot onward (incl. the done freeze-frame). No REC dot.
    void render_overlay(const AppContext& ctx);

private:
    RecorderRuntime() = default;

    struct Shot {
        int64_t from_ms  = 0;   // replay-data unix ms
        int64_t until_ms = 0;   // shot ends when clock >= until_ms
        float   speed    = 1.0f;
        int     tf_sec   = 0;   // 0 = keep current timeframe
        int     hold_ms  = 0;   // freeze-frame after the shot (wall ms)
        std::string focus;      // ""|"dom"|"tape"|"chart" — lightbox the panel
        std::string caption;    // echoed in state; post burns the .ass
    };

    // Phase, in emit order. Waiting = session not yet primed (buffering /
    // seeking); Shot/Hold = the clip is being captured; Done = freeze-frame,
    // harness stops; Error = session died, harness bails.
    enum class Phase : uint8_t { Waiting, Shot, Hold, Done, Error };
    static const char* phase_name(Phase p);

    void begin_shot(int i, const AppContext& ctx);
    void advance(const AppContext& ctx);

    // Script
    bool        valid_ = false;
    std::string symbol_;        // lowercase, badge/source-of-truth for text
    std::string event_id_;
    float       speed_scale_ = 1.0f;
    std::vector<Shot> shots_;
    bool          has_view_ = false;
    ViewOverrides view_;

    // State machine
    Phase  phase_ = Phase::Waiting;
    int    cur_   = -1;          // current shot index (-1 before shot 0)
    float  eff_speed_ = 1.0f;    // clamped speed actually applied (for post)
    double hold_until_wall_ = 0.0;
    double shot_start_wall_ = 0.0;   // overshoot guard: a shot may not end
    double shot_min_wall_ms_ = 0.0;  // before ~3/4 of its expected wall time

    // Watermark badge, built once at load() (P1 rule: replay-DATA date, UTC).
    char badge_[96] = {0};

    // Last resolved focus rect (CSS px), published in state as focusRect so
    // post can crop-zoom against exactly what the spotlight framed.
    struct FocusBox { float x = 0, y = 0, w = 0, h = 0; bool valid = false; } last_focus_;

    // transport_emit mechanics
    uint64_t emit_sig_ = 0;
    double   last_clock_emit_ms_ = 0.0;
    int64_t  clock_floor_ms_ = 0;
};

}  // namespace edu
