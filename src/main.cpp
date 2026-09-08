#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_hints.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include <emscripten/websocket.h>
#include <vector>
#include <array>
#include <imgui_impl_sdl3.h>
#include <zstd.h>
#include <nlohmann/json.hpp>

#include "core/message_handler.h"
#include "core/message_context.h"
#include "core/message_parser.h"
#include "core/data_thread.h"
#include "core/websocket.h"
#include "types/frame_profiler.h"
#include "core/orderbook_manager.h"
#include "core/logo_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/url_router.h"
#include "core/education_boot.h"
#include "core/usage_emit.h"
#include "core/entitlements.h"
#include "core/display_time_zone.h"
#include "core/recorder_glue.h"
#include "education/lesson_runtime.h"
#include "education/studio_runtime.h"
#include "education/event_runtime.h"
#include "education/recorder_runtime.h"
#include "core/symbol_metadata.h"
#include "core/debug_manager.h"
#include "core/volume_profile_manager.h"
#include "core/tpo_manager.h"
#include "core/footprint_manager.h"
#include "core/indicator_series.h"
#include "core/analytics_manager.h"
#include "core/paper_trading_manager.h"
#include "core/ticker_manager.h"
#include "core/scanner_manager.h"
#include "core/drawing_manager.h"
#include "ui/drawing/drawing_style_editor.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "rendering/menu.h"
#include "rendering/app_shell.h"
#include "rendering/performance_diagnostics.h"
#include "rendering/shader_heatmap_resources.h"
#include "stream_handler.h"
#include "replayer/replay_manager.h"
#include "core/data_context.h"


#include "ui/debug_widget.h"
#include "ui/chart_widget.h"
#include "ui/upsell_modal.h"
#include "ui/research_moment_panel.h"
#include "ui/dom_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/trades_widget.h"
#include "ui/watchlist_widget.h"
#include "ui/widget.h"

using json = nlohmann::json;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

struct Timeframe {
    const char* label;
    int64_t seconds;
};

struct AppState {
    bool done = false;
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    // Owned managers (RAII via unique_ptr)
    std::unique_ptr<WebSocketClient> ws_client;
    // The replay lane's socket, non-null ONLY when a separate replay origin is
    // configured (see resolve_replay_ws_url). Live and replay share one /ws
    // handler on the box and the lane is chosen after the handshake, so the
    // split has to happen at the socket, not at a path. When it is null the
    // replay lane rides ws_client exactly as it always has.
    std::unique_ptr<WebSocketClient> replay_ws_client;
    std::unique_ptr<StreamManager> stream_mgr;
    std::unique_ptr<HeatmapManager> heatmap_mgr;
    std::unique_ptr<LiquidationHeatmapManager> liq_heatmap_mgr;
    std::unique_ptr<MessageHandler> msg_handler;
    std::unique_ptr<OrderbookManager> ob_mgr;
    std::unique_ptr<CandleManager> candle_mgr;
    std::unique_ptr<ReplayManager> replay_mgr;
    std::unique_ptr<DebugManager> debug_mgr;
    std::unique_ptr<VolumeProfileManager> vpvr_mgr;
    std::unique_ptr<TPOManager> tpo_mgr;
    std::unique_ptr<FootprintManager> footprint_mgr;
    std::unique_ptr<PaperTradingManager> paper_trading_mgr;
    std::unique_ptr<IndicatorSeriesManager> series_mgr;  // Indicators V1 SeriesCache
    std::unique_ptr<AnalyticsManager> analytics_mgr;     // Positioning/Contagion state
    std::unique_ptr<DrawingManager> drawing_mgr;         // Chart drawing tools (annotations)
    std::unique_ptr<DataThread> data_thread;
    // Message context (rebuilt on connect)
    MessageContext msg_ctx{};
    // App context - single dependency injection point for widgets/menus
    AppContext app_ctx{};
    std::vector<std::unique_ptr<Widget>> widgets;
    std::vector<std::string> debug_log;
    void build_message_context() {
        msg_ctx = MessageContext{
            stream_mgr.get(),
            ob_mgr.get(),
            heatmap_mgr.get(),
            liq_heatmap_mgr.get(),
            debug_mgr.get(),
            vpvr_mgr.get(),
            tpo_mgr.get(),
            footprint_mgr.get(),
            paper_trading_mgr.get(),
            &TickerManager::instance(),
            &ScannerManager::instance(),
            data_thread ? &data_thread->dispatch_queue() : nullptr
        };
        msg_ctx.series = series_mgr.get();
        msg_ctx.analytics = analytics_mgr.get();
    }
    void build_app_context() {
        app_ctx = AppContext{
            stream_mgr.get(),
            ob_mgr.get(),
            heatmap_mgr.get(),
            liq_heatmap_mgr.get(),
            candle_mgr.get(),
            replay_mgr.get(),
            debug_mgr.get(),
            vpvr_mgr.get(),
            tpo_mgr.get(),
            footprint_mgr.get(),
            paper_trading_mgr.get(),
            &TickerManager::instance(),
            series_mgr.get(),
            analytics_mgr.get(),
            drawing_mgr.get()
        };
    }
};

AppState g_app;
Route g_initial_route;

void init_main_chart();

// ─── WebSocket message routing ───────────────────────────────────────────────
// Extracted from connect_websocket() for clarity. Handles JSON control messages
// on the main thread, routes binary protobuf to the data thread.

// Which socket a frame arrived on. Only meaningful once the replay lane has its
// own socket; with a single socket everything is Live and the guard below is
// inert.
enum class WsLane : uint8_t { Live, Replay };

static void on_ws_message(const uint8_t* data, size_t len, WsLane lane) {
    if (len == 0) return;

    // ── THE LANE INVARIANT ──────────────────────────────────────────────────
    // A frame that arrived on the LIVE socket must never be written into replay
    // state. While a replay context is active the AppContext managers ARE the
    // replay managers, so a live depth update lands in the replay orderbook and
    // the DOM renders today's book against a 2026-08-19 chart.
    //
    // This regressed on 2026-08-26 when replay moved to its own socket. Before
    // that, join_replay landed on the SAME box session that held the live
    // subscriptions, and the server paused those consumers ("Paused live
    // consumers for replay"). That server-side pause was doing the real work.
    // Splitting the socket split the session, so hub is never told, and the
    // client-side pause_live_subscriptions() alone did not stop it.
    //
    // Guarded on replay_ws_client so it CANNOT fire in the single-socket
    // configuration, where replay frames legitimately arrive on the live lane.
    // Only binary market data is dropped; JSON control frames (errors, auth,
    // status) still need to reach their handlers on both lanes.
    //
    // The `active_socket` test is what keeps ARCHIVE replay working: those
    // sessions ride hub's LIVE socket by design, because only hub has
    // /data/archives. Dropping live-lane frames during one would throw away the
    // replay's own data. Drop only when the replay is demonstrably NOT on this
    // lane.
    if (lane == WsLane::Live && g_app.replay_ws_client && data[0] != '{' &&
        g_app.replay_mgr && g_app.replay_mgr->replay_context() != nullptr &&
        g_app.replay_mgr->active_socket() != g_app.ws_client.get()) {
        static uint64_t dropped = 0;
        if (++dropped % 2000 == 1) {
            SDL_Log("Dropped %llu live-lane frames during replay (lane invariant)",
                    static_cast<unsigned long long>(dropped));
        }
        return;
    }

    // JSON text frames start with '{', binary protobuf never does
    if (data[0] == '{') {
        try {
            auto parsed = nlohmann::json::parse(
                reinterpret_cast<const char*>(data),
                reinterpret_cast<const char*>(data) + len);
            std::string type = parsed.value("type", "");

            if (g_app.replay_mgr && g_app.replay_mgr->handle_ws_message(type, &parsed))
                return;

            if (type == "debug_range_start" && g_app.debug_mgr) {
                g_app.debug_mgr->handle_range_start(
                    parsed.value("start_time", int64_t(0)),
                    parsed.value("end_time",   int64_t(0)));
                return;
            }
            if (type == "debug_range_complete" && g_app.debug_mgr) {
                g_app.debug_mgr->handle_range_complete(
                    parsed.value("start_time",  int64_t(0)),
                    parsed.value("end_time",    int64_t(0)),
                    parsed.value("msg_count",   0),
                    parsed.value("entry_count", 0));
                return;
            }
        } catch (...) {
            // Malformed JSON - fall through to binary path
        }
    }

    // Binary protobuf path - route to replay or live context
    bool replay_active = g_app.replay_mgr && g_app.replay_mgr->is_active();
    bool has_replay_ctx = replay_active && g_app.replay_mgr->replay_context();
    
    static bool logged_replay_routing = false;
    if (replay_active && !logged_replay_routing) {
        logged_replay_routing = true;
    }
    if (!replay_active && logged_replay_routing) {
        logged_replay_routing = false;  // Reset for next session
    }

    if (has_replay_ctx) {
        // ── Drip-feed gate ────────────────────────────────────────────────
        // During a client-only rewind, the drip-feed dispatches buffered data
        // at the correct pace. Incoming frames from the backend (still streaming
        // from the old position) should be captured into the history buffer
        // but NOT routed to managers - that would mix future data with the
        // drip-feed's past data.
        if (g_app.replay_mgr->is_drip_feeding()) {
            // Decompress + parse to get event_ts for buffer capture
            std::string msg_data(reinterpret_cast<const char*>(data), len);
            auto decomp = MessageParser::decompress_zstd(msg_data);
            if (!decomp.success) return;
            auto parsed = MessageParser::parse_payload(decomp.data);
            if (!parsed.success) return;
            int64_t event_ts = parsed.payload.event_time_ms();
            auto* hist = g_app.replay_mgr->history_buffer();
            if (hist) {
                int64_t ts = (event_ts > 0) ? event_ts : MessageHandler::last_timestamp_ms;
                if (ts > 0) hist->push(ts, Terminal::Stream::Unknown, 0, data, len);
            }
            return;  // Don't route - drip-feed handles dispatch
        }

        // ── Backend rewind gate (out-of-buffer rewind via backend) ────────
        // When the rewind was too far back for the buffer, the backend is
        // reconnecting consumers. Drop stale frames until replay_seeked.
        if (g_app.replay_mgr->is_rewind_pending()) {
            return;  // Stale pre-rewind data - drop
        }

        // During active replay, binary frames are replay data from the Replayer actor.
        // Parse the WSPayload ONCE - used for routing and history capture.
        std::string msg_data(reinterpret_cast<const char*>(data), len);
        auto decomp = MessageParser::decompress_zstd(msg_data);
        if (!decomp.success) return;
        auto parsed = MessageParser::parse_payload(decomp.data);
        if (!parsed.success) return;

        int64_t event_ts = parsed.payload.event_time_ms();

        // Route the pre-parsed payload (no double decompress/parse)
        MessageContext replay_ctx = g_app.replay_mgr->replay_message_context();
        static int replay_msg_count = 0;
        if (++replay_msg_count % 100 == 1) {
        }

        // Safety: warn if incoming frame timestamp is far from the client clock.
        // This detects leaks where the backend sends data from the wrong position.
        if (event_ts > 0) {
            int64_t clock = g_app.replay_mgr->interpolated_time_ms();
            int64_t drift_s = (event_ts - clock) / 1000;
            if (drift_s > 30 || drift_s < -30) {
                static int drift_warn_count = 0;
                if (++drift_warn_count <= 20) {
                }
            }
        }

        MessageHandler::route_parsed(parsed.payload, replay_ctx);

        // Capture into history buffer using event_time_ms from the WSPayload.
        auto* hist = g_app.replay_mgr->history_buffer();
        if (hist) {
            int64_t ts = (event_ts > 0) ? event_ts : MessageHandler::last_timestamp_ms;

            if (ts > 0) {
                hist->push(ts, Terminal::Stream::Unknown, 0, data, len);
            }

            // Log first 10 captures + every 500th
            static int capture_count = 0;
            capture_count++;
            if (capture_count <= 10 || capture_count % 500 == 0) {
            }

            // Periodic OB snapshot for instant rewind support
            if (ts > 0) {
                auto* rctx = g_app.replay_mgr->replay_context();
                if (rctx && rctx->orderbooks && !g_app.replay_mgr->info().symbols.empty()) {
                    Terminal::Pair pair{"binancef", g_app.replay_mgr->info().symbols[0]};
                    hist->snapshot_orderbook(ts, *rctx->orderbooks, pair);
                }
            }
        }
    } else if (g_app.data_thread && g_app.data_thread->is_running()) {
        g_app.data_thread->enqueue(data, len);
    } else if (g_app.msg_handler) {
        std::string msg_data(reinterpret_cast<const char*>(data), len);
        MessageHandler::handle_message(msg_data, g_app.msg_ctx);
    }
}

