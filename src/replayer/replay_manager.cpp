// ═══════════════════════════════════════════════════════════════════════════════
// replay_manager.cpp — Client-side replay orchestration
//
// Two-phase startup:
//   1. POST /replay/session → session_id (via EM_ASM XHR, async)
//   2. WS join_replay { session_id, entitlement_token } → backend verifies
//      ownership, then streams data
//
// All control (pause/resume/seek/speed/stop) uses WS control_replay messages.
// Backend status updates arrive as replay_status WS messages.
//
// The control bar is an ImGui overlay at the bottom of the viewport.
// It's always-on-top, non-dockable, and doesn't interfere with the
// dockspace or any widget interactions.
// ═══════════════════════════════════════════════════════════════════════════════

#include "replay_manager.h"
#include "pack_replay_engine.h"
#include "core/websocket.h"
#include "core/candle_manager.h"
#include "core/orderbook_manager.h"
#include "core/heatmap_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/indicator_series.h"
#include "core/message_handler.h"
#include "core/preview_candle_store.h"
#include "core/symbol_metadata.h"
#include "core/volume_profile_manager.h"
#include "core/education_boot.h"
#include "core/entitlements.h"
#include "core/usage_emit.h"
#include "core/recorder_glue.h"
#include "core/display_time_zone.h"
#include "ui/upsell_modal.h"
#include "ui/research_moment_panel.h"  // Esc arbitration: panel close vs replay stop
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "types/frame_profiler.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <ctime>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

using json = nlohmann::json;

// ── Control-bar layout + palette (mirrors GuidedReplayBar.tsx / tokens.css) ──
// The native live-terminal replay bar is styled to match the React Guided-Replay
// transport: dark panel + hairline top border, a circular play orb, a cyan
// scrubber with an amber playhead, and segmented speed pills.
namespace {
constexpr float kBarH = 54.0f;  // control-bar height (px, logical)

// Vertically center the next ImGui item (of height item_h) within the bar. Call
// AFTER SameLine() since SameLine snaps Y back to the previous item's line.
inline void center_item_y(float item_h) {
    ImGui::SetCursorPosY((kBarH - item_h) * 0.5f);
}
// ImVec4 token → packed color (optional alpha multiplier).
inline ImU32 tok(const ImVec4& c, float a = 1.0f) { return Theme::u32(c, a); }

// Width (px) the scrubber must leave on its right for the right-aligned group
// (speed pills + buffer dot + close). Recomputed each frame in render_control_bar
// and consumed by render_timeline_scrubber. Single ReplayManager instance, so a
// file-static carries it between the two without a header change.
float g_scrub_reserve = 340.0f;
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Global pointer for EM_ASM callbacks (single ReplayManager instance)
// ═══════════════════════════════════════════════════════════════════════════════
static ReplayManager* g_replay_instance = nullptr;

#ifdef __EMSCRIPTEN__
EM_JS(char*, edx_copy_replay_token, (), {
    var token = (window.__EDGEDEPTH_REPLAY_TOKEN__ || "").toString();
    var len = lengthBytesUTF8(token) + 1;
    var buf = _malloc(len);
    stringToUTF8(token, buf, len);
    return buf;
});

static std::string current_replay_token() {
    char* raw = edx_copy_replay_token();
    if (!raw) return {};
    std::string token(raw);
    std::free(raw);
    return token;
}

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void _replay_session_created(const char* json_str, int len) {
        if (g_replay_instance) {
            g_replay_instance->on_session_created(json_str, len);
        }
    }

    EMSCRIPTEN_KEEPALIVE
    void _replay_session_error(int status, const char* msg) {
        if (g_replay_instance) {
            g_replay_instance->on_session_error(status, msg);
        }
    }
}
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════════

ReplayManager::ReplayManager(WebSocketClient* ws_client)
    : ws_client_(ws_client)
{
    g_replay_instance = this;
}

ReplayManager::~ReplayManager() = default;

// ═══════════════════════════════════════════════════════════════════════════════
// Replay Initiation
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::request_replay(
    const std::vector<std::string>& symbols,
    int64_t start_time_ms,
    int64_t end_time_ms,
    float speed,
    int64_t timeframe_seconds)
{
    // ── Free-tier pre-gate (UX funnel; the Go backend is the real enforcer) ──
    // Runs BEFORE tearing down any active replay, so a blocked attempt never kills
    // the current session. A doomed request opens the upsell here instead of
    // round-tripping to a TIER_* error. Pro/admin skip it entirely. Embedded modes
    // (lesson/studio/event) also skip it: those replays are authorized by a signed
    // GRANT token, so the backend validates REQUEST ⊆ GRANT and tier window/symbol
    // rules do not apply (a free-to-all lesson on old data must play).
    if (!Entitlements::is_pro() && !EducationBoot::instance().is_embedded()) {
        for (const auto& s : symbols) {
            if (!Entitlements::symbol_replay_allowed(s)) {
                ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Symbol);
                return;
            }
        }
        const int64_t end_probe = (end_time_ms == 0)
            ? start_time_ms + (2LL * 3600LL * 1000LL) : end_time_ms;
        if (!Entitlements::range_replayable(start_time_ms, end_probe, now_ms())) {
            ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Range);
            return;
        }
    }

    if (is_active()) {
        stop();
        // Full reset — clears session state so the new replay starts clean.
        // The backend stop is async (Poison + resumeAllLiveConsumers), but
        // the POST for the new session goes through REST (not the same WS
        // message path), so it won't race with the stop control message.
        reset();
    }

    // Default end time: start + 2 hours
    if (end_time_ms == 0) {
        end_time_ms = start_time_ms + (2LL * 3600LL * 1000LL);
    }

    // Clamp speed — tier cap first (free ≤2×, silent, mirrors the server clamp),
    // then the absolute preset range.
    speed = static_cast<float>(Entitlements::clamp_speed_for_tier(speed));
    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);

    // Store pending request
    pending_.symbols = symbols;
    pending_.start_time_ms = start_time_ms;
    pending_.end_time_ms = end_time_ms;
    pending_.speed = speed;
    pending_.timeframe_seconds = timeframe_seconds;
    pending_.is_archive = false;

    // Populate info for immediate UI feedback
    info_.symbols = symbols;
    info_.start_time_ms = start_time_ms;
    info_.end_time_ms = end_time_ms;
    info_.current_time_ms = start_time_ms;
    info_.speed = speed;
    info_.timeframe_ms = timeframe_seconds * 1000;  // Use caller's timeframe
    info_.session_created_at = now_ms();

    transition(State::Creating);

    // Find closest speed preset
    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        if (std::abs(SPEED_PRESETS[i] - speed) < 0.01f) {
            current_speed_preset_idx_ = i;
            break;
        }
    }

    // Build JSON payload
    json payload;
    payload["symbols"] = symbols;
    // Convert ms → ISO 8601 string for the REST API
    // The backend accepts both unix timestamps and ISO strings
    payload["start_timestamp"] = start_time_ms;
    payload["end_timestamp"] = end_time_ms;
    payload["speed"] = speed;

    std::string body = payload.dump();

    char start_buf[32], end_buf[32];
    format_timestamp(start_time_ms, start_buf, sizeof(start_buf));
    format_timestamp(end_time_ms, end_buf, sizeof(end_buf));

#ifdef __EMSCRIPTEN__
    // Fire async XHR to create replay session
    EM_ASM({
        var body = UTF8ToString($0);
        var xhr = new XMLHttpRequest();
        xhr.open('POST', 'https://api.edgedepth.com/replay/session', true);
        xhr.setRequestHeader('Content-Type', 'application/json');
        // Entitlement token (design §5.1): read the window global FRESH on each POST
        // (TerminalEmbed refreshes it on a ~9-min timer). resolveClaims() on the Go
        // side requires `Authorization: Bearer <token>` (default-deny).
        try {
            var __edx_tok = (window.__EDGEDEPTH_REPLAY_TOKEN__ || "").toString();
            if (__edx_tok) xhr.setRequestHeader('Authorization', 'Bearer ' + __edx_tok);
        } catch (e) {}

        xhr.onload = function() {
            if (xhr.status === 200 || xhr.status === 201) {
                var text = xhr.responseText;
                var len = lengthBytesUTF8(text);
                var buf = _malloc(len + 1);
                stringToUTF8(text, buf, len + 1);
                __replay_session_created(buf, len);
                _free(buf);
            } else {
                // Forward the JSON body ({error, code}) so on_session_error maps the
                // coded gate (TIER_* / AUTH_REQUIRED / GRANT_REQUIRED) to the right UX.
                var errMsg = xhr.responseText || xhr.statusText || 'Unknown error';
                var len = lengthBytesUTF8(errMsg);
                var buf = _malloc(len + 1);
                stringToUTF8(errMsg, buf, len + 1);
                __replay_session_error(xhr.status, buf);
                _free(buf);
            }
        };

        xhr.onerror = function() {
            var msg = 'Network error';
            var len = lengthBytesUTF8(msg);
            var buf = _malloc(len + 1);
            stringToUTF8(msg, buf, len + 1);
            __replay_session_error(0, buf);
            _free(buf);
        };

        xhr.send(body);
    }, body.c_str());
#else
    // Non-WASM fallback (for testing)
    info_.error_message = "Replay requires WASM build";
    transition(State::Error);
#endif
}

void ReplayManager::request_replay_from(
    const std::string& symbol,
    int64_t timestamp_ms,
    float speed,
    int64_t timeframe_seconds)
{
    // Start exactly at the requested time, end 2 hours after
    int64_t end = timestamp_ms + (2LL * 3600LL * 1000LL);
    request_replay({symbol}, timestamp_ms, end, speed, timeframe_seconds);
}

void ReplayManager::request_archive_replay(
    const std::string& event_id,
    const std::string& symbol,
    float speed)
{
    if (is_active()) {
        stop();
        reset();
    }

    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);

    // Pending request — archive path. The backend resolves event_id → archive_path
    // and defines the replay window, so start/end stay 0 here (filled by the first
    // replay_status). is_archive flags the session type for any downstream logic.
    pending_.symbols       = symbol.empty() ? std::vector<std::string>{}
                                            : std::vector<std::string>{symbol};
    pending_.start_time_ms = 0;
    pending_.end_time_ms   = 0;
    pending_.speed         = speed;
    pending_.is_archive    = true;
    pending_.event_id      = event_id;

    // Minimal info_ for immediate UI feedback. Window/progress arrive with the
    // first replay_status from the backend (archive window is server-defined).
    if (!symbol.empty()) info_.symbols = { symbol };
    info_.session_type       = "archive";
    info_.speed              = speed;
    info_.session_created_at = now_ms();
    // Archive/box event replays open at 1m candles (not the 5m ReplayInfo default).
    // The live "Replay from here" path stays untouched: request_replay sets its own
    // timeframe explicitly, so this only affects the event chrome default.
    info_.timeframe_ms       = 60000;

    transition(State::Creating);

    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        if (std::abs(SPEED_PRESETS[i] - speed) < 0.01f) { current_speed_preset_idx_ = i; break; }
    }

    // POST /replay/session/archive — body matches CreateArchiveReplayRequest
    // (event_id + speed; backend resolves the path). Streams default server-side.
    json payload;
    payload["event_id"] = event_id;
    payload["speed"]    = speed;
    std::string body = payload.dump();

#ifdef __EMSCRIPTEN__
    // Same async XHR + callback pair as request_replay, but the archive endpoint.
    // The session-created/-error callbacks are session-type-agnostic (they read
    // only session_id + status), so on_session_created → join_session() just works.
    // NOTE: no withCredentials — mirrors request_replay (cross-origin api.edgedepth.com;
    // the SSR /terminal?event= gate already authorizes the user before this runs).
    EM_ASM({
        var body = UTF8ToString($0);
        var xhr = new XMLHttpRequest();
        xhr.open('POST', 'https://api.edgedepth.com/replay/session/archive', true);
        xhr.setRequestHeader('Content-Type', 'application/json');
        // Entitlement token (design §5.1): read the window global FRESH on each POST
        // (TerminalEmbed refreshes it on a ~9-min timer). resolveClaims() on the Go
        // side requires `Authorization: Bearer <token>` (default-deny).
        try {
            var __edx_tok = (window.__EDGEDEPTH_REPLAY_TOKEN__ || "").toString();
            if (__edx_tok) xhr.setRequestHeader('Authorization', 'Bearer ' + __edx_tok);
        } catch (e) {}

        xhr.onload = function() {
            if (xhr.status === 200 || xhr.status === 201) {
                var text = xhr.responseText;
                var len = lengthBytesUTF8(text);
                var buf = _malloc(len + 1);
                stringToUTF8(text, buf, len + 1);
                __replay_session_created(buf, len);
                _free(buf);
            } else {
                // Forward the JSON body ({error, code}) so on_session_error maps the
                // coded gate (TIER_* / AUTH_REQUIRED / GRANT_REQUIRED) to the right UX.
                var errMsg = xhr.responseText || xhr.statusText || 'Unknown error';
                var len = lengthBytesUTF8(errMsg);
                var buf = _malloc(len + 1);
                stringToUTF8(errMsg, buf, len + 1);
                __replay_session_error(xhr.status, buf);
                _free(buf);
            }
        };

        xhr.onerror = function() {
            var msg = 'Network error';
            var len = lengthBytesUTF8(msg);
            var buf = _malloc(len + 1);
            stringToUTF8(msg, buf, len + 1);
            __replay_session_error(0, buf);
            _free(buf);
        };

        xhr.send(body);
    }, body.c_str());
#else
    info_.error_message = "Replay requires WASM build";
    transition(State::Error);
#endif
}

void ReplayManager::request_pack_replay(
    const std::string& pack_url,
    const std::string& symbol_hint,
    float speed)
{
    if (is_active()) {
        stop();
        reset();
    }

    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);

    pack_mode_ = true;
    if (!symbol_hint.empty()) info_.symbols = { symbol_hint };
    info_.session_type       = "archive";  // pack replays ARE archive replays, CDN-served
    info_.speed              = speed;
    info_.session_created_at = now_ms();
    // /demo opens at 1s candles (not the 5m ReplayInfo default): the pack builds
    // 1s/15s/30s from bundle trades, and the chart's visible TF tracks the replay
    // candle manager built from info_.timeframe_ms in create_replay_data_context().
    info_.timeframe_ms       = 1000;

    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        if (std::abs(SPEED_PRESETS[i] - speed) < 0.01f) { current_speed_preset_idx_ = i; break; }
    }

    // No POST, no join — the engine fetches the pack header and synthesizes
    // replay_joined through handle_ws_message. Window/symbols come from the
    // header (authoritative).
    transition(State::Creating);
    pack_engine_ = std::make_unique<PackReplayEngine>(this);
    pack_engine_->begin(pack_url, speed);
}

