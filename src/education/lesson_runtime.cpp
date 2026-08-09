// ═══════════════════════════════════════════════════════════════════════════════
// lesson_runtime.cpp - see lesson_runtime.h. Absolute-time gate loop + spotlight.
// ═══════════════════════════════════════════════════════════════════════════════

// Must precede ANY include that pulls in imgui.h (chart_projection.h does).
#define IMGUI_DEFINE_MATH_OPERATORS

#include "education/lesson_runtime.h"
#include "education/chart_projection.h"
#include "education/transport_emit.h"
#include "core/app_context.h"
#include "core/candle_manager.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

// Defined at GLOBAL scope in main.cpp - true while the replay clock is held for
// priming. Declared here at global scope so emit_state references ::g_lesson_loading
// (a block-scope extern inside an edu:: member binds to edu::, which is wrong).
extern bool g_lesson_loading;

namespace edu {

// ─── parse ──────────────────────────────────────────────────────────────────

static Target parse_target(const json& t) {
    Target out;
    if (!t.is_object()) return out;
    const std::string type = t.value("type", "");
    if (type == "el") {
        out.type = Target::Type::El;
        out.el = t.value("el", "");
    } else if (type == "band") {
        out.type = Target::Type::Band;
        out.t = t.value("t", int64_t{0});
        out.below = t.value("below", 0);
        // Authored bands carry explicit price bounds (studio capture); legacy bands
        // carry only `below`. hasPrice gates which path resolve_target takes.
        if (t.contains("pTop") && t.contains("pBot")) {
            out.pTop = t.value("pTop", 0.0);
            out.pBot = t.value("pBot", 0.0);
            out.hasPrice = true;
        }
    } else if (type == "full") {
        out.type = Target::Type::Full;
    } else { // "region" (also the shape used for follow)
        out.type = Target::Type::Region;
        out.t0 = t.value("t0", int64_t{0});
        out.t1 = t.value("t1", int64_t{0});
        if (t.contains("pTop") && t.contains("pBot")) {
            out.pTop = t.value("pTop", 0.0);
            out.pBot = t.value("pBot", 0.0);
            out.hasPrice = true;
        }
        // A region with no explicit time bounds on a track = "follow".
        out.follow = (!t.contains("t0") && !t.contains("t1"));
    }
    return out;
}

bool LessonRuntime::load(const std::string& jsonStr) {
    lesson_ = Lesson{};
    armed_ = 0; cur_step_ = -1; shown_max_ = -1;
    showing_card_ = false; tracking_ = -1;
    emit_sig_ = 0; steps_emitted_ = false;
    pending_cmd_ = Cmd::None; pending_arg_i_ = 0; pending_arg_f_ = 0.0f;

    json doc;
    try {
        doc = json::parse(jsonStr);
    } catch (const json::exception& e) {
        return false;
    }

    lesson_.title   = doc.value("title", "");
    lesson_.course  = doc.value("course", "");
    lesson_.chapter = doc.value("chapter", "");
    lesson_.description = doc.value("description", "");
    if (doc.contains("source") && doc["source"].is_object()) {
        const auto& s = doc["source"];
        lesson_.source.symbol  = s.value("symbol", "");
        lesson_.source.venue   = s.value("venue", "");
        lesson_.source.event   = s.value("event", "");
        lesson_.source.startMs = s.value("startMs", int64_t{0});
        lesson_.source.endMs   = s.value("endMs", int64_t{0});
        lesson_.source.tf      = s.value("tf", "");
    }

    if (!doc.contains("steps") || !doc["steps"].is_array()) {
        return false;
    }

    for (const auto& js : doc["steps"]) {
        Step st;
        const std::string kind = js.value("kind", "beat");
        if (kind == "track")      st.kind = Kind::Track;
        else if (kind == "quiz")  st.kind = Kind::Quiz;
        else                      st.kind = Kind::Beat;

        st.title     = js.value("title", "");
        st.kicker    = js.value("kicker", "");
        st.body      = js.value("body", "");
        st.indicator = js.value("indicator", "");
        if (js.contains("target")) st.target = parse_target(js["target"]);

        if (st.kind == Kind::Track) {
            st.t0 = js.value("t0", int64_t{0});
            st.t1 = js.value("t1", int64_t{0});
            st.t  = st.t0;  // sort key
            st.slowTo = js.value("slowTo", 0.0f);
        } else {
            st.t = js.value("t", int64_t{0});
            st.pause = js.value("pause", true);
        }
        // Scrub-forward lock (schema v0.2.x, additive). Quiz default = locked.
        st.lockForward = js.value("lockForward", st.kind == Kind::Quiz);

        if (st.kind == Kind::Quiz && js.contains("options") && js["options"].is_array()) {
            for (const auto& jo : js["options"]) {
                QuizOption o;
                o.text    = jo.value("t", "");
                o.correct = jo.value("correct", false);
                o.why     = jo.value("why", "");
                st.options.push_back(std::move(o));
            }
        }
        lesson_.steps.push_back(std::move(st));
    }

    std::sort(lesson_.steps.begin(), lesson_.steps.end(),
              [](const Step& a, const Step& b) { return a.t < b.t; });
    for (size_t k = 0; k < lesson_.steps.size(); ++k)
        lesson_.steps[k].n = static_cast<int>(k) + 1;

    lesson_.valid = true;

    return true;
}

// Inverse of load() - used when a Studio export session ends so the lesson's
// gate loop stops firing under the studio's normal free scrub. Mirrors load()'s
// reset block exactly, plus invalidates the doc and drops export-render mode.
void LessonRuntime::unload() {
    lesson_ = Lesson{};
    armed_ = 0; cur_step_ = -1; shown_max_ = -1;
    showing_card_ = false; want_pause_ = false; tracking_ = -1;
    prev_speed_ = 0.0f;
    last_box_ = Box{};
    emit_sig_ = 0; steps_emitted_ = false;
    pending_cmd_ = Cmd::None; pending_arg_i_ = 0; pending_arg_f_ = 0.0f;
    export_render_ = false;
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// gate loop (absolute time)
// ═══════════════════════════════════════════════════════════════════════════════

namespace edu {

int64_t LessonRuntime::replay_now_ms(const AppContext& ctx) const {
    return ctx.replay_mgr().interpolated_time_ms();
}

void LessonRuntime::update(const AppContext& ctx) {
    if (!lesson_.valid) return;

    // Apply any command the JS bridge enqueued since last frame (needs ctx).
    drain_cmd(ctx);

    if (!ctx.replay_mgr().is_active()) return;     // replay not joined yet

    // Enforce a deferred pause: a beat that fired during Buffering couldn't pause
    // (pause() is a no-op off Playing). Once playback actually starts with the
    // card still up, pause it now. Cleared the moment it takes (or on continue).
    if (want_pause_) {
        if (showing_card_ && ctx.replay_mgr().is_playing()) {
            ctx.replay_mgr().pause();
            want_pause_ = false;
        } else if (!showing_card_ || ctx.replay_mgr().is_paused()) {
            want_pause_ = false;  // no longer needed
        }
    }

    const int64_t now = replay_now_ms(ctx);
    if (now <= 0) return;

    // Scrub-forward lock: while a lockForward step is ACTIVE (its card is up,
    // or its track window is running), forward seeks and the ghost preview
    // clamp to the playhead. Re-pushed EVERY frame - the ceiling self-expires
    // in ReplayManager when these pushes stop (lesson unloaded/torn down), so
    // free scrub always returns on its own. Backward motion stays free.
    {
        const Step* locked = nullptr;
        if (showing_card_ && cur_step_ >= 0 &&
            cur_step_ < static_cast<int>(lesson_.steps.size())) {
            locked = &lesson_.steps[cur_step_];
        } else if (tracking_ >= 0) {
            locked = &lesson_.steps[tracking_];
        }
        ctx.replay_mgr().set_seek_ceiling_ms(
            (locked && locked->lockForward) ? now : 0);
    }

    // Gate: fire the next armed step when the replay clock reaches its time.
    // ReplayManager owns the clock (and clamps its own advance), so a stutter
    // can't skip - we advance armed_ one step at a time and test in order.
    if (armed_ < static_cast<int>(lesson_.steps.size()) &&
        now >= lesson_.steps[armed_].t) {
        const Step& ns = lesson_.steps[armed_];
        open_step(armed_, ctx, /*pause=*/ ns.kind != Kind::Track);
    }

    // A track annotation follows the live action until its end time.
    if (tracking_ >= 0) {
        const Step& ts = lesson_.steps[tracking_];
        if (now >= ts.t1) end_track();
    }
}

void LessonRuntime::open_step(int idx, const AppContext& ctx, bool pause) {
    if (idx < 0 || idx >= static_cast<int>(lesson_.steps.size())) return;
    const Step& s = lesson_.steps[idx];
    cur_step_ = idx;
    armed_ = idx + 1;
    shown_max_ = std::max(shown_max_, idx);

    end_track();

    if (s.kind == Kind::Track) {
        start_track(s, ctx);
    } else {
        if (pause) {
            ctx.replay_mgr().pause();   // no-op unless currently Playing…
            // …so if it didn't take (still Buffering on the intro beat), latch the
            // intent and let update() enforce it once playback actually starts.
            want_pause_ = !ctx.replay_mgr().is_paused();
        }
        showing_card_ = true;
    }
}

void LessonRuntime::start_track(const Step& s, const AppContext& ctx) {
    tracking_ = s.n - 1;
    showing_card_ = false;
    if (s.slowTo > 0.0f && ctx.replay_mgr().info().speed > s.slowTo) {
        prev_speed_ = ctx.replay_mgr().info().speed;
        ctx.replay_mgr().set_speed(s.slowTo);
    }
}

void LessonRuntime::end_track() {
    if (tracking_ < 0) return;
    tracking_ = -1;
    if (prev_speed_ > 0.0f) prev_speed_ = 0.0f;  // speed restore is best-effort
}

void LessonRuntime::continue_lesson(const AppContext& ctx) {
    showing_card_ = false;
    want_pause_ = false;  // user advanced - drop any deferred-pause intent
    if (cur_step_ >= static_cast<int>(lesson_.steps.size()) - 1) return;  // completion = chrome's job
    ctx.replay_mgr().resume();  // play loop gates at the next step
}

// ─── deliberate navigation (React chrome → JS bridge → here) ─────────────────
// Seek the replay to a step's anchor and RE-ARM the gate. The gate in update()
// fires steps when the clock reaches step.t; after a seek we set armed_ = idx so
// that step re-fires (and earlier ones don't), and cur_step_ = idx-1 so open_step
// transitions cleanly. We never call open_step directly here - we let the seek
// land and the normal gate loop re-open the step once candle data is back, so the
// spotlight resolves against freshly-streamed candles rather than cleared ones.
void LessonRuntime::jump_to_step(int stepN, const AppContext& ctx) {
    if (!lesson_.valid || lesson_.steps.empty()) return;
    const int last = static_cast<int>(lesson_.steps.size()) - 1;
    const int idx  = std::clamp(stepN - 1, 0, last);

    end_track();
    showing_card_ = false;
    if (prev_speed_ > 0.0f) { ctx.replay_mgr().set_speed(prev_speed_); prev_speed_ = 0.0f; }

    const int64_t t = lesson_.steps[idx].t;  // beat/quiz anchor or track t0
    ctx.replay_mgr().seek(t, /*deliberate=*/true);

    armed_    = idx;        // this step re-fires when the clock reaches t
    cur_step_ = idx - 1;    // open_step(idx) will advance it
    // shown_max_ preserved - the rail keeps its completed ticks.
}

void LessonRuntime::back(const AppContext& ctx) {
    if (!lesson_.valid) return;
    // cur_step_ is 0-based; jump_to_step is 1-based.
    //  - On a step (card/track up): Back → previous step (0-based cur_step_-1
    //    == 1-based cur_step_).
    //  - In a gap (no card/track): Back → re-open the current step (0-based
    //    cur_step_ == 1-based cur_step_+1).
    const bool on_step = (showing_card_ || tracking_ >= 0);
    const int target1 = on_step ? cur_step_ : (cur_step_ + 1);
    jump_to_step(target1, ctx);  // clamped to [1, n] inside
}

void LessonRuntime::restart(const AppContext& ctx) {
    if (!lesson_.valid) return;
    shown_max_ = -1;       // clear the rail's completed ticks
    jump_to_step(1, ctx);
}

void LessonRuntime::scrub_to_progress(float progress, bool deliberate, const AppContext& ctx) {
    if (!lesson_.valid || lesson_.steps.empty()) return;
    progress = std::clamp(progress, 0.0f, 1.0f);
    const int64_t span   = std::max<int64_t>(1, lesson_.source.endMs - lesson_.source.startMs);
    const int64_t target = lesson_.source.startMs +
                           static_cast<int64_t>(progress * static_cast<float>(span));

    // Tear down any active track/card (idempotent when none is up) and restore a
    // slowTo speed - same as jump_to_step. A scrub shouldn't leave a track box or a
    // dimmed card pinned to a stale position.
    if (tracking_ >= 0) end_track();
    showing_card_ = false;
    if (prev_speed_ > 0.0f) { ctx.replay_mgr().set_speed(prev_speed_); prev_speed_ = 0.0f; }

    ctx.replay_mgr().seek(target, deliberate);

    // Re-arm the gate from the landing: the first step whose anchor is strictly
    // after target fires next; steps at/before it count as passed (rail ticks).
    // Mirrors jump_to_step's armed_/cur_step_ bookkeeping, keyed by timestamp.
    const int last = static_cast<int>(lesson_.steps.size()) - 1;
    int idx = 0;
    while (idx <= last && lesson_.steps[idx].t <= target) ++idx;
    armed_    = idx;          // == last+1 when scrubbed past the final step (gate no-ops)
    cur_step_ = idx - 1;      // the step we're "in" (or -1 before the first)
    if (cur_step_ > shown_max_) shown_max_ = cur_step_;
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// target → screen rect (by absolute time)
// ═══════════════════════════════════════════════════════════════════════════════

namespace edu {

static const char* el_window_hint(const std::string& el) {
    if (el.find("Dom") != std::string::npos || el.find("dom") != std::string::npos) return "DOM ";
    if (el.find("Indi") != std::string::npos || el.find("indi") != std::string::npos) return "Chart ";
    if (el.find("Tape") != std::string::npos || el.find("tape") != std::string::npos) return "T ";
    return "";
}

// Min candle low / max candle high over candles whose timestamp ∈ [t0,t1].
// Returns false if no candles fall in range yet (replay hasn't reached it).
static bool hi_lo_in_span(const AppContext& ctx, int64_t t0, int64_t t1,
                          double& hi, double& lo) {
    const auto& ts   = ctx.candle_mgr().timestamps();
    const auto& highs = ctx.candle_mgr().highs();
    const auto& lows  = ctx.candle_mgr().lows();
    const size_t n = std::min({ts.size(), highs.size(), lows.size()});
    hi = -1e18; lo = 1e18;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        if (ts[i] < t0 || ts[i] > t1) continue;
        hi = std::max(hi, highs[i]); lo = std::min(lo, lows[i]); any = true;
    }
    return any;
}

LessonRuntime::Rect LessonRuntime::resolve_target(const Step& s, const AppContext& ctx,
                                                  int64_t now_ms) const {
    Rect r{};
    const ChartProjection& proj = chart_projection();
    const Target& t = s.target;

    // el → an ImGui panel's window rect (substring match on title)
    if (t.type == Target::Type::El) {
        const char* hint = el_window_hint(t.el);
        if (hint && *hint) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            for (ImGuiWindow* w : g.Windows) {
                if (!w || !w->WasActive || w->Hidden) continue;
                if (std::string(w->Name).find(hint) != std::string::npos) {
                    return Rect{ w->Pos.x - 4, w->Pos.y - 4,
                                 w->Size.x + 8, w->Size.y + 8, true };
                }
            }
        }
        // fall through to full-chart if not found
    }

    if (!proj.valid) return r;  // can't project chart-space targets yet

    // Full chart / unknown el → the whole plot rect
    if (t.type == Target::Type::Full || t.type == Target::Type::El) {
        return Rect{ proj.plot_min.x, proj.plot_min.y,
                     proj.plot_max.x - proj.plot_min.x,
                     proj.plot_max.y - proj.plot_min.y, true };
    }

    // Band → a horizontal liquidation shelf. A shelf is a price band, not a point
    // in time: frame it as a horizontal strip spanning the visible chart width
    // (clamped to the plot), so it reads like the bright viridis band in the design
    // rather than a thin box anchored to one candle / the playhead.
    //
    // Vertical extent: PREFER explicit author-drawn bounds (pTop/pBot, captured by
    // drawing the box on the real shelf in the studio - this is the accuracy fix).
    // Fall back to the LEGACY heuristic (candle low around t, `below` price-pts down)
    // only when the band wasn't authored with explicit price (hasPrice == false).
    if (t.type == Target::Type::Band) {
        double top, bot;
        if (t.hasPrice) {
            top = std::max(t.pTop, t.pBot);
            bot = std::min(t.pTop, t.pBot);
        } else {
            double hi, lo;
            // single-candle "around": use a small window centred on t (±1 candle)
            if (!hi_lo_in_span(ctx, t.t - 150000, t.t + 150000, hi, lo)) return r;
            top = lo;
            bot = lo - static_cast<double>(t.below);
        }

        // Horizontal extent: from the anchor candle to the live playhead, but
        // CLAMPED to the visible plot rect so a post-seek/edge anchor can't fling
        // the box into empty future space. Falls back to a sensible visible-width
        // span if the anchor projects off-screen.
        float x0 = proj.x_of_time(t.t) - 40.0f;
        float x1 = proj.x_of_time(static_cast<double>(now_ms)) + 40.0f;
        x0 = std::clamp(x0, proj.plot_min.x, proj.plot_max.x);
        x1 = std::clamp(x1, proj.plot_min.x, proj.plot_max.x);
        if (x1 - x0 < 120.0f) {
            // anchor + playhead collapsed (paused right on the step, or off-screen):
            // span the right portion of the visible chart so the shelf is legible.
            x1 = proj.plot_max.x;
            x0 = std::max(proj.plot_min.x, x1 - std::max(220.0f,
                          (proj.plot_max.x - proj.plot_min.x) * 0.45f));
        }

        // Vertical extent: clamp into the visible plot so a deep band can't run the
        // box off the bottom of the chart.
        float yT = proj.y_of_price(top) - 6.0f;
        float yB = proj.y_of_price(bot);
        yT = std::clamp(yT, proj.plot_min.y, proj.plot_max.y);
        yB = std::clamp(yB, proj.plot_min.y, proj.plot_max.y);
        if (yB - yT < 24.0f) yB = std::min(proj.plot_max.y, yT + 24.0f);
        return Rect{ x0, yT, x1 - x0, yB - yT, true };
    }

    // Region (and follow). For a track, grow from t0 to the live playhead.
    const bool is_track = (t.follow || s.kind == Kind::Track);
    int64_t r0 = t.follow ? s.t0 : t.t0;
    int64_t r1 = is_track ? std::min(t.follow ? s.t1 : t.t1, now_ms) : t.t1;
    if (r1 < r0) std::swap(r0, r1);
    if (r1 <= r0) r1 = r0 + 1;

    double hi, lo;
    if (t.hasPrice) {
        hi = std::max(t.pTop, t.pBot);
        lo = std::min(t.pTop, t.pBot);
    } else if (!hi_lo_in_span(ctx, r0, r1, hi, lo)) {
        return r;  // no candles in span yet
    }

    const float x0 = proj.x_of_time(r0);
    const float x1 = proj.x_of_time(r1);
    const float yT = proj.y_of_price(hi) - 14.0f;
    const float yB = proj.y_of_price(lo) + 14.0f;
    return Rect{ x0 - 6.0f, yT, (x1 - x0) + 12.0f, yB - yT, true };
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// overlay: dim + cut-out + ring + (native fallback coach card)
// ═══════════════════════════════════════════════════════════════════════════════

namespace edu {

// The beat/quiz coach CARD now lives in the React chrome (driven by emit_state).
// The in-canvas ImGui card stays only as a native/Debug fallback for when the
// JS bridge isn't present (standalone terminal build, or bridge-down debugging).
// The SPOTLIGHT (dim + ring + track label) ALWAYS renders here - it needs the
// per-frame chart_projection and belongs in the canvas.
#ifdef __EMSCRIPTEN__
static constexpr bool g_edu_native_card = false;  // React owns the card in WASM
#else
static constexpr bool g_edu_native_card = true;
#endif

void LessonRuntime::render_overlay(const AppContext& ctx) {
    if (!lesson_.valid) return;
    if (!ctx.replay_mgr().is_active()) return;

    const bool is_track = (tracking_ >= 0);
    const bool is_card  = showing_card_;
    if (!is_track && !is_card) { last_box_.valid = false; return; }

    const int idx = is_track ? tracking_ : cur_step_;
    if (idx < 0 || idx >= static_cast<int>(lesson_.steps.size())) { last_box_.valid = false; return; }
    const Step& step = lesson_.steps[idx];

    const int64_t now_ms = replay_now_ms(ctx);
    const Rect box = resolve_target(step, ctx, now_ms);
    if (!box.valid) { last_box_.valid = false; return; }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList(vp);
    const ImVec2 v0 = vp->Pos;
    const ImVec2 v1 = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

    const float bx0 = std::max(v0.x, box.x);
    const float by0 = std::max(v0.y, box.y);
    const float bx1 = std::min(v1.x, box.x + box.w);
    const float by1 = std::min(v1.y, box.y + box.h);

    // Publish the clamped spotlight rect (CSS px, viewport-relative) for the React
    // floating card. ImGui DisplaySize is in CSS px (DPR rides in FramebufferScale),
    // so these coords map directly onto the canvas in CSS space.
    last_box_ = Box{ bx0 - v0.x, by0 - v0.y, bx1 - bx0, by1 - by0, true };

    // Dim everything except the box (four rects, no stencil). Beat dims hard,
    // track stays light so the live action it follows remains readable.
    const ImU32 dim = is_track ? IM_COL32(0, 0, 0, 92) : IM_COL32(0, 0, 0, 184);
    fg->AddRectFilled(v0, ImVec2(v1.x, by0), dim);
    fg->AddRectFilled(ImVec2(v0.x, by1), v1, dim);
    fg->AddRectFilled(ImVec2(v0.x, by0), ImVec2(bx0, by1), dim);
    fg->AddRectFilled(ImVec2(bx1, by0), ImVec2(v1.x, by1), dim);

    const ImU32 ring = is_track ? IM_COL32(255, 191, 0, 230) : IM_COL32(34, 197, 219, 235);
    fg->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), ring, 3.0f, 0, 1.6f);

    if (is_track) {
        char label[160];
        snprintf(label, sizeof(label), " %d  %s  (watch) ", step.n, step.kicker.c_str());
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float lx = std::clamp((bx0 + bx1) * 0.5f - ts.x * 0.5f, v0.x + 8, v1.x - ts.x - 8);
        const float ly = std::max(v0.y + 60.0f, by0 - ts.y - 12.0f);
        fg->AddRectFilled(ImVec2(lx - 8, ly - 5), ImVec2(lx + ts.x + 8, ly + ts.y + 5),
                          IM_COL32(20, 24, 30, 230), 5.0f);
        fg->AddRect(ImVec2(lx - 8, ly - 5), ImVec2(lx + ts.x + 8, ly + ts.y + 5), ring, 5.0f, 0, 1.2f);
        fg->AddText(ImVec2(lx, ly), IM_COL32(220, 230, 240, 255), label);
        return;
    }

    // Beat/quiz card. In WASM the React chrome renders this (driven by emit_state),
    // so we draw only the dim+ring spotlight above and skip the ImGui window. The
    // native card remains for non-WASM / bridge-down fallback - AND for the Studio
    // export session (export_render_): captureStream sees only the canvas, so the
    // card must burn in. In export mode it is CONTENT ONLY (no Continue button -
    // the author advances from the React studio footer, outside the capture) and
    // styled with Theme::Tokens (it ships inside produced videos).
    if (!g_edu_native_card && !export_render_) return;

    // Beat/quiz card: place to the side of the box with the most room.
    const float cw = 360.0f;
    float cx, cy;
    const float rightRoom = v1.x - bx1, leftRoom = bx0 - v0.x;
    if (rightRoom > cw + 28) cx = bx1 + 18;
    else if (leftRoom > cw + 28) cx = bx0 - cw - 18;
    else cx = std::clamp((bx0 + bx1) * 0.5f - cw * 0.5f, v0.x + 12, v1.x - cw - 12);
    cy = std::clamp(by0, v0.y + 60.0f, v1.y - 220.0f);

    ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.98f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (export_render_) {
        // No input in a burned-in card - don't let it steal focus/hover from the
        // chart mid-take either.
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border,   Theme::Tokens::BD2);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        export_render_ ? Theme::Radius::R3 : 8.0f);
    if (ImGui::Begin("##edu_coach", nullptr, flags)) {
        const bool quiz = (step.kind == Kind::Quiz);
        if (export_render_) {
            // Kicker row: "N · KICKER" in the micro-label font, brand on-state text.
            std::string kick = std::to_string(step.n) + " \xc2\xb7 " +
                               (step.kicker.empty() ? (quiz ? "PREDICT" : "STEP")
                                                    : step.kicker);
            for (auto& ch : kick)
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
            ImGui::PushFont(Theme::Fonts::label());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_TX);
            ImGui::TextUnformatted(kick.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushFont(Theme::Fonts::heading());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX1);
            ImGui::TextWrapped("%s", step.title.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (!step.body.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
                ImGui::TextWrapped("%s", step.body.c_str());
                ImGui::PopStyleColor();
            }
            if (quiz) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
                for (size_t oi = 0; oi < step.options.size(); ++oi) {
                    char b[8]; snprintf(b, sizeof(b), "%c. ", static_cast<char>('A' + oi));
                    ImGui::TextWrapped("%s%s", b, step.options[oi].text.c_str());
                }
                ImGui::PopStyleColor();
            }
            // NO button, NO separator - pure content in the recording.
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(34, 197, 219, 255));
            ImGui::TextUnformatted((std::to_string(step.n) + "  " +
                                    (step.kicker.empty() ? (quiz ? "PREDICT" : "STEP") : step.kicker)).c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::TextWrapped("%s", step.title.c_str());
            if (!step.body.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 175, 190, 255));
                ImGui::TextWrapped("%s", step.body.c_str());
                ImGui::PopStyleColor();
            }
            if (quiz) {
                ImGui::Spacing();
                for (size_t oi = 0; oi < step.options.size(); ++oi) {
                    char b[8]; snprintf(b, sizeof(b), "%c. ", static_cast<char>('A' + oi));
                    ImGui::TextWrapped("%s%s", b, step.options[oi].text.c_str());
                }
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            const bool last = cur_step_ >= static_cast<int>(lesson_.steps.size()) - 1;
            if (ImGui::Button(last ? "Finish" : (quiz ? "Watch it play out" : "Continue"),
                              ImVec2(-1, 0))) {
                continue_lesson(ctx);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (export_render_) ImGui::PopStyleColor(2);
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// JS ↔ C++ bridge: command queue (in) + state emit (out)
// ═══════════════════════════════════════════════════════════════════════════════

namespace edu {

void LessonRuntime::enqueue_cmd(Cmd c, int arg_i, float arg_f) {
    // Single-slot, last-write-wins. Commands are discrete clicks; if two land in
    // one frame the later one is what the user saw most recently.
    pending_cmd_   = c;
    pending_arg_i_ = arg_i;
    pending_arg_f_ = arg_f;
}

void LessonRuntime::drain_cmd(const AppContext& ctx) {
    const Cmd c = pending_cmd_;
    if (c == Cmd::None) return;
    const int   ai = pending_arg_i_;
    const float af = pending_arg_f_;
    pending_cmd_ = Cmd::None;  // consume before dispatch (re-entrancy safe)

    switch (c) {
        case Cmd::Continue: continue_lesson(ctx); break;
        case Cmd::Back:     back(ctx);            break;
        case Cmd::Restart:  restart(ctx);         break;
        case Cmd::Jump:     jump_to_step(ai, ctx); break;
        case Cmd::Play:     if (ctx.replay_mgr().is_paused()) ctx.replay_mgr().resume(); break;
        case Cmd::Pause:    if (ctx.replay_mgr().is_playing()) ctx.replay_mgr().pause(); break;
        case Cmd::SetSpeed: ctx.replay_mgr().set_speed(af); break;
        case Cmd::Seek:     scrub_to_progress(static_cast<float>(ai) / 1000.0f, af > 0.5f, ctx); break;
        case Cmd::None:     break;
    }
}

// ─── state emit ──────────────────────────────────────────────────────────────

static const char* phase_name(bool active, bool playing, bool showing_card,
                              bool tracking, bool done) {
    if (done)         return "done";
    if (!active)      return "replay_pending";
    if (showing_card) return "card";
    if (tracking)     return "track";
    if (playing)      return "playing";
    return "playing";  // active but between steps - still "playing" to the UI
}

void LessonRuntime::emit_state(const AppContext& ctx) {
    if (!lesson_.valid) return;
#ifdef __EMSCRIPTEN__
    const ReplayManager& rm = ctx.replay_mgr();
    const bool active  = rm.is_active();
    const bool playing = rm.is_playing();
    const bool paused  = rm.is_paused();
    // loading = clock HELD for initial prime OR mid-seek re-buffer after a scrub -
    // either way no fresh data, so the React chrome shows a spinner and freezes the
    // clock instead of extrapolating into an empty window.
    const bool loading = ::g_lesson_loading || rm.state() == ReplayManager::State::Seeking;
    const int  last    = static_cast<int>(lesson_.steps.size()) - 1;
    const bool done    = (cur_step_ >= last) && !showing_card_ && tracking_ < 0 && armed_ > last;
    const bool track_on = tracking_ >= 0;

    const char* phase = phase_name(active, playing, showing_card_, track_on, done);

    // Discrete-field signature (FNV-1a). Clock/progress deliberately EXCLUDED so
    // we emit on state transitions, not every frame. React interpolates the clock.
    transport::SigHasher hasher;
    hasher.mix(static_cast<uint64_t>(phase[0]) | (static_cast<uint64_t>(phase[1]) << 8));
    hasher.mix(static_cast<uint64_t>(cur_step_ + 2));   // +2 so -1 stays nonzero/distinct
    hasher.mix(static_cast<uint64_t>(shown_max_ + 2));
    hasher.mix(static_cast<uint64_t>(armed_));
    hasher.mix(static_cast<uint64_t>(paused ? 1 : 0));
    hasher.mix(static_cast<uint64_t>(static_cast<int>(rm.info().speed * 100.0f)));
    hasher.mix(static_cast<uint64_t>(track_on ? (tracking_ + 1) : 0));
    hasher.mix(static_cast<uint64_t>(::g_lesson_loading ? 1 : 0));
    hasher.mix(static_cast<uint64_t>(last_box_.valid ? 1 : 0));  // spotlight appear/disappear
    // Scrub-forward lock: hash the BOOL only (the ceiling value tracks the
    // playhead while locked; hashing it would emit every frame - React derives
    // the ceiling from its interpolated clock instead).
    const bool lock_forward = rm.seek_ceiling_active();
    hasher.mix(static_cast<uint64_t>(lock_forward ? 1 : 0));
    uint64_t sig = hasher.value();
    if (!steps_emitted_) sig ^= 0xA5A5A5A5ULL;   // force first emit to carry steps[]

    if (!transport::should_emit(sig, emit_sig_, active, playing, paused, loading,
                                emscripten_get_now(), last_clock_emit_ms_)) {
        return;  // nothing discrete changed & no clock tick due
    }

    // Build the snapshot. nlohmann handles all escaping; we pass dump() to JS.
    json st;
    st["v"]         = 1;
    st["phase"]     = phase;
    st["stepIndex"] = cur_step_;
    st["stepCount"] = static_cast<int>(lesson_.steps.size());
    st["shownMax"]  = shown_max_;
    st["title"]     = lesson_.title;
    st["course"]    = lesson_.course;
    st["chapter"]   = lesson_.chapter;
    st["description"] = lesson_.description;
    st["symbol"]    = lesson_.source.symbol;
    st["tf"]        = lesson_.source.tf;
    st["event"]     = lesson_.source.event;
    st["startMs"]   = lesson_.source.startMs;
    st["endMs"]     = lesson_.source.endMs;
    // Monotonic clock floor (see clock_floor_ms_): during a steady playing stretch,
    // absorb small backward steps from replay_now_ms() (the initial-catch-up jitter
    // that snapped the React clock 00:01→00:00) by holding the max; let a LARGE
    // backward jump (a real seek/rewind) through and re-baseline. Re-baseline too
    // whenever not steadily playing (loading/seeking/paused/idle), so a backward
    // scrub that routes through Seeking is honored.
    int64_t now_ms = active ? replay_now_ms(ctx) : lesson_.source.startMs;
    {
        constexpr int64_t kBackwardTolMs = 2500;
        if (active && playing && !paused && !loading) {
            if (now_ms < clock_floor_ms_ && (clock_floor_ms_ - now_ms) <= kBackwardTolMs)
                now_ms = clock_floor_ms_;
            clock_floor_ms_ = now_ms;
        } else {
            clock_floor_ms_ = now_ms;
        }
    }
    st["clockMs"]   = now_ms;
    st["paused"]    = paused;
    st["loading"]   = loading;
    st["speed"]     = rm.info().speed;
    // True while the active step forbids scrubbing ahead of the playhead
    // (lockForward beat/quiz card up, or a lockForward track running). The
    // C++ seek path enforces it; the bar renders the locked region + disables
    // forward jumps from this flag, using its own clock as the boundary.
    st["lockForward"] = lock_forward;
    {
        const int64_t span = std::max<int64_t>(1, lesson_.source.endMs - lesson_.source.startMs);
        const int64_t off  = std::clamp<int64_t>(now_ms - lesson_.source.startMs, 0, span);
        st["progress"] = active ? (static_cast<double>(off) / static_cast<double>(span)) : 0.0;
    }

    // Serialize one step to JSON (shared by `step` and `currentStep`).
    auto step_json = [&](int idx) -> json {
        const Step& s = lesson_.steps[idx];
        json js;
        js["n"]      = s.n;
        js["kind"]   = (s.kind == Kind::Track ? "track" : s.kind == Kind::Quiz ? "quiz" : "beat");
        js["title"]  = s.title;
        js["kicker"] = s.kicker;
        js["body"]   = s.body;
        js["indicator"] = s.indicator;
        js["t"]      = s.t;
        if (s.kind == Kind::Track) js["t1"] = s.t1;
        if (s.kind == Kind::Quiz) {
            json opts = json::array();
            for (const auto& o : s.options)
                opts.push_back({{"text", o.text}, {"correct", o.correct}, {"why", o.why}});
            js["options"] = std::move(opts);
        }
        return js;
    };

    // `step` = the ACTIVE card/track (drives the quiz panel + transient state),
    // or null between steps.
    const int active_idx = track_on ? tracking_ : (showing_card_ ? cur_step_ : -1);
    st["step"] = (active_idx >= 0 && active_idx <= last) ? step_json(active_idx) : json(nullptr);

    // `currentStep` = the current step's full data, PERSISTENT across the gaps
    // between cards - so the React coach bar can always show context, not blink
    // out when no card is up. Falls back to step 1 before the first step fires.
    const int cur_idx = (cur_step_ >= 0 && cur_step_ <= last) ? cur_step_
                       : (last >= 0 ? 0 : -1);
    st["currentStep"] = (cur_idx >= 0) ? step_json(cur_idx) : json(nullptr);

    // Rail list (static after load) - included in every emit so a late-mounting
    // listener gets a self-contained snapshot from one read. Cheap (~steps×~60B).
    json rail = json::array();
    for (const auto& s : lesson_.steps) {
        rail.push_back({
            {"n", s.n},
            {"kind", (s.kind == Kind::Track ? "track" : s.kind == Kind::Quiz ? "quiz" : "beat")},
            {"title", s.title},
            {"kicker", s.kicker},
            {"t", s.t},
        });
    }
    st["steps"] = std::move(rail);

    // Spotlight rect (CSS px, canvas-relative) so the React floating card can pin
    // itself beside the cut-out. Excluded from the dirty-check sig on purpose -
    // it rides whatever emit a discrete change triggers (card open = a phase/step
    // change → fresh rect). While a beat is paused the chart is static so the rect
    // stays accurate; null when no spotlight is up.
    if (last_box_.valid) {
        st["spotlight"] = { {"x", last_box_.x}, {"y", last_box_.y},
                            {"w", last_box_.w}, {"h", last_box_.h} };
    } else {
        st["spotlight"] = nullptr;
    }
    steps_emitted_ = true;

    transport::dispatch_state("edgedepth:lesson", "__EDGEDEPTH_LESSON_STATE__", st.dump());
#else
    (void)ctx;
#endif
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// KEEPALIVE command callbacks - React calls these via Module._edu_cmd_*().
// They only ENQUEUE; LessonRuntime::update(ctx) applies them with a live ctx.
// Mirrors education_boot.cpp's _education_on_lesson_fetch pattern.
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef __EMSCRIPTEN__
extern "C" {

EMSCRIPTEN_KEEPALIVE void _edu_cmd_continue(void) {
    edu::LessonRuntime::instance().enqueue_cmd(edu::LessonRuntime::Cmd::Continue);
}
EMSCRIPTEN_KEEPALIVE void _edu_cmd_back(void) {
    edu::LessonRuntime::instance().enqueue_cmd(edu::LessonRuntime::Cmd::Back);
}
EMSCRIPTEN_KEEPALIVE void _edu_cmd_restart(void) {
    edu::LessonRuntime::instance().enqueue_cmd(edu::LessonRuntime::Cmd::Restart);
}
EMSCRIPTEN_KEEPALIVE void _edu_cmd_jump(int stepN) {
    edu::LessonRuntime::instance().enqueue_cmd(edu::LessonRuntime::Cmd::Jump, stepN);
}
EMSCRIPTEN_KEEPALIVE void _edu_cmd_set_paused(int paused) {
    edu::LessonRuntime::instance().enqueue_cmd(
        paused ? edu::LessonRuntime::Cmd::Pause : edu::LessonRuntime::Cmd::Play);
}
EMSCRIPTEN_KEEPALIVE void _edu_cmd_set_speed(int centi_speed) {
    // Speed crosses the JS↔wasm boundary as an INT (centi-speed: 400 = 4.0×).
    // A float param read straight off the raw wasm export mis-marshals the JS
    // f64 as f32 (the direct-export path does no arg conversion, unlike ccall),
    // which silently corrupted the speed - that was the "fast-forward does
    // nothing" bug. Int args marshal cleanly. Convert back to a float here.
    const float speed = static_cast<float>(centi_speed) / 100.0f;
    edu::LessonRuntime::instance().enqueue_cmd(edu::LessonRuntime::Cmd::SetSpeed, 0, speed);
}
// Free scrub from the transport timeline. milli_progress 0..1000 = 0.0..1.0 of the
// window; deliberate!=0 bypasses the scrubber-drag debounce (1 on drop/click, 0 on a
// live drag). Int args marshal cleanly across the raw export (see set_speed).
EMSCRIPTEN_KEEPALIVE void _edu_cmd_seek(int milli_progress, int deliberate) {
    edu::LessonRuntime::instance().enqueue_cmd(
        edu::LessonRuntime::Cmd::Seek, milli_progress, deliberate != 0 ? 1.0f : 0.0f);
}

} // extern "C"
#endif