static void on_ws_status(const std::string& status) {
    if (status == "Reconnecting") {
        if (g_app.ob_mgr) g_app.ob_mgr->set_realtime_transport_open(false);
        g_app.stream_mgr->update_websocket_handle(0);
        if (g_app.replay_mgr) g_app.replay_mgr->on_transport_interrupted(g_app.ws_client.get());
    }
    if (status == "Connected") {
        if (g_app.ob_mgr) g_app.ob_mgr->set_realtime_transport_open(true);
        g_app.stream_mgr->update_websocket_handle(g_app.ws_client->get_handle());

        // Embedded lesson/studio mode: the socket is still needed (replay frames
        // ride this same WS via the Replayer actor), but the LIVE auto-subscribes
        // below are pure waste in a lesson - and ticker24h alone is ~150KB/s. Skip
        // them; a replay session is driven separately from EducationBoot's source.
        if (EducationBoot::instance().is_embedded()) {
            return;
        }

        // [2026-04-24] Explicit paper trading subscribe - server no longer auto-subscribes.
        StreamKey paper_key{
            Terminal::Pair{"binancef", "global"},
            Terminal::Stream::PaperTrading,
            0
        };
        g_app.stream_mgr->send_subscribe(paper_key);

        // Subscribe to global 24h ticker - always on, data is ~150KB/sec
        // TODO: make on-demand once confirmed working
        StreamKey ticker24h_key{
            Terminal::Pair{"binancef", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_key);

        // HL 24h ticker - the venue-parameterized global feed (ticker24h.hl.global,
        // ~0.7KB/sec). Always-on so the statsbar + picker LAST/24H%/VOLUME stay live
        // whichever venue is active. Keyed by exchange in TickerManager.
        StreamKey ticker24h_hl_key{
            Terminal::Pair{"hl", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_hl_key);
    }
}

// Backend endpoint resolution, first match wins:
//   1. ?ws=<url> query param        (dev / self-hosters pointing at a local gateway)
//   2. window.__EDGEDEPTH_WS_URL__  (host page, set before the glue loads)
//   3. wss://api.edgedepth.com/ws   (production default)
// Only ws:// and wss:// schemes are accepted; anything else falls through.
static std::string resolve_ws_url() {
    static const char* kDefaultWsUrl = "wss://api.edgedepth.com/ws";
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var url = new URLSearchParams(window.location.search).get('ws') ||
                      window.__EDGEDEPTH_WS_URL__ || "";
            url = String(url);
            if (url.indexOf('ws://') !== 0 && url.indexOf('wss://') !== 0) return 0;
            var len = lengthBytesUTF8(url);
            var buf = _malloc(len + 1);
            stringToUTF8(url, buf, len + 1);
            return buf;
        } catch (e) {
            return 0;
        }
    }));
    if (raw) {
        std::string url(raw);
        free(raw);
        return url;
    }
#endif
    return kDefaultWsUrl;
}

void connect_websocket() {
    if (g_app.ws_client) {
        g_app.ws_client->disconnect();
        g_app.ws_client.reset();
    }
    g_app.ws_client = std::make_unique<WebSocketClient>();
    g_app.ws_client->set_message_callback(
        [](const uint8_t* d, size_t n) { on_ws_message(d, n, WsLane::Live); });
    g_app.ws_client->set_status_callback(on_ws_status);

    if (!g_app.ws_client->connect(resolve_ws_url())) {
    }
}

// Replay-lane endpoint resolution, first match wins:
//   1. ?replay_ws=<url> query param   (dev / pointing at a local replay-server)
//   2. window.__EDGEDEPTH_REPLAY_WS_URL__  (host page, before the glue loads)
//   3. "" = no separate replay origin, so the replay lane rides the live socket
//
// Returning "" by default is the whole safety property: shipping this build
// changes nothing until the host page sets the variable, exactly like
// RESEARCH_STORE_ENGINE_URL did for the research offload.
//
// WHY A SECOND SOCKET AT ALL. The box mounts ONE /ws handler and the client
// picks its lane after the handshake, so no path-based proxy rule can separate
// live from replay. Splitting them means a second hostname in front of a second
// process, which means a second socket here.
static std::string resolve_replay_ws_url() {
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var url = new URLSearchParams(window.location.search).get('replay_ws') ||
                      window.__EDGEDEPTH_REPLAY_WS_URL__ || "";
            url = String(url);
            if (url.indexOf('ws://') !== 0 && url.indexOf('wss://') !== 0) return 0;
            var len = lengthBytesUTF8(url);
            var buf = _malloc(len + 1);
            stringToUTF8(url, buf, len + 1);
            return buf;
        } catch (e) {
            return 0;
        }
    }));
    if (raw) {
        std::string url(raw);
        free(raw);
        return url;
    }
#endif
    return std::string();
}

// The socket for NATS/cold-parquet replay: the dedicated one when it exists,
// otherwise the live one. Every such call site goes through this so there is
// exactly one place that knows the lane split exists.
WebSocketClient* replay_lane_socket() {
    return g_app.replay_ws_client ? g_app.replay_ws_client.get() : g_app.ws_client.get();
}

// The socket for ARCHIVE replay of curated events, which is always hub. Those
// sessions stream the absolute market_events.archive_path under /data/archives,
// a 232 GB tree that exists ONLY on hub; server 2 has no such directory. The
// session would create there and then fail opening metadata.json.
WebSocketClient* archive_lane_socket() {
    return g_app.ws_client.get();
}

// Status callback for the REPLAY socket only. Deliberately does almost nothing:
// it must not touch stream_mgr's websocket handle and must not fire the live
// auto-subscribes, or a replay connection would start paying for ticker24h and
// paper-trading traffic on a socket that serves neither.
static void on_replay_ws_status(const std::string& status) {
    if (status == "Reconnecting" && g_app.replay_mgr)
        g_app.replay_mgr->on_transport_interrupted(g_app.replay_ws_client.get());
    if (status == "Connected") {
        SDL_Log("Replay lane socket connected");
    }
}

// Opens the replay lane's own socket, but ONLY when a separate origin is
// configured. With no origin set this is a no-op and nothing about the app
// changes, so the split is inert until the host page opts in.
void connect_replay_websocket() {
    const std::string replay_url = resolve_replay_ws_url();
    if (replay_url.empty() || replay_url == resolve_ws_url()) {
        // Same endpoint as live: opening a second socket to it would double
        // every user's connection count for no benefit.
        g_app.replay_ws_client.reset();
        return;
    }

    if (g_app.replay_ws_client) {
        g_app.replay_ws_client->disconnect();
        g_app.replay_ws_client.reset();
    }
    g_app.replay_ws_client = std::make_unique<WebSocketClient>();
    // Same handler as live, but TAGGED, so the lane invariant at the top of
    // on_ws_message can tell replay frames from live ones. The routing inside
    // is content-based and shared; only the source needs distinguishing.
    g_app.replay_ws_client->set_message_callback(
        [](const uint8_t* d, size_t n) { on_ws_message(d, n, WsLane::Replay); });
    g_app.replay_ws_client->set_status_callback(on_replay_ws_status);

    // Connected eagerly rather than on first replay request. The join path
    // already has a pending_join_ latch for "socket not up yet", but the
    // replay DataContext grabs get_handle() during context creation, and a
    // lazily-opened socket can still be mid-handshake at that moment.
    if (!g_app.replay_ws_client->connect(replay_url)) {
    }
}

void render_debug() {
    ImGui::Begin("WebSocket Debug");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Presentation: %.1f FPS (%.3f ms interval, smoothed)",
                io.Framerate,
                io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    ImGui::Separator();
    ImGui::Text("Display: %.0f x %.0f (Scale: %.2fx%.2f)",
                io.DisplaySize.x, io.DisplaySize.y,
                io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);

#ifdef __EMSCRIPTEN__
    double dpr = EM_ASM_DOUBLE({
        return window.devicePixelRatio || 1.0;
    });
    ImGui::Text("Device Pixel Ratio: %.2f", dpr);
    int window_w = EM_ASM_INT({ return window.innerWidth; });
    int window_h = EM_ASM_INT({ return window.innerHeight; });
    ImGui::Text("Window Size: %d x %d", window_w, window_h);
    int canvas_w, canvas_h;
    emscripten_get_canvas_element_size("#canvas", &canvas_w, &canvas_h);
    ImGui::Text("Canvas Resolution: %d x %d", canvas_w, canvas_h);
    double zoom = EM_ASM_DOUBLE({
        return (window.outerWidth / window.innerWidth) * 100;
    });
    ImGui::Text("Browser Zoom: %.0f%%", zoom);
#endif

    ImGui::Separator();
    if (ImGui::Button("Reconnect WebSocket")) {
        connect_websocket();
        // connect_websocket() destroys and recreates g_app.ws_client, so any
        // replay lane riding the live socket is now holding a freed pointer.
        if (g_app.replay_mgr) {
            g_app.replay_mgr->set_lanes(replay_lane_socket(), archive_lane_socket());
        }
    }
    ImGui::Separator();
    ImGui::Text("WebSocket: %s",
                g_app.ws_client && g_app.ws_client->is_connected() ? "Connected" : "Disconnected");
    ImGui::End();
}

// File-scope so maybe_start_lesson_replay() (and other helpers) can observe when
// the terminal has finished its first-time initialization. check_initialization
// sets this once its widget/chart setup completes.
static bool g_init_complete = false;