void ReplayManager::tick_pack_engine() {
    if (pack_engine_) pack_engine_->tick();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Session Creation Callbacks (from EM_ASM XHR)
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::on_session_created(const char* response_json, int len) {
    if (info_.state != State::Creating) {
        return;
    }

    try {
        auto resp = json::parse(std::string(response_json, len));

        if (resp.contains("session_id")) {
            info_.session_id = resp["session_id"].get<std::string>();
        }
        if (resp.contains("status")) {
            std::string status = resp["status"].get<std::string>();
            if (status == "error") {
                info_.error_message = resp.value("message", "Session creation failed");
                transition(State::Error);
                return;
            }
        }

        // Mirror the server's per-UTC-day session count for the "N of 6 today" chip
        // (server enforces TIER_DAILY; this is display-only, free tier).
        if (!Entitlements::is_pro()) Entitlements::note_session_started();
        join_session();

    } catch (const std::exception& e) {
        info_.error_message = "Invalid server response";
        transition(State::Error);
    }
}

void ReplayManager::on_session_error(int status_code, const char* error_msg) {
    // The XHR forwards the JSON response body ({error, code}); parse the coded gate
    // so we map it to the right UX (design §8.4) and NEVER surface raw server text.
    const std::string body = error_msg ? error_msg : "";
    std::string code, human;
    try {
        if (!body.empty() && body.front() == '{') {
            auto j = json::parse(body);
            if (j.contains("code")  && j["code"].is_string())  code  = j["code"].get<std::string>();
            if (j.contains("error") && j["error"].is_string()) human = j["error"].get<std::string>();
        }
    } catch (const std::exception&) { /* non-JSON (network error etc.) — fall through */ }

    auto& modal = ui::UpsellModal::instance();

    // Embedded event/lesson boot: the denied replay IS the page's whole reason to
    // exist (?event= / ?lesson= chrome). Arm the modal's dismiss redirect so
    // "Maybe later" / Escape does a full navigation to the live terminal for the
    // symbol the request used, instead of stranding the user in event chrome over
    // plain live data. Boot-mode gated (NOT studio, NOT the native live-terminal
    // control-bar flow), and open() clears it, so live-terminal gates never
    // inherit a stale redirect. Must be called AFTER modal.open*/open_login.
    const auto& boot = EducationBoot::instance();
    const bool historical_boot = boot.is_event() || boot.is_lesson();
    const char* redirect_sym = nullptr;
    if (boot.is_event()) {
        redirect_sym = boot.event_symbol().c_str();
    } else if (boot.is_lesson() && !pending_.symbols.empty()) {
        redirect_sym = pending_.symbols[0].c_str();
    }

    auto finish_gate_failure = [&](const char* message) {
        if (historical_boot) {
            info_.error_message = message;
            transition(State::Error);
        } else {
            reset();
        }
    };

    // AUTH_REQUIRED / GRANT_REQUIRED → depends on whether the viewer is signed in.
    // Only a genuinely anonymous visitor should be told to "log in" (§8.4). A
    // logged-in user hitting GRANT_REQUIRED needs Pro (upsell); one hitting
    // AUTH_REQUIRED has a token/session/config problem (the entitlement token failed
    // to mint or verify) — that's a neutral notice, NOT a "log in" they already did.
    if (code == "AUTH_REQUIRED" || code == "GRANT_REQUIRED") {
        if (!Entitlements::is_authenticated()) {
            modal.open_login();
            if (redirect_sym) modal.set_dismiss_redirect(redirect_sym);
        } else if (code == "GRANT_REQUIRED") {
            modal.open(ui::UpsellModal::Trigger::Events);
            if (redirect_sym) modal.set_dismiss_redirect(redirect_sym);
        } else {
            modal.toast("Replay unavailable. Reload to refresh your session.");
        }
        finish_gate_failure("Replay authorization failed");
        return;
    }
    // Any TIER_* gate → the one upsell modal. Map the well-known codes to a
    // contextual surface; the modal supplies the friendly copy (never raw text).
    if (code.rfind("TIER_", 0) == 0) {
        using T = ui::UpsellModal::Trigger;
        T t = T::ServerTier;
        const char* detail = nullptr;
        if      (code == "TIER_SYMBOL") t = T::Symbol;
        else if (code == "TIER_WINDOW") t = T::Range;
        else if (code == "TIER_DAILY")  t = T::Daily;
        else if (code == "TIER_CONCURRENT")
            detail = "You can run one replay at a time on the free plan. Pro runs three.";
        modal.open(t, detail);
        if (redirect_sym) modal.set_dismiss_redirect(redirect_sym);
        finish_gate_failure("Replay access is unavailable for this account");
        return;
    }

    // Uncoded / non-tier failure. Keep the message generic and never surface raw text.
    info_.error_message = (status_code == 0)
        ? "Network error. Check your connection."
        : "Couldn't start replay. Please try again.";
    transition(State::Error);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Session Join (WS)
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::join_session() {
    if (info_.session_id.empty()) {
        info_.error_message = "No session ID";
        transition(State::Error);
        return;
    }

    if (!ws_client_ || !ws_client_->is_connected()) {
        // WS not up yet? Don't error — the studio path fires request_replay as
        // soon as the wasm runtime is ready (calledRun), which is earlier than
        // the WS handshake to wss://api.edgedepth.com/ws. Latch and let
        // flush_pending_join() send it once is_connected() flips. (The session
        // is already created on the backend in Creating state; we just need a
        // live socket to join it.)
        if (!pending_join_) {
            pending_join_ = true;
            pending_join_since_ms_ = now_ms();
        }
        return;  // stay in Creating; flush_pending_join() takes over
    }

    transition(State::Joining);

    json data = {{"session_id", info_.session_id}};
#ifdef __EMSCRIPTEN__
    const std::string entitlement_token = current_replay_token();
    if (!entitlement_token.empty()) {
        data["entitlement_token"] = entitlement_token;
    }
#endif
    json msg = {{"method", "join_replay"}, {"data", std::move(data)}};

    if (!ws_client_->send_text(msg.dump())) {
        info_.error_message = "Failed to send join_replay";
        transition(State::Error);
        return;
    }

}

// Per-frame: complete a join that was deferred because the WS wasn't connected
// when the session-created callback fired. See join_session().
void ReplayManager::flush_pending_join() {
    if (!pending_join_) return;

    // A stop/reset (or a new request) between latch and flush invalidates this.
    // Only a session still in Creating with a real id is joinable.
    if (info_.session_id.empty() || info_.state != State::Creating) {
        pending_join_ = false;
        return;
    }

    if (ws_client_ && ws_client_->is_connected()) {
        pending_join_ = false;
        join_session();  // now takes the connected path
        return;
    }

    // Safety valve: don't wait forever on a socket that never comes up.
    if (now_ms() - pending_join_since_ms_ > JOIN_WS_WAIT_MAX_MS) {
        pending_join_ = false;
        info_.error_message = "WebSocket never connected for replay join";
        transition(State::Error);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Playback Control
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::pause() {
    if (info_.state != State::Playing) return;
    // Snapshot the current interpolated time so it's stable while paused.
    // Without this, info_.current_time_ms might be behind the interpolated
    // position, and resume would briefly show the old time before jumping.
    info_.current_time_ms = interpolated_time_ms();
    info_.last_status_update = now_ms();
    send_control("pause");
    transition(State::Paused);
}

void ReplayManager::resume() {
    if (info_.state != State::Paused) return;
    // Re-anchor the interpolation clock. While paused, wall time advanced
    // but market time didn't. Without this, interpolated_time_ms() would
    // calculate wall_elapsed from the stale last_status_update and jump
    // the clock forward by (pause_duration * speed).
    info_.last_status_update = now_ms();
    send_control("resume");
    transition(State::Playing);
}

void ReplayManager::toggle_pause() {
    if (info_.state == State::Playing) pause();
    else if (info_.state == State::Paused) resume();
}

void ReplayManager::stop() {
    if (!is_active()) return;
    send_control("stop");
    transition(State::Stopped);
}

void ReplayManager::set_speed(float speed) {
    speed = std::clamp(speed, MIN_SPEED, MAX_SPEED);
    if (!is_active()) return;

    // Snapshot current interpolated time so the speed change doesn't
    // cause a time jump between now and the next replay_status.
    info_.current_time_ms = interpolated_time_ms();
    info_.last_status_update = now_ms();
    // Also re-anchor local clock if active (mid-drip-feed speed change)
    if (using_local_clock_) {
        local_clock_base_ms_ = info_.current_time_ms;
        local_clock_wall_start_ = info_.last_status_update;
    }

    send_control_with_value("set_speed", "speed", static_cast<double>(speed));
    info_.speed = speed;

    // Sync preset index
    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        if (std::abs(SPEED_PRESETS[i] - speed) < 0.01f) {
            current_speed_preset_idx_ = i;
            return;
        }
    }
}

void ReplayManager::speed_up() {
    if (current_speed_preset_idx_ < NUM_SPEED_PRESETS - 1) {
        current_speed_preset_idx_++;
        set_speed(SPEED_PRESETS[current_speed_preset_idx_]);
    }
}

void ReplayManager::speed_down() {
    if (current_speed_preset_idx_ > 0) {
        current_speed_preset_idx_--;
        set_speed(SPEED_PRESETS[current_speed_preset_idx_]);
    }
}

void ReplayManager::seek(int64_t timestamp_ms, bool deliberate) {
    if (!is_active()) return;

    // No session yet → refuse WITHOUT touching the state machine. A seek fired
    // during Creating/Joining (a mistimed deep-link latch, a stray transport
    // command) cannot be serviced: the window is unknown (archive info_ is 0/0
    // → the clamp below would target garbage), the control message would draw
    // the backend's "No active replay" error, and — worst — transitioning to
    // Seeking here makes on_session_created's `state != Creating` guard discard
    // the created session, stranding the boot forever (the ?event=&t= and
    // ?replay=&t= stillborn-session bug). Callers that need a boot-time seek
    // must wait for Playing/Paused (EventRuntime / ResearchReplayShell do).
    if (info_.state == State::Creating || info_.state == State::Joining) {
        printf("[Replay] seek(%lld) ignored - session not joined yet (state=%d)\n",
               static_cast<long long>(timestamp_ms),
               static_cast<int>(info_.state));
        return;
    }

    // Debounce rapid seeks (from scrubber dragging)
    int64_t now = now_ms();
    if (!deliberate && now - last_seek_time_ms_ < SEEK_DEBOUNCE_MS) return;
    last_seek_time_ms_ = now;

    // Usage: count each committed (non-debounced) scrub; re-baseline the market
    // clock so the seek jump isn't counted as watched market-time.
    if (usage_run_active_) {
        usage_scrub_count_++;
        usage_last_market_clock_ = -1;
    }

    // Clamp to replay window
    timestamp_ms = std::clamp(timestamp_ms, info_.start_time_ms, info_.end_time_ms);
    // Seek ceiling (lesson scrub-forward lock): forward motion stops at the
    // ceiling; backward motion is never restricted. All transports and the
    // keyboard shortcuts converge here, so this is the one enforcement point.
    if (seek_ceiling_active() && timestamp_ms > seek_ceiling_ms_ &&
        timestamp_ms > info_.current_time_ms) {
        timestamp_ms = std::max(seek_ceiling_ms_, info_.start_time_ms);
        if (timestamp_ms <= info_.current_time_ms) return;  // nothing to gain
    }

    // Clear stale data BEFORE sending the seek to the backend. The backend
    // will reconnect consumers and send a fresh OB seed + data from the new
    // position. We must clear here (not in replay_seeked handler) because
    // the OB seed binary frame and the replay_seeked JSON message arrive
    // from different goroutines with no ordering guarantee — clearing in
    // replay_seeked risks wiping the newly-arrived OB seed.
    if (replay_ctx_) {
        if (replay_ctx_->candles) {
            replay_ctx_->candles->reset_for_seek(timestamp_ms);
        }
        if (replay_ctx_->orderbooks) {
            replay_ctx_->orderbooks->clear_all();
        }
        if (replay_ctx_->heatmaps) {
            replay_ctx_->heatmaps->clear_all();
        }
        if (replay_ctx_->liq_heatmaps) {
            replay_ctx_->liq_heatmaps->clear_all();
        }
        // Indicator SeriesCache: full seek → clear; the backend re-runs
        // fetchReplaySeeds (incl. fetchVPINTimeline) from the new position.
        if (replay_ctx_->series) {
            replay_ctx_->series->clear_all();
        }
    }

    send_control_with_int64("seek", "timestamp", timestamp_ms);
    info_.current_time_ms = timestamp_ms;
    transition(State::Seeking);
}

void ReplayManager::seek_to_progress(float progress) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    int64_t range = info_.end_time_ms - info_.start_time_ms;
    int64_t target = info_.start_time_ms + static_cast<int64_t>(progress * range);
    seek(target);
}

void ReplayManager::skip_forward(int64_t seconds) {
    seek(info_.current_time_ms + seconds * 1000);
}

void ReplayManager::skip_backward(int64_t seconds) {
    seek(info_.current_time_ms - seconds * 1000);
}

// ═══════════════════════════════════════════════════════════════════════════════
// State Queries

// Cumulative forward-skip distance past which the client clears its book +
// candle timeline and expects the backend's consumer-reconnect (fresh OB
// seed, no intermediates). Used per-press in skip_forward_to and re-derived
// at flush time for the pack engine — keep both in sync.
static constexpr int64_t kLargeSkipThresholdMs = 2 * 60 * 1000;  // 2 minutes

void ReplayManager::skip_forward_to(int64_t timestamp_ms) {
    if (!is_active()) return;
    timestamp_ms = std::clamp(timestamp_ms, info_.start_time_ms, info_.end_time_ms);
    // Seek ceiling (lesson scrub-forward lock): >> stops at the ceiling.
    if (seek_ceiling_active() && timestamp_ms > seek_ceiling_ms_) {
        timestamp_ms = std::max(seek_ceiling_ms_, info_.start_time_ms);
        if (timestamp_ms <= info_.current_time_ms) return;
    }

    // Cancel any pending << debounce. Without this, a << debounce armed
    // just before this >> would fire AFTER the forward skip, sending the
    // backward target to the backend and overriding our forward seek.
    if (pending_skip_deadline_ms_ > 0 && !pending_skip_is_forward_) {
        pending_skip_deadline_ms_ = 0;
        pending_skip_target_ms_ = 0;
        pending_skip_needs_flush_ = false;
    }

    // Reset OB continuity — the batch-delivered data from >> has a
    // completely different last_update_id chain. Without zeroing, the first
    // delta triggers a desync → blank DOM.
    if (replay_ctx_ && replay_ctx_->orderbooks) {
        replay_ctx_->orderbooks->reset_update_ids();
    }

    // Large forward skip detection: measure cumulative distance from the
    // pre-debounce origin, not just this individual press. Rapid >> presses
    // each jump 60s, and each one updates info_.current_time_ms. Without
    // tracking the origin, three rapid presses look like three 60s jumps
    // (none "large"), but the cumulative 180s jump triggers a backend
    // reconnect + candle timeline reload that the client must match with
    // reset_for_seek. The origin is the clock position before the first
    // >> in the current debounce window.
    // (Threshold lives at kLargeSkipThresholdMs — flush_pending_skip
    // recomputes the same verdict for the pack engine.)

    // Track the origin: if no debounce is pending, this is the first press —
    // capture current time. If debounce is already active, keep the origin.
    if (pending_skip_deadline_ms_ == 0 || !pending_skip_is_forward_) {
        skip_origin_ms_ = info_.current_time_ms;
    }
    const int64_t cumulative_jump_ms = timestamp_ms - skip_origin_ms_;
    const bool is_large_skip = cumulative_jump_ms > kLargeSkipThresholdMs;

    if (is_large_skip && replay_ctx_) {
        if (replay_ctx_->orderbooks) {
            replay_ctx_->orderbooks->clear_all();
        }
        if (replay_ctx_->candles) {
            replay_ctx_->candles->reset_for_seek(timestamp_ms);
        }
    }

    // ─── Trailing-edge debounce: coalesce rapid >> into one backend message ──
    // Every press resets the 200ms timer. One message sent on expiry via
    // flush_pending_skip(). This prevents split deliveries from rapid presses.
    const int64_t wall_now = now_ms();

    if (pending_skip_deadline_ms_ > 0 && wall_now < pending_skip_deadline_ms_ && pending_skip_is_forward_) {
    }

    pending_skip_target_ms_ = timestamp_ms;
    pending_skip_deadline_ms_ = wall_now + SKIP_DEBOUNCE_WINDOW_MS;
    pending_skip_is_forward_ = true;
    pending_skip_needs_flush_ = true;

    info_.current_time_ms = timestamp_ms;
    info_.last_status_update = wall_now;

    // For large skips, transition to Seeking so the user sees visual feedback
    // (SEEKING badge, frozen clock) during the 2-5s backend reconnect.
    // replay_seeked (lightweight) will transition back to Playing when ready.
    if (is_large_skip) {
        transition(State::Seeking);
    }
}

void ReplayManager::skip_backward_to(int64_t timestamp_ms) {
    if (!is_active()) return;
    timestamp_ms = std::clamp(timestamp_ms, info_.start_time_ms, info_.end_time_ms);

    // Cancel any pending >> debounce. Without this, a >> debounce armed
    // just before this << would fire AFTER the rewind, sending the forward
    // target to the backend and overriding our backward seek.
    if (pending_skip_deadline_ms_ > 0 && pending_skip_is_forward_) {
        pending_skip_deadline_ms_ = 0;
        pending_skip_target_ms_ = 0;
        pending_skip_needs_flush_ = false;
    }

    // Check if buffer can handle this rewind purely client-side
    bool buffer_hit = history_buffer_ && history_buffer_->contains(timestamp_ms) && replay_ctx_;

    if (!buffer_hit) {
        // Out-of-buffer: full seek via backend. Gate all binary frames until
        // the backend confirms the rewind (replay_seeked).
        rewind_pending_ = true;

        // ─── Trailing-edge debounce: coalesce ALL << presses into one backend message ───
        const int64_t wall_now = now_ms();

        if (pending_skip_deadline_ms_ > 0 && wall_now < pending_skip_deadline_ms_ && !pending_skip_is_forward_) {
            // Within debounce window — update target, extend deadline, suppress backend.
            // Trim candles immediately so the user sees the chart update even
            // though the backend message is deferred.
            pending_skip_target_ms_ = timestamp_ms;
            pending_skip_deadline_ms_ = wall_now + SKIP_DEBOUNCE_WINDOW_MS;
            pending_skip_needs_flush_ = true;

            // Always trim future candles — even during debounce
            if (replay_ctx_ && replay_ctx_->candles) {
                replay_ctx_->candles->trim_candles_after(timestamp_ms);
            }
            rewind_cutoff_ms_ = timestamp_ms;
            return;
        }

        // First press (or direction changed) — defer the send, arm the window
        pending_skip_target_ms_ = timestamp_ms;
        pending_skip_deadline_ms_ = wall_now + SKIP_DEBOUNCE_WINDOW_MS;
        pending_skip_is_forward_ = false;
        pending_skip_needs_flush_ = true;
    }
    // If buffer_hit: pure client-side rewind — don't touch the backend

    if (replay_ctx_) {
        // Trim candles after target (always — independent of buffer path)
        if (replay_ctx_->candles) {
            replay_ctx_->candles->trim_candles_after(timestamp_ms);
        }

        // Clear OB, trades, DOM, debug — they'll be rebuilt from buffer or backend
        if (replay_ctx_->orderbooks) {
            replay_ctx_->orderbooks->clear_all();
        }
        // Invalidate VPVR — forces re-request from server with correct time range.
        // tick_volume data is stored per-minute in TimescaleDB, so the backend
        // will return the correct profile for the rewound visible range.
        if (replay_ctx_->vpvr) {
            replay_ctx_->vpvr->invalidate_all();
        }
        // DON'T clear heatmaps or liq_heatmaps — they're rewind-exempt
        // (slow-moving visualizations that don't change meaningfully in 50s)
    }

    // Notify widgets to clear internal state (trades, DOM, debug, etc.)
    rewind_cutoff_ms_ = timestamp_ms;
    if (on_rewind_) {
        on_rewind_(timestamp_ms);
    }

    // Try client-side replay from history buffer
    if (history_buffer_ && history_buffer_->contains(timestamp_ms) && replay_ctx_) {
        // OB restore callback — applies snapshot directly to the OB manager
        auto ob_restore = [this](const ReplayHistoryBuffer::OBSnapshot& snap) {
            if (!replay_ctx_ || !replay_ctx_->orderbooks) return;
            auto& ob_mgr = *replay_ctx_->orderbooks;
            // Build a pair from the session symbols
            if (info_.symbols.empty()) return;
            Terminal::Pair pair{"binancef", info_.symbols[0]};
            // Build a synthetic pb::BookUpdate snapshot and apply via existing API
            pb::BookUpdate snapshot_pb;
            snapshot_pb.set_snapshot(true);
            snapshot_pb.set_last_update_id(snap.last_update_id);
            snapshot_pb.set_timestamp_ms(snap.timestamp_ms);
            snapshot_pb.set_last_price(snap.last_price);
            for (const auto& lev : snap.bids) {
                auto* bid = snapshot_pb.add_bids();
                bid->set_price(lev.price);
                bid->set_size(lev.size);
            }
            for (const auto& lev : snap.asks) {
                auto* ask = snapshot_pb.add_asks();
                ask->set_price(lev.price);
                ask->set_size(lev.size);
            }
            // Zero last_update_id in the snapshot — this bypasses the
            // continuity check in apply_book_update_from_pb for the first
            // delta after restore. The OB snapshot from the history buffer
            // was captured from read_buf which lags write_buf, so the
            // last_update_id may not chain perfectly with the next delta
            // in the buffer. Setting to 0 uses the existing sentinel path
            // ("skip continuity check, chain normally from next delta").
            snapshot_pb.set_last_update_id(0);
            ob_mgr.apply_orderbook_snapshot_from_pb(pair, snapshot_pb);
        };

        // Message replay callback — dispatch through normal pipeline
        int replay_dispatch_count = 0;
        auto replay_msg = [this, &replay_dispatch_count](const uint8_t* data, size_t len) {
            if (!replay_ctx_) return;
            std::string msg_data(reinterpret_cast<const char*>(data), len);
            MessageContext ctx = replay_message_context();
            MessageHandler::handle_message(msg_data, ctx);
            ++replay_dispatch_count;
        };

        // Suppress candle messages during buffer replay only (not ongoing).
        // Server candle messages carry accumulated OHLCV for their tick
        // period — replaying them after a rewind would re-introduce price
        // data from after the rewind target. Trades rebuild candles correctly.
        if (replay_ctx_->candles) {
            replay_ctx_->candles->set_suppress_candle_messages(true);
        }

        // Replay up to target + 1s slack. This rebuilds state AT the target
        // (OB snapshot, recent trades for the trades widget). The backend
        // delivers everything after the target once it processes our seek.
        bool ok = history_buffer_->replay_from(timestamp_ms, timestamp_ms + 1000, ob_restore, replay_msg);
        (void)ok;

        // Re-enable candle messages — the backend will stream fresh data
        // from the rewind target once it processes our skip_forward.
        if (replay_ctx_->candles) {
            replay_ctx_->candles->set_suppress_candle_messages(false);
        }

        // Tell the backend to seek to the rewind target using trailing-edge
        // debounce. Rapid << presses coalesce into ONE backend message.
        // This is critical because each immediate send increments
        // lightweight_seek_count_, and if the backend coalesces rapid seeks
        // (sending fewer replay_seeked than skip_forwards received), the
        // count never reaches 0 and rewind_pending_ stays true forever.
        //
        // The buffer replay already gave instant visual feedback. The
        // debounce adds 200ms before the backend starts seeking — invisible
        // since the chart already looks correct.
        rewind_pending_ = true;

        // Arm/update the trailing-edge debounce for backward skip.
        // flush_pending_skip() will send the final coalesced target.
        pending_skip_target_ms_ = timestamp_ms;
        pending_skip_deadline_ms_ = now_ms() + SKIP_DEBOUNCE_WINDOW_MS;
        pending_skip_is_forward_ = false;
        pending_skip_needs_flush_ = true;
    } else {
    }

    info_.current_time_ms = timestamp_ms;
    info_.last_status_update = now_ms();
}

// ═══════════════════════════════════════════════════════════════════════════════

bool ReplayManager::is_active() const {
    switch (info_.state) {
        case State::Creating:
        case State::Joining:
        case State::Buffering:
        case State::Playing:
        case State::Paused:
        case State::Seeking:
            return true;
        default:
            return false;
    }
}

bool ReplayManager::context_primed() const {
    if (!replay_ctx_ || !replay_ctx_->candles) return false;
    if (replay_ctx_->candles->count() == 0) return false;       // chart still empty

    // Old servers and local pack lifecycle frames may omit `streams`; preserve
    // the safe historical default and require the book in that case. When the
    // server supplies its effective grant, do not wait for a stream it excluded.
    const bool orderbook_expected = info_.streams.empty() ||
        std::find(info_.streams.begin(), info_.streams.end(), "orderbook") != info_.streams.end() ||
        std::find(info_.streams.begin(), info_.streams.end(), "depth") != info_.streams.end();
    if (!orderbook_expected) return true;

    if (!replay_ctx_->orderbooks) return false;
    if (info_.symbols.empty()) return false;
    const Terminal::Pair pair{"binancef", info_.symbols[0]};
    const Terminal::Orderbook* ob = replay_ctx_->orderbooks->get_orderbook(pair);
    if (!ob) return false;
    // Renderable as soon as the chart has candles AND the OB seed has filled the
    // book (last_update_id is set by the snapshot). The seed is a real full-depth
    // snapshot, so the terminal shows true market state immediately; live depth
    // deltas then animate it.
    //
    // We previously required >=5 INCREMENTAL deltas here. But the playback clock
    // runs at 1x while in Buffering, so a window that starts in a depth-sparse
    // stretch made the gate wait that many seconds of *market* time before the
    // first frame — measured ~8-10s on btcusdt even though the seed + candles were
    // ready in ~1s. delta_updates is still tracked (diagnostics / smoothness), it's
    // just no longer the readiness bar.
    return ob->last_update_id != 0;
}

void ReplayManager::tick_buffering_gate() {
    if (info_.state != State::Buffering) {
        buffering_since_ms_ = 0;
        return;
    }
    // First frame in Buffering — stamp the entry time for the safety valve.
    if (buffering_since_ms_ == 0) buffering_since_ms_ = now_ms();

    const bool primed   = context_primed();
    const bool timed_out = (now_ms() - buffering_since_ms_) > BUFFERING_MAX_MS;

    // Release to Playing once the context has real data, OR after the safety
    // window so an empty/stalled session can't hang the spinner forever.
    if (primed || timed_out) {
        // Re-anchor the interpolation clock so the held wall-time doesn't get
        // counted as elapsed market time on the first Playing frame.
        info_.last_status_update = now_ms();
        transition(State::Playing);
        buffering_since_ms_ = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// WS Message Handling
// ═══════════════════════════════════════════════════════════════════════════════

bool ReplayManager::handle_ws_message(const std::string& type, const void* json_data) {
    const json& msg = *static_cast<const json*>(json_data);

    // Backend sends {"type":"...", "data":{payload}, "session_id":"..."}.
    // Extract the inner "data" object where the actual payload lives.
    // Fallback to the full message if "data" is missing (e.g. error messages).
    const json& data = (msg.contains("data") && msg["data"].is_object())
                       ? msg["data"]
                       : msg;

    if (type == "replay_joined") {
        if (data.contains("session_id")) {
            info_.session_id = data["session_id"].get<std::string>();
        }
        if (data.contains("type")) {
            info_.session_type = data["type"].get<std::string>();
        }
        if (data.contains("symbols")) {
            info_.symbols.clear();
            for (const auto& s : data["symbols"]) {
                info_.symbols.push_back(s.get<std::string>());
            }
        }
        if (data.contains("streams") && data["streams"].is_array()) {
            info_.streams.clear();
            for (const auto& stream : data["streams"]) {
                if (stream.is_string()) info_.streams.push_back(stream.get<std::string>());
            }
        }
        if (data.contains("speed")) {
            info_.speed = data["speed"].get<float>();
        }
        if (data.contains("start_time")) {
            info_.start_time_ms = data["start_time"].get<int64_t>();
        }
        if (data.contains("end_time")) {
            info_.end_time_ms = data["end_time"].get<int64_t>();
        }
        // Anchor the clock at the window start so the held (Buffering) clock
        // shows the start, not 0, while priming.
        info_.current_time_ms = info_.start_time_ms;
        transition(State::Buffering);
        return true;
    }

    if (type == "replay_status") {
        // While seeking (scrubber drag), ignore status updates entirely.
        if (info_.state == State::Seeking) {
            return true;
        }

        // During client-only rewind catch-up, the client owns the clock.
        // Server status updates are from the old (ahead) position — ignore
        // the time field but still accept buffer health and progress.
        bool ignore_server_time = using_local_clock_;

        if (data.contains("current_time") && !ignore_server_time) {
            int64_t server_time = data["current_time"].get<int64_t>();

            // Clamp to session range — server should never report a time
            // outside the replay window. Can happen when the playback loop
            // re-anchors from a buffered message that predates the session.
            server_time = std::clamp(server_time, info_.start_time_ms, info_.end_time_ms);

            // After a skip (forward or backward), reject status updates that
            // would jump the client time in the wrong direction. Stale status
            // updates from before the backend processed the skip carry the OLD
            // time — accepting them would revert the clock to pre-skip position.
            //
            // Forward jump guard: prevents stale status from jumping ahead
            // (e.g., after backward skip, old status has higher time).
            // Backward jump guard: prevents stale status from jumping back
            // (e.g., after forward skip, old status has lower time).
            int64_t max_forward_jump_ms = static_cast<int64_t>(10000.0 * std::max(1.0f, info_.speed));
            int64_t max_backward_jump_ms = static_cast<int64_t>(5000.0 * std::max(1.0f, info_.speed));
            int64_t diff = server_time - info_.current_time_ms;
            if (diff > max_forward_jump_ms) {
                return true;
            }
            if (diff < -max_backward_jump_ms) {
                return true;
            }

            info_.current_time_ms = server_time;
        }
        if (data.contains("progress")) {
            info_.progress = data["progress"].get<float>();
        }
        // Parse buffer stats from extra field
        if (data.contains("extra")) {
            const auto& extra = data["extra"];
            if (extra.contains("speed")) {
                info_.speed = extra["speed"].get<float>();
            }
            if (extra.contains("buffer_ahead")) {
                float total_ahead = 0.0f;
                for (auto& [key, val] : extra["buffer_ahead"].items()) {
                    if (val.contains("buffered_ahead_seconds")) {
                        total_ahead = std::max(total_ahead,
                            val["buffered_ahead_seconds"].get<float>());
                    }
                }
                info_.buffer_ahead_seconds = total_ahead;
            }
        }
        info_.last_status_update = now_ms();

        // Update progress from time if not provided directly
        if (info_.progress <= 0.0f && info_.end_time_ms > info_.start_time_ms) {
            int64_t range = info_.end_time_ms - info_.start_time_ms;
            int64_t elapsed = info_.current_time_ms - info_.start_time_ms;
            info_.progress = static_cast<float>(elapsed) / static_cast<float>(range);
        }

        // First status update → DON'T auto-promote to Playing here. The
        // tick_buffering_gate() (called per-frame from the main loop) owns the
        // Buffering→Playing transition now, holding the clock until candles +
        // OB seed are actually present. Promoting on first status (as before)
        // started playback against empty buffers. The gate re-checks every frame.
        // (Left intentionally empty — see tick_buffering_gate.)
        return true;
    }

    if (type == "replay_control") {
        if (data.contains("status")) {
            std::string status = data["status"].get<std::string>();
            // Don't let stale control messages override Seeking state.
            // Only replay_seeked should transition out of Seeking.
            if (info_.state == State::Seeking) {
                return true;
            }
            if (status == "paused") {
                transition(State::Paused);
            } else if (status == "playing") {
                transition(State::Playing);
            }
        }
        return true;
    }

    if (type == "replay_seeked") {
        int64_t seek_ts = 0;
        if (data.contains("timestamp")) {
            seek_ts = data["timestamp"].get<int64_t>();
        }

        const bool lightweight = (lightweight_seek_count_ > 0);
        if (lightweight_seek_count_ > 0) {
            lightweight_seek_count_--;  // Consume one pending lightweight response
        }

        if (lightweight) {
            // For lightweight seeks (<<, >>, arrow keys): the time was already
            // set in skip_forward_to/skip_backward_to. Don't reset it here —
            // by the time replay_seeked arrives (after OB re-seed DB query),
            // the client may have already accepted a post-skip replay_status
            // that advanced the time past the target. Snapping back would
            // cause a visible flicker loop.
            //
            // We also don't clear candle data — it's already on screen and
            // the backend will continue streaming from the new position.

            // Clear the rewind gate — but only if this replay_seeked is for
            // the most recent rewind. If rapid << presses issued a tighter
            // cutoff after this one, don't clear prematurely.
            if (rewind_cutoff_ms_ > 0 && seek_ts <= rewind_cutoff_ms_) {
                rewind_cutoff_ms_ = 0;
            }
            // Always clear rewind_pending on ANY lightweight seeked.
            // The debounce coalesces rapid presses into one backend message,
            // so in the common case there's only one in-flight seek. But if
            // the backend coalesces rapid skip_forwards (sending fewer
            // replay_seekeds than received), the count model breaks. Clearing
            // unconditionally is safe — the latest seeked means the backend
            // is at SOME position, and subsequent seeks will re-arm the gate.
            rewind_pending_ = false;
        } else {
            // Full seek (scrubber drag, large jumps): update time.
            // Data clearing already happened in seek() before the WS message
            // was sent — doing it here would risk wiping the fresh OB seed
            // that arrived between the seek request and this response.
            rewind_cutoff_ms_ = 0;
            rewind_pending_ = false;
            info_.current_time_ms = seek_ts;
            info_.last_status_update = now_ms();

            // Recalculate progress
            if (info_.end_time_ms > info_.start_time_ms) {
                int64_t range = info_.end_time_ms - info_.start_time_ms;
                int64_t elapsed = seek_ts - info_.start_time_ms;
                info_.progress = static_cast<float>(elapsed) / static_cast<float>(range);
            }
        }

        // Restore playback state. The backend auto-resumes after seek,
        // so transition back to Playing (or stay Paused if we were paused).
        if (info_.state == State::Seeking) {
            transition(State::Playing);
        }
        return true;
    }

    if (type == "replay_finished") {
        transition(State::Stopped);
        return true;
    }

    if (type == "error") {
        if (is_active()) {
            // Check both nesting levels — errors may not use data envelope
            std::string error = data.value("error", "");
            if (error.empty()) {
                error = msg.value("error", "Unknown error");
            }
            if (error.find("replay") != std::string::npos ||
                error.find("session") != std::string::npos ||
                error.find("Replay") != std::string::npos) {
                info_.error_message = error;
                transition(State::Error);
                return true;
            }
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Internal Helpers
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::send_control(const char* action) {
    // Pack mode: the engine IS the session — route the action locally.
    if (pack_mode_ && pack_engine_) {
        if (std::strcmp(action, "pause") == 0)  pack_engine_->control_pause();
        else if (std::strcmp(action, "resume") == 0) pack_engine_->control_resume();
        else if (std::strcmp(action, "stop") == 0)   pack_engine_->stop();
        return;
    }
    if (!ws_client_ || !ws_client_->is_connected()) return;

    json msg = {
        {"method", "control_replay"},
        {"data", {
            {"action", action}
        }}
    };
    ws_client_->send_text(msg.dump());
}

void ReplayManager::send_control_with_value(
    const char* action, const char* key, double value)
{
    if (pack_mode_ && pack_engine_) {
        if (std::strcmp(action, "set_speed") == 0) pack_engine_->control_set_speed(value);
        return;
    }
    if (!ws_client_ || !ws_client_->is_connected()) return;

    json msg = {
        {"method", "control_replay"},
        {"data", {
            {"action", action},
            {key, value}
        }}
    };
    ws_client_->send_text(msg.dump());
}

void ReplayManager::send_control_with_int64(
    const char* action, const char* key, int64_t value)
{
    if (pack_mode_ && pack_engine_) {
        if (std::strcmp(action, "seek") == 0) {
            pack_engine_->control_seek(value);
        } else if (std::strcmp(action, "skip_forward") == 0) {
            pack_engine_->control_skip_forward(value, last_flushed_skip_large_);
        }
        return;
    }
    if (!ws_client_ || !ws_client_->is_connected()) return;

    json msg = {
        {"method", "control_replay"},
        {"data", {
            {"action", action},
            {key, value}
        }}
    };
    ws_client_->send_text(msg.dump());
}

void ReplayManager::transition(State new_state) {
    if (info_.state == new_state) return;

    State old_state = info_.state;
    info_.state = new_state;

    // Usage instrumentation (design §3): replay_start on the first Playing of a
    // run; a single cumulative replay_end on the terminal state. Watch-seconds
    // accrue in tick_usage(). This is the seam BOTH the WS path and the pack
    // engine drive (the engine synthesizes lifecycle through handle_ws_message ->
    // this transition), so demo + live replay are measured identically.
    if (new_state == State::Playing && !usage_run_active_) {
        usage_begin_run();
    } else if (usage_run_active_ &&
               (new_state == State::Stopped || new_state == State::Error)) {
        const bool completed = info_.end_time_ms > 0 &&
                               interpolated_time_ms() >= (info_.end_time_ms - 1500);
        usage_emit_replay_end(completed);
    }

    // Reserve space at bottom of dockspace for the control bar — but NOT in
    // embedded lesson mode, where the React chrome owns the transport (outside
    // the canvas) and the native bar isn't drawn. Reserving there leaves a dead
    // gap under the widgets.
    if ((new_state == State::Buffering || new_state == State::Playing ||
         new_state == State::Paused || new_state == State::Seeking) &&
        !EducationBoot::instance().is_embedded()) {
        // Reserve the control-bar height PLUS clearance so the chart's time axis and
        // its floating date label (drawn at the bottom of the plot) aren't clipped by
        // the bar. (Was a hard-coded 36 — shorter than the restyled 54px bar, which
        // is what cut off the axis + tooltip.)
        LayoutManager::bottom_reserve = kBarH + 14.0f;
    }

    // Create replay DataContext when entering Buffering (replay_joined received)
    if (new_state == State::Buffering && !replay_ctx_) {
        create_replay_data_context();
        // Create history buffer for client-side rewind support. Pack mode
        // skips it — rewinds are local block re-reads through the engine, so
        // every << goes down the "backend" path, which the engine answers.
        if (!pack_mode_) {
            history_buffer_ = std::make_unique<ReplayHistoryBuffer>();
        }
    }

    // Historical mounts retain their final replay frame after normal completion.
    // Navigation owns teardown; swapping to live here would be a silent fallback.
    const auto& boot = EducationBoot::instance();
    const bool retain_historical_frame =
        new_state == State::Stopped && (boot.is_event() || boot.is_lesson());
    if ((new_state == State::Stopped && !retain_historical_frame) ||
        new_state == State::Error) {
        LayoutManager::bottom_reserve = 0.0f;
        destroy_replay_data_context();
    }
}

void ReplayManager::reset() {
    // Flush any in-flight replay run before the state is wiped (a reset without a
    // Stopped transition — e.g. starting a new replay over an active one).
    if (usage_run_active_) usage_emit_replay_end(false);
    destroy_replay_data_context();
    history_buffer_.reset();
    pack_engine_.reset();
    pack_mode_ = false;
    LayoutManager::bottom_reserve = 0.0f;
    info_ = SessionInfo{};
    pending_ = PendingRequest{};
    scrubber_dragging_ = false;
    last_seek_time_ms_ = 0;
    lightweight_seek_count_ = 0;
    skip_ignore_status_count_ = 0;
    rewind_cutoff_ms_ = 0;
    rewind_pending_ = false;
    pending_skip_target_ms_ = 0;
    pending_skip_deadline_ms_ = 0;
    pending_skip_is_forward_ = false;
    pending_skip_needs_flush_ = false;
    drip_feed_active_ = false;
    drip_feed_cursor_ = 0;
    drip_feed_end_ms_ = 0;
    using_local_clock_ = false;
    local_clock_base_ms_ = 0;
    local_clock_wall_start_ = 0;
    current_speed_preset_idx_ = 3; // 1.0x
    scrub_preview_ms_ = 0;
    scrub_hover_since_ = 0.0;
    seek_ceiling_ms_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Replay DataContext — DemoNetDriver pattern (parallel data pipeline)
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::create_replay_data_context() {
    if (replay_ctx_) {
        destroy_replay_data_context();
    }

    // Get the live WS handle so the replay context can request historical data.
    // Pack mode: NO handle — historical requests are served from the pack via
    // the StreamManager hook; nothing may reach the box from a pack session.
    int ws_handle = 0;
    if (!pack_mode_ && ws_client_ && ws_client_->is_connected()) {
        ws_handle = ws_client_->get_handle();
    }

    auto ctx = std::make_unique<DataContext>(
        DataContext::create_replay_context(
            info_.session_id,
            info_.symbols,
            ws_handle,
            info_.start_time_ms,
            info_.timeframe_ms / 1000  // Use the chart's timeframe, not hardcoded 5m
        ));
    replay_ctx_ = std::move(ctx);

    // Notify AppState to swap AppContext pointers to replay managers
    if (on_context_swap_) {
        on_context_swap_(replay_ctx_.get());
    }
}

void ReplayManager::destroy_replay_data_context() {
    if (!replay_ctx_) return;

    // Notify AppState to swap AppContext pointers back to live managers
    if (on_context_swap_) {
        on_context_swap_(nullptr);
    }

    replay_ctx_.reset();  // RAII destroys all replay managers
}

// ═══════════════════════════════════════════════════════════════════════════════
// Drip-feed: dispatch buffered messages as the clock advances after rewind
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::drip_feed_pending_replay() {
    if (!drip_feed_active_ || !history_buffer_ || !replay_ctx_) return;

    int64_t now = interpolated_time_ms();
    const auto& entries = history_buffer_->entries();
    int dispatched = 0;

    // TIME-budget dispatches per frame to avoid frame drops. Each dispatch is
    // a ZSTD decompress + protobuf parse + route — roughly 50-100μs. The old
    // fixed cap of 2000 was tuned for a 16ms/60fps budget; at 160+ FPS the
    // frame budget is ~6ms, and a 2000-message frame is a 90-FPS dip. A 2.5ms
    // budget keeps the render loop fluid; the cursor simply resumes next frame
    // (the drip clock leads delivery, so spreading catch-up over a few frames
    // is invisible). The count cap stays as a belt-and-braces upper bound.
    static constexpr int    MAX_DISPATCHES_PER_FRAME = 2000;
    static constexpr double DRIP_BUDGET_MS           = 2.5;
    const auto drip_t0 = std::chrono::steady_clock::now();

    while (drip_feed_cursor_ < entries.size() && dispatched < MAX_DISPATCHES_PER_FRAME) {
        // Budget check every 16 messages (amortized clock read).
        if (dispatched > 0 && (dispatched & 15) == 0) {
            const double spent = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - drip_t0).count();
            if (spent >= DRIP_BUDGET_MS) break;
        }
        const auto& entry = entries[drip_feed_cursor_];

        // Stop if we've reached entries beyond the current clock time.
        // The cursor will resume from here next frame.
        if (entry.timestamp_ms > now) break;

        // Stop if we've reached the drip-feed end point — entries beyond
        // the pre-rewind position belong to the normal streaming region.
        if (drip_feed_end_ms_ > 0 && entry.timestamp_ms > drip_feed_end_ms_) {
            break;
        }

        // Safety: skip entries with timestamps absurdly far from the clock.
        // This catches any buffer corruption or cursor misalignment.
        int64_t drift = entry.timestamp_ms - now;
        if (drift > 30000) {  // > 30s ahead of clock
            static int skip_log_count = 0;
            if (++skip_log_count <= 10) {
            }
            ++drip_feed_cursor_;
            continue;
        }

        // Skip OB snapshots — they're for rewind restore, not playback
        if (!entry.is_ob_snapshot) {
            std::string msg_data(reinterpret_cast<const char*>(entry.data.data()), entry.data.size());
            MessageContext ctx = replay_message_context();
            MessageHandler::handle_message(msg_data, ctx);
            ++dispatched;
        }

        ++drip_feed_cursor_;
    }
    g_profiler.add_count("Drip", dispatched);

    // ── Gap detection: fast-forward clock past empty regions ──────────
    // >> creates gaps in the buffer (skipped-over regions with no data).
    // If the next entry's timestamp is far ahead of the clock, the user
    // would see a stall (no trades, no OB, no candles) while the clock
    // slowly ticks through empty time. Detect the gap and teleport the
    // clock to just before the next entry.
    if (drip_feed_cursor_ < entries.size() && drip_feed_active_) {
        const auto& next_entry = entries[drip_feed_cursor_];
        // Only skip gaps within the drip-feed range
        if (drip_feed_end_ms_ == 0 || next_entry.timestamp_ms <= drip_feed_end_ms_) {
            int64_t gap_ms = next_entry.timestamp_ms - now;
            if (gap_ms > 5000) {  // > 5 seconds of market time with no data
                int64_t jump_to = next_entry.timestamp_ms - 500;  // 500ms before next entry
                local_clock_base_ms_ = jump_to;
                local_clock_wall_start_ = now_ms();
                info_.current_time_ms = jump_to;
            }
        }
    }

    // Drip-feed is complete when we've passed the end point or exhausted
    // all entries up to the clock time. Check both: timestamp-based end
    // (we've reached drip_feed_end_ms_) and clock-based (now >= end).
    bool reached_end = (drip_feed_end_ms_ > 0 && now >= drip_feed_end_ms_);
    bool cursor_exhausted = (drip_feed_cursor_ >= entries.size());

    if (reached_end || cursor_exhausted) {
        // At high speeds (10x), the clock can outrun the cursor — it reaches
        // drip_feed_end_ms_ while the cursor still has undispatched entries.
        // Synchronously drain remaining entries up to drip_feed_end_ms_ to
        // prevent candle gaps from undispatched data.
        if (reached_end) {
            int drain_count = 0;
            while (drip_feed_cursor_ < entries.size()) {
                const auto& entry = entries[drip_feed_cursor_];
                if (drip_feed_end_ms_ > 0 && entry.timestamp_ms > drip_feed_end_ms_) break;
                if (!entry.is_ob_snapshot) {
                    std::string msg_data(reinterpret_cast<const char*>(entry.data.data()), entry.data.size());
                    MessageContext ctx = replay_message_context();
                    MessageHandler::handle_message(msg_data, ctx);
                    ++drain_count;
                }
                ++drip_feed_cursor_;
            }
            (void)drain_count;
        }

        drip_feed_active_ = false;
        using_local_clock_ = false;
        drip_feed_end_ms_ = 0;  // Reset so std::max doesn't carry stale values
        // Re-enable candle messages now that drip-feed catch-up is done.
        // Normal streaming from the backend will have correct incremental data.
        if (replay_ctx_ && replay_ctx_->candles) {
            replay_ctx_->candles->set_suppress_candle_messages(false);
        }
        // Reset OB update_ids — the backend stream has been running at the
        // original position during the entire drip-feed. Its update_id chain
        // doesn't match the buffer's OB chain. Without resetting, every
        // delta after drip-feed gets rejected as snapshot=false.
        if (replay_ctx_ && replay_ctx_->orderbooks) {
            replay_ctx_->orderbooks->reset_update_ids();
        }
        // Re-sync clock to the BACKEND's actual streaming position.
        // During a buffer-hit rewind, the backend was never told about the
        // rewind — it kept streaming. While the drip-feed ran for N minutes
        // of real time, the backend advanced N minutes too. Setting the clock
        // to `now` (the drip-feed end time) would leave it minutes behind the
        // backend. The replay_status jump guard would then reject every update
        // (diff > 10s), freezing the clock permanently.
        //
        // The buffer's latest entry reflects the backend's approximate position
        // (the drip-feed gate captured all incoming frames during catch-up).
        if (history_buffer_ && history_buffer_->latest_ms() > now) {
            info_.current_time_ms = history_buffer_->latest_ms();
        } else {
            info_.current_time_ms = now;
        }
        info_.last_status_update = now_ms();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// JS scrub-hover bridge (ghost preview from the React transports)
//
// The React bars (lesson / studio / event) forward their track hover/drag aim
// as milli-progress via Module.__replay_cmd_scrub_hover; -1 clears. The export
// only STORES the value (mirrors the edu-command enqueue rule: exports run
// with no context in hand); flush_pending_skip, which the main loop already
// calls every frame in all modes, applies it against the live session window.
// The hover-intent dwell lives on the JS side (lib/useScrubPreview.ts); the
// native bar arms its own preview directly and never routes through here.
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
constexpr int kScrubHoverNone = INT_MIN;  // sentinel: nothing pending
int g_pending_scrub_hover = kScrubHoverNone;
}  // namespace

#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE void _replay_cmd_scrub_hover(int milli_progress) {
    g_pending_scrub_hover = milli_progress;
}
}  // extern "C"
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Skip Debounce: flush coalesced skip when the window expires
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::flush_pending_skip() {
    // Apply any JS-forwarded scrub-hover aim first: this function runs every
    // frame in every mode (embedded transports included), so it doubles as the
    // preview drain without touching the main loop. Last write wins.
    if (g_pending_scrub_hover != kScrubHoverNone) {
        const int mp = g_pending_scrub_hover;
        g_pending_scrub_hover = kScrubHoverNone;
        if (mp < 0) {
            set_scrub_preview_ms(0);
        } else if (is_active() && info_.end_time_ms > info_.start_time_ms) {
            const double p = std::clamp(static_cast<double>(mp) / 1000.0, 0.0, 1.0);
            const int64_t range = info_.end_time_ms - info_.start_time_ms;
            const int64_t aim = info_.start_time_ms +
                static_cast<int64_t>(p * static_cast<double>(range));
            set_scrub_preview_ms(std::clamp(aim, info_.start_time_ms, info_.end_time_ms));
        }
    }

    if (pending_skip_deadline_ms_ == 0) return;

    const int64_t wall_now = now_ms();
    if (wall_now < pending_skip_deadline_ms_) return;  // Window still open

    // Window expired — send the coalesced final target to the backend.
    // Forward targets respect the seek ceiling (lesson scrub-forward lock);
    // the per-press handler clamped too, but the coalesced sum re-checks.
    int64_t target = pending_skip_target_ms_;
    const bool is_forward = pending_skip_is_forward_;
    if (is_forward && seek_ceiling_active() && target > seek_ceiling_ms_) {
        target = std::max(seek_ceiling_ms_, info_.start_time_ms);
    }

    // Clear debounce state
    pending_skip_deadline_ms_ = 0;
    pending_skip_target_ms_ = 0;
    pending_skip_needs_flush_ = false;

    // Recompute the coalesced jump's "large" verdict (same rule the per-press
    // handler used when it decided to clear_all) so the pack engine can mirror
    // the box's reconnect re-seed. skip_origin_ms_ still holds this debounce
    // window's origin.
    last_flushed_skip_large_ =
        is_forward && (target - skip_origin_ms_ > kLargeSkipThresholdMs);

    send_control_with_int64("skip_forward", "timestamp", target);
    lightweight_seek_count_++;

    // Sync client clock to the flushed target (may already be set, but
    // ensure consistency after the backend message is sent).
    info_.current_time_ms = target;
    info_.last_status_update = wall_now;
}

MessageContext ReplayManager::replay_message_context() const {
    if (!replay_ctx_) return MessageContext{};
    MessageContext mc{
        replay_ctx_->streams,
        replay_ctx_->orderbooks,
        replay_ctx_->heatmaps,
        replay_ctx_->liq_heatmaps,
        replay_ctx_->debug,
        replay_ctx_->vpvr,
        replay_ctx_->tpo,
        replay_ctx_->footprint,
        nullptr  // No paper-trading manager in replay.
    };
    mc.series = replay_ctx_->series;  // Indicators V1 SeriesCache (set by name — see message_context.h)
    mc.analytics = replay_ctx_->analytics;  // Positioning/Contagion (set by name - same rule as series)
    mc.preview = replay_ctx_->preview;  // Scrub-preview store (set by name - same rule as series)
    return mc;
}

void ReplayManager::set_timeframe_ms(int64_t tf_ms) {
    const bool changed = info_.timeframe_ms != tf_ms;
    info_.timeframe_ms = tf_ms;
    // Chart TF changed mid-replay: re-fit the scrub-preview (ghost) candles to
    // the new timeframe. The store dedupes repeat requests; the old batch stays
    // renderable until the new one lands, so there is no ghost flicker.
    if (changed && tf_ms > 0 && replay_ctx_ && replay_ctx_->preview &&
        replay_ctx_->streams && !replay_ctx_->symbols.empty()) {
        Terminal::Pair pair{"binancef", replay_ctx_->symbols[0]};
        replay_ctx_->preview->request(*replay_ctx_->streams, pair, tf_ms / 1000);
    }
}

int64_t ReplayManager::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Usage instrumentation (design §3) — watch-seconds clock + replay lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

const char* ReplayManager::usage_source() const {
    if (pack_mode_) return "demo";
    const auto m = EducationBoot::instance().mode();
    if (m == EducationBoot::Mode::Lesson) return "lesson";
    if (m == EducationBoot::Mode::Event)  return "event";
    if (info_.session_type == "archive")  return "event";
    return "live";
}

std::string ReplayManager::usage_symbol() const {
    if (!info_.symbols.empty() && !info_.symbols.front().empty()) return info_.symbols.front();
    const auto& edu = EducationBoot::instance();
    if (edu.is_event() && !edu.event_symbol().empty()) return edu.event_symbol();
    if (edu.is_pack() && !edu.pack_symbol().empty())   return edu.pack_symbol();
    return "";
}

std::string ReplayManager::usage_event_ref() const {
    const auto& edu = EducationBoot::instance();
    if (edu.is_event() && !edu.event_id().empty()) return edu.event_id();
    if (!pending_.event_id.empty()) return pending_.event_id;
    return "";
}

std::string ReplayManager::usage_build_props() const {
    json p = {
        {"replay_id", usage_replay_id_},
        {"source", usage_source()},
        {"symbol", usage_symbol()},
        {"watch_s", static_cast<int64_t>(usage_watch_seconds_ + 0.5)},
        {"market_s_covered", usage_market_ms_ / 1000},
        {"max_speed", usage_max_speed_},
        {"scrub_count", usage_scrub_count_},
    };
    return p.dump();
}

void ReplayManager::usage_begin_run() {
    usage_run_active_ = true;
    usage_replay_id_ = std::string(usage_source()) + "-" + std::to_string(now_ms());
    usage_watch_seconds_ = 0.0;
    usage_market_ms_ = 0;
    usage_last_market_clock_ = -1;
    usage_scrub_count_ = 0;
    usage_max_speed_ = current_speed();
    usage_run_start_market_ts_ = interpolated_time_ms();
    usage_last_heartbeat_ms_ = now_ms();

    const std::string sym = usage_symbol();
    const std::string ref = usage_event_ref();
    json detail = {
        {"event", "replay_start"},
        {"mode", usage_source()},
        {"symbol", sym},
        {"dedupeKey", usage_replay_id_ + ":start"},
        {"props", {
            {"replay_id", usage_replay_id_},
            {"source", usage_source()},
            {"symbol", sym},
            {"start_market_ts", usage_run_start_market_ts_},
            {"speed", current_speed()},
        }},
    };
    if (!ref.empty()) detail["eventRef"] = ref;
    usage::dispatch_detail(detail.dump());
}

void ReplayManager::usage_emit_heartbeat() {
    // NOT rowed by the bridge: held as the pending replay_end (keyed by dedupeKey)
    // and materialized only on tab-close, so watch time survives an unload with no
    // clean end. A later clean end shares the dedupeKey and dedupes it.
    const std::string ref = usage_event_ref();
    json props = json::parse(usage_build_props());
    props["completed"] = false;
    json detail = {
        {"event", "replay_heartbeat"},
        {"heartbeatFor", "replay_end"},
        {"mode", usage_source()},
        {"symbol", usage_symbol()},
        {"umami", false},
        {"dedupeKey", usage_replay_id_ + ":end"},
        {"props", props},
    };
    if (!ref.empty()) detail["eventRef"] = ref;
    usage::dispatch_detail(detail.dump());
}

void ReplayManager::usage_emit_replay_end(bool completed) {
    if (!usage_run_active_) return;
    usage_run_active_ = false;

    const std::string ref = usage_event_ref();
    json props = json::parse(usage_build_props());
    props["completed"] = completed;
    json detail = {
        {"event", "replay_end"},
        {"mode", usage_source()},
        {"symbol", usage_symbol()},
        {"dedupeKey", usage_replay_id_ + ":end"},
        {"props", props},
    };
    if (!ref.empty()) detail["eventRef"] = ref;
    usage::dispatch_detail(detail.dump());
}

void ReplayManager::tick_usage(double dt_seconds, bool document_visible) {
    if (!usage_run_active_) return;

    // Accrue only while actually playing AND visible. Paused/seeking/buffering or
    // hidden -> re-baseline the market clock so the gap isn't counted as watched.
    if (!is_playing() || !document_visible) {
        usage_last_market_clock_ = -1;
        return;
    }

    // Wall-clock watch seconds; dt clamped so a resumed-after-hidden frame (a big
    // gap) can't dump seconds into the counter.
    double d = dt_seconds;
    if (d < 0.0) d = 0.0;
    if (d > 0.25) d = 0.25;
    usage_watch_seconds_ += d;

    const float sp = current_speed();
    if (sp > usage_max_speed_) usage_max_speed_ = sp;

    // Speed-weighted market span: forward progress of the playback clock only.
    const int64_t clk = interpolated_time_ms();
    if (usage_last_market_clock_ >= 0 && clk > usage_last_market_clock_) {
        const int64_t md = clk - usage_last_market_clock_;
        if (md > 0 && md < 5 * 60 * 1000) usage_market_ms_ += md;  // cap absorbs seek jumps
    }
    usage_last_market_clock_ = clk;

    // ~15s heartbeat (running totals) so a tab-close still records watch time.
    const int64_t noww = now_ms();
    if (noww - usage_last_heartbeat_ms_ >= 15000) {
        usage_last_heartbeat_ms_ = noww;
        usage_emit_heartbeat();
    }
}

int64_t ReplayManager::interpolated_time_ms() const {
    // Frozen while not playing: paused, seeking, OR still buffering (the gate
    // holds the clock at the window start until candles + OB seed are primed —
    // without this, the clock interpolates forward off wall-time during the hold
    // and the "loading" period looks like it's already playing).
    if (!is_active() || is_paused() || info_.state == State::Seeking ||
        info_.state == State::Buffering || info_.last_status_update == 0) {
        return info_.current_time_ms;
    }
    // During drip-feed catch-up after client-only rewind, the client
    // owns the clock. Advance from rewind target using local wall time.
    if (using_local_clock_) {
        const int64_t wall_elapsed = now_ms() - local_clock_wall_start_;
        const int64_t market_elapsed = static_cast<int64_t>(
            static_cast<double>(wall_elapsed) * info_.speed);
        int64_t interpolated = local_clock_base_ms_ + market_elapsed;
        if (info_.end_time_ms > 0 && interpolated > info_.end_time_ms) {
            interpolated = info_.end_time_ms;
        }
        return interpolated;
    }
    // Advance from the last known server time using local wall clock × speed
    const int64_t wall_elapsed = now_ms() - info_.last_status_update;
    const int64_t market_elapsed = static_cast<int64_t>(
        static_cast<double>(wall_elapsed) * info_.speed);
    int64_t interpolated = info_.current_time_ms + market_elapsed;

    // Bound the OPEN-LOOP extrapolation: never dead-reckon more than a few
    // status-intervals of market time past the last server-reported position. On a
    // healthy stream the next replay_status (~2s cadence) re-anchors current_time_ms
    // long before this bites, so steady playback stays perfectly smooth. But on a
    // slow / sparse START — e.g. a quiet "now-12h" studio window where the depth +
    // tape data only trickles in — status updates stall, and pure wall-clock
    // dead-reckoning would race the displayed clock ~20-30s AHEAD of the data that's
    // actually on screen (then snap back when a status finally lands: the 00:00:00 →
    // 00:00:01 → 00:00:00 stutter). Capping the lead pins the clock near the real
    // data position until delivery catches up. The lesson never hit this because its
    // curated event windows stream densely from frame one. Scales with speed so fast
    // playback (larger gaps between statuses) isn't throttled. Applies to ALL replays
    // (the live terminal's "replay from here" benefits too).
    const int64_t max_lead_ms =
        std::max<int64_t>(4000, static_cast<int64_t>(4000.0 * info_.speed));
    const int64_t lead_cap = info_.current_time_ms + max_lead_ms;
    if (interpolated > lead_cap) interpolated = lead_cap;

    // Clamp to replay window
    if (info_.end_time_ms > 0 && interpolated > info_.end_time_ms) {
        interpolated = info_.end_time_ms;
    }
    return interpolated;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Time Formatting
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::format_timestamp(int64_t ms, char* buf, size_t buf_size) {
    if (ms == 0) {
        snprintf(buf, buf_size, "--:--:--");
        return;
    }
    if (!DisplayTimeZone::instance().format(ms, TimeZoneFormat::TimeSeconds,
                                             buf, buf_size)) {
        snprintf(buf, buf_size, "--:--:--");
    }
}

void ReplayManager::format_duration(int64_t ms, char* buf, size_t buf_size) {
    if (ms < 0) ms = -ms;
    int64_t total_sec = ms / 1000;
    int hours = static_cast<int>(total_sec / 3600);
    int mins = static_cast<int>((total_sec % 3600) / 60);
    int secs = static_cast<int>(total_sec % 60);

    if (hours > 0) {
        snprintf(buf, buf_size, "%dh %02dm %02ds", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buf, buf_size, "%dm %02ds", mins, secs);
    } else {
        snprintf(buf, buf_size, "%ds", secs);
    }
}

void ReplayManager::format_speed(float speed, char* buf, size_t buf_size) {
    if (speed >= 1.0f && std::fmod(speed, 1.0f) < 0.01f) {
        snprintf(buf, buf_size, "%.0fx", speed);
    } else {
        snprintf(buf, buf_size, "%.1fx", speed);
    }
}

void ReplayManager::format_timestamp_full(int64_t ms, char* buf, size_t buf_size) {
    if (ms == 0) {
        snprintf(buf, buf_size, "--:--:--");
        return;
    }
    if (!DisplayTimeZone::instance().format(ms, TimeZoneFormat::FullInspection,
                                             buf, buf_size)) {
        snprintf(buf, buf_size, "--:--:--");
    }
}

void ReplayManager::format_duration_short(int64_t seconds, char* buf, size_t buf_size) {
    if (seconds < 0) seconds = -seconds;
    if (seconds >= 3600) {
        snprintf(buf, buf_size, "%lldh", (long long)(seconds / 3600));
    } else if (seconds >= 60) {
        snprintf(buf, buf_size, "%lldm", (long long)(seconds / 60));
    } else {
        snprintf(buf, buf_size, "%llds", (long long)seconds);
    }
}

// ─── Candle Boundary Helpers ─────────────────────────────────────────────────

int64_t ReplayManager::snap_to_candle_boundary(int64_t ms) const {
    int64_t tf = info_.timeframe_ms;
    if (tf <= 0) tf = 300000;
    int64_t lower = (ms / tf) * tf;
    int64_t upper = lower + tf;
    return (ms - lower <= upper - ms) ? lower : upper;
}

int64_t ReplayManager::next_candle_boundary(int64_t ms) const {
    int64_t tf = info_.timeframe_ms;
    if (tf <= 0) tf = 300000;
    return ((ms / tf) + 1) * tf;
}

int64_t ReplayManager::prev_candle_boundary(int64_t ms) const {
    int64_t tf = info_.timeframe_ms;
    if (tf <= 0) tf = 300000;
    int64_t boundary = (ms / tf) * tf;
    // If we're exactly on a boundary, go to the previous one
    if (boundary == ms) boundary -= tf;
    return boundary;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Keyboard Shortcuts
// ═══════════════════════════════════════════════════════════════════════════════

bool ReplayManager::process_keyboard_shortcuts() {
    // Don't consume shortcuts when typing in an input field
    if (ImGui::GetIO().WantTextInput) return false;

    const ImGuiIO& io = ImGui::GetIO();

    // ─── Active replay controls ──────────────────────────────────
    if (is_active()) {
        // Space: toggle pause/play
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            toggle_pause();
            return true;
        }

        // Escape: stop replay — unless the research panel owns this press
        // (open now, or closed BY this Escape): closing a floating reader
        // must never also kill the replay under it.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
            !ui::ResearchMomentPanel::instance().blocks_replay_escape()) {
            stop();
            return true;
        }

        // Right arrow: skip forward (clock-only, instant)
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            int64_t current = interpolated_time_ms();
            int64_t target = next_candle_boundary(current);
            if (io.KeyShift) {
                for (int i = 1; i < 5; i++) target = next_candle_boundary(target);
            }
            skip_forward_to(std::min(target, info_.end_time_ms));
            return true;
        }
        // Left arrow: skip backward (clock-only + trim + clear OB)
        // Uses info_.current_time_ms (not interpolated) so rapid presses accumulate
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            int64_t current = info_.current_time_ms;
            int64_t target = prev_candle_boundary(current);
            if (io.KeyShift) {
                for (int i = 1; i < 5; i++) target = prev_candle_boundary(target);
            }
            skip_backward_to(std::max(target, info_.start_time_ms));
            return true;
        }

        // Up/Down: speed control
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            speed_up();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            speed_down();
            return true;
        }

        // Number keys: direct speed presets
        // 1=0.1x, 2=0.2x, 3=0.5x, 4=1x, 5=2x, 6=4x
        for (int i = 0; i < NUM_SPEED_PRESETS && i < 9; i++) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) {
                current_speed_preset_idx_ = i;
                set_speed(SPEED_PRESETS[i]);
                return true;
            }
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Chart Context Menu Integration
// ═══════════════════════════════════════════════════════════════════════════════

// Navigate the host browser to the focused research-replay viewer for an
// explicit window (see header). Full-document nav via EM_ASM (the WASM canvas
// is a non-remountable singleton, so it can't soft-nav); the destination page
// enforces sign-in + entitlement.
void ReplayManager::open_focused_replay(const std::string& symbol, int64_t from_ms,
                                        int64_t to_ms, int64_t seek_ms) {
#ifdef __EMSCRIPTEN__
    if (EducationBoot::instance().is_embedded()) return;  // already a focused chrome
    char url[224];
    if (seek_ms > 0)
        snprintf(url, sizeof(url), "/terminal?replay=%s&from=%lld&to=%lld&t=%lld",
                 symbol.c_str(), static_cast<long long>(from_ms),
                 static_cast<long long>(to_ms), static_cast<long long>(seek_ms));
    else
        snprintf(url, sizeof(url), "/terminal?replay=%s&from=%lld&to=%lld",
                 symbol.c_str(), static_cast<long long>(from_ms),
                 static_cast<long long>(to_ms));
    EM_ASM({ window.location.href = UTF8ToString($0); }, url);
#else
    // Native dev build: no browser to navigate — keep an in-place replay so
    // local replay testing still works.
    request_replay_from(symbol, seek_ms > 0 ? seek_ms : from_ms, 1.0f, 300);
#endif
}

// A 4h window AROUND an anchor bar (30m lead), seeked to the bar.
void ReplayManager::open_focused_replay_at(const std::string& symbol, int64_t anchor_ms) {
    const int64_t lead_ms = 30LL * 60000;      // 30 min before
    const int64_t span_ms = 4LL * 60 * 60000;  // 4h total window
    const int64_t from_ms = anchor_ms - lead_ms;
    open_focused_replay(symbol, from_ms, from_ms + span_ms, anchor_ms);
}

// Minimal lock glyph for the locked replay row (the ac_ic_* icon helpers are
// file-local to app_shell.cpp, so we draw a small one here).
static void draw_mini_lock(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const ImVec2 b0(c.x - s * 0.5f, c.y - s * 0.1f);
    const ImVec2 b1(c.x + s * 0.5f, c.y + s * 0.62f);
    dl->AddRectFilled(b0, b1, col, 1.5f);
    dl->PathArcTo(ImVec2(c.x, b0.y), s * 0.32f, IM_PI, IM_PI * 2.0f, 10);
    dl->PathStroke(col, 0, 1.4f);
}

bool ReplayManager::render_chart_context_menu(
    const std::string& symbol,
    int64_t hovered_time_ms,
    int64_t timeframe_seconds,
    int64_t selection_start_ms,
    int64_t selection_end_ms)
{
    bool initiated = false;

    if (hovered_time_ms > 0) {
        char time_buf[48];
        format_timestamp_full(hovered_time_ms, time_buf, sizeof(time_buf));

        // Section label (redesign 1f: the replay actions live under a REPLAY group).
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
        ImGui::TextUnformatted("REPLAY");
        ImGui::PopStyleColor();

        // Plan gate: a non-Pro user can only replay the recent window + the major
        // symbols. Rather than hide the item (confusing), show WHY it's locked and
        // route to upgrade. The Go backend still enforces the real cap on the
        // session request — this is the friendly front door, not the lock itself.
        const bool sym_ok  = Entitlements::symbol_replay_allowed(symbol);
        const bool time_ok = Entitlements::time_replayable(hovered_time_ms, now_ms());

        if (sym_ok && time_ok) {
            // Replay from the clicked bar → the FOCUSED research-replay viewer
            // (the web app's /terminal?replay=): a distraction-free chrome (marker
            // rail + scrubber, NO watchlist) over a 4h window around the bar,
            // seeked to it. Native dev build falls back to in-place; an embedded
            // chrome is a no-op (see open_focused_replay).
            if (ImGui::MenuItem("Replay from here", time_buf)) {
                open_focused_replay_at(symbol, hovered_time_ms);
                initiated = true;
            }
        } else if (!sym_ok) {
            // Symbol isn't one of the free majors, so the free day can't rescue
            // this (wrong symbol). Explain + route to upgrade. The Go backend
            // still enforces the real cap on the session request.
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::WARN);
            if (ImGui::MenuItem("Replay locked - Pro unlocks this symbol"))
                ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Symbol);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                Theme::tooltip("Free replay covers 6 majors (%s).\n"
                                  "Pro replays all 660+ symbols.",
                                  Entitlements::free_symbols_label());
        } else if (Entitlements::is_pro()) {
            // Pro, but this point is older than the 30-day archive. No free-day
            // rescue applies; just say why.
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::WARN);
            char lock_lbl[64], lock_tip[96];
            snprintf(lock_lbl, sizeof(lock_lbl), "Replay locked - beyond the %d-day archive",
                     Entitlements::pro_lookback_days());
            snprintf(lock_tip, sizeof(lock_tip),
                     "Replay reaches back %d days. This point is older than that.",
                     Entitlements::pro_lookback_days());
            if (ImGui::MenuItem(lock_lbl))
                ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Range);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                Theme::tooltip("%s", lock_tip);
        } else {
            // Free tier, free symbol, clicked outside the free day. Redesign 1f:
            // the free-day rescue (teal + 24H), a locked "this moment" row
            // (amber + PRO tag), then an inset panel that explains the lock with
            // a mini 30-day timeline and the upgrade CTA. Backend re-verifies.
            char offer[64];
            snprintf(offer, sizeof(offer), "Replay %s: your free day",
                     Entitlements::free_window_short_label().c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_TX);
            if (ImGui::MenuItem(offer, "24H")) {
                int64_t fw_s = 0, fw_e = 0;
                Entitlements::free_window_range(now_ms(), fw_s, fw_e);
                open_focused_replay(symbol, fw_s, fw_e, fw_s);
                initiated = true;
            }
            ImGui::PopStyleColor();

            // Locked "replay this moment" row: amber-tinted, lock + bordered PRO tag.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 rp = ImGui::GetCursorScreenPos();
                const float rw = ImGui::GetContentRegionAvail().x, rh = 34.0f;
                const bool rclk = ImGui::InvisibleButton("##rlock", ImVec2(rw, rh));
                const bool rhov = ImGui::IsItemHovered();
                dl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rh),
                                  Theme::u32(Theme::Tokens::WARN, rhov ? 0.16f : 0.10f));
                const float cy = rp.y + rh * 0.5f;
                draw_mini_lock(dl, ImVec2(rp.x + 13.0f, cy), 9.0f, Theme::u32(Theme::Tokens::WARN));
                dl->AddText(ImVec2(rp.x + 26.0f, cy - ImGui::GetFontSize() * 0.5f),
                            Theme::u32(Theme::Tokens::TX1), "Replay this moment");
                const ImVec2 tsz = ImGui::CalcTextSize("PRO");
                const float tw = tsz.x + 12.0f, tx = rp.x + rw - tw - 8.0f, ty = cy - 8.0f;
                dl->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + 16.0f),
                            Theme::u32(Theme::Tokens::WARN, 0.60f), 2.0f, 0, 1.0f);
                dl->AddText(ImVec2(tx + 6.0f, ty + (16.0f - tsz.y) * 0.5f),
                            Theme::u32(Theme::Tokens::WARN), "PRO");
                if (rclk) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Range);
            }

            // Inset hint panel (darker bg-0 box): reason + mini timeline + CTAs.
            // Height derived from the wrapped copy so the CTAs never clip.
            char hbuf[256];
            snprintf(hbuf, sizeof(hbuf),
                     "Free replay covers %s on the 6 majors. Pro replays any moment "
                     "of the last 30 days, on all 660+ pairs.",
                     Entitlements::free_window_label().c_str());
            const float pw = ImGui::GetContentRegionAvail().x;
            const float text_h = ImGui::CalcTextSize(hbuf, nullptr, false, pw - 24.0f).y;
            const float child_h = 112.0f + text_h;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Tokens::BASE);
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
            ImGui::BeginChild("##rlockhint", ImVec2(-1.0f, child_h), true,
                              ImGuiWindowFlags_NoScrollbar);
            {
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::WARN);
                ImGui::TextUnformatted("OUTSIDE YOUR FREE WINDOW");
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 4));
                // The explanation copy is sans (var(--font-sans) in the design);
                // the surrounding menu chrome is mono (pushed by the caller).
                ImGui::PushFont(Theme::Fonts::ui());
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
                ImGui::TextWrapped("%s", hbuf);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 8));
                // mini 30-day timeline: track + amber free-day slice + white tick.
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const float bw = ImGui::GetContentRegionAvail().x, bh = 5.0f;
                cdl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), Theme::u32(Theme::Tokens::BD2));
                cdl->AddRectFilled(ImVec2(bp.x + bw * 0.93f, bp.y), ImVec2(bp.x + bw, bp.y + bh),
                                   Theme::u32(Theme::Tokens::WARN));
                cdl->AddRectFilled(ImVec2(bp.x + bw * 0.40f, bp.y - 2.0f),
                                   ImVec2(bp.x + bw * 0.40f + 1.5f, bp.y + bh + 2.0f),
                                   Theme::u32(Theme::Tokens::TX1));
                const float ly = bp.y + bh + 6.0f;
                const ImU32 lc = Theme::u32(Theme::Tokens::TX3);
                cdl->AddText(ImVec2(bp.x, ly), lc, "-30D");
                const ImVec2 mmsz = ImGui::CalcTextSize("THIS MOMENT");
                cdl->AddText(ImVec2(bp.x + (bw - mmsz.x) * 0.5f, ly), lc, "THIS MOMENT");
                const float nw = ImGui::CalcTextSize("NOW").x;
                cdl->AddText(ImVec2(bp.x + bw - nw, ly), lc, "NOW");
                ImGui::Dummy(ImVec2(bw, bh + 22.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Tokens::BRAND);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND_TX);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::BRAND);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
                if (ImGui::Button("See Pro: $20/mo billed yearly"))
                    ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Range);
                ImGui::PopStyleColor(4);
                ImGui::SameLine(0.0f, 10.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::ELEV);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::ELEV);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
                if (ImGui::Button("Not now")) ImGui::CloseCurrentPopup();
                ImGui::PopStyleColor(4);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
        }

        // Replay an EXACT window → the same focused viewer. The window is a
        // shift-drag selection on the chart; without one there's nothing to
        // bound, so show a disabled hint rather than the old in-place launcher.
        if (selection_start_ms > 0 && selection_end_ms > selection_start_ms) {
            if (ImGui::MenuItem("Replay selected range\xe2\x80\xa6")) {
                open_focused_replay(symbol, selection_start_ms, selection_end_ms,
                                    selection_start_ms);
                initiated = true;
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Replay range (shift-drag to select)");
            ImGui::EndDisabled();
        }
    }

    if (is_active()) {
        if (ImGui::MenuItem("Stop Replay", "Esc")) {
            stop();
        }
    }

    return initiated;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Control Bar Rendering
