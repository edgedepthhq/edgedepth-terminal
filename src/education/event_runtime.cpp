// ═══════════════════════════════════════════════════════════════════════════════
// event_runtime.cpp - see event_runtime.h
// ═══════════════════════════════════════════════════════════════════════════════

#include "education/event_runtime.h"
#include "education/transport_emit.h"
#include "core/app_context.h"
#include "core/education_boot.h"
#include "replayer/replay_manager.h"

#include <nlohmann/json.hpp>
#include <algorithm>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

namespace edu {

void EventRuntime::cmd_set_paused(bool paused) {
    pending_transport_ = paused ? TransportCmd::Pause : TransportCmd::Play;
}

void EventRuntime::cmd_set_speed(int centi_speed) {
    pending_speed_     = static_cast<float>(centi_speed) / 100.0f;
    pending_transport_ = TransportCmd::SetSpeed;
}

void EventRuntime::cmd_seek_progress(int milli_progress, bool deliberate) {
    pending_seek_progress_   = static_cast<float>(milli_progress) / 1000.0f;
    pending_seek_deliberate_ = deliberate;
    pending_seek_            = true;
}

void EventRuntime::cmd_skip(int seconds) {
    pending_skip_seconds_ = seconds;
    pending_skip_         = true;
}

void EventRuntime::cmd_restart() {
    pending_restart_ = true;
}

void EventRuntime::update(const AppContext& ctx) {
    if (!ctx.replayer) return;
    ReplayManager& rm = ctx.replay_mgr();

    // One-shot deep-link seek (?event=…&t=…): a key-moment link on the public
    // event page lands the replay AT its timestamp. Deferred until the session
    // is genuinely live - Playing or Paused, i.e. joined AND past the buffering
    // gate - so the ONE deliberate seek hits a primed, seeded book - then the
    // latch closes and the user owns the transport.
    if (!deep_link_seek_done_) {
        // Event deep link (?event=…&t=…) or the pack's start-at / ?t= target
        // (__EDGEDEPTH_PACK__.seekToMs - /demo bakes the catalog startAtMs or a
        // key-moment deep link into it). Same one-shot deliberate-seek contract.
        int64_t want = EducationBoot::instance().event_seek_to_ms();
        if (want <= 0) want = EducationBoot::instance().pack_seek_to_ms();
        if (want <= 0) {
            deep_link_seek_done_ = true;  // no deep link - never check again
        } else if (rm.state() == ReplayManager::State::Playing ||
                   rm.state() == ReplayManager::State::Paused) {
            // Playing/Paused ONLY. The old gate (is_active && !is_loading &&
            // !Seeking) admitted Creating/Joining - is_active() counts them and
            // is_loading() covers only Buffering - so the seek fired while the
            // session POST was still in flight. That premature seek (a) went out
            // against no session, drawing the backend's "No active replay" error
            // (→ the fatal historical-error overlay), and (b) transitioned the
            // state machine to Seeking, which made on_session_created's
            // `state != Creating` guard silently DISCARD the created session -
            // the join was never sent and the event page died on
            // "Historical replay unavailable" (every ?event=…&t= deep link).
            int64_t start = rm.info().start_time_ms;
            int64_t end   = rm.info().end_time_ms;
            if (end <= start) {
                start = EducationBoot::instance().event_start_ms();
                end   = EducationBoot::instance().event_end_ms();
            }
            if (end > start) {
                const int64_t target = std::clamp<int64_t>(want, start, end);
                rm.seek(target, /*deliberate=*/true);
                deep_link_seek_done_ = true;
            }
        }
    }

    // "Watch again" (one latch, drained before transport so a queued restart
    // wins). Ended pack (Stopped tears down the replay data context, so a seek
    // cannot revive it) → re-request the pack: the same boot main.cpp did,
    // fresh engine + context, buffering gate and all. A still-active session -
    // or the box event path, whose session survives finish - restarts as a
    // deliberate seek to the window start (auto-resumes, box semantics).
    if (pending_restart_) {
        pending_restart_ = false;
        const auto& boot = EducationBoot::instance();
        if (boot.is_pack() && !rm.is_active() && !boot.pack_url().empty()) {
            // Watch-again starts from the TOP: the start-at deep link was for
            // the first impression only, so keep the one-shot latch closed.
            deep_link_seek_done_ = true;
            rm.request_pack_replay(boot.pack_url(), boot.pack_symbol(), 1.0f);
        } else if (rm.is_active()) {
            int64_t start = rm.info().start_time_ms;
            const int64_t end = rm.info().end_time_ms;
            if (end <= start) start = boot.event_start_ms();
            if (start > 0) rm.seek(start, /*deliberate=*/true);
        }
    }

    // Transport (pause/play/speed) - applies to the live archive replay.
    if (pending_transport_ != TransportCmd::None) {
        const TransportCmd cmd = pending_transport_;
        pending_transport_ = TransportCmd::None;
        switch (cmd) {
            case TransportCmd::Pause:    rm.pause();  break;
            case TransportCmd::Play:     rm.resume(); break;
            case TransportCmd::SetSpeed: rm.set_speed(pending_speed_); break;
            case TransportCmd::None:     break;
        }
    }

    // Seek (own latch - don't clobber a queued pause/speed). Convert the window
    // fraction → target ms; seek() clamps. Prefer the backend's archive window once
    // it has arrived; before that, fall back to the web-provided window.
    if (pending_seek_) {
        pending_seek_ = false;
        int64_t start = rm.info().start_time_ms;
        int64_t end   = rm.info().end_time_ms;
        if (end <= start) {
            start = EducationBoot::instance().event_start_ms();
            end   = EducationBoot::instance().event_end_ms();
        }
        if (end > start) {
            const int64_t range  = end - start;
            const int64_t target = start +
                static_cast<int64_t>(pending_seek_progress_ * static_cast<float>(range));
            rm.seek(target, pending_seek_deliberate_);
        }
    }

    // Skip (own latch) - clock-only nudge from the LIVE interpolated clock (not the
    // lagging info_.current_time_ms), same path the studio bar uses.
    if (pending_skip_) {
        pending_skip_ = false;
        const int64_t now    = rm.interpolated_time_ms();
        const int64_t target = now + static_cast<int64_t>(pending_skip_seconds_) * 1000;
        if (pending_skip_seconds_ >= 0) rm.skip_forward_to(target);
        else                            rm.skip_backward_to(target);
    }
}

void EventRuntime::emit_state(const AppContext& ctx) {
#ifdef __EMSCRIPTEN__
    if (!ctx.replayer) return;
    const ReplayManager& rm = ctx.replay_mgr();

    // Window: prefer the backend's archive window once it has arrived (authoritative),
    // else the web-provided window so the transport renders from frame one. Either
    // way we need a valid (end > start) window to report anything.
    int64_t start_ms = rm.info().start_time_ms;
    int64_t end_ms   = rm.info().end_time_ms;
    if (end_ms <= start_ms) {
        start_ms = EducationBoot::instance().event_start_ms();
        end_ms   = EducationBoot::instance().event_end_ms();
    }
    if (end_ms <= start_ms) return;  // no window yet - nothing to report

    const bool active  = rm.is_active();
    const bool playing = rm.is_playing();
    const bool paused  = rm.is_paused();
    const bool loading = rm.is_loading() || rm.state() == ReplayManager::State::Seeking;
    // Ended = the replay ran to replay_finished (Stopped). Distinct from "not
    // started" because emit_state never fires before a window exists; drives
    // the chrome's "Watch again" state (pack /demo: the native transport hides
    // on Stopped and v1 relied on a page reload).
    const bool ended   = rm.state() == ReplayManager::State::Stopped;
    const float speed  = rm.info().speed;

    // Discrete-field signature (clock/progress EXCLUDED - React interpolates).
    transport::SigHasher hasher;
    hasher.mix(active  ? 1u : 0u);
    hasher.mix(playing ? 1u : 0u);
    hasher.mix(paused  ? 1u : 0u);
    hasher.mix(loading ? 1u : 0u);
    hasher.mix(ended   ? 1u : 0u);
    hasher.mix(static_cast<uint64_t>(static_cast<int>(speed * 100.0f)));

    if (!transport::should_emit(hasher.value(), emit_sig_, active, playing, paused, loading,
                                emscripten_get_now(), last_clock_emit_ms_)) {
        return;  // nothing discrete changed & no clock tick due
    }

    // Inactive clock: a FINISHED replay reports the window end (a completed bar
    // at 0% looks broken), anything else (pre-start) reports the start.
    int64_t now_ms = active ? rm.interpolated_time_ms() : (ended ? end_ms : start_ms);
    now_ms = std::clamp<int64_t>(now_ms, start_ms, end_ms);
    now_ms = transport::apply_clock_floor(now_ms, clock_floor_ms_, active, playing, paused, loading);

    json st;
    st["v"] = 1;
    if (EducationBoot::instance().is_pack()) {
        // Embedded pack (/demo): symbol from the pack handoff; event id from the
        // synthesized session id ("pack:<event_id>", set by replay_joined).
        st["symbol"] = EducationBoot::instance().pack_symbol();
        const std::string& sid = rm.info().session_id;
        st["eventId"] = (sid.rfind("pack:", 0) == 0) ? sid.substr(5) : sid;
    } else {
        st["symbol"]  = EducationBoot::instance().event_symbol();
        st["eventId"] = EducationBoot::instance().event_id();
    }
    st["startMs"] = start_ms;
    st["endMs"]   = end_ms;
    st["clockMs"] = now_ms;
    st["active"]  = active;
    st["playing"] = playing;
    st["paused"]  = paused;
    st["loading"] = loading;
    st["ended"]   = ended;
    st["speed"]   = speed;
    {
        const int64_t span = std::max<int64_t>(1, end_ms - start_ms);
        const int64_t off  = std::clamp<int64_t>(now_ms - start_ms, 0, span);
        st["progress"] = static_cast<double>(off) / static_cast<double>(span);
    }

    transport::dispatch_state("edgedepth:event", "__EDGEDEPTH_EVENT_STATE__", st.dump());
#else
    (void)ctx;
#endif
}

} // namespace edu