void check_initialization() {
    static bool initialization_complete = false;
    if (initialization_complete) return;

    static bool chart_initialized = false;
    static bool widgets_created = false;
    static double metadata_wait_start_s = 0.0;

    const Terminal::Pair pair{g_initial_route.exchange, g_initial_route.symbol};

    // Pack mode boots with NO WebSocket (the .edpack + metadata fetch are the
    // only network I/O - the box stays out of the per-viewer loop), so init
    // proceeds on the pack path without a socket.
    const bool comms_ready =
        (g_app.ws_client && g_app.ws_client->is_connected()) ||
        EducationBoot::instance().is_pack();
    if (!chart_initialized && comms_ready) {
        // Wait for metadata, but only to avoid a visible placeholder: widgets
        // built without it rebind through Widget::refresh_instrument() when the
        // registry lands, so timing out is no longer destructive.
        //
        // The budget is WALL CLOCK, not frames. The old "120 frames ≈ 2 seconds
        // at 60fps" was 0.66s on a 180fps client, and in pack mode the counter
        // starts at frame one (comms_ready is immediate with no socket) while
        // the 500 KB metadata fetch races the pack's own range requests: the
        // /demo terminal lost that race consistently and drew every price at
        // placeholder precision for the whole session.
        const double now_s = ImGui::GetTime();
        if (metadata_wait_start_s == 0.0) metadata_wait_start_s = now_s;
        bool metadata_ready = SymbolRegistry::instance().is_loaded();
        if (!metadata_ready && (now_s - metadata_wait_start_s) < 3.0) {
            return;  // Try again next frame
        }
        // 0 = unknown. No invented tick anywhere: ChartWidget draws on
        // provisional precision and rebinds, the DOM says so and waits.
        double tick_size = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);
        // In embedded lesson mode the replay is the ONLY data source - skip the
        // live initial candle load (otherwise the chart floods with current
        // market data before the replay swaps in). The replay session populates
        // candles for the lesson window via the normal pipeline.
        if (!EducationBoot::instance().is_embedded() &&
            !EducationBoot::instance().is_pack()) {
            g_app.candle_mgr->initial_load();
        }
        g_app.widgets.push_back(std::make_unique<ChartWidget>(
            pair,
            g_app.app_ctx,
            tick_size
        ));
        // The native shell (topbar + stats strip) is suppressed in embedded
        // lesson mode (React owns it), so skip its init too - it subscribes the
        // global ticker24h feed, which would be a live leak in a replay lesson.
        // Pack mode also skips it: no WS means the strip would render empty.
        if (!EducationBoot::instance().is_embedded() &&
            !EducationBoot::instance().is_pack()) {
            AppShell::init(g_app.app_ctx, pair);
        }
        chart_initialized = true;
        return;
    }
    if (chart_initialized && !widgets_created && LayoutManager::is_initialized) {
        static int frames_waited = 0;
        if (frames_waited++ > 3) {
            auto fmt = SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol);
            double dom_tick = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);

            if (EducationBoot::instance().is_embedded() ||
                EducationBoot::instance().is_pack()) {
                // Lesson layout: the chart (already created above) carries the
                // heatmap; the right side is the Order Flow read (DOM ladder +
                // tape). NO Watchlist, NO separate Depth/Orderbook widget - a
                // lesson is a focused single-symbol study, not a scanning desk.
                // Pack (/demo) mode uses the same focused set: the watchlist is
                // a live-data widget and there is no WS in a pack session.
                g_app.widgets.push_back(std::make_unique<DOMWidget>(
                    pair, g_app.app_ctx, dom_tick, 20
                ));
                g_app.widgets.push_back(std::make_unique<TradesWidget>(
                    pair, g_app.app_ctx, fmt
                ));
            } else {
                // Phase E: Depth (OrderbookWidget) is no longer docked by default -
                // the right column is the Order Flow read (DOM ladder + tape). Depth
                // stays available on demand via the +Widget menu.
                g_app.widgets.push_back(std::make_unique<DOMWidget>(
                    pair, g_app.app_ctx, dom_tick, 20
                ));
                g_app.widgets.push_back(std::make_unique<TradesWidget>(
                    pair, g_app.app_ctx, fmt
                ));
                g_app.widgets.push_back(std::make_unique<WatchlistWidget>(
                    pair, g_app.app_ctx
                ));
            }
            widgets_created = true;
            initialization_complete = true;
            g_init_complete = true;
        }
    }
}

// ─── Instrument metadata rebind ──────────────────────────────────────────────
// Tick size and price precision are not available at a fixed point in the boot:
// the metadata fetch is ~500 KB over the network, a /demo pack carries its own
// tick in a header that arrives later still, and widgets are built in between.
// Every widget that caches either one re-reads it here when the registry moves.
// Driven from the frame loop rather than from the fetch callback on purpose:
// the callback re-enters wasm from a browser event and would rebuild widget
// state mid-frame, between an ImGui Begin and its End. Same safe-point rule as
// the +widget drain and the live-flow rebuild above.
void resolve_instrument_metadata_rebind() {
    static uint32_t seen_epoch = 0;
    const uint32_t epoch = SymbolRegistry::instance().epoch();
    if (epoch == seen_epoch) return;
    seen_epoch = epoch;
    for (auto& w : g_app.widgets) {
        if (w) w->refresh_instrument();
    }
}

// ─── Live order-flow widgets torn down by a replay ───────────────────────────
// Entering a replay DESTROYS the live DOM and tape. They are not hidden: the
// swap sets is_open = false and update_and_render_widgets ends every frame with
// erase_if(!is_open), so the object is gone. The exit side used to "restore"
// them by setting is_open = true on widgets that no longer existed, which is
// why leaving a replay left the chart and watchlist sitting over empty space.
//
// Rebuilding is the only thing that can work, and it cannot happen inside the
// context-swap callback: constructing a widget subscribes through a stream
// manager and pushes into g_app.widgets, and stop() is reachable from ImGui
// item handlers. It is drained at the same safe point as the +widget queue.
namespace {
struct LiveFlowRebuild {
    bool armed = false;
    bool want_dom = false;
    bool want_trades = false;
    bool want_chart = false;   // set when a replay of another symbol replaced it
    Terminal::Pair pair{};
};
LiveFlowRebuild g_live_flow_rebuild;
}  // namespace

void resolve_live_flow_widget_rebuild() {
    if (!g_live_flow_rebuild.armed) return;
    // Wait for the sweep to actually retire the replay-owned pair. Building now
    // would put two widgets on one ImGui docking identity, and that does not
    // stack, it corrupts the frame (see the BeginCount guard in
    // ChartWidget::render).
    const bool replay_widgets_gone = std::none_of(
        g_app.widgets.begin(), g_app.widgets.end(),
        [](const auto& w) { return w && w->is_replay_widget; });
    if (!replay_widgets_gone) return;

    g_live_flow_rebuild.armed = false;
    const Terminal::Pair pair = g_live_flow_rebuild.pair;
    if (pair.symbol.empty()) return;

    const double dom_tick = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);
    auto fmt = SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol);

    // A replay of a different symbol replaced the chart, so the dock tree in
    // place is keyed on the REPLAY pair's titles. Re-key it for the live pair
    // before the windows are submitted, or they come up floating for the same
    // reason the library launch used to float its DOM. render_dockspace picks
    // the queued pair up on the very next call, which is the line after this.
    if (g_live_flow_rebuild.want_chart) {
        LayoutManager::reset_layout_for(pair.exchange, pair.symbol);
        g_app.widgets.push_back(
            std::make_unique<ChartWidget>(pair, g_app.app_ctx, dom_tick));
        g_live_flow_rebuild.want_chart = false;
    }

    // Same order and constructor arguments as the boot layout, so the rebuilt
    // pair inherits the dock nodes the originals held (ImGui keys them by the
    // window title, which is derived from the pair).
    if (g_live_flow_rebuild.want_dom) {
        g_app.widgets.push_back(
            std::make_unique<DOMWidget>(pair, g_app.app_ctx, dom_tick, 20));
    }
    if (g_live_flow_rebuild.want_trades) {
        g_app.widgets.push_back(
            std::make_unique<TradesWidget>(pair, g_app.app_ctx, fmt));
    }
    g_live_flow_rebuild.want_dom = false;
    g_live_flow_rebuild.want_trades = false;
}

// Embedded lesson mode: start the replay for the lesson's window exactly once,
// as soon as BOTH the terminal is initialized AND the lesson doc has parsed.
// These race - the lesson arrives via async XHR that can land before or after
// check_initialization - so this is a standalone per-frame latch rather than
// inlined in check_initialization (where it would miss if the doc came later).
void pause_live_for_historical_replay() {
    if (g_app.stream_mgr) {
        g_app.stream_mgr->pause_live_subscriptions();
    }
    if (g_app.debug_mgr) {
        g_app.debug_mgr->unsubscribe();
    }
}

void maybe_start_lesson_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    // Lesson-only: is_embedded() now also covers studio + event (which start their
    // replays via their own paths), so gate on the lesson mode specifically.
    if (!EducationBoot::instance().is_lesson()) return;
    if (!edu::LessonRuntime::instance().loaded()) return;
    if (!g_app.replay_mgr) return;

    const auto& src = edu::LessonRuntime::instance().lesson().source;
    if (src.symbol.empty() || src.startMs <= 0 || src.endMs <= src.startMs) return;

    std::string sym = src.symbol;
    std::transform(sym.begin(), sym.end(), sym.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Historical pages fail closed. Stop live delivery before the session POST,
    // including the Creating and Joining states where no replay context exists.
    pause_live_for_historical_replay();
    g_app.replay_mgr->request_replay({sym}, src.startMs, src.endMs, 1.0f);
    started = true;
}

// Event (archive) replay: start the archived market-event replay exactly once,
// as soon as the terminal is initialized. Mirrors maybe_start_lesson_replay but
// uses the archive session path (request_archive_replay → POST
// /replay/session/archive → join). The backend resolves event_id → archive_path
// and streams the event window. Transport is owned by the React EventReplayBar
// (event mode is now is_embedded(), so the native control bar is suppressed) -
// EventRuntime mirrors this session's state to it. seekToStart is implicit: the
// archive plays from the start of the event window.
void maybe_start_event_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    if (!EducationBoot::instance().is_event()) return;
    if (!g_app.replay_mgr) return;

    const std::string& eid = EducationBoot::instance().event_id();
    if (eid.empty()) return;

    std::string sym = EducationBoot::instance().event_symbol();
    std::transform(sym.begin(), sym.end(), sym.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    pause_live_for_historical_replay();
    g_app.replay_mgr->request_archive_replay(eid, sym, 1.0f);
    started = true;
}

// Pack (CDN .edpack) replay: start exactly once, as soon as the terminal is
// initialized. Mirrors maybe_start_event_replay but there is no session POST
// and no WebSocket - ReplayManager::request_pack_replay boots the
// PackReplayEngine, which fetches the pack header and drives the same replay
// state machine (Hot Replay Path B, slice 2).
void maybe_start_pack_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    if (!EducationBoot::instance().is_pack()) return;
    if (!g_app.replay_mgr) return;

    const std::string& url = EducationBoot::instance().pack_url();
    if (url.empty()) return;

    g_app.replay_mgr->request_pack_replay(url, EducationBoot::instance().pack_symbol(), 1.0f);
    started = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Usage: session lifecycle (design §3). session_start once at boot; a ~30s
// session_heartbeat the bridge holds as the pending session_end (materialized on
// tab-close). Replay watch-seconds are ReplayManager's job (tick_usage); session
// tracks time-in-terminal. Symbol switches are full navigations (fresh mount), so
// a session ~= one page mount here.
// ═══════════════════════════════════════════════════════════════════════════════
namespace {
std::string g_usage_session_id;
double      g_usage_session_start_wall = 0.0;   // ImGui::GetTime() at session_start
double      g_usage_session_active_s = 0.0;     // wall seconds while visible
int64_t     g_usage_session_last_hb_ms = 0;
bool        g_usage_session_started = false;

int64_t usage_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* usage_chrome() {
    switch (EducationBoot::instance().mode()) {
        case EducationBoot::Mode::Lesson: return "lesson";
        case EducationBoot::Mode::Studio: return "studio";
        case EducationBoot::Mode::Event:  return "event";
        case EducationBoot::Mode::Pack:   return "demo";
        default:                          return "terminal";
    }
}

std::string usage_session_symbol() {
    const auto& edu = EducationBoot::instance();
    if (edu.is_event() && !edu.event_symbol().empty())  return edu.event_symbol();
    if (edu.is_pack()  && !edu.pack_symbol().empty())   return edu.pack_symbol();
    if (edu.is_studio() && !edu.studio_symbol().empty()) return edu.studio_symbol();
    if (!g_initial_route.symbol.empty()) return g_initial_route.symbol;
    return "";
}
}  // namespace