//
// Layout:
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ ⏸ ◀◀ ▶ ▶▶ │ 0.5x [1x] 2x 5x │ 02:34:15 ════════●══════ 04:00:00  │ ✕ │
// └─────────────────────────────────────────────────────────────────────────┘
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::render_control_bar() {
    if (!is_active() && info_.state != State::Error && info_.state != State::Stopped) {
        return;
    }

    // Show stopped/error states briefly (3 seconds) then auto-dismiss
    if (info_.state == State::Stopped || info_.state == State::Error) {
        int64_t elapsed = now_ms() - info_.last_status_update;
        if (info_.last_status_update == 0) {
            info_.last_status_update = now_ms();
        }
        if (elapsed > 3000) {
            reset();
            return;
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    // Sit above the bottom status bar (status_reserve = its height, 0 when embedded).
    const float bar_y = io.DisplaySize.y - kBarH - LayoutManager::status_reserve;

    ImGui::SetNextWindowPos(ImVec2(0, bar_y));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kBarH));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    // GRB chrome: panel fill, square corners, no ImGui border — we draw only the
    // top hairline ourselves (border-top: 1px var(--bd-2)). Zero vertical padding;
    // every item vertically centers itself via center_item_y().
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::PANEL);

    if (ImGui::Begin("##ReplayControlBar", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float  ww = ImGui::GetWindowSize().x;
        // border-top
        dl->AddLine(ImVec2(wp.x, wp.y + 0.5f), ImVec2(wp.x + ww, wp.y + 0.5f),
                    tok(Theme::Tokens::BD2), 1.0f);

        // ── Measure the right-aligned group (speeds + buffer + close) so the
        //    scrubber can fill the gap and the group sits flush at the right edge.
        ImGui::PushFont(Theme::Fonts::mono());
        const float pill_pad_x = 9.0f, seg_pad = 3.0f, pill_gap = 2.0f;
        float speeds_w = seg_pad * 2.0f;
        for (int i = 0; i < NUM_SPEED_PRESETS; ++i) {
            char lbl[16];
            format_speed(SPEED_PRESETS[i], lbl, sizeof(lbl));
            speeds_w += ImGui::CalcTextSize(lbl).x + pill_pad_x * 2.0f + (i > 0 ? pill_gap : 0.0f);
        }
        ImGui::PopFont();
        const float buffer_w = 14.0f, close_w = 26.0f;
        const float gap_sp_buf = 10.0f, gap_buf_cl = 8.0f, gap_scrub_sp = 16.0f;
        const float right_group_w = speeds_w + gap_sp_buf + buffer_w + gap_buf_cl + close_w;
        g_scrub_reserve = right_group_w + gap_scrub_sp;

        const float content_max_x = ImGui::GetWindowContentRegionMax().x;
        const float x_speeds = content_max_x - right_group_w;

        // ── Left group: status pill · transport · clock ──────────────────────
        render_status_badge();
        ImGui::SameLine(0, 14);
        render_transport_controls();
        ImGui::SameLine(0, 10);
        render_record_button();
        ImGui::SameLine(0, 14);
        render_time_display();

        // ── Center: scrubber (fills to x_speeds, reserving right_group_w) ─────
        ImGui::SameLine(0, gap_scrub_sp);
        render_timeline_scrubber();

        // ── Right group (flush right): speed pills · buffer dot · close ──────
        ImGui::SameLine(x_speeds);
        render_speed_control();

        ImGui::SameLine(x_speeds + speeds_w + gap_sp_buf);
        render_buffer_indicator();

        ImGui::SameLine(content_max_x - close_w);
        center_item_y(close_w);
        {
            ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##replay_close", ImVec2(close_w, close_w));
            bool ch = ImGui::IsItemHovered();
            if (ch) Theme::tooltip("Exit replay");
            if (ImGui::IsItemClicked()) stop();
            ImVec2 cc = ImVec2(cpos.x + close_w * 0.5f, cpos.y + close_w * 0.5f);
            if (ch) {
                dl->AddCircleFilled(cc, close_w * 0.5f, tok(Theme::Tokens::DOWN, 0.18f));
            }
            const float k = 5.0f;
            ImU32 xcol = ch ? tok(Theme::Tokens::DOWN) : tok(Theme::Tokens::TX3);
            dl->AddLine(ImVec2(cc.x - k, cc.y - k), ImVec2(cc.x + k, cc.y + k), xcol, 1.6f);
            dl->AddLine(ImVec2(cc.x - k, cc.y + k), ImVec2(cc.x + k, cc.y - k), xcol, 1.6f);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);
}

// ─── Status Badge ────────────────────────────────────────────────────────────

void ReplayManager::render_status_badge() {
    const char* label = "";
    ImVec4 color;

    switch (info_.state) {
        case State::Creating:
        case State::Joining:   label = "CONNECTING"; color = Theme::Tokens::WARN;  break;
        case State::Buffering: label = "BUFFERING";  color = Theme::Tokens::WARN;  break;
        case State::Playing:   label = "REPLAY";     color = Theme::Tokens::BRAND; break;
        case State::Paused:    label = "PAUSED";     color = Theme::Tokens::TX2;   break;
        case State::Seeking:   label = "SEEKING";    color = Theme::Tokens::BRAND; break;
        case State::Stopped:   label = "STOPPED";    color = Theme::Tokens::TX3;   break;
        case State::Error:     label = "ERROR";      color = Theme::Tokens::DOWN;  break;
        default:               return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(Theme::Fonts::label());  // uppercase micro-label face

    const float pill_h = 20.0f, pad_x = 8.0f, dot_r = 3.0f, dot_gap = 6.0f;
    const ImVec2 tsz = ImGui::CalcTextSize(label);
    const float pill_w = pad_x + dot_r * 2.0f + dot_gap + tsz.x + pad_x;

    center_item_y(pill_h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 pmax = ImVec2(p.x + pill_w, p.y + pill_h);
    const float cy = p.y + pill_h * 0.5f;

    // Soft pill fill + hairline outline, in the state color (cyan/amber/red/grey).
    dl->AddRectFilled(p, pmax, tok(color, 0.14f), pill_h * 0.5f);
    dl->AddRect(p, pmax, tok(color, 0.45f), pill_h * 0.5f, 0, 1.0f);
    // Status dot — gentle pulse while connecting/buffering/seeking.
    float dot_a = 1.0f;
    if (info_.state == State::Creating || info_.state == State::Joining ||
        info_.state == State::Buffering || info_.state == State::Seeking) {
        dot_a = 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f));
    }
    dl->AddCircleFilled(ImVec2(p.x + pad_x + dot_r, cy), dot_r, tok(color, dot_a));
    dl->AddText(ImVec2(p.x + pad_x + dot_r * 2.0f + dot_gap, cy - tsz.y * 0.5f),
                tok(color), label);

    ImGui::PopFont();
    ImGui::Dummy(ImVec2(pill_w, pill_h));

    // Symbol (uppercased) in secondary text, vertically centered next to the pill.
    if (!info_.symbols.empty()) {
        ImGui::SameLine(0, 8);
        ImGui::PushFont(Theme::Fonts::ui_semibold());
        center_item_y(ImGui::GetTextLineHeight());
        std::string sym = info_.symbols[0];
        std::transform(sym.begin(), sym.end(), sym.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        ImGui::TextColored(Theme::Tokens::TX2, "%s", sym.c_str());
        ImGui::PopFont();
    }
}

// ─── Transport Controls ─────────────────────────────────────────────────────

void ReplayManager::render_transport_controls() {
    const bool can_control = (info_.state == State::Playing || info_.state == State::Paused);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // << uses logical position (info_.current_time_ms) so rapid presses accumulate.
    // >> uses interpolated time since forward data is always in the buffer.
    const int64_t backward_current = info_.current_time_ms;
    const int64_t forward_current  = interpolated_time_ms();

    // Ghost skip button — bar + triangle glyph (GRB SkipBack/SkipForward),
    // tx-3 idle → tx-1 on hover with a soft rounded hover fill.
    auto skip_btn = [&](const char* id, bool forward, const char* tip) -> bool {
        const float sz = 30.0f;
        center_item_y(sz);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!can_control);
        ImGui::InvisibleButton(id, ImVec2(sz, sz));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (hovered && can_control) Theme::tooltip("%s", tip);
        ImGui::EndDisabled();
        const ImVec2 c = ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f);
        if (hovered && can_control)
            dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), tok(Theme::Tokens::HOVER), 6.0f);
        const ImU32 ic = !can_control ? tok(Theme::Tokens::TX4)
                       : hovered      ? tok(Theme::Tokens::TX1)
                                      : tok(Theme::Tokens::TX3);
        const float tw = 5.0f, th = 6.0f, bar = 1.6f;
        if (forward) {
            dl->AddTriangleFilled(ImVec2(c.x - tw, c.y - th), ImVec2(c.x - tw, c.y + th),
                                  ImVec2(c.x + 3.0f, c.y), ic);
            dl->AddRectFilled(ImVec2(c.x + 4.5f, c.y - th), ImVec2(c.x + 4.5f + bar, c.y + th), ic);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x + tw, c.y - th), ImVec2(c.x + tw, c.y + th),
                                  ImVec2(c.x - 3.0f, c.y), ic);
            dl->AddRectFilled(ImVec2(c.x - 4.5f - bar, c.y - th), ImVec2(c.x - 4.5f, c.y + th), ic);
        }
        return clicked && can_control;
    };

    // << previous candle (Shift = 5)
    if (skip_btn("##rp_prev", false, "Previous candle (Shift = 5)")) {
        int64_t target = prev_candle_boundary(backward_current);
        if (ImGui::GetIO().KeyShift)
            for (int i = 1; i < 5; i++) target = prev_candle_boundary(target);
        skip_backward_to(std::max(target, info_.start_time_ms));
    }

    ImGui::SameLine(0, 6);

    // ── Play / pause orb (38px circle, brand on hover, spinner while loading) ──
    {
        const float sz = 36.0f, r = 18.0f;
        center_item_y(sz);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!can_control);
        ImGui::InvisibleButton("##rp_play", ImVec2(sz, sz));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (hovered && can_control) Theme::tooltip("Play / Pause (Space)");
        ImGui::EndDisabled();

        const ImVec2 c = ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f);
        const bool loading = (info_.state == State::Creating || info_.state == State::Joining ||
                              info_.state == State::Buffering || info_.state == State::Seeking);
        const ImU32 orb_bg = !can_control ? tok(Theme::Tokens::INPUT)
                           : hovered      ? tok(Theme::Tokens::BRAND)
                                          : tok(Theme::Tokens::ACTIVE);
        dl->AddCircleFilled(c, r, orb_bg);
        const ImU32 glyph = !can_control ? tok(Theme::Tokens::TX3)
                          : hovered      ? tok(Theme::Tokens::BRAND_INK)
                                         : tok(Theme::Tokens::TX1);
        if (loading) {
            const float t = (float)ImGui::GetTime();
            dl->PathClear();
            dl->PathArcTo(c, 8.0f, t * 3.2f, t * 3.2f + 4.2f, 32);
            dl->PathStroke(tok(Theme::Tokens::BRAND), 0, 2.5f);
        } else if (info_.state == State::Playing) {
            dl->AddRectFilled(ImVec2(c.x - 6, c.y - 7), ImVec2(c.x - 2, c.y + 7), glyph, 1.0f);
            dl->AddRectFilled(ImVec2(c.x + 2, c.y - 7), ImVec2(c.x + 6, c.y + 7), glyph, 1.0f);
        } else {
            dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 7), ImVec2(c.x - 5, c.y + 7),
                                  ImVec2(c.x + 8, c.y), glyph);
        }
        if (clicked && can_control) toggle_pause();
    }

    ImGui::SameLine(0, 6);

    // >> next candle (Shift = 5)
    if (skip_btn("##rp_next", true, "Next candle (Shift = 5)")) {
        int64_t target = next_candle_boundary(forward_current);
        if (ImGui::GetIO().KeyShift)
            for (int i = 1; i < 5; i++) target = next_candle_boundary(target);
        skip_forward_to(std::min(target, info_.end_time_ms));
    }
}