// ═══════════════════════════════════════════════════════════════════════════════
// KEEPALIVE command callbacks - React calls these via Module.__event_cmd_*().
// They only ENQUEUE; EventRuntime::update(ctx) applies them with a live ctx.
// Mirrors the _studio_cmd_* family: int args (centi-speed / milli-progress) marshal
// cleanly across the raw export, unlike a float (f64→f32 mis-marshal).
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef __EMSCRIPTEN__
extern "C" {

EMSCRIPTEN_KEEPALIVE void _event_cmd_set_paused(int paused) {
    edu::EventRuntime::instance().cmd_set_paused(paused != 0);
}
EMSCRIPTEN_KEEPALIVE void _event_cmd_set_speed(int centi_speed) {
    edu::EventRuntime::instance().cmd_set_speed(centi_speed);
}
EMSCRIPTEN_KEEPALIVE void _event_cmd_seek(int milli_progress, int deliberate) {
    edu::EventRuntime::instance().cmd_seek_progress(milli_progress, deliberate != 0);
}
EMSCRIPTEN_KEEPALIVE void _event_cmd_skip(int seconds) {
    edu::EventRuntime::instance().cmd_skip(seconds);
}
EMSCRIPTEN_KEEPALIVE void _event_cmd_restart() {
    edu::EventRuntime::instance().cmd_restart();
}

} // extern "C"
#endif
