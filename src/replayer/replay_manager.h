#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// ReplayManager - Client-side replay orchestration
//
// Lifecycle:
//   1. User initiates replay (chart right-click, keyboard R, alert button)
//   2. POST /replay/session → receive session_id (async, via EM_ASM XHR)
//   3. WS: join_replay { session_id, entitlement_token } → backend verifies
//      ownership, then spawns Replayer actor
//   4. Backend streams protobuf frames → same message pipeline as live
//   5. Control bar: play/pause, speed, seek/scrub, step, stop
//
// The render pipeline is agnostic - replay data flows through the same
// MessageHandler → StreamManager → Widget path as live data.
//
// State machine:
//   Idle → Creating → Joining → Buffering → Playing ⇄ Paused → Stopped → Idle
//
// Entry points (call request_replay):
//   - Chart right-click context menu ("Replay from here")
//   - Chart drag-select time range ("Replay this range")
//   - Keyboard shortcut (R with chart focused)
//   - Alert card "Replay from" button
//   - Menu bar → Replay
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>
#include <memory>
#include "../core/data_context.h"
#include "../core/message_context.h"
#include "replay_history_buffer.h"

class WebSocketClient;
class PackReplayEngine;

class ReplayManager {
public:
    // ─── State Machine ───────────────────────────────────────────────────
    enum class State : uint8_t {
        Idle,       // No replay active
        Creating,   // POST /replay/session in flight
        Joining,    // WS join_replay sent, waiting for replay_joined
        Buffering,  // Joined, backend is buffering initial data
        Playing,    // Replay playing at speed_
        Paused,     // Replay paused, can resume/seek
        Seeking,    // Seek in progress (brief - backend re-buffers)
        Stopped,    // Replay ended (completed or user stopped)
        Error,      // Something went wrong
    };

    // ─── Session Info ────────────────────────────────────────────────────
    struct SessionInfo {
        std::string session_id;
        std::string session_type;       // "nats" or "archive"
        std::vector<std::string> symbols;
        std::vector<std::string> streams; // Effective server-enforced stream grant
        int64_t start_time_ms = 0;      // Replay window start
        int64_t end_time_ms = 0;        // Replay window end
        int64_t current_time_ms = 0;    // Current playback position
        float speed = 1.0f;             // Playback speed multiplier
        float progress = 0.0f;          // 0.0 to 1.0
        int64_t timeframe_ms = 300000;  // Current candle TF in ms (default 5m)
        State state = State::Idle;

        // Buffer health (from backend status updates)
        int buffer_size = 0;
        int buffer_capacity = 0;
        float buffer_ahead_seconds = 0.0f;

        // Error info
        std::string error_message;

        // Timing
        int64_t session_created_at = 0; // Wall time when session was created
        int64_t last_status_update = 0; // Last backend status received
    };

    // ─── Speed Presets ───────────────────────────────────────────────────
    static constexpr float SPEED_PRESETS[] = {
        0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 4.0f
    };
    static constexpr int NUM_SPEED_PRESETS = 6;
    static constexpr float MIN_SPEED = 0.1f;
    static constexpr float MAX_SPEED = 4.0f;

    // ─── Skip Durations (seconds) ────────────────────────────────────────
    static constexpr int64_t SKIP_SMALL  = 10;     // 10 seconds
    static constexpr int64_t SKIP_MEDIUM = 60;     // 1 minute
    static constexpr int64_t SKIP_LARGE  = 300;    // 5 minutes

    // ─── Construction ────────────────────────────────────────────────────
    explicit ReplayManager(WebSocketClient* ws_client);
    ~ReplayManager();  // Defined in .cpp - DataContext has unique_ptr members

    // ─── Context Swap Callback ───────────────────────────────────────────
    // AppState provides this callback so ReplayManager can trigger
    // AppContext pointer swaps without knowing about AppState internals.
    // Called with (replay_ctx_ptr) when entering replay,
    // called with (nullptr) when exiting replay (swap back to live).
    using ContextSwapFn = std::function<void(DataContext*)>;
    void set_context_swap_callback(ContextSwapFn fn) { on_context_swap_ = std::move(fn); }

    // Called on backward skip (<<) so widgets can clear internal state
    // (trades buffer, DOM accumulators, debug logs, etc.) that contain
    // "future" data from past the rewind target. cutoff_ms is the
    // rewind target timestamp.
    using RewindFn = std::function<void(int64_t cutoff_ms)>;
    void set_rewind_callback(RewindFn fn) { on_rewind_ = std::move(fn); }