void maybe_emit_session_start() {
    if (g_usage_session_started) return;
    if (!g_init_complete) return;
    g_usage_session_started = true;

    g_usage_session_id = std::string(usage_chrome()) + "-" + std::to_string(usage_now_ms());
    g_usage_session_start_wall = ImGui::GetTime();
    g_usage_session_active_s = 0.0;
    g_usage_session_last_hb_ms = usage_now_ms();

    const std::string sym = usage_session_symbol();
    std::string plan = usage::read_window_string("__EDGEDEPTH_TIER__");
    if (plan.empty()) plan = "free";
    const std::string ref = usage::read_referrer();

    json detail = {
        {"event", "session_start"},
        {"mode", usage_chrome()},
        {"symbol", sym},
        {"dedupeKey", g_usage_session_id + ":start"},
        {"props", {
            {"session_run_id", g_usage_session_id},
            {"chrome", usage_chrome()},
            {"symbol", sym},
            {"plan", plan},
            {"referrer", ref},
        }},
    };
    usage::dispatch_detail(detail.dump());
}

void tick_session_usage(double dt_seconds, bool document_visible) {
    if (!g_usage_session_started) return;
    if (document_visible) {
        double d = dt_seconds;
        if (d < 0.0) d = 0.0;
        if (d > 0.25) d = 0.25;
        g_usage_session_active_s += d;
    }
    const int64_t noww = usage_now_ms();
    if (noww - g_usage_session_last_hb_ms < 30000) return;
    g_usage_session_last_hb_ms = noww;

    const double duration_s = ImGui::GetTime() - g_usage_session_start_wall;
    json detail = {
        {"event", "session_heartbeat"},
        {"heartbeatFor", "session_end"},
        {"mode", usage_chrome()},
        {"symbol", usage_session_symbol()},
        {"umami", false},
        {"dedupeKey", g_usage_session_id + ":end"},
        {"props", {
            {"session_run_id", g_usage_session_id},
            {"chrome", usage_chrome()},
            {"symbol", usage_session_symbol()},
            {"duration_s", static_cast<int64_t>(duration_s + 0.5)},
            {"active_s", static_cast<int64_t>(g_usage_session_active_s + 0.5)},
        }},
    };
    usage::dispatch_detail(detail.dump());
}

// STANDALONE pack start-at (?packt=<epoch-ms>): the embedded chrome applies
// __EDGEDEPTH_PACK__.seekToMs via EventRuntime's one-shot deep-link seek, but
// EventRuntime never runs standalone. Mirror the exact same contract here:
// ONE deliberate ReplayManager::seek once the session is live and primed
// (Playing or Paused - joined AND past the buffering gate) so the chart's
// re-request cascade and the seeded book behave identically to a user seek -
// then the latch closes.
void maybe_apply_pack_start_at() {
    static bool done = false;
    if (done) return;
    const auto& boot = EducationBoot::instance();
    if (!boot.is_pack() || boot.is_embedded()) { done = true; return; }
    const int64_t want = boot.pack_seek_to_ms();
    if (want <= 0) { done = true; return; }
    if (!g_app.replay_mgr) return;
    ReplayManager& rm = *g_app.replay_mgr;
    // Playing/Paused ONLY (same fix as EventRuntime's deep-link latch): the old
    // gate admitted Creating/Joining (is_active() counts them; is_loading() is
    // Buffering-only), firing the seek before the engine had a session to serve.
    if (rm.state() != ReplayManager::State::Playing &&
        rm.state() != ReplayManager::State::Paused) {
        return;  // not primed yet - retry next frame
    }
    const int64_t start = rm.info().start_time_ms;
    const int64_t end   = rm.info().end_time_ms;
    if (end <= start) return;
    rm.seek(std::clamp<int64_t>(want, start, end), /*deliberate=*/true);
    done = true;
}

// ── Lesson loading gate ──────────────────────────────────────────────────────
// The actual clock-hold now lives in ReplayManager::tick_buffering_gate() so it
// applies to EVERY replay (lessons AND the live terminal's "replay from here") -
// no replay starts ticking against empty buffers. Here we only mirror that into
// g_lesson_loading so the lesson canvas spinner + the React 'loading' emit track
// it. (Trades aren't a separate manager; candles+OB is the readiness bar - see
// ReplayManager::context_primed.)
bool g_lesson_loading = false;

void tick_lesson_loading_gate() {
    // Cover the entire asynchronous boot, plus post-seek buffering, so an event
    // or lesson never exposes the underlying live canvas while replay is pending.
    g_lesson_loading = false;
    if (!EducationBoot::instance().is_embedded() || !g_app.replay_mgr) return;
    const ReplayManager::State state = g_app.replay_mgr->state();
    const auto& boot = EducationBoot::instance();
    g_lesson_loading =
        ((boot.is_event() || boot.is_lesson()) && state == ReplayManager::State::Idle) ||
        state == ReplayManager::State::Creating ||
        state == ReplayManager::State::Joining ||
        state == ReplayManager::State::Buffering ||
        state == ReplayManager::State::Seeking;
}

// Canvas loading overlay - masks the empty-chart window while the lesson holds
// the clock. Drawn on the foreground draw list so it sits above all widgets.
// A later React chrome can show its own polished overlay (driven by the emitted
// 'loading' flag); this is the zero-gap, no-dependency canvas fallback.
void render_lesson_loading_overlay() {
    if (!g_lesson_loading) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList(vp);
    const ImVec2 c = ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                            vp->Pos.y + vp->Size.y * 0.5f);

    // Full-viewport scrim so the half-built chart doesn't flash through.
    fg->AddRectFilled(vp->Pos,
                      ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      IM_COL32(10, 14, 18, 235));

    // Spinning arc.
    const float t = static_cast<float>(ImGui::GetTime());
    const float r = 22.0f;
    const float a0 = t * 3.2f;
    const float a1 = a0 + 4.2f;  // ~240° sweep
    const ImU32 col = IM_COL32(34, 197, 219, 255);  // cyan brand
    fg->PathClear();
    fg->PathArcTo(c, r, a0, a1, 48);
    fg->PathStroke(col, 0, 3.0f);
    // Faint full ring behind it.
    fg->AddCircle(c, r, IM_COL32(34, 197, 219, 40), 48, 3.0f);

    const char* msg = "Loading replay. Priming order book and candles...";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    fg->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + 16.0f),
                IM_COL32(180, 195, 210, 255), msg);
}

// Historical event and lesson pages never fall back to live data. This modal
// remains until navigation or reload and captures canvas input while visible.
void render_historical_replay_error_overlay() {
    const auto& boot = EducationBoot::instance();
    if ((!boot.is_event() && !boot.is_lesson()) || !g_app.replay_mgr) return;
    if (g_app.replay_mgr->state() != ReplayManager::State::Error) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 14, 18, 250));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 24.0f));
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##historical_replay_error", nullptr, flags)) {
        const char* title = "Historical replay unavailable";
        const char* detail = "Live market data is not shown on this page. Reload to retry or use Back to leave.";
        const ImVec2 title_size = ImGui::CalcTextSize(title);
        const ImVec2 detail_size = ImGui::CalcTextSize(detail);
        const float center_x = vp->Size.x * 0.5f;
        const float center_y = vp->Size.y * 0.5f;
        ImGui::SetCursorPos(ImVec2(center_x - title_size.x * 0.5f, center_y - 24.0f));
        ImGui::TextColored(ImVec4(0.95f, 0.76f, 0.32f, 1.0f), "%s", title);
        ImGui::SetCursorPos(ImVec2(center_x - detail_size.x * 0.5f, center_y + 12.0f));
        ImGui::TextColored(ImVec4(0.71f, 0.78f, 0.84f, 1.0f), "%s", detail);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void update_and_render_widgets() {
    // Drain queued dispatches from data thread → fire widget callbacks on main
    // thread. TIME-BUDGETED: ingest storms (replay catch-up, reconnect bursts)
    // used to execute unbounded in one frame - the classic 90-FPS dip. The
    // remainder stays queued in order and resumes next frame.
    static constexpr double DISPATCH_BUDGET_MS = 3.0;
    if (g_app.data_thread && g_app.data_thread->is_running()) {
        g_profiler.begin("Dispatch");
        const size_t n = g_app.data_thread->drain_dispatches(*g_app.stream_mgr,
                                                             DISPATCH_BUDGET_MS);
        g_profiler.end("Dispatch");
        g_profiler.add_count("Dispatch", static_cast<long>(n));
        if (const size_t backlog = g_app.data_thread->carry_backlog(); backlog > 0) {
            g_profiler.add_count("DispBacklog", static_cast<long>(backlog));
        }
    }
    // Swap the ACTIVE orderbook manager's buffers (write→read).
    // During replay, app_ctx.orderbooks points to the replay OB manager.
    // During live, it points to the live OB manager. Either way, this
    // swaps the one the widgets are actually reading from.
    if (g_app.app_ctx.orderbooks) {
        g_profiler.begin("OBSwap");
        g_app.app_ctx.orderbooks->swap_buffers();
        g_profiler.end("OBSwap");
    }
    g_profiler.begin("WidgetsUpd");
    for (const auto& widget: g_app.widgets) {
        widget->update();
    }
    g_profiler.end("WidgetsUpd");
    // REC focus layout: don't SUBMIT the watchlist while a focused clip records -
    // its dock node collapses and the chart takes the width; on stop the window
    // re-docks into its remembered node. (Render-skip only: the widget object,
    // subscriptions and is_open are untouched - is_open=false would ERASE it.)
    const bool rec_focus_widgets = ClipRecorder::focus_active() ||
                                   edu::RecorderRuntime::instance().active();
    // Price transforms are only final after ImPlot renders. Charts go first,
    // then matching DOMs consume that same frame, regardless of widget order.
    for (int pass = 0; pass < 3; ++pass) for (const auto& widget: g_app.widgets) {
        const int widget_pass = widget->type() == WidgetType::Chart ? 0 :
            widget->type() == WidgetType::DOM ? 1 : 2;
        if (widget_pass != pass) continue;
        if (widget->type() == WidgetType::DOM) {
            auto* dom = static_cast<DOMWidget*>(widget.get());
            dom->link_realtime(nullptr);
            for (const auto& candidate : g_app.widgets) {
                if (!candidate->is_open || candidate->type() != WidgetType::Chart) continue;
                const auto* chart = static_cast<const ChartWidget*>(candidate.get());
                if (chart->rt_mode() && chart->pair().exchange == dom->pair().exchange &&
                    chart->pair().symbol == dom->pair().symbol) {
                    dom->link_realtime(&chart->realtime_dom_frame());
                    break;
                }
            }
        }
        if (widget->type() == WidgetType::Trades) {
            const auto* tape = static_cast<const TradesWidget*>(widget.get());
            bool covered = false;
            for (const auto& candidate : g_app.widgets) {
                if (!candidate->is_open || candidate->type() != WidgetType::DOM) continue;
                const auto* dom = static_cast<const DOMWidget*>(candidate.get());
                if (!dom->links_realtime() || dom->pair().exchange != tape->pair().exchange ||
                    dom->pair().symbol != tape->pair().symbol) continue;
                if (LayoutManager::vertical_siblings(*dom, *tape)) covered = true;
            }
            // Like clip-focus layout, skip submission only. Subscriptions and
            // remembered docking survive; independent mode restores the tape.
            if (covered) continue;
        }
        if (rec_focus_widgets && widget->type() == WidgetType::Watchlist) continue;
        // Per-widget render timing - labels by widget type so the once/sec
        // console profile shows exactly where frame time goes.
        const char* sec = "Widget";
        switch (widget->type()) {
            case WidgetType::Chart:        sec = "Chart";     break;
            case WidgetType::DOM:          sec = "DOM";       break;
            case WidgetType::Orderbook:    sec = "Orderbook"; break;
            case WidgetType::Trades:       sec = "Trades";    break;
            case WidgetType::Watchlist:    sec = "Watchlist"; break;
            case WidgetType::Stats:        sec = "Stats";     break;
            case WidgetType::PaperTrading: sec = "Paper";     break;
            case WidgetType::ReplayLibrary: sec = "ReplayLib"; break;
            default: break;
        }
        g_profiler.begin(sec);
        widget->render();
        g_profiler.end(sec);
    }
    std::erase_if(g_app.widgets,
                  [](const auto& w) { return !w->is_open; });
}


