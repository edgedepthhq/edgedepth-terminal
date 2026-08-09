#pragma once
#include <cstdint>
#include <emscripten/websocket.h>
#include <emscripten/html5.h>
#include <string>
#include <functional>

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const uint8_t*, size_t)>;
    using StatusCallback = std::function<void(const std::string&)>;

    WebSocketClient();
    ~WebSocketClient();

    bool connect(const std::string& url);
    void disconnect();
    bool is_connected() const { return is_connected_; }

    bool send_text(const std::string& message) const;
    bool send_binary(const std::uint8_t* data, size_t length) const;

    void set_message_callback(MessageCallback callback) {
        message_callback_ = callback;
    }
    void set_status_callback(StatusCallback callback) {
        status_callback_ = callback;
    }

    EMSCRIPTEN_WEBSOCKET_T get_handle() const { return socket_; }

private:
    EMSCRIPTEN_WEBSOCKET_T socket_;
    bool is_connected_;
    MessageCallback message_callback_;
    StatusCallback status_callback_;
    static EM_BOOL on_open(int eventType,
                              const EmscriptenWebSocketOpenEvent* event,
                              void* userData);
    static EM_BOOL on_message(int eventType,
                              const EmscriptenWebSocketMessageEvent* event,
                              void* userData);
    static EM_BOOL on_error(int eventType,
                            const EmscriptenWebSocketErrorEvent* event,
                            void* userData);
    static EM_BOOL on_close(int eventType,
                            const EmscriptenWebSocketCloseEvent* event,
                            void* userData);
};