    // ─── Replay DataContext Access ───────────────────────────────────────
    // Returns the replay data context (null if no replay active).
    // Used by message routing to get the replay MessageContext.
    DataContext* replay_context() { return replay_ctx_.get() ? replay_ctx_.get() : nullptr; }
    const DataContext* replay_context() const { return replay_ctx_.get(); }

    // Build a MessageContext from the replay DataContext for message routing.
    // Returns a zero-initialized context if no replay is active.
    MessageContext replay_message_context() const;

    // ─── Replay Initiation ───────────────────────────────────────────────
    // Primary entry point. Creates session via REST, then joins via WS.
    // symbols: e.g. {"dogeusdt"}, or {"btcusdt", "ethusdt"}
    // start/end: unix ms. If end==0, defaults to start + 2 hours.
    // speed: initial playback speed (0.1 to 10.0)
    void request_replay(
        const std::vector<std::string>& symbols,
        int64_t start_time_ms,
        int64_t end_time_ms = 0,
        float speed = 1.0f,
        int64_t timeframe_seconds = 300
    );

    // Convenience: single symbol replay from a specific time
    void request_replay_from(const std::string& symbol, int64_t timestamp_ms, float speed = 1.0f, int64_t timeframe_seconds = 300);

    // Archive replay (for historical events beyond the 30-day live window). Boots
    // an archived market-event session: POST /replay/session/archive {event_id} →
    // join (same WS join path as NATS). `symbol` is for client-side display/context
    // (control-bar caption); the backend resolves event_id → archive_path + window.
    void request_archive_replay(const std::string& event_id, const std::string& symbol = "", float speed = 1.0f);

    // Pack replay (Hot Replay Path B): play a self-contained .edpack served
    // static from R2/CDN - no session POST, no WebSocket, no token (public
    // showcase packs). The PackReplayEngine reproduces the box's session
    // behavior client-side and drives this manager through the same
    // handle_ws_message / route_parsed entries the WS path uses; controls are
    // rerouted to the engine by send_control*. `symbol_hint` is display-only
    // (the pack header is authoritative).
    void request_pack_replay(const std::string& pack_url, const std::string& symbol_hint = "", float speed = 1.0f);

    // Per-frame pack engine driver - call from the main loop alongside
    // flush_pending_skip (no-op outside pack mode).
    void tick_pack_engine();
    bool is_pack_mode() const { return pack_mode_; }

    // ─── Usage instrumentation (design §3) ───────────────────────────────
    // Per-frame: accrue replay WATCH-seconds only while playing AND the document
    // is visible (wall clock, the "minutes actually watched" metric), plus the
    // speed-weighted market-seconds span of interpolated_time_ms(), and emit a
    // ~15s heartbeat carrying the running totals. replay_start / replay_end are
    // emitted at the transition() choke point so both the WS ReplayManager path
    // and the PackReplayEngine (which drives this same manager) are measured
    // identically. Call from the main loop alongside the other per-frame ticks.
    void tick_usage(double dt_seconds, bool document_visible);

    // ─── Playback Control ────────────────────────────────────────────────
    void pause();
    void resume();
    void toggle_pause();        // Space bar
    void stop();
    void set_speed(float speed);
    void speed_up();            // Next preset
    void speed_down();          // Previous preset
    // deliberate=true bypasses the scrubber-drag debounce - use for discrete
    // nav (lesson rail click, Back, Restart) where a swallowed call = dead button.
    void seek(int64_t timestamp_ms, bool deliberate = false);
    void seek_to_progress(float progress); // 0.0 to 1.0
    void skip_forward(int64_t seconds = SKIP_MEDIUM);
    void skip_backward(int64_t seconds = SKIP_MEDIUM);

    // Clock-only forward skip - sends "skip_forward" control message.
    // No consumer teardown, no buffer clearing. Instant.
    void skip_forward_to(int64_t timestamp_ms);

    // Clock-only backward skip - repositions clock + trims future candles.
    // No consumer teardown. The playback loop will re-deliver data naturally
    // when the clock catches up to the buffer position.
    void skip_backward_to(int64_t timestamp_ms);