// ─── Record Clip (CLIP_FACTORY P1 — replay-only v1) ─────────────────────────
// ⏺ on the transport, ghost-styled like the skip buttons; ⏹ + red ring while
// recording. The button renders ClipRecorder's REPORTED state (the JS callback),
// so it can't claim a recording the browser isn't making. Disabled when the
// boot probe failed (no MediaRecorder/captureStream/codec). The watermark badge
// + elapsed timer are drawn by ClipRecorder::tick_and_render (main loop,
// foreground draw list), NOT here — they must burn into the capture even when
// the pointer isn't on the bar.

void ReplayManager::render_record_button() {
    namespace CR = ClipRecorder;
    const bool supported   = CR::supported();
    const bool recording   = CR::is_recording();
    const bool saving      = CR::state() == CR::State::Stopped;
    const bool errored     = CR::state() == CR::State::Error;
    const bool can_control = (info_.state == State::Playing || info_.state == State::Paused);
    const bool enabled     = recording ||
                             (supported && can_control && CR::state() == CR::State::Idle);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── FOCUS pill — clip layout toggle (default ON; locked while recording).
    //    ON  → recording hides topbar/statsbar/status bar + watchlist + perf
    //          overlay: the clip frames chart+DOM+tape only.
    //    OFF → full-UI clip. Session-static, no persistence.
    {
        const bool focus_on = CR::focus_enabled();
        ImGui::PushFont(Theme::Fonts::label());
        const ImVec2 fsz = ImGui::CalcTextSize("FOCUS");
        const float pill_h = 20.0f, pad_x = 8.0f;
        const float pill_w = fsz.x + pad_x * 2.0f;
        center_item_y(pill_h);
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(recording || saving);
        ImGui::InvisibleButton("##rp_rec_focus", ImVec2(pill_w, pill_h));
        const bool f_clicked = ImGui::IsItemClicked();
        ImGui::EndDisabled();
        const bool f_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const ImVec2 fmax(fp.x + pill_w, fp.y + pill_h);
        if (focus_on) {
            dl->AddRectFilled(fp, fmax, tok(Theme::Tokens::ACTIVE), pill_h * 0.5f);
            dl->AddRect(fp, fmax, tok(Theme::Tokens::BRAND_LINE), pill_h * 0.5f, 0, 1.0f);
        } else if (f_hovered && !recording && !saving) {
            dl->AddRectFilled(fp, fmax, tok(Theme::Tokens::HOVER), pill_h * 0.5f);
        }
        const ImU32 f_txt = focus_on              ? tok(Theme::Tokens::BRAND_TX)
                          : (recording || saving) ? tok(Theme::Tokens::TX4)
                          : f_hovered             ? tok(Theme::Tokens::TX1)
                                                  : tok(Theme::Tokens::TX3);
        dl->AddText(ImVec2(fp.x + pad_x, fp.y + (pill_h - fsz.y) * 0.5f), f_txt, "FOCUS");
        ImGui::PopFont();
        if (f_hovered) {
            if (recording || saving)
                Theme::tooltip("Clip layout is locked while recording");
            else if (focus_on)
                Theme::tooltip("Focused clip: hides topbar + watchlist while recording (click for full UI)");
            else
                Theme::tooltip("Full-UI clip (click to focus: chart + DOM + tape only)");
        }
        if (f_clicked && !recording && !saving) CR::set_focus_enabled(!focus_on);
    }

    ImGui::SameLine(0, 8);
    const float sz = 30.0f;
    center_item_y(sz);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    ImGui::InvisibleButton("##rp_record", ImVec2(sz, sz));
    const bool clicked = ImGui::IsItemClicked();
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);

    if (hovered) {
        if (!supported)     Theme::tooltip("Recording not supported in this browser");
        else if (recording) Theme::tooltip("Stop recording - downloads the clip");
        else if (saving)    Theme::tooltip("Saving clip...");
        else if (errored)   Theme::tooltip("Recording failed - try again");
        else if (enabled)   Theme::tooltip("Record clip - watermarked video, 3:00 max");
    }

    const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    if (hovered && enabled)
        dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), tok(Theme::Tokens::HOVER), 6.0f);

    if (recording) {
        // ⏹ — filled square + blinking ring (mirrors the burned badge's REC dot).
        const float pulse = 0.55f + 0.45f *
            (0.5f + 0.5f * sinf(static_cast<float>(ImGui::GetTime()) * 4.0f));
        dl->AddCircle(c, 9.5f, tok(Theme::Tokens::DOWN, pulse), 0, 1.5f);
        dl->AddRectFilled(ImVec2(c.x - 4.5f, c.y - 4.5f), ImVec2(c.x + 4.5f, c.y + 4.5f),
                          tok(Theme::Tokens::DOWN), 1.5f);
    } else {
        // ⏺ — ring + red core; grey when it can't start (unsupported/loading/saving).
        const ImU32 ring = !enabled ? tok(Theme::Tokens::TX4)
                         : hovered  ? tok(Theme::Tokens::DOWN)
                                    : tok(Theme::Tokens::TX3);
        const ImU32 core = !enabled ? tok(Theme::Tokens::TX4, 0.55f)
                                    : tok(Theme::Tokens::DOWN, hovered ? 1.0f : 0.85f);
        dl->AddCircle(c, 8.0f, ring, 0, 1.5f);
        dl->AddCircleFilled(c, 4.0f, core);
    }

    if (clicked && enabled) {
        if (recording) {
            CR::stop();
        } else {
            // UPPERCASE display symbol — stack buffer, no STL in the render loop.
            char sym[24] = "REPLAY";
            if (!info_.symbols.empty()) {
                const std::string& s = info_.symbols[0];
                size_t n = 0;
                for (; n < s.size() && n < sizeof(sym) - 1; ++n) {
                    const char ch = s[n];
                    sym[n] = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 32) : ch;
                }
                sym[n] = '\0';
            }
            // Timestamp = replay DATA position (drives watermark + filename).
            CR::start(sym, interpolated_time_ms());
        }
    }
}