// (render_replay_overlay removed - the amber full-viewport border + top-left
//  REPLAY badge were redundant with the restyled control bar + topbar indicator.)

// ── Embedded-mode canvas sizing ───────────────────────────────────────────────
// In the Next host (/studio, /lesson) the canvas is NOT the full window - it lives
// in a flex box (symbol tree + inspector around it). Polling window.innerWidth
// there is wrong on two counts: (1) ImGui lays the dock out over a virtual
// full-window space, so the right column (DOM/Depth) lands off-screen right; and
// (2) the client forces canvas.width/height to full-window, fighting
// TerminalEmbed's ResizeObserver (which sizes the backing buffer to the CSS box ×
// dpr) → the squished render. The React side pushes the real CSS box here via
// _edu_set_viewport (instant); main_loop also polls the canvas box as a fallback
// seed (the RO can fire before the wasm exports are callable).
static int    g_embed_view_w     = 0;
static int    g_embed_view_h     = 0;
static double g_embed_view_dpr   = 1.0;
static bool   g_embed_view_dirty = false;
// Deferred dock reflow: during a live resize (devtools drag, host panel drag)
// the RO pushes a new size every few ms. Rebuilding the dock per step cleared
// the canvas + burned a full DockBuilder pass per frame; instead the size is
// applied instantly (ImGui rescales the dock tree proportionally) and ONE
// reflow snaps the 248/flex/300 design widths after the size settles.
static bool   g_embed_reflow_pending = false;
static double g_embed_last_resize_t  = 0.0;

#ifdef __EMSCRIPTEN__
// Apply a corrected embedded canvas size to ImGui + SDL and re-flow the dock.
// Dedupes on (w,h,dpr), so calling it every frame (push) or at 10Hz (poll) is cheap.
static void apply_embedded_viewport(int css_w, int css_h, double dpr) {
    if (css_w <= 0 || css_h <= 0) return;
    if (dpr <= 0.0) dpr = 1.0;
    // Floor the applied size: a devtools pane / host flexbox can squeeze the CSS
    // box to near-zero mid-drag. Layout + widgets never see a degenerate
    // viewport; the canvas may letterbox for a moment, which beats a dead frame.
    css_w = std::max(css_w, 320);
    css_h = std::max(css_h, 240);
    static int    last_w   = -1;
    static int    last_h   = -1;
    static double last_dpr = -1.0;
    if (css_w == last_w && css_h == last_h && std::abs(dpr - last_dpr) <= 0.01) return;
    last_w = css_w; last_h = css_h; last_dpr = dpr;

    // Logical CSS-px window size drives ImGui layout. SDL3's NewFrame recomputes
    // io.DisplaySize from SDL_GetWindowSize and the framebuffer scale from
    // GetWindowSizeInPixels/GetWindowSize, so the SDL window MUST track the CSS box
    // or NewFrame clobbers io.DisplaySize back to the stale boot size.
    SDL_SetWindowSize(g_app.window, css_w, css_h);

    // SDL_SetWindowSize reset the canvas backing buffer to the logical size (no
    // dpr), so re-apply the dpr scale for a crisp hi-dpi render - exactly what the
    // standalone path does. This is the SAME value TerminalEmbed's ResizeObserver
    // sets (CSS box × dpr), so the two don't fight: writing the width/height attrs
    // can't re-trigger the RO (it observes the CSS box, not the attrs). The old bug
    // was writing innerWidth × dpr (full window) here vs the RO's CSS box → squish.
    EM_ASM({
        var canvas = Module.canvas;
        canvas.width  = $0;
        canvas.height = $1;
    }, (int)(css_w * dpr + 0.5), (int)(css_h * dpr + 0.5));

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)css_w, (float)css_h);
    io.DisplayFramebufferScale = ImVec2((float)dpr, (float)dpr);

    // The dock layout is built ONCE from GetMainViewport()->Size; if that was the
    // stale boot size the DOM/Depth column is off-screen right. First real size →
    // reflow NOW (kills the boot squish). Later sizes → defer ONE reflow until the
    // size settles (see g_embed_reflow_pending). Embedded mode always uses the
    // default layout, so nothing the user arranged by hand is lost.
    static bool first_apply = true;
    if (first_apply) {
        first_apply = false;
        LayoutManager::reset_layout();
    } else {
        g_embed_reflow_pending = true;
        g_embed_last_resize_t  = ImGui::GetTime();
    }
}
#endif