    // ─── State Queries ───────────────────────────────────────────────────
    bool is_active() const;     // Any state except Idle/Stopped/Error
    bool is_playing() const { return info_.state == State::Playing; }
    bool is_paused() const { return info_.state == State::Paused; }
    bool is_idle() const { return info_.state == State::Idle; }
    // Current playback speed multiplier (for the chart status chip).
    float current_speed() const { return SPEED_PRESETS[current_speed_preset_idx_]; }
    // True while the session is joined but its data context hasn't been primed
    // yet (no candles / no OB seed). The clock is HELD here so playback never
    // starts against empty buffers. Drives loading spinners (lesson + live).
    bool is_loading() const { return info_.state == State::Buffering; }
    State state() const { return info_.state; }
    const SessionInfo& info() const { return info_; }

    // Per-frame. Promotes Buffering→Playing once the replay context is primed
    // (candles populated + orderbook seed received). Call every frame from the
    // main loop alongside flush_pending_skip / drip_feed_pending_replay. This is
    // what holds the replay clock until real data is flowing - for ALL replays
    // (lessons AND the live terminal's "replay from here"), not just lessons.
    void tick_buffering_gate();

    // Update the candle timeframe (called when user switches TF during replay).
    // Affects scrubber tick marks, candle-boundary snapping, skip amounts, and
    // re-fits the scrub-preview (ghost) candle batch. Defined in the .cpp.
    void set_timeframe_ms(int64_t tf_ms);

    // ─── Scrub preview (ghost candles) ───────────────────────────────────
    // While the user hovers/drags the timeline, this is the aimed-at replay
    // time (unix ms); 0 = no preview. Set by the native scrubber (hover with
    // intent delay, drag immediately) and by the React transports via the
    // bridge. ChartWidget reads it each frame to ghost-render the future and
    // temporarily extend the axis fit. Always cleared when preview input ends.
    // Clamped to the seek ceiling while one is armed: a locked lesson step
    // must not ghost-reveal the region it forbids seeking into.
    void set_scrub_preview_ms(int64_t ms) {
        if (ms > 0 && seek_ceiling_active() && ms > seek_ceiling_ms_)
            ms = seek_ceiling_ms_;
        scrub_preview_ms_ = ms;
    }
    int64_t scrub_preview_ms() const {
        return is_active() ? scrub_preview_ms_ : 0;
    }
    // The preview candle batch for the active replay (null when none/live).
    const PreviewCandleStore* preview_candles() const {
        return replay_ctx_ ? replay_ctx_->preview : nullptr;
    }

    // ─── Seek ceiling (lesson scrub-forward lock) ────────────────────────
    // While armed, every FORWARD motion (seek, scrub commit, >> skip, step
    // jump - all transports plus keyboard) clamps to this replay time, and the
    // ghost preview clamps with it. Backward motion is never restricted.
    // LessonRuntime re-pushes it EVERY frame while a lockForward step is
    // active (typically = the playhead); the ceiling self-expires when pushes
    // stop (kSeekCeilingTTLMs), so a torn-down lesson/export can never leave a
    // stale lock armed. This is the ONE enforcement point; the React bars only
    // mirror it visually.
    void set_seek_ceiling_ms(int64_t ms) {
        seek_ceiling_ms_ = ms;
        if (ms > 0) seek_ceiling_set_wall_ms_ = now_ms();
    }
    bool seek_ceiling_active() const {
        return seek_ceiling_ms_ > 0 &&
               (now_ms() - seek_ceiling_set_wall_ms_) <= kSeekCeilingTTLMs;
    }
    int64_t seek_ceiling_ms() const { return seek_ceiling_active() ? seek_ceiling_ms_ : 0; }

    // After a backward skip, this is the cutoff timestamp. Widgets should
    // discard/hide data with timestamps after this value. Reset to 0 when
    // the playback clock passes the cutoff (data is flowing again).
    int64_t rewind_cutoff_ms() const { return rewind_cutoff_ms_; }

    // True while a rewind is in flight - binary frames should be dropped.
    bool is_rewind_pending() const { return rewind_pending_; }

    // True during drip-feed catch-up after a client-only rewind.
    // Binary frames should be captured but NOT routed while this is true.
    bool is_drip_feeding() const { return drip_feed_active_; }

    // Flush any debounced skip whose window has expired.
    // Call this every frame from the main loop (alongside drip_feed_pending_replay).
    void flush_pending_skip();

    // Flush a deferred WS join. join_session() latches instead of erroring when
    // the WS handshake hasn't completed yet (the studio boot path fires
    // request_replay as soon as the wasm runtime is up - calledRun - which is
    // EARLIER than the WS connect; the lesson path dodged this via g_init_complete).
    // Call this every frame from the main loop alongside flush_pending_skip().
    void flush_pending_join();

