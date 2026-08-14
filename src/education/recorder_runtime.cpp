// ═══════════════════════════════════════════════════════════════════════════════
// recorder_runtime.cpp - see recorder_runtime.h
// ═══════════════════════════════════════════════════════════════════════════════

#include "education/recorder_runtime.h"
#include "education/transport_emit.h"
#include "core/app_context.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include "rendering/layout.h"

#include <imgui.h>
#include <imgui_internal.h>   // g.Windows scan for the focus panel rect
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Defined in main.cpp (Module.__set_chart_timeframe) - same change_timeframe
// path the live topbar and the lesson Explore pill use. Main thread only.
extern "C" void _set_chart_timeframe(int sec);
#endif

using json = nlohmann::json;

namespace edu {

namespace {

// Wall clock (ms) for hold timing + emit cadence. Monotonic under Emscripten.
double now_wall_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return ImGui::GetTime() * 1000.0;
#endif
}

// Positioning tolerance (replay-DATA ms): inside it a shot plays through from
// the current clock (no jump). Outside it, forward → skip_forward_to (clock-
// only, instant), backward → one deliberate seek (full re-prime).
constexpr int64_t kJumpTolMs = 2000;

// Max forward span for skip_forward_to. The clock-only skip keeps formed
// candles but leaves the skipped range EMPTY on the chart - a visible candle
// gap in the produced clip (unacceptable beyond a candle or two). Bigger
// forward jumps use a deliberate seek instead: full re-prime with backfilled
// history, so the chart is continuous at the landing point. The harness logs
// the loading span and post cuts it out (a hard scene cut at the jump).
constexpr int64_t kSkipMaxMs = 120000;

// Focus keyword → docked-panel window-title substring (the lesson runtime's
// el_window_hint mapping - DOMWidget "DOM …", TradesWidget "T …", ChartWidget
// "Chart …"). Empty = no focus.
const char* focus_window_hint(const std::string& f) {
    if (f == "dom")   return "DOM ";
    if (f == "tape")  return "T ";
    if (f == "chart") return "Chart ";
    return "";
}

}  // namespace

const char* RecorderRuntime::phase_name(Phase p) {
    switch (p) {
        case Phase::Waiting: return "waiting";
        case Phase::Shot:    return "shot";
        case Phase::Hold:    return "hold";
        case Phase::Done:    return "done";
        case Phase::Error:   return "error";
    }
    return "waiting";
}