// ─── Speed Control ──────────────────────────────────────────────────────────

void ReplayManager::render_speed_control() {
    const bool can_control = (info_.state == State::Playing || info_.state == State::Paused);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushFont(Theme::Fonts::mono());
    const float pill_h = 22.0f, pill_pad_x = 9.0f, seg_pad = 3.0f, pill_gap = 2.0f;
    const float seg_h = pill_h + seg_pad * 2.0f;

    // Measure pill widths (must match render_control_bar's reserve math).
    char labels[NUM_SPEED_PRESETS][16];
    float widths[NUM_SPEED_PRESETS];
    float total_w = seg_pad * 2.0f;
    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        format_speed(SPEED_PRESETS[i], labels[i], sizeof(labels[i]));
        widths[i] = ImGui::CalcTextSize(labels[i]).x + pill_pad_x * 2.0f;
        total_w += widths[i] + (i > 0 ? pill_gap : 0.0f);
    }

    center_item_y(seg_h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Segmented container (var(--bg-input), r2).
    dl->AddRectFilled(p, ImVec2(p.x + total_w, p.y + seg_h),
                      tok(Theme::Tokens::INPUT), Theme::Radius::R2);

    float x = p.x + seg_pad;
    const float y = p.y + seg_pad;
    for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
        const float w = widths[i];
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::BeginDisabled(!can_control);
        ImGui::InvisibleButton(labels[i], ImVec2(w, pill_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        ImGui::EndDisabled();

        const bool locked = !Entitlements::speed_allowed(SPEED_PRESETS[i]);
        const bool active = (i == current_speed_preset_idx_) && !locked;
        if (active)
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + pill_h),
                              tok(Theme::Tokens::ACTIVE), Theme::Radius::R1);
        else if (hovered && (can_control || locked))
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + pill_h),
                              tok(Theme::Tokens::HOVER), Theme::Radius::R1);

        const ImU32 tcol = (locked || !can_control) ? tok(Theme::Tokens::TX4)
                         : (active || hovered)       ? tok(Theme::Tokens::TX1)
                                                     : tok(Theme::Tokens::TX3);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float lbl_x = locked ? x + (w - ts.x) * 0.5f - 3.0f
                                   : x + (w - ts.x) * 0.5f;
        dl->AddText(ImVec2(lbl_x, y + (pill_h - ts.y) * 0.5f), tcol, labels[i]);
        if (locked) {
            // mini padlock at the pill's right edge (shackle arc + body)
            const float lx = x + w - 6.0f, ly = y + pill_h * 0.5f;
            dl->PathArcTo(ImVec2(lx, ly - 1.0f), 2.0f, 3.14159265f, 6.2831853f, 8);
            dl->PathStroke(tok(Theme::Tokens::TX4), 0, 1.0f);
            dl->AddRectFilled(ImVec2(lx - 2.5f, ly - 1.0f), ImVec2(lx + 2.5f, ly + 3.0f),
                              tok(Theme::Tokens::TX4), 1.0f);
        }

        if (clicked) {
            if (locked)
                ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Speed);
            else if (can_control) {
                current_speed_preset_idx_ = i;
                set_speed(SPEED_PRESETS[i]);
            }
        }
        if (locked && hovered)
            Theme::tooltip("Pro plays up to 10\xc3\x97");
        x += w + pill_gap;
    }
    ImGui::PopFont();

    // Reserve the group's footprint (right group is absolute-positioned, but keep
    // the cursor sane for any future SameLine).
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total_w, seg_h));
}