    // ─── History Buffer Access ───────────────────────────────────────────
    // Returns the history buffer (null if no replay active).
    ReplayHistoryBuffer* history_buffer() { return history_buffer_.get(); }
    const ReplayHistoryBuffer* history_buffer() const { return history_buffer_.get(); }

    // Drip-feed pending buffer entries as the clock advances after a rewind.
    // Call this every frame from the main loop.
    void drip_feed_pending_replay();

    // Smooth interpolated time for display - advances frame-by-frame between
    // server status updates using local wall clock × speed. Prevents the timer
    // from jumping in 2-second increments.
    int64_t interpolated_time_ms() const;

    // ─── Message Handling ────────────────────────────────────────────────
    // Called by message routing layer when WS messages arrive.
    // Returns true if the message was handled (replay-related).
    bool handle_ws_message(const std::string& type, const void* json_data);

    // ─── Rendering ───────────────────────────────────────────────────────
    // Renders the persistent replay control bar at the bottom of the screen.
    // Call this in main_loop after widgets. Only renders when is_active().
    void render_control_bar();

    // Renders the free-window replay range picker (design §8.1-8.2): the hero
    // FREE band (48-72h ago), locked RECENT/ARCHIVE bands, presets, live countdown,
    // a tier-locked speed row, and the "N of 6 today" chip. Renders only when the
    // launcher is open (open_replay_launcher). Call from the main loop.
    void render_replay_launcher();

    // Open the range picker for `symbol` (from the chart right-click "Replay
    // range…" item). tf drives the initial replay timeframe.
    void open_replay_launcher(const std::string& symbol, int64_t timeframe_seconds = 300);

    // Navigate the host browser to the FOCUSED research-replay viewer
    // (the web app's /terminal?replay=) for an explicit window, seeked to
    // seek_ms (0 = none). Live web terminal only: a no-op inside an embedded
    // chrome (studio/lesson/event), and an in-place replay fallback in native
    // dev builds (no browser to navigate to).
    void open_focused_replay(const std::string& symbol, int64_t from_ms, int64_t to_ms,
                             int64_t seek_ms);
    // Convenience: a 4h window AROUND an anchor bar (30m lead), seeked to it.
    void open_focused_replay_at(const std::string& symbol, int64_t anchor_ms);

    // Renders a context menu section for chart integration.
    // Called from within an ImGui::BeginPopup context.
    // hovered_time_ms: timestamp under cursor (from ImPlot mouse pos)
    // symbol: current chart symbol
    // Returns true if a replay was initiated.
    bool render_chart_context_menu(
        const std::string& symbol,
        int64_t hovered_time_ms,
        int64_t timeframe_seconds = 300,
        int64_t selection_start_ms = 0,
        int64_t selection_end_ms = 0
    );

    // ─── Keyboard Shortcuts ──────────────────────────────────────────────
    // Call each frame. Processes replay-related keyboard shortcuts.
    // Returns true if a shortcut was consumed.
    bool process_keyboard_shortcuts();

    // ─── Time Formatting ─────────────────────────────────────────────────
    static void format_timestamp(int64_t ms, char* buf, size_t buf_size);
    static void format_duration(int64_t ms, char* buf, size_t buf_size);
    static void format_speed(float speed, char* buf, size_t buf_size);
    static void format_timestamp_full(int64_t ms, char* buf, size_t buf_size);
    static void format_duration_short(int64_t seconds, char* buf, size_t buf_size);

    // Callback invoked when session is successfully created (internal use)
    void on_session_created(const char* response_json, int len);
    // Callback invoked on session creation failure (internal use)
    void on_session_error(int status_code, const char* error_msg);

private:
    WebSocketClient* ws_client_;
    SessionInfo info_;