void main_loop() {
    if (g_app.ws_client) g_app.ws_client->tick();
    if (g_app.replay_ws_client) g_app.replay_ws_client->tick();
    static std::chrono::steady_clock::time_point last_frame{};
    static bool has_previous_frame = false;
    const auto now = std::chrono::steady_clock::now();
    [[maybe_unused]] ProfileFrameScope frame_cpu_scope(g_profiler, now);

    // Record frame time for percentile tracking (interval between loop entries -
    // includes browser idle between rAF ticks; what the FPS counter reflects)
    float frame_ms = 0.0f;
    if (has_previous_frame) {
        frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
        g_frame_tracker.record(frame_ms);
    }
    last_frame = now;
    has_previous_frame = true;

#ifdef __EMSCRIPTEN__
    // Viewport/DPR sync. Crosses the JS↔WASM boundary, so we throttle.
    if (EducationBoot::instance().is_embedded()) {
        // Embedded host: the canvas is a flex-box child, not the window. Consume a
        // ResizeObserver push immediately (no poll latency → no squished first
        // frames); fall back to measuring the canvas box at ~10Hz if no push has
        // landed yet (the RO can fire before the wasm exports are callable).
        if (g_embed_view_dirty) {
            g_embed_view_dirty = false;
            apply_embedded_viewport(g_embed_view_w, g_embed_view_h, g_embed_view_dpr);
        } else {
            static double last_poll = 0.0;
            const double t_now = ImGui::GetTime();
            if (t_now - last_poll >= 0.1) {
                last_poll = t_now;
                int    cw  = EM_ASM_INT({ var r = Module.canvas.getBoundingClientRect(); return Math.round(r.width); });
                int    ch  = EM_ASM_INT({ var r = Module.canvas.getBoundingClientRect(); return Math.round(r.height); });
                double dpr = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
                apply_embedded_viewport(cw, ch, dpr);
            }
        }
        // Size settled (no new push for 250ms) → snap the dock back to the
        // 248/flex/300 design widths in one reflow.
        if (g_embed_reflow_pending &&
            ImGui::GetTime() - g_embed_last_resize_t > 0.25) {
            g_embed_reflow_pending = false;
            LayoutManager::reset_layout();
        }
    } else {
        // Standalone terminal: the canvas IS the full window and there's no React
        // ResizeObserver - poll innerWidth at ~10Hz and own the backing buffer.
        static double last_viewport_poll = 0.0;
        const double t_now = ImGui::GetTime();
        if (t_now - last_viewport_poll >= 0.1) {
            last_viewport_poll = t_now;

            int window_w = EM_ASM_INT({ return window.innerWidth; });
            int window_h = EM_ASM_INT({ return window.innerHeight; });
            double dpr = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });

            static int last_w = 0;
            static int last_h = 0;
            static double last_dpr = 0.0;

            // Check if window size or DPI changed
            if (window_w != last_w || window_h != last_h || std::abs(dpr - last_dpr) > 0.01) {
                last_w = window_w;
                last_h = window_h;
                last_dpr = dpr;
                // CRITICAL: Resize the actual SDL window
                SDL_SetWindowSize(g_app.window, window_w, window_h);
                // Update canvas resolution
                EM_ASM({
                    var canvas = Module.canvas;
                    var dpr = window.devicePixelRatio || 1;
                    canvas.width = $0 * dpr;
                    canvas.height = $1 * dpr;
                }, window_w, window_h);
                // Update ImGui
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)window_w, (float)window_h);
                io.DisplayFramebufferScale = ImVec2((float)dpr, (float)dpr);
            }
        }
    }
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        // if (event.type == SDL_EVENT_QUIT) {
        //     g_app.done = true;
        // }
        // if (event.type == SDL_EVENT_WINDOW_RESIZED) {
        //     printf("SDL resize event: %d x %d\n",
        //            event.window.data1, event.window.data2);
        // }
    }
    if (!g_app.window) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // Drain freshly-decoded coin/exchange logos into GL textures (safe here:
    // between NewFrame and Render, no draw in flight) + run the LRU eviction.
    LogoManager::instance().tick();
    // REC focus layout (CLIP_FACTORY P1): while a focused clip records, hide the
    // perf overlay + app shell (below) + watchlist (update_and_render_widgets) so
    // the capture frames chart+DOM+tape only. Everything returns on stop.
    // Recorder mode (CLIP_FACTORY P2) takes the same chrome-free path for the
    // whole session - the harness captures the X display, so the shell/status
    // bar must never enter a frame.
    const bool rec_focus = ClipRecorder::focus_active() ||
                           edu::RecorderRuntime::instance().active();
    check_initialization();
    maybe_start_lesson_replay();
    maybe_start_event_replay();
    maybe_start_pack_replay();
    maybe_apply_pack_start_at();
    tick_lesson_loading_gate();
    // Usage instrumentation (design §3): session_start once, per-frame session +
    // replay watch-clock accounting. Visibility computed once (used by both).
    maybe_emit_session_start();
    // Visibility gates the watch clock. Throttle the JS<->WASM read to ~10Hz (the
    // codebase convention for boundary crossings); 100ms staleness is immaterial
    // to watch-second accuracy.
    static double s_usage_vis_poll = 0.0;
    static bool s_usage_visible = true;
    if (ImGui::GetTime() - s_usage_vis_poll >= 0.1) {
        s_usage_vis_poll = ImGui::GetTime();
        s_usage_visible = usage::is_document_visible();
    }
    const bool g_doc_visible = s_usage_visible;
    tick_session_usage(static_cast<double>(frame_ms) / 1000.0, g_doc_visible);
    // Native fixed chrome (topbar + stats strip). In embedded lesson mode the
    // React host owns the top nav + chart-header strip, so skip the ImGui shell
    // and give the dockspace the full height (no top reserve). Same branch while
    // a focused clip records - the shell must not be in the capture.
    g_profiler.begin("Shell+Dock");
    // Pack mode never ran AppShell::init (no WS → empty strip), so it takes
    // the no-shell branch even though it renders the native transport bar.
    const auto& edu = EducationBoot::instance();
    const bool full_shell = !edu.is_embedded() && !edu.is_pack() && !rec_focus;
    // Event (archive) + demo (pack) chromes suppress the native topbar/statsbar
    // (the React host owns the top nav) but still want the live terminal's bottom
    // telemetry strip (symbol · WS · FPS · present interval · UTC). Draw the status bar only.
    const bool status_bar_only = !rec_focus && !full_shell &&
                                 (edu.is_event() || edu.is_pack());
    if (full_shell) {
        LayoutManager::top_reserve    = AppShell::total_height();
        LayoutManager::status_reserve = Theme::Layout::STATUSBAR_H;  // bottom telemetry bar
        // Drawing rail moved INTO ChartWidget (2026-08-06) - no left reserve.
        LayoutManager::left_reserve   = 0.0f;
        AppShell::render(g_app.widgets, g_app.app_ctx, g_app.ws_client.get());
        drawing::render_style_editor(g_app.app_ctx);
    } else {
        LayoutManager::top_reserve    = 0.0f;
        LayoutManager::left_reserve   = 0.0f;
        LayoutManager::status_reserve = status_bar_only ? Theme::Layout::STATUSBAR_H : 0.0f;
        if (status_bar_only) {
            // Symbol from the education boot globals (g_pair is unset without
            // AppShell::init); connection pill = live WS for an archive event,
            // or the pack engine running for the box-free /demo replay.
            const std::string sym = edu.is_event() ? edu.event_symbol() : edu.pack_symbol();
            AppShell::render_statusbar(g_app.app_ctx, sym, g_app.ws_client.get(), edu.is_pack());
        }
    }
    // Drain a pending chart-toolbar "+ widget" add (embedded chromes file a
    // request here since their native topbar picker never renders). Runs in every
    // host mode; no-op when nothing is queued. Must be OUTSIDE the widget render
    // loop below (it mutates g_app.widgets).
    Menu::resolve_widget_add_request(g_app.widgets, g_app.app_ctx);
    resolve_live_flow_widget_rebuild();  // same safety requirement
    resolve_instrument_metadata_rebind();
    LayoutManager::render_dockspace(nullptr,
        g_initial_route.exchange, g_initial_route.symbol);
    g_profiler.end("Shell+Dock");
    if (g_app.drawing_mgr) g_app.drawing_mgr->tick();  // debounced persist flush
    if (LayoutManager::is_initialized) {
        update_and_render_widgets();
    }
    if (g_app.replay_mgr) {
        // Pack engine first: it processes fetch responses, serves queued
        // candle requests, and drips due frames - the gate + skip logic below
        // then observe the freshest state (no-op outside pack mode).
        g_app.replay_mgr->tick_pack_engine();
        g_app.replay_mgr->tick_buffering_gate();
        // Usage: accrue replay watch-seconds (playing AND visible) + ~15s heartbeat.
        g_app.replay_mgr->tick_usage(static_cast<double>(frame_ms) / 1000.0, g_doc_visible);
        g_app.replay_mgr->flush_pending_skip();
        g_app.replay_mgr->flush_pending_join();
        g_profiler.begin("Drip");
        g_app.replay_mgr->drip_feed_pending_replay();
        g_profiler.end("Drip");
        // A drawing-tool Esc (cancel placement/deselect, consumed by the chart's
        // DrawingLayer during the widget pass above) must not ALSO stop a
        // running replay - the replay Esc handler reads the raw key.
        if ((!g_app.drawing_mgr ||
             !g_app.drawing_mgr->escape_consumed(ImGui::GetFrameCount())) &&
            !ChartWidget::selection_escape_consumed(ImGui::GetFrameCount()))
            g_app.replay_mgr->process_keyboard_shortcuts();
        // In embedded lesson mode the React chrome owns the transport - don't
        // render the native ImGui control bar (it would stack under the React one).
        if (!EducationBoot::instance().is_embedded()) {
            g_app.replay_mgr->render_control_bar();
            g_app.replay_mgr->render_replay_launcher();
        }
        // LAST in the frame, after every stop() site: the widget sweep above has
        // run, so a context retired on an earlier frame has no subscribers left
        // and its managers can go.
        g_app.replay_mgr->release_retired_context();
        // Clip recorder (CLIP_FACTORY P1): authoritative 3:00 cap, auto-stop when
        // the replay session dies, and the burned-in watermark badge (foreground
        // draw list). No-op unless recording - and v1 recordings can only start
        // from the native transport bar, so embedded lesson/studio never record.
        ClipRecorder::tick_and_render(g_app.replay_mgr->is_active());
    }
    // The upsell modal is the ONE funnel every free-tier gate routes into (locked
    // range/preset/speed/symbol/layer pill, TIER_* server errors, login prompts) -
    // render it in ALL host modes so a gate hit anywhere reaches the same surface.
    ui::UpsellModal::instance().render();
    // The floating "Investigate this minute" reader (right-click on a chart).
    // Rendered in all host modes too: the web bridge exists wherever
    // TerminalEmbed does, and without it the panel shows its own fallback.
    ui::ResearchMomentPanel::instance().render();
    // (Replay is signaled by the restyled bottom control bar + the topbar REPLAY
    //  indicator. The old full-viewport amber border + top-left REPLAY badge were
    //  redundant with those, so they've been removed - see render_replay_overlay's
    //  deletion.)

    // Education runtimes. Lesson mode: gate the playhead + draw the spotlight +
    // emit state. Studio mode: free scrub (no gate) + drain the picker's source
    // into a replay (+ draw/capture overlay, next slice). They are mutually
    // exclusive - one host chrome per mount.
    if (EducationBoot::instance().is_lesson()) {
        edu::LessonRuntime::instance().update(g_app.app_ctx);
        edu::LessonRuntime::instance().render_overlay(g_app.app_ctx);
        // Push lesson state to the React chrome (dirty-checked, emits on change).
        edu::LessonRuntime::instance().emit_state(g_app.app_ctx);
        // Loading scrim sits ABOVE the spotlight while the clock is held.
        render_lesson_loading_overlay();
        render_historical_replay_error_overlay();
    } else if (EducationBoot::instance().is_studio()) {
        edu::StudioRuntime::instance().update(g_app.app_ctx);
        // Export session (CLIP_FACTORY P3-v1): while the studio exports a lesson,
        // the LessonRuntime drives the gate loop and burns the spotlight + cards
        // into the canvas (export-render mode - native card, no button). Outside
        // an export the studio stays a free scrub with no lesson gates.
        if (edu::StudioRuntime::instance().export_session_active()) {
            edu::LessonRuntime::instance().update(g_app.app_ctx);
            edu::LessonRuntime::instance().render_overlay(g_app.app_ctx);
        } else {
            // Draw/capture overlay (phase 1): armed from the React inspector, the
            // author drags a region/band on the chart and the inverse-projected
            // bounds are emitted back as 'edgedepth:capture'. Never during an
            // export take (it would burn into the video + steal the mouse).
            edu::StudioRuntime::instance().render_capture_overlay(g_app.app_ctx);
        }
        // Push transport state (clock/progress/paused/speed + export) to React.
        edu::StudioRuntime::instance().emit_state(g_app.app_ctx);
        // Export badge + cam bubble + 15:00 cap (no-op unless export recording).
        ClipRecorder::export_tick_and_render(
            g_app.replay_mgr && g_app.replay_mgr->is_active());
        // Loading scrim while the chosen window buffers, same as lesson mode.
        render_lesson_loading_overlay();
    } else if (EducationBoot::instance().is_event() ||
               (EducationBoot::instance().is_pack() &&
                EducationBoot::instance().is_embedded())) {
        // Event = embedded archive replay; embedded Pack (/demo) reuses the SAME
        // chrome against the pack engine. The React EventReplayShell owns the chrome
        // + transport; EventRuntime mirrors the replay transport state to it
        // (edgedepth:event) and drains its seek/pause/speed commands - all through
        // ReplayManager, which routes to the box session (event) or the pack engine
        // (pack) identically. No gate loop, no spotlight - the key_moments rail is
        // built on the web (EventRecord / showcase catalog).
        edu::EventRuntime::instance().update(g_app.app_ctx);
        edu::EventRuntime::instance().emit_state(g_app.app_ctx);
        // Loading scrim while the archive window primes (OB seed + candles), same as
        // lesson/studio. A failed historical session remains covered and paused.
        render_lesson_loading_overlay();
        render_historical_replay_error_overlay();
    }
    // Recorder driver (CLIP_FACTORY P2) - ORTHOGONAL to the session mode: layered
    // over whichever event/pack replay booted above, it walks the injected
    // RecorderScript's shots[] against ReplayManager (skip/seek/speed/tf/hold),
    // emits progress to the render harness (CustomEvent 'edgedepth:recorder'),
    // and burns the EDGEDEPTH badge. No-op unless a script global was injected.
    if (edu::RecorderRuntime::instance().active()) {
        edu::RecorderRuntime::instance().update(g_app.app_ctx);
        edu::RecorderRuntime::instance().emit_state(g_app.app_ctx);
        edu::RecorderRuntime::instance().render_overlay(g_app.app_ctx);
    }
    if (!rec_focus && PerformanceDiagnostics::enabled()) {
        const QueueBacklogSnapshot queues = g_app.data_thread
            ? g_app.data_thread->queue_metrics()
            : QueueBacklogSnapshot{};
        PerformanceDiagnostics::render(g_profiler, g_frame_tracker, queues);
    }
    // render_debug();
    g_profiler.begin("ImGui::Render");
    ImGui::Render();
    g_profiler.end("ImGui::Render");
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f ||
        std::isnan(io.DisplaySize.x) || std::isnan(io.DisplaySize.y)) {
        return;
    }
    const int display_w = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    const int display_h = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    if (display_w <= 0 || display_h <= 0) {
        return;
    }
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    g_profiler.begin("GL Draw");
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_profiler.end("GL Draw");
    g_profiler.begin("SwapWindow");
    SDL_GL_SwapWindow(g_app.window);
    g_profiler.end("SwapWindow");
}