// ─── Time Display ───────────────────────────────────────────────────────────

void ReplayManager::render_time_display() {
    // Use interpolated time for smooth display
    int64_t display_time = interpolated_time_ms();
    char current_buf[32], end_buf[32];
    format_timestamp(display_time, current_buf, sizeof(current_buf));
    format_timestamp(info_.end_time_ms, end_buf, sizeof(end_buf));

    // Elapsed / Remaining
    int64_t elapsed = display_time - info_.start_time_ms;
    int64_t remaining = info_.end_time_ms - display_time;
    char elapsed_buf[32], remaining_buf[32];
    format_duration(elapsed, elapsed_buf, sizeof(elapsed_buf));
    format_duration(remaining, remaining_buf, sizeof(remaining_buf));

    // Date line in the selected display zone under the clock.
    char date_buf[32] = "";
    if (display_time > 0) {
        DisplayTimeZone::instance().format(display_time, TimeZoneFormat::ReplayDate,
                                            date_buf, sizeof(date_buf));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(Theme::Fonts::mono_md());          // JBM Medium 14 — replay clock
    const ImVec2 tsz = ImGui::CalcTextSize(current_buf);
    ImGui::PopFont();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 dsz = ImGui::CalcTextSize(date_buf);
    ImGui::PopFont();

    const float line_gap = 1.0f;
    const float block_w = std::max(tsz.x, dsz.x);
    const float block_h = tsz.y + line_gap + dsz.y;
    center_item_y(block_h);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushFont(Theme::Fonts::mono_md());
    dl->AddText(p, tok(Theme::Tokens::TX1), current_buf);
    ImGui::PopFont();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    dl->AddText(ImVec2(p.x, p.y + tsz.y + line_gap), tok(Theme::Tokens::TX3), date_buf);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(block_w, block_h));
    if (ImGui::IsItemHovered()) {
        Theme::tooltip("Elapsed: %s   Remaining: %s", elapsed_buf, remaining_buf);
    }
}