    // ─── Internal State ──────────────────────────────────────────────────
    int current_speed_preset_idx_ = 3; // Index into SPEED_PRESETS (1.0x)
    bool scrubber_dragging_ = false;
    float scrubber_drag_progress_ = 0.0f;
    float scrubber_hover_anim_ = 0.0f;   // 0→1 track hover expansion
    int64_t snap_target_ms_ = 0;          // Candle boundary snap during drag
    // Scrub preview (ghost candles): aimed-at replay time while hovering or
    // dragging the timeline, 0 = off. Native bar arms it directly (hover after
    // an intent delay, drag instantly); React bars arm it via the bridge.
    int64_t scrub_preview_ms_ = 0;
    double  scrub_hover_since_ = 0.0;     // ImGui::GetTime() when track hover began (intent delay)
    // Seek ceiling (lesson scrub-forward lock): forward motion clamps to this
    // replay time while armed. Owned per frame by LessonRuntime; self-expires
    // when the per-frame pushes stop (see seek_ceiling_active).
    static constexpr int64_t kSeekCeilingTTLMs = 500;
    int64_t seek_ceiling_ms_ = 0;
    int64_t seek_ceiling_set_wall_ms_ = 0;
    int64_t last_seek_time_ms_ = 0;    // Debounce seeks
    static constexpr int64_t SEEK_DEBOUNCE_MS = 200;
    int lightweight_seek_count_ = 0;   // Number of pending lightweight replay_seeked responses
    // After a backward skip, ignore this many replay_status messages.
    // The playback loop goroutine may have sent statuses with the OLD time
    // before the clock re-anchor took effect. Combined with a time-based
    // jump guard (rewind_cutoff_ms_) in the status handler for robustness.
    int skip_ignore_status_count_ = 0;
    // Set on backward skip to the target time. Widgets use this to
    // discard/hide data with timestamps after this value.
    int64_t rewind_cutoff_ms_ = 0;

    // True between skip_backward_to() and replay_seeked - during this window,
    // all binary frames are dropped because they're stale data from before
    // the backend processed the rewind. The backend pauses its playback loop
    // during rewind, so no NEW-position data arrives until after replay_seeked.
    bool rewind_pending_ = false;

    // ─── Pending request (stored while POST is in-flight) ────────────────
    struct PendingRequest {
        std::vector<std::string> symbols;
        int64_t start_time_ms = 0;
        int64_t end_time_ms = 0;
        float speed = 1.0f;
        int64_t timeframe_seconds = 300;
        bool is_archive = false;
        std::string event_id;
    };
    PendingRequest pending_;

    // ─── Replay range picker (free-window launcher, §8.1-8.2) ─────────────
    bool        launcher_open_        = false;
    std::string launcher_symbol_;                  // symbol the picker will replay
    int64_t     launcher_tf_seconds_  = 300;       // initial replay timeframe
    int         launcher_preset_      = 0;         // 0 Yesterday · 1 Last6h · 2 Last24h · 3 PickDay
    int         launcher_days_ago_    = 2;         // PickDay day offset (Pro): 2..30
    float       launcher_speed_       = 1.0f;      // chosen initial speed
    double      launcher_lock_pulse_  = 0.0;       // ImGui::GetTime() when a locked band/preset was tapped

    // ─── Replay Data Context (DemoNetDriver pattern) ─────────────────────
    // Owns a complete set of managers for replay data isolation.
    // Created on replay_joined, destroyed on stop/error/reset.
    std::unique_ptr<DataContext> replay_ctx_;
    ContextSwapFn on_context_swap_;
    RewindFn on_rewind_;

    // ─── History Buffer (client-side rewind) ─────────────────────────────
    // Stores last 10 minutes of delivered replay messages for instant << rewind.
    // Created on replay start, destroyed on stop/reset.
    // NOT created in pack mode - pack rewinds are local block re-reads (CDN
    // range GETs), so the buffer's RAM cost buys nothing there.
    std::unique_ptr<ReplayHistoryBuffer> history_buffer_;

    // ─── Pack replay (Hot Replay Path B) ─────────────────────────────────
    // When pack_mode_ is set, send_control* route to the engine instead of
    // the WS, the replay DataContext gets no WS handle (historical requests
    // are served from the pack via the StreamManager hook), and no history
    // buffer is created.
    bool pack_mode_ = false;
    std::unique_ptr<PackReplayEngine> pack_engine_;

    // ─── Usage instrumentation state (design §3) ─────────────────────────
    // A "run" spans the first Playing to the terminal state. Accumulators are
    // cumulative for the run; the emitted totals are cumulative (never deltas) so
    // the bridge/server dedupe by replay_id and count each run exactly once.
    bool        usage_run_active_ = false;
    std::string usage_replay_id_;              // uuid-ish per run (dedupe base)
    double      usage_watch_seconds_ = 0.0;    // wall seconds while playing+visible
    int64_t     usage_market_ms_ = 0;          // forward span of interpolated_time_ms()
    int64_t     usage_last_market_clock_ = -1; // -1 = re-baseline (paused/hidden/seek)
    int         usage_scrub_count_ = 0;
    float       usage_max_speed_ = 1.0f;
    int64_t     usage_run_start_market_ts_ = 0;
    int64_t     usage_last_heartbeat_ms_ = 0;  // wall ms of last heartbeat emit

