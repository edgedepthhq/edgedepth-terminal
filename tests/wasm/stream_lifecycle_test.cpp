// Compile with the production StreamManager and Emscripten headers. Only the
// WebSocket boundary is replaced, so this exercises the real lifecycle paths.
#include <span>
#include <cstdio>
#include <cstdlib>
#include "stream_handler.h"

static std::vector<nlohmann::json> sent;
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(
    EMSCRIPTEN_WEBSOCKET_T, const char* text) {
    sent.push_back(nlohmann::json::parse(text));
    return EMSCRIPTEN_RESULT_SUCCESS;
}
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_get_ready_state(
    EMSCRIPTEN_WEBSOCKET_T, unsigned short* state) {
    *state = 1;
    return EMSCRIPTEN_RESULT_SUCCESS;
}
static void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    for (auto stream : {Terminal::Stream::TickVolume, Terminal::Stream::Heatmap}) {
        sent.clear();
        StreamManager sm(1);
        int a = 0, b = 0;
        const StreamKey key{{"binancef", "btcusdt"}, stream,
                            stream == Terminal::Stream::TickVolume ? 60 : 0};
        sm.subscribe_direct(key, &a);
        sm.subscribe_direct(key, &a);
        sm.subscribe_direct(key, &b);
        check(sent.size() == 1, "owners share one subscription");
        check(sent.back()["data"]["timeframe"] == key.timeframe, "wire timeframe");
        check(sent.back()["data"]["stream"] == static_cast<int>(stream), "wire stream");
        sm.update_websocket_handle(2);
        check(sent.size() == 2, "reconnect restores subscription");
        sm.pause_live_subscriptions();
        check(sent.size() == 3 && sent.back()["method"] == "unsubscribe", "replay pauses feed");
        sm.update_websocket_handle(3);
        check(sent.size() == 3, "reconnect while paused stays paused");
        sm.resume_live_subscriptions();
        check(sent.size() == 4 && sent.back()["method"] == "subscribe", "resume restores feed");
        sm.unsubscribe_direct(key, &a);
        check(sent.size() == 4, "another owner still needs feed");
        sm.unsubscribe_direct(key, &b);
        check(sent.size() == 5 && sent.back()["method"] == "unsubscribe", "last owner releases feed");
        sm.update_websocket_handle(4);
        check(sent.size() == 5, "released feeds stay released");
        StreamManager replay(1);
        replay.set_replay_mode(true);
        replay.subscribe_direct(key, &a);
        replay.update_websocket_handle(5);
        replay.unsubscribe_direct(key, &a);
        check(sent.size() == 5, "replay never creates live subscriptions");
    }
    std::puts("Stream lifecycle passed for footprints and heatmaps");
    for (bool chart_first : {false, true}) {
        sent.clear();
        StreamManager sm(1);
        int chart = 0;
        const StreamKey key{{"binancef", "btcusdt"}, Terminal::Stream::Orderbook, 0};
        if (chart_first) sm.subscribe_direct(key, &chart);
        sm.subscribe_orderbook(key);
        if (!chart_first) sm.subscribe_direct(key, &chart);
        check(sent.size() == 1, "RT and DOM share the existing orderbook subscription");
        sm.update_websocket_handle(2);
        check(sent.size() == 2, "RT and DOM restore once on reconnect");
        sm.pause_live_subscriptions();
        check(sent.size() == 3, "shared depth pauses once");
        sm.update_websocket_handle(3);
        check(sent.size() == 3, "shared paused depth stays paused");
        sm.resume_live_subscriptions();
        check(sent.size() == 4, "shared depth resumes once");
        sm.unsubscribe_direct(key, &chart);
        check(sent.size() == 4, "closing RT does not unsubscribe the DOM");
    }
}
