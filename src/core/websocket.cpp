#include "core/websocket.h"
#include <cstdio>
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
    if (is_connected_) {
        return false;
    }
    EmscriptenWebSocketCreateAttributes attrs;
    emscripten_websocket_init_create_attributes(&attrs);
    attrs.url = url.c_str();
    socket_ = emscripten_websocket_new(&attrs);
    if (socket_ <= 0) {
        return false;
    }
    // Set callbacks (pass 'this' as user data)
    emscripten_websocket_set_onopen_callback(socket_, this, on_open);
    emscripten_websocket_set_onmessage_callback(socket_, this, on_message);
    emscripten_websocket_set_onerror_callback(socket_, this, on_error);
    emscripten_websocket_set_onclose_callback(socket_, this, on_close);
    return true;
}

void WebSocketClient::disconnect() {
    if (socket_ > 0) {
        emscripten_websocket_close(socket_, 1000, "Client disconnect");
        emscripten_websocket_delete(socket_);
        socket_ = 0;
    }
    is_connected_ = false;
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
    client->is_connected_ = true;
    if (client->status_callback_) {
        client->status_callback_("Connected");
    }
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_message(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (client->message_callback_) {
        const auto* data = static_cast<const uint8_t*>(event->data);
        client->message_callback_(data, event->numBytes);
    }
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_error(int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    if (client->status_callback_) {
        client->status_callback_("Error");
    }
    return EM_TRUE;
}

EM_BOOL WebSocketClient::on_close(int eventType, const EmscriptenWebSocketCloseEvent* event, void* userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    client->is_connected_ = false;
    if (client->status_callback_) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Closed: %s", event->reason);
        client->status_callback_(buf);
    }
    return EM_TRUE;
}