    void        usage_begin_run();
    void        usage_emit_replay_end(bool completed);
    void        usage_emit_heartbeat();
    std::string usage_build_props() const;     // shared cumulative payload
    const char* usage_source() const;          // live|event|demo|lesson
    std::string usage_symbol() const;
    std::string usage_event_ref() const;

    // ─── Skip Debounce (coalesce rapid >> / << into one backend message) ──
    // Only used for CASE 3 (backend skip - no drip-feed active, or target
    // past drip-feed end). During drip-feed, >> is a pure client-side
    // buffer fast-forward and doesn't need debouncing or backend messages.
    // Trailing-edge: every press resets the 200ms timer. One message sent
    // on expiry via flush_pending_skip().
    static constexpr int64_t SKIP_DEBOUNCE_WINDOW_MS = 200;
    int64_t pending_skip_target_ms_ = 0;     // Coalesced target timestamp
    int64_t pending_skip_deadline_ms_ = 0;   // Wall time when window expires
    bool pending_skip_is_forward_ = false;    // Direction of pending skip
    bool pending_skip_needs_flush_ = false;   // True when a debounced press deferred a send
    int64_t skip_origin_ms_ = 0;             // Clock position before first >> in debounce sequence
    bool last_flushed_skip_large_ = false;   // Flushed skip crossed the large threshold (book was cleared) - pack engine mirrors the box's reconnect re-seed

    // ─── Deferred join latch (WS-not-connected-yet at join time) ──────────
    // Set by join_session() when the session is created but the WS isn't up.
    // flush_pending_join() (per-frame) sends the join once is_connected() flips,
    // or transitions to Error if the WS never comes up within the timeout.
    static constexpr int64_t JOIN_WS_WAIT_MAX_MS = 15000;
    bool pending_join_ = false;           // A join is waiting on the WS
    int64_t pending_join_since_ms_ = 0;   // Wall time the join was first deferred

    // ─── Drip-feed state (after rewind, feed buffered data as clock advances)
    bool drip_feed_active_ = false;
    size_t drip_feed_cursor_ = 0;      // Index into history buffer entries
    int64_t drip_feed_end_ms_ = 0;     // Stop drip-feeding at this time

    // ─── Client-local clock (used during drip-feed catch-up after rewind)
    // When the rewind is purely client-side (buffer hit), the client owns
    // the clock. interpolated_time_ms() advances from the rewind target
    // using local wall clock × speed, ignoring replay_status until catch-up
    // completes.
    int64_t local_clock_base_ms_ = 0;      // Rewind target time
    int64_t local_clock_wall_start_ = 0;   // Wall clock at rewind start
    bool    using_local_clock_ = false;     // True during drip-feed catch-up

    // Create/destroy the replay DataContext
    void create_replay_data_context();
    void destroy_replay_data_context();

    // True once the replay context has real data: candle history populated and,
    // when the effective session grant includes orderbook, an OB seed received.
    // Used by tick_buffering_gate to release the clock without waiting for a
    // stream the entitlement deliberately omitted.
    bool context_primed() const;
    // Safety valve: if data never arrives, release after this long in Buffering
    // so a stalled/empty window doesn't hang the spinner forever. Sized to exceed
    // the legitimate worst case - depth/DOM ticks come from TimescaleDB and can
    // take ~10s to retrieve + buffer + send, while candles/heatmap are near-instant.
    int64_t buffering_since_ms_ = 0;
    static constexpr int64_t BUFFERING_MAX_MS = 20000;

    // ─── Internal Methods ────────────────────────────────────────────────
    void send_control(const char* action);
    void send_control_with_value(const char* action, const char* key, double value);
    void send_control_with_int64(const char* action, const char* key, int64_t value);
    void join_session();
    void transition(State new_state);
    void reset();
    int64_t now_ms() const;

    // Control bar sub-renderers
    void render_transport_controls();
    void render_record_button();   // CLIP_FACTORY P1 - ⏺/⏹ share-clip recorder
    void render_speed_control();
    void render_timeline_scrubber();
    void render_time_display();
    void render_buffer_indicator();
    void render_status_badge();

    // Candle boundary helpers
    int64_t snap_to_candle_boundary(int64_t ms) const;
    int64_t next_candle_boundary(int64_t ms) const;
    int64_t prev_candle_boundary(int64_t ms) const;
};