// ─── Timeline Scrubber ──────────────────────────────────────────────────────

void ReplayManager::render_timeline_scrubber() {
    // Fill the gap between the clock and the right-aligned group (speed pills +
    // buffer + close). g_scrub_reserve is set in render_control_bar each frame.
    float available_width = ImGui::GetContentRegionAvail().x - g_scrub_reserve;
    if (available_width < 100.0f) available_width = 100.0f;

    // Use interpolated time for smooth scrubber movement between server updates
    float progress = 0.0f;
    int64_t interp_range = info_.end_time_ms - info_.start_time_ms;
    if (interp_range > 0) {
        int64_t interp_time = interpolated_time_ms();
        progress = static_cast<float>(interp_time - info_.start_time_ms) /
                   static_cast<float>(interp_range);
        progress = std::clamp(progress, 0.0f, 1.0f);
    }
    if (scrubber_dragging_) {
        progress = scrubber_drag_progress_;
    }

    // ─── Layout geometry (vertically centered in the bar) ────────────
    const float hit_height = 24.0f;
    center_item_y(hit_height);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 scrubber_min = ImVec2(cursor.x, cursor.y);
    ImVec2 scrubber_max = ImVec2(cursor.x + available_width, cursor.y + hit_height);

    ImGui::InvisibleButton("##scrubber", ImVec2(available_width, hit_height));

    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Hover animation — expand track on hover (smooth lerp)
    float target_anim = (hovered || active || scrubber_dragging_) ? 1.0f : 0.0f;
    scrubber_hover_anim_ += (target_anim - scrubber_hover_anim_) * 0.15f;
    if (std::abs(scrubber_hover_anim_ - target_anim) < 0.01f)
        scrubber_hover_anim_ = target_anim;

    float track_height = 4.0f + scrubber_hover_anim_ * 4.0f; // 4px → 8px
    float track_y = scrubber_min.y + hit_height * 0.5f - track_height * 0.5f;

    // ─── Track background ────────────────────────────────────────────
    int64_t range = info_.end_time_ms - info_.start_time_ms;

    dl->AddRectFilled(
        ImVec2(scrubber_min.x, track_y),
        ImVec2(scrubber_max.x, track_y + track_height),
        tok(Theme::from_hex(0x96a8b8), 0.16f),   // GRB track: rgba(150,168,184,0.16)
        track_height * 0.5f
    );

    // ─── Timeframe tick marks (on top of track bg) ───────────────────
    if (range > 0 && info_.timeframe_ms > 0) {
        int64_t tf = info_.timeframe_ms;
        int64_t first_tick = next_candle_boundary(info_.start_time_ms);

        // Count ticks to decide density
        int total_ticks = static_cast<int>((info_.end_time_ms - first_tick) / tf) + 1;
        int step = 1;
        if (total_ticks > 120) step = 10;
        else if (total_ticks > 60) step = 5;
        else if (total_ticks > 30) step = 2;

        int64_t hour_ms = 3600000LL;
        int tick_idx = 0;
        for (int64_t t = first_tick; t < info_.end_time_ms; t += tf, tick_idx++) {
            if (tick_idx % step != 0 && (t % hour_ms) != 0) continue;

            float t_progress = static_cast<float>(t - info_.start_time_ms) / static_cast<float>(range);
            float tick_x = scrubber_min.x + t_progress * available_width;

            bool is_hour = (t % hour_ms) == 0;
            float tick_h = is_hour ? 14.0f : 8.0f;
            ImU32 tick_color = is_hour ? tok(Theme::Tokens::TX4) : tok(Theme::Tokens::BD2);

            float tick_top = track_y + track_height * 0.5f - tick_h * 0.5f;
            dl->AddLine(
                ImVec2(tick_x, tick_top),
                ImVec2(tick_x, tick_top + tick_h),
                tick_color, 1.0f
            );
        }
    }

    // ─── Progress fill (cyan brand, GRB style) ───────────────────────
    float fill_x = scrubber_min.x + progress * available_width;
    dl->AddRectFilled(
        ImVec2(scrubber_min.x, track_y),
        ImVec2(fill_x, track_y + track_height),
        tok(Theme::Tokens::BRAND),
        track_height * 0.5f
    );

    // ─── Buffer ahead indicator ──────────────────────────────────────
    if (info_.buffer_ahead_seconds > 0 && range > 0) {
        float buffer_progress = progress +
            (info_.buffer_ahead_seconds * 1000.0f) / static_cast<float>(range);
        buffer_progress = std::min(buffer_progress, 1.0f);
        float buffer_x = scrubber_min.x + buffer_progress * available_width;
        dl->AddRectFilled(
            ImVec2(fill_x, track_y),
            ImVec2(buffer_x, track_y + track_height),
            tok(Theme::Tokens::BRAND, 0.22f),
            track_height * 0.5f
        );
    }

    // ─── Snap indicator during drag ──────────────────────────────────
    if (scrubber_dragging_ && range > 0) {
        int64_t drag_time = info_.start_time_ms +
            static_cast<int64_t>(scrubber_drag_progress_ * range);
        snap_target_ms_ = snap_to_candle_boundary(drag_time);
        snap_target_ms_ = std::clamp(snap_target_ms_, info_.start_time_ms, info_.end_time_ms);
        float snap_progress = static_cast<float>(snap_target_ms_ - info_.start_time_ms) /
                              static_cast<float>(range);
        float snap_x = scrubber_min.x + snap_progress * available_width;

        // Thin bright line at snap target
        dl->AddLine(
            ImVec2(snap_x, track_y - 3.0f),
            ImVec2(snap_x, track_y + track_height + 3.0f),
            tok(Theme::Tokens::BRAND), 2.0f
        );
    }

    // ─── Playhead (amber bar, GRB style) ─────────────────────────────
    ImVec2 head_center = ImVec2(fill_x, track_y + track_height * 0.5f);
    // Soft amber grab-glow on hover/drag for an easy target.
    if (hovered || scrubber_dragging_) {
        dl->AddCircleFilled(head_center, 7.0f + scrubber_hover_anim_ * 2.0f,
                            tok(Theme::Tokens::WARN, 0.18f));
    }
    const float ph_h = 14.0f;
    dl->AddRectFilled(ImVec2(fill_x - 1.0f, head_center.y - ph_h * 0.5f),
                      ImVec2(fill_x + 1.0f, head_center.y + ph_h * 0.5f),
                      tok(Theme::Tokens::WARN), 1.0f);

    // ─── Source caption + window bounds (above the track) ────────────
    // "tick-by-tick · binancef · <30d|24h> archive" centered, with the window's
    // start/end timestamps at the ends. The archive depth is tier-aware, so the
    // scrubber itself advertises what a free plan covers vs Pro.
    if (available_width > 360.0f) {
        ImGui::PushFont(Theme::Fonts::label());
        const float cap_y = track_y - 15.0f;
        char cap[72];
        snprintf(cap, sizeof(cap), "tick-by-tick \xc2\xb7 binancef \xc2\xb7 %s",
                 Entitlements::archive_label());
        const float capw = ImGui::CalcTextSize(cap).x;
        dl->AddText(ImVec2(scrubber_min.x + (available_width - capw) * 0.5f, cap_y),
                    tok(Theme::Tokens::TX4), cap);
        char sb[40], eb[40];
        format_timestamp_full(info_.start_time_ms, sb, sizeof(sb));
        format_timestamp_full(info_.end_time_ms, eb, sizeof(eb));
        dl->AddText(ImVec2(scrubber_min.x, cap_y), tok(Theme::Tokens::TX4), sb);
        dl->AddText(ImVec2(scrubber_max.x - ImGui::CalcTextSize(eb).x, cap_y),
                    tok(Theme::Tokens::TX4), eb);
        ImGui::PopFont();
    }

    // ─── Drag interaction ────────────────────────────────────────────
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        scrubber_dragging_ = true;
        float mouse_x = ImGui::GetIO().MousePos.x;
        scrubber_drag_progress_ = std::clamp(
            (mouse_x - scrubber_min.x) / available_width, 0.0f, 1.0f);
        // Ghost preview follows the drag instantly (no intent delay): the grab
        // is deliberate, and aiming is exactly when the ghosts matter.
        if (range > 0) {
            const int64_t drag_ms = info_.start_time_ms +
                static_cast<int64_t>(static_cast<double>(scrubber_drag_progress_) *
                                     static_cast<double>(range));
            set_scrub_preview_ms(std::clamp(drag_ms, info_.start_time_ms, info_.end_time_ms));
        }
    }
    if (scrubber_dragging_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Mouse released — commit seek snapped to candle boundary
        if (range > 0 && snap_target_ms_ > 0) {
            seek(snap_target_ms_);
        } else {
            seek_to_progress(scrubber_drag_progress_);
        }
        scrubber_dragging_ = false;
        snap_target_ms_ = 0;
        // Preview off on commit — the chart re-fits to the landing position.
        set_scrub_preview_ms(0);
        scrub_hover_since_ = 0.0;
    }

    // ─── Click to seek (non-drag) — snap to nearest candle ──────────
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !scrubber_dragging_) {
        float mouse_x = ImGui::GetIO().MousePos.x;
        float click_progress = std::clamp(
            (mouse_x - scrubber_min.x) / available_width, 0.0f, 1.0f);
        if (range > 0) {
            int64_t click_time = info_.start_time_ms +
                static_cast<int64_t>(click_progress * range);
            int64_t snapped = snap_to_candle_boundary(click_time);
            snapped = std::clamp(snapped, info_.start_time_ms, info_.end_time_ms);
            seek(snapped);
        } else {
            seek_to_progress(click_progress);
        }
    }

    // ─── Scrub preview arming (hover with intent delay) ─────────────
    // The pointer crosses the transport constantly on its way elsewhere, so a
    // bare hover must not lurch the chart into map view. Hover arms the ghost
    // preview only after a short dwell; a drag armed it instantly above.
    if (!scrubber_dragging_) {
        if (hovered && range > 0) {
            const double now_s = ImGui::GetTime();
            if (scrub_hover_since_ == 0.0) scrub_hover_since_ = now_s;
            if (now_s - scrub_hover_since_ >= 0.30) {
                const float mouse_x = ImGui::GetIO().MousePos.x;
                const float hp = std::clamp(
                    (mouse_x - scrubber_min.x) / available_width, 0.0f, 1.0f);
                const int64_t hover_ms = info_.start_time_ms +
                    static_cast<int64_t>(static_cast<double>(hp) *
                                         static_cast<double>(range));
                set_scrub_preview_ms(std::clamp(hover_ms, info_.start_time_ms,
                                                info_.end_time_ms));
            }
        } else {
            scrub_hover_since_ = 0.0;
            set_scrub_preview_ms(0);
        }
    }

    // ─── Enhanced tooltip ────────────────────────────────────────────
    if (hovered && range > 0) {
        float mouse_x = ImGui::GetIO().MousePos.x;
        float hover_progress = std::clamp(
            (mouse_x - scrubber_min.x) / available_width, 0.0f, 1.0f);
        int64_t hover_time = info_.start_time_ms +
            static_cast<int64_t>(hover_progress * range);
        int64_t snapped = snap_to_candle_boundary(hover_time);
        snapped = std::clamp(snapped, info_.start_time_ms, info_.end_time_ms);

        char hover_buf[48], snap_buf[48];
        format_timestamp_full(hover_time, hover_buf, sizeof(hover_buf));
        format_timestamp(snapped, snap_buf, sizeof(snap_buf));

        // Price at the aimed-at time + delta vs the playhead, from the scrub-
        // preview store (empty string until the preview batch lands).
        char px_line[80] = "";
        if (const PreviewCandleStore* pv = preview_candles(); pv && pv->ready()) {
            double aim_px = 0.0, now_px = 0.0;
            const bool have_aim = pv->close_at(snapped, aim_px);
            const bool have_now = pv->close_at(interpolated_time_ms(), now_px) && now_px > 0.0;
            if (have_aim) {
                char px_buf[24];
                const std::string sym = info_.symbols.empty() ? "" : info_.symbols.front();
                SymbolRegistry::instance()
                    .get_formatter("binancef", sym)
                    .format_price(px_buf, sizeof(px_buf), aim_px);
                if (have_now) {
                    snprintf(px_line, sizeof(px_line), "\n%s  (%+.2f%% from now)",
                             px_buf, (aim_px - now_px) / now_px * 100.0);
                } else {
                    snprintf(px_line, sizeof(px_line), "\n%s", px_buf);
                }
            }
        }

        if (scrubber_dragging_) {
            Theme::tooltip("%s\nSnaps to: %s%s", hover_buf, snap_buf, px_line);
        } else {
            Theme::tooltip("%s (click: %s)%s", hover_buf, snap_buf, px_line);
        }
    }
    // (no trailing end-time label — the right-aligned speed/buffer/close group
    //  owns the space to the scrubber's right; the clock + tooltip cover timing.)
}

