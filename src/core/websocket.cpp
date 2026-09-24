#include "core/websocket.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#include <emscripten/html5.h>


WebSocketClient::WebSocketClient()
    : socket_(0)
    , is_connected_(false)
    , message_callback_(nullptr)
    , status_callback_(nullptr)
{}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

bool WebSocketClient::connect(const std::string& url) {
    if (reconnect_enabled_) return false;
    url_ = url;
    reconnect_enabled_ = true;
    retry_count_ = 0;
    return open_socket();
}

bool WebSocketClient::open_socket() {
    connecting_at_ms_ = emscripten_get_now();
    EmscriptenWebSocketCreateAttributes attrs;
    emscripten_websocket_init_create_attributes(&attrs);
    attrs.url = url_.c_str();
    socket_ = emscripten_websocket_new(&attrs);
    if (socket_ <= 0) {
        socket_ = 0;
        schedule_retry();
        return false;
    }
    emscripten_websocket_set_onopen_callback(socket_, this, on_open);
    emscripten_websocket_set_onmessage_callback(socket_, this, on_message);
    emscripten_websocket_set_onerror_callback(socket_, this, on_error);
    emscripten_websocket_set_onclose_callback(socket_, this, on_close);
    return true;
}

void WebSocketClient::release_socket() {
    const auto old = socket_;
    socket_ = 0;
    is_connected_ = false;
    if (old <= 0) return;
    emscripten_websocket_set_onopen_callback(old, nullptr, nullptr);
    emscripten_websocket_set_onmessage_callback(old, nullptr, nullptr);
    emscripten_websocket_set_onerror_callback(old, nullptr, nullptr);
    emscripten_websocket_set_onclose_callback(old, nullptr, nullptr);
    emscripten_websocket_close(old, 1000, "Client disconnect");
    emscripten_websocket_delete(old);
}

void WebSocketClient::schedule_retry() {
    release_socket();
    if (!reconnect_enabled_) return;
    double delay_ms;
    if (planned_restart_active()) {
        // The hub told us it is coming back: poll it, don't back off. A refused
        // connect while the port is closed costs nothing.
        delay_ms = 2000;
    } else {
        delay_ms = std::min(30000u, 1000u << std::min(retry_count_, 5u));
        retry_count_ = std::min(retry_count_ + 1, 6u);
    }
    retry_at_ms_ = emscripten_get_now() + delay_ms;
    if (status_callback_) status_callback_("Reconnecting");
}

bool WebSocketClient::planned_restart_active() const {
    // Keep polling for a grace period past the ETA; a slow model load is not a
    // reason to fall back to 30 s retries.
    return !is_connected_ && planned_restart_until_ms_ > 0 &&
           emscripten_get_now() < planned_restart_until_ms_ + 45000;
}

double WebSocketClient::planned_restart_remaining_ms() const {
    return std::max(0.0, planned_restart_until_ms_ - emscripten_get_now());
}

bool WebSocketClient::take_reconnected_after_restart() {
    const bool r = reconnected_after_restart_;
    reconnected_after_restart_ = false;
    return r;
}

double WebSocketClient::reconnected_age_ms() const {
    return reconnected_at_ms_ <= 0 ? -1 : emscripten_get_now() - reconnected_at_ms_;
}

void WebSocketClient::disconnect() {
    reconnect_enabled_ = false;
    release_socket();
    retry_at_ms_ = 0;
    last_frame_ms_ = -1;
}

void WebSocketClient::tick() {
    if (!reconnect_enabled_) return;
    const double now = emscripten_get_now();
    if (socket_ > 0) {
        unsigned short state = 0;
        const auto result = emscripten_websocket_get_ready_state(socket_, &state);
        if (result != EMSCRIPTEN_RESULT_SUCCESS || state >= 2 ||
            (!is_connected_ && now - connecting_at_ms_ >= 15000)) {
            schedule_retry();
        } else if (is_connected_ && now - opened_at_ms_ >= 10000) {
            retry_count_ = 0; // A flapping connection must still back off.
        }
    } else if (now >= retry_at_ms_) {
        open_socket();
    }
}

double WebSocketClient::last_frame_age_ms() const {
    return last_frame_ms_ < 0 ? -1 : std::max(0.0, emscripten_get_now() - last_frame_ms_);
}

void WebSocketClient::format_connection_status(char* text, size_t size) const {
    if (is_connected_) {
        const double age = last_frame_age_ms();
        if (age < 0) {
            snprintf(text, size, "WS OPEN / WAITING FOR DATA");
        } else {
            snprintf(text, size, "WS OPEN / %sFRAME %.0fs AGO",
                     age >= 15000 ? "STALE " : "", age / 1000.0);
        }
    } else if (!reconnect_enabled_) {
        snprintf(text, size, "WS DISCONNECTED");
    } else if (planned_restart_active()) {
        snprintf(text, size, "WS RESTARTING ~%.0fs", planned_restart_remaining_ms() / 1000.0);
    } else if (socket_ > 0) {
        snprintf(text, size, "WS CONNECTING");
    } else {
        snprintf(text, size, "WS RETRY IN %.0fs",
                 std::max(0.0, retry_at_ms_ - emscripten_get_now()) / 1000.0);
    }
}

bool WebSocketClient::send_text(const std::string& message) const {
    if (!is_connected_) {
        return false;
    }
    EMSCRIPTEN_RESULT result = emscripten_websocket_send_utf8_text(
        socket_, message.c_str()
    );
    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        return false;
    }
    return true;
}

bool WebSocketClient::send_binary(const uint8_t* data, size_t length) const {
    if (!is_connected_) {
        return false;
    }
    EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(
        socket_, (void*)data, length
    );
    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        return false;
    }
    return true;
}

EM_BOOL WebSocketClient::on_open(int eventType, const EmscriptenWebSocketOpenEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (event->socket != client->socket_ || !client->reconnect_enabled_) return EM_TRUE;
    client->is_connected_ = true;
    client->opened_at_ms_ = emscripten_get_now();
    client->last_frame_ms_ = -1;
    if (client->planned_restart_since_ms_ > 0) {
        client->last_outage_ms_ = client->opened_at_ms_ - client->planned_restart_since_ms_;
        client->reconnected_at_ms_ = client->opened_at_ms_;
        client->reconnected_after_restart_ = true;
        client->planned_restart_since_ms_ = 0;
        client->planned_restart_until_ms_ = 0;
    }
    if (client->status_callback_) {
        client->status_callback_("Connected");
    }
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_message(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (event->socket != client->socket_ || !client->is_connected_) return EM_TRUE;
    if (!event->isText && event->numBytes > 0) client->last_frame_ms_ = emscripten_get_now();
    if (client->message_callback_) {
        const auto* data = static_cast<const uint8_t*>(event->data);
        client->message_callback_(data, event->numBytes);
    }
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_error(int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (event->socket == client->socket_) client->schedule_retry();
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_close(int, const EmscriptenWebSocketCloseEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (event->socket != client->socket_) return EM_TRUE;
    // 1001 Going Away with "restart <eta>" is the hub announcing a redeploy
    // (server_session.AnnounceRestart). Anything else is an unplanned drop.
    if (event->code == 1001 && std::strncmp(event->reason, "restart", 7) == 0) {
        int eta_s = std::atoi(event->reason + 7);
        if (eta_s <= 0 || eta_s > 600) eta_s = 45;
        const double now = emscripten_get_now();
        client->planned_restart_since_ms_ = now;
        client->planned_restart_until_ms_ = now + eta_s * 1000.0;
    }
    client->schedule_retry();
    return EM_TRUE;
}