extern "C" {
    // A modifier may have been pressed while a surrounding web control had
    // focus. Browser mouse events carry its current state even when SDL never
    // received that keydown. Queue it before SDL's mouse press is processed.
    EMSCRIPTEN_KEEPALIVE
    void sync_pointer_modifiers(int ctrl, int shift, int alt, int super) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl != 0);
        io.AddKeyEvent(ImGuiMod_Shift, shift != 0);
        io.AddKeyEvent(ImGuiMod_Alt, alt != 0);
        io.AddKeyEvent(ImGuiMod_Super, super != 0);
    }

    // Set the primary chart's timeframe (seconds). Called by the embedded host
    // (lesson Explore / studio) via Module.__set_chart_timeframe. Runs on the main
    // thread between frames - same change_timeframe path the live topbar uses.
    EMSCRIPTEN_KEEPALIVE
    void _set_chart_timeframe(int sec) {
        if (sec <= 0) return;
        for (auto& w : g_app.widgets)
            if (w && w->type() == WidgetType::Chart) {
                static_cast<ChartWidget*>(w.get())->change_timeframe(sec);
                return;
            }
    }

    EMSCRIPTEN_KEEPALIVE
    void on_popstate() {
        // Embedded in the Next host: Next owns navigation. Do nothing - a
        // reload here would tear down the React route and the canvas with it.
        if (EducationBoot::instance().is_embedded()) return;

        Route route = parse_route(url_get_current_path(), url_get_current_search());
        if (route.symbol.empty()) return;

        // For now: full reload on back/forward.
        // A proper implementation would tear down widgets and recreate,
        // but that requires a "switch_symbol" flow you don't have yet.
        EM_ASM({ location.reload(); });
    }

    // Pushed by TerminalEmbed's ResizeObserver with the canvas CSS box (logical px)
    // + milli-dpr (1000 = 1.0×). dpr crosses as an INT to dodge the f64→f32
    // mis-marshal that bit the lesson speed command (see _edu_cmd_set_speed). This
    // only RECORDS the size - main_loop applies it (never touch ImGui/SDL from a JS
    // callback that lands mid-frame). Mirrors the _edu_cmd_* deferral.
    EMSCRIPTEN_KEEPALIVE
    void _edu_set_viewport(int css_w, int css_h, int milli_dpr) {
        if (css_w <= 0 || css_h <= 0) return;
        g_embed_view_w     = css_w;
        g_embed_view_h     = css_h;
        g_embed_view_dpr   = (milli_dpr > 0) ? (milli_dpr / 1000.0) : 1.0;
        g_embed_view_dirty = true;
    }
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return -1;
    }
    // Setup OpenGL ES 3.0 context for WebGL2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    constexpr auto window_flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_OPENGL |
        SDL_WINDOW_HIGH_PIXEL_DENSITY
        // SDL_WINDOW_RESIZABLE |
        // SDL_WINDOW_ALLOW_HIGHDPI
    );
    int window_width = 1920;
    int window_height = 1080;
#ifdef __EMSCRIPTEN__
    // Disable SDL's window event handling (it's broken)
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    window_width = EM_ASM_INT({ return window.innerWidth || 1920; });
    window_height = EM_ASM_INT({ return window.innerHeight || 1080; });
#endif
    g_app.window = SDL_CreateWindow(
        "EdgeDepth Terminal",
        // SDL_WINDOWPOS_CENTERED,
        // SDL_WINDOWPOS_CENTERED,
        window_width, window_height,
        window_flags
    );
    if (!g_app.window) {
        SDL_Quit();
        return -1;
    }
    g_app.gl_context = SDL_GL_CreateContext(g_app.window);
    if (!g_app.gl_context) {
        SDL_DestroyWindow(g_app.window);
        SDL_Quit();
        return -1;
    }
    g_app.stream_mgr = std::make_unique<StreamManager>(0);  // Dummy handle
    g_app.ob_mgr = std::make_unique<OrderbookManager>();
    g_app.msg_handler = std::make_unique<MessageHandler>();
    g_app.heatmap_mgr = std::make_unique<HeatmapManager>();
    g_app.liq_heatmap_mgr = std::make_unique<LiquidationHeatmapManager>();
    // Detect embedded education mode (window.__EDGEDEPTH_LESSON__) BEFORE any
    // URL writes - when hosted inside the Next app, Next owns the URL and the
    // client must not pushState over it (see EducationBoot).
    EducationBoot::instance().detect();
    // Read the signed-in user's plan (window.__EDGEDEPTH_TIER__) once, for replay
    // gating UX. Backend still enforces the real cap. Defaults to Pro if unset.
    Entitlements::detect();
    // Display-only IANA preference. Browser Intl remains the authority for
    // named zones and historical DST; no epoch or replay state changes here.
    DisplayTimeZone::instance().initialize();
    // Recorder script (CLIP_FACTORY P2): inline in the window global, so it
    // parses synchronously right here - no fetch, no ready-latch. On a parse
    // failure the runtime stays inert and the boot is a plain event/pack replay.
    if (EducationBoot::instance().has_recorder_script()) {
        edu::RecorderRuntime::instance().load(EducationBoot::instance().recorder_json());
    }
    const bool embedded = EducationBoot::instance().is_embedded();

    g_initial_route = parse_route(url_get_current_path(), url_get_current_search());
    if (g_initial_route.symbol.empty()) {
        g_initial_route.symbol = "btcusdt";
    }
    // Studio mode: the picker already chose the symbol (carried in the studio
    // global, read by EducationBoot::detect). Use it as the initial route so the
    // widgets + dock layout build for the RIGHT symbol at boot - the client has no
    // in-place symbol switch, so the symbol must be correct from frame one (a
    // different symbol = a fresh canvas mount, driven by the studio shell).
    if (EducationBoot::instance().is_studio() &&
        !EducationBoot::instance().studio_symbol().empty()) {
        g_initial_route.symbol = EducationBoot::instance().studio_symbol();
    }
    // Event (archive replay) mode: boot the BARE terminal but for the event's
    // symbol, so widgets + dock layout build for the right symbol from frame one
    // (the client has no in-place symbol switch). The archive replay itself is
    // started from maybe_start_event_replay() once WS + metadata are ready.
    if (EducationBoot::instance().is_event() &&
        !EducationBoot::instance().event_symbol().empty()) {
        g_initial_route.symbol = EducationBoot::instance().event_symbol();
    }
    // Pack (CDN replay) mode: same rule as event mode - widgets + dock layout
    // must build for the pack's symbol from frame one. The replay itself is
    // started from maybe_start_pack_replay().
    if (EducationBoot::instance().is_pack() &&
        !EducationBoot::instance().pack_symbol().empty()) {
        g_initial_route.symbol = EducationBoot::instance().pack_symbol();
    }
    // Only own the URL when running standalone. Embedded lesson/studio leaves the
    // host's /terminal?lesson=N path untouched; event mode is hosted by Next at
    // /terminal?event=<id> (Next owns that URL too), so don't push over it either.
    // Pack mode keeps its ?pack= query params (dev) / host URL (embedded).
    if (!embedded && !EducationBoot::instance().is_event() &&
        !EducationBoot::instance().is_pack()) {
        url_push(build_terminal_path(g_initial_route.exchange, g_initial_route.symbol));
    }

    // Lesson mode: fetch the LessonDoc (credentialed → paywalled) so the replay
    // runtime can consume it. Lesson-only - studio has no doc (picker-driven) and
    // event has no doc (the archive id + window ride __EDGEDEPTH_EVENT__), and
    // is_embedded() now covers both, so gate on is_lesson() specifically.
    if (EducationBoot::instance().is_lesson()) {
        EducationBoot::instance().fetch_lesson([]() {
            auto& eb = EducationBoot::instance();
            if (eb.lesson_ready()) {
                // Parse now (no replay dependency). The archive replay is started
                // from check_initialization() once WS + metadata are ready, to
                // avoid racing replay_mgr construction.
                edu::LessonRuntime::instance().load(eb.lesson_json());
            }
        });
    }

    // Set active symbol for menu bar display
    {
        Menu::g_symbol_picker.active_symbol = g_initial_route.symbol;
        Menu::g_symbol_picker.active_exchange = g_initial_route.exchange;
        // Build display label - uppercase base, try registry first
        std::string sym = g_initial_route.symbol;
        std::transform(sym.begin(), sym.end(), sym.begin(),
            [](unsigned char c) { return std::toupper(c); });
        if (sym.size() > 4 && sym.substr(sym.size() - 4) == "USDT")
            sym = sym.substr(0, sym.size() - 4) + "/USDT";
        std::string exch = (g_initial_route.exchange == "binancef") ? "BINANCEF" : g_initial_route.exchange;
        Menu::g_symbol_picker.active_symbol_label = sym + "@" + exch;
    }

    const Terminal::Pair initial_pair{g_initial_route.exchange, g_initial_route.symbol};
    g_app.candle_mgr = std::make_unique<CandleManager>(
        initial_pair,
        300,
        *g_app.stream_mgr
    );
    g_app.debug_mgr = std::make_unique<DebugManager>(*g_app.stream_mgr, initial_pair);
    g_app.vpvr_mgr = std::make_unique<VolumeProfileManager>();
    g_app.tpo_mgr = std::make_unique<TPOManager>();
    g_app.footprint_mgr = std::make_unique<FootprintManager>();
    g_app.paper_trading_mgr = std::make_unique<PaperTradingManager>();
    g_app.series_mgr = std::make_unique<IndicatorSeriesManager>();
    g_app.analytics_mgr = std::make_unique<AnalyticsManager>();
    g_app.drawing_mgr = std::make_unique<DrawingManager>();
    g_app.drawing_mgr->init(g_initial_route.exchange, g_initial_route.symbol);

    // Create and start data thread BEFORE build_message_context so it can
    // set the dispatch_queue pointer in the context
    g_app.data_thread = std::make_unique<DataThread>();
    g_app.stream_mgr->set_dispatch_queue(&g_app.data_thread->dispatch_queue());
    g_app.build_message_context();

    // Start data thread with the fully-built context
    g_app.data_thread->start(g_app.msg_ctx);

    // Pack mode: NO WebSocket at all - the pack + symbol metadata are the only
    // network I/O, so N viewers cost the box zero streaming sessions.
    if (!EducationBoot::instance().is_pack()) {
        connect_websocket();
        // Pack mode is excluded for the same reason live is: a pack session
        // does zero network I/O, so it must not open a replay socket either.
        connect_replay_websocket();
    }
    g_app.replay_mgr = std::make_unique<ReplayManager>(replay_lane_socket(), archive_lane_socket());
    if (EducationBoot::instance().is_event() || EducationBoot::instance().is_lesson()) {
        // Arm the persistent pause before the socket-open callback can replay
        // subscriptions and before a lesson document finishes loading.
        pause_live_for_historical_replay();
    }

    // Wire context-swap callback: when replay starts, swap AppContext pointers
    // to replay managers. When replay stops, swap back to live managers.
    g_app.replay_mgr->set_context_swap_callback([](DataContext* replay_ctx) {
        if (replay_ctx) {
            // Stop live data from the backend
            g_app.stream_mgr->pause_live_subscriptions();
            // Also unsubscribe live debug (managed separately from StreamManager)
            if (g_app.debug_mgr) {
                g_app.debug_mgr->unsubscribe();
            }
            // Swap AppContext to replay managers
            g_app.app_ctx.streams      = replay_ctx->streams;
            g_app.app_ctx.orderbooks   = replay_ctx->orderbooks;
            g_app.app_ctx.heatmaps     = replay_ctx->heatmaps;
            g_app.app_ctx.liq_heatmaps = replay_ctx->liq_heatmaps;
            g_app.app_ctx.candles      = replay_ctx->candles;
            g_app.app_ctx.debug        = replay_ctx->debug;
            g_app.app_ctx.vpvr         = replay_ctx->vpvr;
            g_app.app_ctx.tpo          = replay_ctx->tpo;
            g_app.app_ctx.footprint    = replay_ctx->footprint;
            g_app.app_ctx.series       = replay_ctx->series;
            g_app.app_ctx.analytics    = replay_ctx->analytics;

            // Liq heatmap data is now delivered by the Replayer's DB drip-feed
            // (fetchLiqHeatmapTimeline in replay_seeds.go), NOT by a client-side
            // historical request. This ensures data arrives progressively at the
            // correct timestamps, preventing "future leak".

            // Reset chart widget overlay state so subscriptions are re-established
            // on the replay StreamManager, and stale heatmap data is cleared.
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::Chart) {
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    chart->reset_overlay_subscriptions();
                }
            }

            // Tear the live order-flow widgets down: they subscribe to the live
            // managers and would keep drawing live data over a replay. Note this
            // DESTROYS them (erase_if(!is_open) at the end of the frame), it does
            // not hide them, so record what went so the exit side can rebuild it.
            // Their destructors unsubscribe through the manager they subscribed
            // to, which is the LIVE one, so tearing down after the pointer swap
            // above is still correct.
            // A rebuild can still be owed from a previous exit if the user
            // re-entered a replay within a frame or two of leaving one. Do not
            // clear that debt: the widgets it owes are exactly the ones this
            // teardown would otherwise have found and recorded.
            if (!g_live_flow_rebuild.armed) {
                g_live_flow_rebuild.want_dom = false;
                g_live_flow_rebuild.want_trades = false;
                g_live_flow_rebuild.want_chart = false;
            }
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::DOM) {
                    g_live_flow_rebuild.want_dom = true;
                    g_live_flow_rebuild.pair = static_cast<DOMWidget*>(w.get())->pair();
                    w->is_open = false;
                    w->is_replay_widget = false;  // not replay-owned: it predates the swap
                } else if (w && w->type() == WidgetType::Trades) {
                    g_live_flow_rebuild.want_trades = true;
                    g_live_flow_rebuild.pair = static_cast<TradesWidget*>(w.get())->pair();
                    w->is_open = false;
                    w->is_replay_widget = false;
                }
            }

            // Create replay widgets subscribed to the replay StreamManager.
            if (!replay_ctx->symbols.empty()) {
                Terminal::Pair replay_pair{"binancef", replay_ctx->symbols[0]};
                auto fmt = SymbolRegistry::instance().get_formatter(replay_pair.exchange, replay_pair.symbol);
                double dom_tick = SymbolRegistry::instance().tick_or_zero(replay_pair.exchange, replay_pair.symbol);

                // The chart has to BELONG to the replay pair, not merely exist.
                // The old guard asked "is any chart open", which is symbol-blind,
                // and every library pack is a different symbol from the live boot
                // symbol. So a library launch kept the live btcusdt chart and drew
                // TUT into it: the panel title said btcusdt, the on-chart badge
                // said BTCUSDT, and the dock tree stayed keyed on
                // "DOM binancef btcusdt" while the swap created
                // "DOM binancef tutusdt", which is why the DOM came up floating
                // over the watchlist with no tape at all. The `?pack=` path looked
                // fine only because main.cpp rewrites the route symbol before any
                // widget is built, so its layout already matched.
                //
                // Ask the layout what pair it was BUILT for.
                if (!LayoutManager::layout_matches(replay_pair.exchange,
                                                   replay_pair.symbol)) {
                    // Retire charts built for another pair. This DESTROYS them
                    // (erase_if(!is_open) at the end of the frame) rather than
                    // hiding them, which is what is wanted here: they are being
                    // replaced. Record the pair so the exit rebuild can restore
                    // the chart that was live.
                    for (auto& w : g_app.widgets) {
                        if (!w || w->type() != WidgetType::Chart) continue;
                        if (!w->is_replay_widget) {
                            g_live_flow_rebuild.want_chart = true;
                            g_live_flow_rebuild.pair =
                                static_cast<ChartWidget*>(w.get())->pair();
                        }
                        w->is_open = false;
                    }
                    LayoutManager::reset_layout_for(replay_pair.exchange, replay_pair.symbol);
                    auto chart_w = std::make_unique<ChartWidget>(
                        replay_pair, g_app.app_ctx, dom_tick);
                    chart_w->is_replay_widget = true;
                    g_app.widgets.push_back(std::move(chart_w));
                }

                auto trades_w = std::make_unique<TradesWidget>(replay_pair, g_app.app_ctx, fmt);
                trades_w->is_replay_widget = true;
                // No title suffix - inherit live widget's dock position via same ImGui window name
                g_app.widgets.push_back(std::move(trades_w));

                auto dom_w = std::make_unique<DOMWidget>(replay_pair, g_app.app_ctx, dom_tick, 20);
                dom_w->is_replay_widget = true;
                // No title suffix - inherit live widget's dock position via same ImGui window name
                g_app.widgets.push_back(std::move(dom_w));
            }
        } else {
            // Close replay-created widgets and ARM the live rebuild. There is
            // nothing here to "restore": the live DOM and tape were destroyed on
            // the way in, so setting is_open = true on them matched nothing. The
            // rebuild runs at the safe drain point once this sweep has removed
            // the replay-owned pair, see resolve_live_flow_widget_rebuild.
            bool closed_replay_chart = false;
            for (auto& w : g_app.widgets) {
                if (!w) continue;
                if (w->is_replay_widget) {
                    closed_replay_chart = closed_replay_chart ||
                                          w->type() == WidgetType::Chart;
                    w->is_open = false;  // Will be erased by cleanup loop
                }
            }
            g_live_flow_rebuild.armed = g_live_flow_rebuild.want_dom ||
                                        g_live_flow_rebuild.want_trades ||
                                        g_live_flow_rebuild.want_chart;
            // No blind reset_layout() here. It cleared the dock tree without
            // queueing a pair, so the rebuild had nothing to key on; the chart
            // arm below re-docks for the pair that was live instead.
            g_live_flow_rebuild.want_chart =
                g_live_flow_rebuild.want_chart && closed_replay_chart;
            g_app.build_app_context();
            // Reset chart overlay subscriptions so they re-subscribe on live
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::Chart) {
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    chart->reset_overlay_subscriptions();
                }
            }
            // Historical event/lesson mounts never fall back to live delivery.
            // Navigation creates a fresh terminal mount when the user leaves.
            const auto& boot = EducationBoot::instance();
            if (!boot.is_event() && !boot.is_lesson()) {
                g_app.stream_mgr->resume_live_subscriptions();
                // Re-subscribe live debug
                if (g_app.debug_mgr) {
                    g_app.debug_mgr->subscribe(g_initial_route.symbol);
                }
            }
        }
    });

    // Wire rewind callback: when << is pressed, clear widget data
    g_app.replay_mgr->set_rewind_callback([](int64_t cutoff_ms) {
        for (auto& w : g_app.widgets) {
            if (w && w->is_replay_widget) {
                w->on_rewind(cutoff_ms);
            }
        }
        // Clear debug manager (it's a manager, not a widget)
        auto* replay_ctx = g_app.replay_mgr->replay_context();
        if (replay_ctx && replay_ctx->debug) {
            replay_ctx->debug->clear_entries();
        }
        // WS4 Observed markers: drop @forceOrder events past the rewind target -
        // they're the replay's future now (would paint ahead of the playback head).
        if (replay_ctx && replay_ctx->liq_heatmaps) {
            replay_ctx->liq_heatmaps->trim_observed_after(cutoff_ms);
        }
        // Indicator SeriesCache (VPIN et al): same rule - points past the
        // rewind target are the replay's future; idempotent re-delivery
        // from the buffer refills up to the head.
        if (replay_ctx && replay_ctx->series) {
            replay_ctx->series->trim_after(cutoff_ms);
        }
        if (replay_ctx && replay_ctx->analytics) {
            replay_ctx->analytics->trim_after(cutoff_ms);
        }
    });

    // Build app context AFTER all managers are constructed
    g_app.build_app_context();

    // Fetch symbol metadata (tick_size, step_size) from API
    SymbolRegistry::instance().fetch_metadata([]() {
    });