bool RecorderRuntime::load(const std::string& js) {
    valid_ = false;
    shots_.clear();

    json doc = json::parse(js, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        printf("[Recorder] script parse failed\n");
        return false;
    }

    symbol_      = doc.value("symbol", std::string());
    event_id_    = doc.value("eventId", std::string());
    speed_scale_ = doc.value("speedScale", 1.0f);
    if (!(speed_scale_ > 0.0f)) speed_scale_ = 1.0f;
    std::transform(symbol_.begin(), symbol_.end(), symbol_.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Optional view overrides (see header): pin the layer state per script.
    has_view_ = false;
    view_ = ViewOverrides{};
    if (doc.contains("view") && doc["view"].is_object()) {
        const json& v = doc["view"];
        auto tri = [&](const char* key) -> int {
            return v.contains(key) && v[key].is_boolean() ? (v[key].get<bool>() ? 1 : 0) : -1;
        };
        view_.liq_field    = tri("liqField");
        view_.liq_levels   = tri("liqLevels");
        view_.liq_profile  = tri("liqProfile");
        view_.liq_observed = tri("liqObserved");
        view_.ob_depth     = tri("obDepth");
        view_.vpvr         = tri("vpvr");
        if (v.contains("viewSpanMinutes") && v["viewSpanMinutes"].is_number())
            view_.view_span_min = v["viewSpanMinutes"].get<int>();
        has_view_ = true;
    }

    if (!doc.contains("shots") || !doc["shots"].is_array()) {
        printf("[Recorder] script has no shots[]\n");
        return false;
    }
    for (const auto& s : doc["shots"]) {
        if (!s.is_object()) continue;
        Shot sh;
        sh.from_ms  = s.value("fromMs",  static_cast<int64_t>(0));
        sh.until_ms = s.value("untilMs", static_cast<int64_t>(0));
        sh.speed    = s.value("speed",   1.0f);
        sh.tf_sec   = s.value("tf",      0);
        sh.hold_ms  = s.value("holdMs",  0);
        sh.focus    = s.value("focus",   std::string());
        sh.caption  = s.value("caption", std::string());
        if (sh.from_ms <= 0 || sh.until_ms <= sh.from_ms) {
            printf("[Recorder] dropping malformed shot (from=%lld until=%lld)\n",
                   static_cast<long long>(sh.from_ms),
                   static_cast<long long>(sh.until_ms));
            continue;
        }
        if (!(sh.speed > 0.0f)) sh.speed = 1.0f;
        if (sh.hold_ms < 0) sh.hold_ms = 0;
        shots_.push_back(std::move(sh));
    }
    if (shots_.empty()) {
        printf("[Recorder] script had no usable shots\n");
        return false;
    }
    std::sort(shots_.begin(), shots_.end(),
              [](const Shot& a, const Shot& b) { return a.from_ms < b.from_ms; });

    // Watermark badge, built once (P1 rule: replay-DATA date, UTC; wall clock
    // never appears anywhere). Symbol uppercased for display.
    {
        std::string sym_upper = symbol_.empty() ? std::string("EDGEDEPTH") : symbol_;
        std::transform(sym_upper.begin(), sym_upper.end(), sym_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        const time_t s = static_cast<time_t>(shots_.front().from_ms / 1000);
        struct tm tmv;
        gmtime_r(&s, &tmv);
        snprintf(badge_, sizeof(badge_),
                 "EDGEDEPTH \xc2\xb7 %s \xc2\xb7 REPLAY %04d-%02d-%02d",
                 sym_upper.c_str(),
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }

    phase_ = Phase::Waiting;
    cur_   = -1;
    valid_ = true;
    printf("[Recorder] script loaded: %d shots, scale %.2f, symbol %s\n",
           static_cast<int>(shots_.size()),
           static_cast<double>(speed_scale_), symbol_.c_str());
    return true;
}

void RecorderRuntime::begin_shot(int i, const AppContext& ctx) {
    ReplayManager& rm = ctx.replay_mgr();
    cur_   = i;
    phase_ = Phase::Shot;
    const Shot& s = shots_[static_cast<size_t>(i)];

    // Re-stamp the badge date from THIS shot's replay-data position (P1 rule).
    // Scripts may open with a throwaway settle shot at the pack start, which
    // can sit on a different UTC day than the shots that make the clip.
    {
        std::string sym_upper = symbol_.empty() ? std::string("EDGEDEPTH") : symbol_;
        std::transform(sym_upper.begin(), sym_upper.end(), sym_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        const time_t sh = static_cast<time_t>(s.from_ms / 1000);
        struct tm tmv;
        gmtime_r(&sh, &tmv);
        snprintf(badge_, sizeof(badge_),
                 "EDGEDEPTH \xc2\xb7 %s \xc2\xb7 REPLAY %04d-%02d-%02d",
                 sym_upper.c_str(),
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }

    // Timeframe first - cheap, and the chart re-request happens against the
    // shot's landing position rather than the previous one.
#ifdef __EMSCRIPTEN__
    if (s.tf_sec > 0) _set_chart_timeframe(s.tf_sec);
#endif

    // Position. Forward jump = clock-only skip (instant, keeps formed candles
    // on the chart - visual continuity between shots). Backward jump = ONE
    // deliberate seek (full re-prime; well-formed scripts never need it).
    const int64_t now = rm.interpolated_time_ms();
    const int64_t d   = s.from_ms - now;
    if (d > kJumpTolMs && d <= kSkipMaxMs) {
        rm.skip_forward_to(s.from_ms);
    } else if (d > kSkipMaxMs || d < -kJumpTolMs) {
        rm.seek(s.from_ms, /*deliberate=*/true);  // see kSkipMaxMs above
    }

    // Effective speed = scripted × scale, clamped to the transport's range.
    // The clamped value is what post-production retimes against, so it's the
    // one emitted in state (never the scripted value).
    eff_speed_ = std::clamp(s.speed * speed_scale_, 0.1f, 10.0f);
    rm.set_speed(eff_speed_);
    if (rm.is_paused()) rm.resume();  // e.g. resuming out of a hold

    // Overshoot guard: after a reposition the engine's catch-up burst can run
    // interpolated_time_ms ahead of wall-scaled speed for a moment (observed:
    // a 14s moment shot ended in 1.4s, then the clock snapped back). A shot
    // may not end before ~3/4 of its expected wall duration at eff speed; by
    // then any transient overshoot has been corrected and the clock >= untilMs
    // check is trustworthy again.
    shot_start_wall_ = now_wall_ms();
    const int64_t clock_now = rm.interpolated_time_ms();
    const int64_t span = s.until_ms - std::max(s.from_ms, std::min(clock_now, s.until_ms));
    shot_min_wall_ms_ = 0.75 * (static_cast<double>(span) / eff_speed_);

    printf("[Recorder] shot %d/%d: from=%lld until=%lld speed=%.2f tf=%d\n",
           i + 1, static_cast<int>(shots_.size()),
           static_cast<long long>(s.from_ms),
           static_cast<long long>(s.until_ms),
           static_cast<double>(eff_speed_), s.tf_sec);
}

void RecorderRuntime::advance(const AppContext& ctx) {
    if (cur_ + 1 >= static_cast<int>(shots_.size())) {
        // Freeze the final frame so the capture tail is a clean hold - the
        // harness stops on "done"; the end card is a post step.
        ctx.replay_mgr().pause();
        phase_ = Phase::Done;
        printf("[Recorder] done\n");
        return;
    }
    begin_shot(cur_ + 1, ctx);
}

void RecorderRuntime::update(const AppContext& ctx) {
    if (!valid_ || phase_ == Phase::Done || phase_ == Phase::Error) return;
    if (!ctx.replayer) return;
    ReplayManager& rm = ctx.replay_mgr();

    // Session died → the harness bails on the emitted "error".
    if (rm.state() == ReplayManager::State::Error) {
        phase_ = Phase::Error;
        printf("[Recorder] replay session errored\n");
        return;
    }
    // Replay ran out (Stopped tears the data context down - a seek cannot
    // revive a pack). Whatever is on screen is the clip's final frame.
    if (rm.state() == ReplayManager::State::Stopped) {
        phase_ = Phase::Done;
        printf("[Recorder] replay ended before script - done\n");
        return;
    }

    // Primed = the buffering gate has promoted to PLAYING against a known
    // window. is_active()/!is_loading() alone is NOT enough: Creating/Joining
    // are "active", not Buffering, and not Seeking - so the loose check arms
    // shot 0 against an empty canvas before the pack/session even loads (the
    // EVAA smoke-test bug). A real window (end > start) only exists post-join.
    const bool primed = rm.is_playing() &&
                        rm.info().end_time_ms > rm.info().start_time_ms;

    switch (phase_) {
        case Phase::Waiting:
            // One-shot arm: the first shot starts only against a primed,
            // seeded book (mirrors the event deep-link contract).
            if (primed) begin_shot(0, ctx);
            break;

        case Phase::Shot: {
            if (!primed) break;  // mid-reposition: hold the shot boundary check
            const Shot& s = shots_[static_cast<size_t>(cur_)];
            if (now_wall_ms() - shot_start_wall_ < shot_min_wall_ms_) break;
            if (rm.interpolated_time_ms() >= s.until_ms) {
                if (s.hold_ms > 0) {
                    rm.pause();
                    hold_until_wall_ = now_wall_ms() + s.hold_ms;
                    phase_ = Phase::Hold;
                } else {
                    advance(ctx);
                }
            }
            break;
        }

        case Phase::Hold:
            // pause() is a no-op outside Playing - keep asking until it takes
            // (mirrors the lesson's want_pause_ latch), then run out the wall
            // clock for the freeze-frame beat.
            if (rm.is_playing()) rm.pause();
            if (now_wall_ms() >= hold_until_wall_) advance(ctx);
            break;

        case Phase::Done:
        case Phase::Error:
            break;
    }
}

void RecorderRuntime::emit_state(const AppContext& ctx) {
#ifdef __EMSCRIPTEN__
    if (!valid_ || !ctx.replayer) return;
    const ReplayManager& rm = ctx.replay_mgr();

    const bool active  = rm.is_active();
    const bool playing = rm.is_playing();
    const bool paused  = rm.is_paused();
    const bool loading = rm.is_loading() || rm.state() == ReplayManager::State::Seeking;

    // Discrete-field signature (clock EXCLUDED - it rides the cadence).
    transport::SigHasher hasher;
    hasher.mix(static_cast<uint64_t>(phase_));
    hasher.mix(static_cast<uint64_t>(static_cast<int64_t>(cur_)));
    hasher.mix(active  ? 1u : 0u);
    hasher.mix(loading ? 1u : 0u);
    hasher.mix(static_cast<uint64_t>(static_cast<int>(eff_speed_ * 100.0f)));

    if (!transport::should_emit(hasher.value(), emit_sig_, active, playing, paused, loading,
                                now_wall_ms(), last_clock_emit_ms_)) {
        return;
    }

    int64_t now_ms = active ? rm.interpolated_time_ms() : 0;
    now_ms = transport::apply_clock_floor(now_ms, clock_floor_ms_,
                                          active, playing, paused, loading);

    json st;
    st["v"]         = 1;
    st["phase"]     = phase_name(phase_);
    st["shot"]      = cur_;                                  // -1 before shot 0
    st["shotCount"] = static_cast<int>(shots_.size());
    st["clockMs"]   = now_ms;
    st["wallMs"]    = now_wall_ms();
    st["speed"]     = eff_speed_;                            // EFFECTIVE (clamped)
    st["symbol"]    = symbol_;
    st["eventId"]   = event_id_;
    st["active"]    = active;
    st["loading"]   = loading;
    if (cur_ >= 0 && cur_ < static_cast<int>(shots_.size())) {
        const Shot& s   = shots_[static_cast<size_t>(cur_)];
        st["fromMs"]    = s.from_ms;
        st["untilMs"]   = s.until_ms;
        st["caption"]   = s.caption;
        st["focus"]     = s.focus;
        if (last_focus_.valid) {
            // CSS px (DisplaySize space) - post can crop-zoom to exactly what
            // the spotlight framed, at whatever capture resolution.
            st["focusRect"] = { {"x", last_focus_.x}, {"y", last_focus_.y},
                                {"w", last_focus_.w}, {"h", last_focus_.h} };
        }
    }

    // Panel rects (CSS px), every emit: post frames the 9:16 crop from these
    // (chart live-edge anchor for wide shots, DOM/tape framing for focused
    // ones) with zero layout assumptions baked into the pipeline.
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        auto rect_of = [&](const char* hint) -> json {
            for (ImGuiWindow* w : g.Windows) {
                if (!w || !w->WasActive || w->Hidden) continue;
                if (std::string(w->Name).find(hint) == std::string::npos) continue;
                return json{{"x", w->Pos.x}, {"y", w->Pos.y},
                            {"w", w->Size.x}, {"h", w->Size.y}};
            }
            return json();  // null
        };
        json panels;
        if (json c = rect_of(focus_window_hint("chart")); !c.is_null()) panels["chart"] = c;
        if (json d2 = rect_of(focus_window_hint("dom")); !d2.is_null()) panels["dom"] = d2;
        if (json t = rect_of(focus_window_hint("tape")); !t.is_null()) panels["tape"] = t;
        if (!panels.empty()) st["panels"] = panels;
    }

    transport::dispatch_state("edgedepth:recorder", "__EDGEDEPTH_RECORDER_STATE__",
                              st.dump());
#else
    (void)ctx;
#endif
}

void RecorderRuntime::render_overlay(const AppContext& ctx) {
    (void)ctx;
    if (!valid_ || badge_[0] == '\0') return;
    // Overlays burn from the first shot through the done freeze-frame - never
    // during Waiting (capture hasn't started) or Error.
    if (phase_ != Phase::Shot && phase_ != Phase::Hold && phase_ != Phase::Done) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    // ── Focus lightbox - lesson-spotlight style (dim + accent ring) around the
    //    shot's panel. Drawn BEFORE the badge so the badge stays above the dim.
    //    Rect resolves per frame (docked panels can only move between frames in
    //    recorder mode, but the scan is cheap and correctness is free).
    last_focus_.valid = false;
    if (cur_ >= 0 && cur_ < static_cast<int>(shots_.size()) &&
        !shots_[static_cast<size_t>(cur_)].focus.empty()) {
        const char* hint = focus_window_hint(shots_[static_cast<size_t>(cur_)].focus);
        if (hint && *hint) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            for (ImGuiWindow* w : g.Windows) {
                if (!w || !w->WasActive || w->Hidden) continue;
                if (std::string(w->Name).find(hint) == std::string::npos) continue;
                const float bx0 = w->Pos.x - 4.0f, by0 = w->Pos.y - 4.0f;
                const float bx1 = bx0 + w->Size.x + 8.0f, by1 = by0 + w->Size.y + 8.0f;
                last_focus_ = FocusBox{ bx0, by0, bx1 - bx0, by1 - by0, true };
                break;
            }
        }
    }

    // Fade the lightbox in/out (~200ms) instead of popping. The fade-out
    // keeps dimming the departing panel via fade_box_ after the shot's
    // focus clears; a focused frame retargets fade_box_ every frame so
    // docked-layout moves still track.
    constexpr float kFadeS = 0.2f;
    const float fade_step = io.DeltaTime > 0.0f ? io.DeltaTime / kFadeS : 1.0f;
    if (last_focus_.valid) {
        fade_box_ = last_focus_;
        lightbox_alpha_ = std::min(1.0f, lightbox_alpha_ + fade_step);
    } else {
        lightbox_alpha_ = std::max(0.0f, lightbox_alpha_ - fade_step);
    }
    if (fade_box_.valid && lightbox_alpha_ > 0.01f) {
        const float bx0 = fade_box_.x, by0 = fade_box_.y;
        const float bx1 = fade_box_.x + fade_box_.w, by1 = fade_box_.y + fade_box_.h;
        const ImVec2 v0(0.0f, 0.0f);
        const ImVec2 v1(io.DisplaySize.x, io.DisplaySize.y);
        // Dim everything except the panel (four rects, no stencil).
        // Between the lesson's track (92) and beat (184) alphas: the
        // panel must pop while the chart stays readable as context.
        const ImU32 dim = IM_COL32(0, 0, 0,
            static_cast<int>(150.0f * lightbox_alpha_));
        dl->AddRectFilled(v0, ImVec2(v1.x, by0), dim);
        dl->AddRectFilled(ImVec2(v0.x, by1), v1, dim);
        dl->AddRectFilled(ImVec2(v0.x, by0), ImVec2(bx0, by1), dim);
        dl->AddRectFilled(ImVec2(bx1, by0), ImVec2(v1.x, by1), dim);
        // Brand-accent ring (#35c9c4 - the one accent, tokens.css).
        dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1),
                    IM_COL32(53, 201, 196,
                             static_cast<int>(235.0f * lightbox_alpha_)),
                    3.0f, 0, 1.6f);
    }

    // ── Badge - export style (recorder_glue.cpp export_tick_and_render): the
    //    EDGEDEPTH line only, no REC dot, no timer. This is a produced video.

    ImFont* f_line = Theme::Fonts::ui_semibold();
    ImGui::PushFont(f_line);
    const ImVec2 line_sz = ImGui::CalcTextSize(badge_);
    ImGui::PopFont();

    const float pad_x = 10.0f, pad_y = 6.0f;
    const float w = pad_x + line_sz.x + pad_x;
    const float h = pad_y * 2.0f + line_sz.y;
    // rec_focus suppresses the shell in recorder mode, so top_reserve is 0 -
    // but keep the same placement rule as P1/export for consistency.
    const float x = io.DisplaySize.x - w - 12.0f;
    const float y = LayoutManager::top_reserve + 10.0f;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      Theme::u32(Theme::Tokens::BASE, 0.88f), Theme::Radius::R3);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                Theme::u32(Theme::Tokens::BD2), Theme::Radius::R3, 0, 1.0f);
    ImGui::PushFont(f_line);
    dl->AddText(ImVec2(x + pad_x, y + pad_y),
                Theme::u32(Theme::Tokens::TX1, 0.95f), badge_);
    ImGui::PopFont();
}

}  // namespace edu
