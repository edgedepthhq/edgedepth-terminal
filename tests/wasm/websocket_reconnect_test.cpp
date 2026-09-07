#include <span>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "core/websocket.h"
#include "stream_handler.h"

static double clock_ms = 100;
struct Socket {
    void* user = nullptr;
    em_websocket_open_callback_func open = nullptr;
    em_websocket_message_callback_func message = nullptr;
    em_websocket_error_callback_func error = nullptr;
    em_websocket_close_callback_func close = nullptr;
    unsigned short state = 0;
};
static std::map<int, Socket> sockets;
static int next_socket = 0;
static bool fail_create = false;
static std::vector<nlohmann::json> sent;
extern "C" {
double emscripten_get_now() { return clock_ms; }
EMSCRIPTEN_WEBSOCKET_T emscripten_websocket_new(EmscriptenWebSocketCreateAttributes*) {
    if (fail_create) return -1;
    sockets[++next_socket] = {};
    return next_socket;
}
#define CALLBACK(kind, type) \
EMSCRIPTEN_RESULT emscripten_websocket_set_on##kind##_callback_on_thread(int id, void* user, type cb, pthread_t) { \
    sockets[id].user = user; sockets[id].kind = cb; return EMSCRIPTEN_RESULT_SUCCESS; }
CALLBACK(open, em_websocket_open_callback_func)
CALLBACK(message, em_websocket_message_callback_func)
CALLBACK(error, em_websocket_error_callback_func)
CALLBACK(close, em_websocket_close_callback_func)
EMSCRIPTEN_RESULT emscripten_websocket_close(int id, unsigned short, const char*) {
    sockets[id].state = 3; return EMSCRIPTEN_RESULT_SUCCESS;
}
EMSCRIPTEN_RESULT emscripten_websocket_delete(int id) {
    sockets.erase(id); return EMSCRIPTEN_RESULT_SUCCESS;
}
EMSCRIPTEN_RESULT emscripten_websocket_get_ready_state(int id, unsigned short* state) {
    if (!sockets.contains(id)) return EMSCRIPTEN_RESULT_INVALID_TARGET;
    *state = sockets[id].state; return EMSCRIPTEN_RESULT_SUCCESS;
}
EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(int, const char* text) {
    sent.push_back(nlohmann::json::parse(text)); return EMSCRIPTEN_RESULT_SUCCESS;
}
EMSCRIPTEN_RESULT emscripten_websocket_send_binary(int, void*, uint32_t) {
    return EMSCRIPTEN_RESULT_SUCCESS;
}
}
static void opened(WebSocketClient& ws) {
    const int id = ws.get_handle();
    sockets[id].state = 1;
    EmscriptenWebSocketOpenEvent event{id};
    sockets[id].open(0, &event, sockets[id].user);
}
static void closed(WebSocketClient& ws) {
    const int id = ws.get_handle();
    auto old = sockets[id];
    EmscriptenWebSocketCloseEvent event{}; event.socket = id;
    old.close(0, &event, old.user);
}
int main() {
    WebSocketClient ws;
    StreamManager streams(0);
    int owner_a, owner_b;
    const StreamKey key{{"binancef", "btcusdt"}, Terminal::Stream::Heatmap, 0};
    streams.subscribe_direct(key, &owner_a);
    streams.subscribe_direct(key, &owner_b);
    int connects = 0, messages = 0;
    ws.set_status_callback([&](const std::string& status) {
        if (status == "Connected") { ++connects; streams.update_websocket_handle(ws.get_handle()); }
        if (status == "Reconnecting") streams.update_websocket_handle(0);
    });
    ws.set_message_callback([&](const uint8_t*, size_t) { ++messages; });
    assert(ws.connect("ws://fixture"));
    assert(!ws.connect("ws://duplicate"));
    auto obsolete = sockets[ws.get_handle()];
    const int obsolete_id = ws.get_handle();
    opened(ws);
    assert(connects == 1 && sent.size() == 1);
    assert(ws.last_frame_age_ms() < 0);
    uint8_t byte = 1;
    EmscriptenWebSocketMessageEvent msg{ws.get_handle(), &byte, 1, false};
    sockets[msg.socket].message(0, &msg, sockets[msg.socket].user);
    assert(messages == 1 && ws.last_frame_age_ms() == 0);
    clock_ms += 16000;
    char status[80];
    ws.format_connection_status(status, sizeof(status));
    assert(std::string(status).find("STALE") != std::string::npos);
    ws.tick(); // Stable connection resets backoff.
    closed(ws);
    assert(!ws.is_connected() && sockets.empty());
    ws.tick(); assert(sockets.empty());
    clock_ms += 1000; ws.tick(); opened(ws);
    assert(connects == 2 && sent.size() == 2);
    assert(ws.last_frame_age_ms() < 0); // Old traffic cannot make the new socket fresh.
    EmscriptenWebSocketOpenEvent late_open{obsolete_id};
    obsolete.open(0, &late_open, obsolete.user);
    EmscriptenWebSocketCloseEvent late_close{}; late_close.socket = obsolete_id;
    obsolete.close(0, &late_close, obsolete.user);
    assert(connects == 2 && ws.is_connected());
    streams.pause_live_subscriptions();
    auto count = sent.size();
    closed(ws); clock_ms += 30000; ws.tick(); opened(ws);
    assert(sent.size() == count); // Reconnect cannot unpause an owner.
    streams.unsubscribe_direct(key, &owner_a);
    streams.resume_live_subscriptions();
    assert(sent.size() == count + 1);
    streams.unsubscribe_direct(key, &owner_b);
    count = sent.size();
    closed(ws); clock_ms += 30000; ws.tick(); opened(ws);
    assert(sent.size() == count); // Released feeds stay released.
    ws.disconnect(); clock_ms += 60000; ws.tick();
    assert(sockets.empty());
    fail_create = true;
    assert(!ws.connect("ws://fixture"));
    clock_ms += 30000; ws.tick(); assert(sockets.empty());
    fail_create = false;
    clock_ms += 30000; ws.tick(); assert(sockets.size() == 1);
    clock_ms += 15000; ws.tick(); assert(sockets.empty()); // Hung connect timeout.
    clock_ms += 30000; ws.tick(); opened(ws);
    auto error_socket = sockets[ws.get_handle()];
    EmscriptenWebSocketErrorEvent error{ws.get_handle()};
    error_socket.error(0, &error, error_socket.user);
    assert(sockets.empty() && !ws.is_connected());
    ws.disconnect();
    std::puts("PASS: retry, timeout, stale callbacks, freshness, ownership and pause lifecycle");
}