// ─── Buffer Health Indicator ────────────────────────────────────────────────

void ReplayManager::render_buffer_indicator() {
    const float box = 14.0f;
    if (info_.buffer_ahead_seconds <= 0) {
        ImGui::Dummy(ImVec2(box, box));  // hold the slot so the close button stays put
        return;
    }

    // Buffer-health dot: green healthy / amber low / red critical (tokens).
    const ImVec4 color = info_.buffer_ahead_seconds > 60.0f ? Theme::Tokens::UP
                       : info_.buffer_ahead_seconds > 10.0f ? Theme::Tokens::WARN
                                                            : Theme::Tokens::DOWN;
    center_item_y(box);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(p.x + box * 0.5f, p.y + box * 0.5f);
    dl->AddCircleFilled(c, 3.5f, tok(color));
    dl->AddCircle(c, 5.5f, tok(color, 0.35f), 16, 1.0f);
    ImGui::Dummy(ImVec2(box, box));

    if (ImGui::IsItemHovered()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Buffer: %.0fs ahead", info_.buffer_ahead_seconds);
        Theme::tooltip("%s", buf);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Replay Range Picker (free-window launcher) — design §8.1–8.2
//
// The primary free-tier surface: frame free replay as "a new free window unlocks
// every day", not a locked archive. The FREE band (48-72h ago — always archived
// parquet days, never the warm DB) is the hero; RECENT + ARCHIVE are visible,
// labelled and locked; locked bands / presets / speeds all funnel into the ONE
// upsell modal. Opened from the chart right-click "Replay range…".
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayManager::open_replay_launcher(const std::string& symbol, int64_t timeframe_seconds) {
    launcher_symbol_     = symbol;
    launcher_tf_seconds_ = timeframe_seconds > 0 ? timeframe_seconds : 300;
    launcher_preset_     = 0;                 // default = the free archived-day band
    launcher_days_ago_   = 2;
    launcher_speed_      = static_cast<float>(Entitlements::clamp_speed_for_tier(launcher_speed_));
    if (launcher_speed_ <= 0.0f) launcher_speed_ = 1.0f;
    launcher_open_       = true;
}

void ReplayManager::render_replay_launcher() {
    if (!launcher_open_) return;
    using namespace Theme;
    const int64_t now = now_ms();
    const bool pro = Entitlements::is_pro();
    const float CW = 486.0f;                  // fixed content width (auto-resize friendly)

    if (!pro) launcher_preset_ = 0;           // free can only select the free band

    const ImVec2 vc = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vc, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border,        Tokens::BD2);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, Tokens::PANEL);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(18.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("Replay range###edx_replay_launcher", &open, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        auto mini_lock = [&](float lx, float ly, ImU32 c) {
            dl->PathArcTo(ImVec2(lx, ly - 1.0f), 2.0f, 3.14159265f, 6.2831853f, 8);
            dl->PathStroke(c, 0, 1.0f);
            dl->AddRectFilled(ImVec2(lx - 2.5f, ly - 1.0f), ImVec2(lx + 2.5f, ly + 3.0f), c, 1.0f);
        };

        // ── Header: symbol + free-symbol strip ───────────────────────────────
        std::string su = launcher_symbol_;
        std::transform(su.begin(), su.end(), su.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ImGui::PushFont(Fonts::heading());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::Text("Replay %s", su.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        if (pro) {
            char reach[64];
            snprintf(reach, sizeof(reach), "Any symbol, any moment in the last %d days.",
                     Entitlements::pro_lookback_days());
            ImGui::TextUnformatted(reach);
        }
        else     ImGui::Text("Free replay: %s \xc2\xb7 new window daily.", Entitlements::free_symbols_label());
        ImGui::PopStyleColor();

        // ── 30-day rail: RECENT (locked) · FREE band (hero) · ARCHIVE (locked)
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        const float rail_h = 48.0f;
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float w_today = CW * 0.22f, w_free = CW * 0.44f, w_arch = CW - w_today - w_free;
        const ImVec2 aT0 = r0,                         aT1 = ImVec2(r0.x + w_today, r0.y + rail_h);
        const ImVec2 aF0 = ImVec2(aT1.x, r0.y),        aF1 = ImVec2(aT1.x + w_free, r0.y + rail_h);
        const ImVec2 aG0 = ImVec2(aF1.x, r0.y),        aG1 = ImVec2(r0.x + CW, r0.y + rail_h);
        const bool free_sel = (launcher_preset_ == 0);
        dl->AddRectFilled(aT0, aT1, tok(Tokens::ELEV), 0.0f);
        dl->AddRectFilled(aF0, aF1, tok(Tokens::BRAND, free_sel ? 0.22f : 0.14f), 0.0f);
        dl->AddRectFilled(aG0, aG1, tok(Tokens::ELEV), 0.0f);
        dl->AddRect(aF0, aF1, tok(Tokens::BRAND_LINE), 0.0f, 0, free_sel ? 2.0f : 1.2f);
        dl->AddRect(aT0, aG1, tok(Tokens::BD2), 0.0f, 0, 1.0f);

        float pulse = 0.0f;
        if (launcher_lock_pulse_ > 0.0) {
            const double dt = ImGui::GetTime() - launcher_lock_pulse_;
            if (dt < 0.6) pulse = 1.0f - static_cast<float>(dt / 0.6);
            else launcher_lock_pulse_ = 0.0;
        }
        auto band_text = [&](ImVec2 b0, ImVec2 b1, const char* top, const char* sub,
                             ImU32 topc, ImU32 subc) {
            const float cx = (b0.x + b1.x) * 0.5f, cy = (b0.y + b1.y) * 0.5f;
            ImGui::PushFont(Fonts::label());
            const ImVec2 tsz = ImGui::CalcTextSize(top);
            dl->AddText(ImVec2(cx - tsz.x * 0.5f, cy - tsz.y - 1.0f), topc, top);
            ImGui::PopFont();
            ImGui::PushFont(Fonts::mono_sm());
            const ImVec2 ssz = ImGui::CalcTextSize(sub);
            dl->AddText(ImVec2(cx - ssz.x * 0.5f, cy + 1.0f), subc, sub);
            ImGui::PopFont();
        };
        const ImU32 lock_c = tok(Tokens::TX3, 0.6f + 0.4f * pulse);
        band_text(aT0, aT1, "RECENT", "PRO \xc2\xb7 48h", lock_c, lock_c);
        band_text(aF0, aF1, "FREE \xc2\xb7 2 DAYS AGO", "full fidelity",
                  tok(Tokens::BRAND_TX), tok(Tokens::TX2));
        band_text(aG0, aG1, "ARCHIVE", "PRO \xc2\xb7 30d", lock_c, lock_c);
        mini_lock(aT1.x - 9.0f, aT0.y + 9.0f, lock_c);
        mini_lock(aG1.x - 9.0f, aG0.y + 9.0f, lock_c);

        ImGui::SetCursorScreenPos(aT0); ImGui::InvisibleButton("##bandT", ImVec2(w_today, rail_h));
        const bool clkT = ImGui::IsItemClicked();
        ImGui::SetCursorScreenPos(aF0); ImGui::InvisibleButton("##bandF", ImVec2(w_free, rail_h));
        const bool clkF = ImGui::IsItemClicked();
        ImGui::SetCursorScreenPos(aG0); ImGui::InvisibleButton("##bandG", ImVec2(w_arch, rail_h));
        const bool clkG = ImGui::IsItemClicked();
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rail_h + 6.0f));
        if (clkF) launcher_preset_ = 0;
        if (clkT || clkG) {
            if (pro) launcher_preset_ = clkT ? 2 : 3;   // Pro: today→Last24h, archive→PickDay
            else { launcher_lock_pulse_ = ImGui::GetTime();
                   ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Range); }
        }

        // ── Countdown caption ────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextUnformatted("A new free window unlocks every day.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_TX);
        ImGui::Text("Free window: %s", Entitlements::free_window_label().c_str());
        ImGui::PopStyleColor();
        {
            time_t s = static_cast<time_t>(now / 1000);
            struct tm tmv;
#ifdef _WIN32
            gmtime_s(&tmv, &s);
#else
            gmtime_r(&s, &tmv);
#endif
            const int to_mid = 86400 - (tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
            ImGui::PushFont(Fonts::mono_sm());
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_TX);
            ImGui::Text("A new day fully unlocks in %dh %02dm", to_mid / 3600, (to_mid % 3600) / 60);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        // ── Presets ──────────────────────────────────────────────────────────
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        auto chip = [&](const char* label, int idx, bool locked) {
            const bool sel = (launcher_preset_ == idx) && !locked;
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button,        Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_Text,          Tokens::BRAND_TX);
                ImGui::PushStyleColor(ImGuiCol_Border,        Tokens::BRAND_LINE);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text,          locked ? Tokens::TX4 : Tokens::TX2);
                ImGui::PushStyleColor(ImGuiCol_Border,        Tokens::BD2);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            char lb[48];
            snprintf(lb, sizeof(lb), locked ? "%s    " : "%s", label);  // reserve room for the lock
            const bool clicked = ImGui::Button(lb, ImVec2(0.0f, 26.0f));
            if (locked) {
                const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                mini_lock(rmax.x - 12.0f, (rmin.y + rmax.y) * 0.5f, tok(Tokens::TX4));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            if (clicked) {
                if (locked) { launcher_lock_pulse_ = ImGui::GetTime();
                              ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Preset); }
                else launcher_preset_ = idx;
            }
        };
        chip("Free window", 0, false);    ImGui::SameLine(0, 6);
        chip("Last 6h",   1, !pro);       ImGui::SameLine(0, 6);
        chip("Last 24h",  2, !pro);       ImGui::SameLine(0, 6);
        chip("Pick any day", 3, !pro);

        if (pro && launcher_preset_ == 3) {
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderInt("##daysago", &launcher_days_ago_, 2, 30, "%d days ago");
        }

        // ── Speed row (0.5/1/2 free · 3/5/10 Pro) ────────────────────────────
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::PushFont(Fonts::label());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::TextUnformatted("SPEED");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        static const float kRow[] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f };
        for (int i = 0; i < 6; ++i) {
            const bool locked = !Entitlements::speed_allowed(kRow[i]);
            const bool sel = std::abs(launcher_speed_ - kRow[i]) < 0.01f && !locked;
            if (i) ImGui::SameLine(0, 6);
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button,        Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_Text,          Tokens::BRAND_TX);
                ImGui::PushStyleColor(ImGuiCol_Border,        Tokens::BRAND_LINE);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text,          locked ? Tokens::TX4 : Tokens::TX2);
                ImGui::PushStyleColor(ImGuiCol_Border,        Tokens::BD2);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), locked ? "%g\xc3\x97   " : "%g\xc3\x97", kRow[i]);
            const bool clicked = ImGui::Button(lbl, ImVec2(0.0f, 24.0f));
            if (locked) {
                const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                mini_lock(rmax.x - 8.0f, (rmin.y + rmax.y) * 0.5f, tok(Tokens::TX4));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            if (clicked) {
                if (locked) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Speed);
                else launcher_speed_ = kRow[i];
            }
        }

        // ── Sessions-today chip (free) ───────────────────────────────────────
        if (!pro) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            const int used = Entitlements::sessions_today();
            const int cap  = Entitlements::FREE_MAX_SESSIONS_PER_DAY;
            ImGui::PushStyleColor(ImGuiCol_Text, used >= cap ? Tokens::WARN : Tokens::TX3);
            ImGui::Text("Replays today: %d of %d", used > cap ? cap : used, cap);
            ImGui::PopStyleColor();
        }

        // ── Play / Cancel ────────────────────────────────────────────────────
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        int64_t sms, ems;
        const int64_t DAY = 86400000LL;
        switch (launcher_preset_) {
            case 1: sms = now - 6LL * 3600000LL;  ems = now; break;             // Last 6h (Pro)
            case 2: sms = now - 24LL * 3600000LL; ems = now; break;             // Last 24h (Pro)
            case 3: ems = now - static_cast<int64_t>(launcher_days_ago_) * DAY; // Pick any day (Pro)
                    sms = ems - DAY; break;
            default: {  // Free window — request EXACTLY the window Entitlements resolves:
                        // the backend's archived-day bounds when known (server-authoritative,
                        // so the modal requests precisely what the backend enforces), else the
                        // static [72h,48h] band inset for clock drift. One source of truth.
                Entitlements::free_window_range(now, sms, ems);
            } break;
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_TX);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_Text,          Tokens::BRAND_INK);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        ImGui::PushFont(Fonts::ui_semibold());
        const char* play_lbl = (!pro || launcher_preset_ == 0) ? "Play free window" : "Play";
        if (ImGui::Button(play_lbl, ImVec2(CW, 38.0f))) {
            launcher_open_ = false;
            request_replay({ launcher_symbol_ }, sms, ems, launcher_speed_, launcher_tf_seconds_);
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) launcher_open_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    if (!open) launcher_open_ = false;
}