SDL_GL_MakeCurrent(g_app.window, g_app.gl_context);
    SDL_GL_SetSwapInterval(0); // Enable vsync
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The renderer owns its capability flags. WebGL cannot apply base-vertex
    // offsets; the application uses 32-bit indices for dense chart geometry.
    static_assert(sizeof(ImDrawIdx) == 4);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#ifdef __EMSCRIPTEN__
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Disable ini file in browser
    // EM_ASM({
    //     // Remove any keyboard event blocking
    //     Module.canvas.onkeydown = null;
    //     Module.canvas.onkeyup = null;
    //     Module.canvas.onkeypress = null;
    //     // Allow F12 and dev tools shortcuts
    //     document.addEventListener('keydown', function(e) {
    //         // F12
    //         if (e.key === 'F12') {
    //             e.stopPropagation();
    //             return;
    //         }
    //         // Ctrl+Shift+I or Cmd+Opt+I
    //         if ((e.ctrlKey && e.shiftKey && e.key === 'I') ||
    //             (e.metaKey && e.altKey && e.key === 'I')) {
    //             e.stopPropagation();
    //             return;
    //         }
    //     }, true);
    // });
#endif
    Theme::apply_dark_theme();
    Theme::apply_trading_colors();
    if (!Theme::load_fonts()) {
    }
    ImGui_ImplSDL3_InitForOpenGL(g_app.window, g_app.gl_context);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Initialize shared GPU resources for shader-based heatmap rendering
    if (!ShaderHeatmapResources::instance().init()) {
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var canvas = Module.canvas;
        // Focus immediately
        canvas.focus();
        // Only canvas clicks focus the terminal. Surrounding web inputs keep
        // their focus, and mouse modifiers repair keydowns missed while away.
        var syncModifiers = function(event) {
            Module['_sync_pointer_modifiers'](
                event.ctrlKey, event.shiftKey, event.altKey, event.metaKey);
        };
        canvas.addEventListener('mousedown', function(event) {
            syncModifiers(event);
            canvas.focus();
        }, true);
        canvas.addEventListener('mousemove', syncModifiers, true);
        canvas.addEventListener('mouseup', syncModifiers, true);
    });
    // Clip recorder support probe - once at boot. Negotiates the MediaRecorder
    // container (vp9 → vp8 → webm → mp4); a failed probe renders the transport's
    // record button disabled instead of letting a click fail silently.
    ClipRecorder::probe_support();
    url_register_popstate();
    emscripten_set_main_loop(main_loop, 0, 1);
#else
    while (!g_app.done) {
        main_loop();
    }
#endif
    // Cleanup - unique_ptrs auto-destroy, just clear widgets first
    g_app.widgets.clear();
    if (g_app.ws_client) {
        g_app.ws_client->disconnect();
    }
    if (g_app.replay_ws_client) {
        g_app.replay_ws_client->disconnect();
    }
    // Stop data thread before destroying managers it references
    if (g_app.data_thread) {
        g_app.data_thread->stop();
        g_app.data_thread.reset();
    }
    // Reset in reverse order of construction
    g_app.replay_mgr.reset();
    g_app.debug_mgr.reset();
    g_app.candle_mgr.reset();
    g_app.liq_heatmap_mgr.reset();
    g_app.heatmap_mgr.reset();
    ShaderHeatmapResources::instance().destroy();
    g_app.msg_handler.reset();
    g_app.ob_mgr.reset();
    g_app.stream_mgr.reset();
    // replay_mgr holds a raw pointer to whichever socket is the replay lane, so
    // it is already reset above; drop both sockets after it, never before.
    g_app.replay_ws_client.reset();
    g_app.ws_client.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(g_app.gl_context);
    SDL_DestroyWindow(g_app.window);
    SDL_Quit();

    return 0;
}
