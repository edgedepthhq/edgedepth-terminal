// Compile with the production StreamManager and Emscripten headers. Only the
// WebSocket boundary is replaced, so this exercises the real lifecycle paths.
#include <span>
#include <cstdio>
#include <cstdlib>
#include "stream_handler.h"
#include "core/trade_at_price.h"
#include "core/data_queues.h"

static std::vector<nlohmann::json> sent;
static unsigned short ready_state = 1;
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(
    EMSCRIPTEN_WEBSOCKET_T, const char* text) {
    sent.push_back(nlohmann::json::parse(text));
    return EMSCRIPTEN_RESULT_SUCCESS;
}
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_get_ready_state(
    EMSCRIPTEN_WEBSOCKET_T, unsigned short* state) {
    *state = ready_state;
    return EMSCRIPTEN_RESULT_SUCCESS;
}
static void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    {
        StreamManager live(1), replay(0);
        replay.set_replay_mode(true);
        QueueBacklogCounters counters;
        DispatchQueue pending(counters);
        live.set_dispatch_queue(&pending);
        const StreamKey key{{"binancef", "btcusdt"}, Terminal::Stream::Stats, 300000};
        int retired_calls = 0, current_calls = 0;
        StreamHandler<Terminal::Stat> retired{&retired_calls,
            [](void* owner, const Terminal::Stat&) { ++*static_cast<int*>(owner); }};
        live.subscribe_stats(key, retired);
        live.dispatch_stat(key, {}); // queued before the context changes
        live.pause_live_subscriptions();
        // The owner releases the original manager, never whichever manager the
        // shared AppContext happens to point to after the swap.
        live.unsubscribe_stats(key, retired.widget_ptr);
        replay.subscribe_stats(key, retired);
        replay.unsubscribe_stats(key, retired.widget_ptr);
        live.subscribe_stats(key, {&current_calls, retired.callback});
        live.resume_live_subscriptions();
        std::vector<PendingDispatch> items;
        pending.drain(items);
        for (auto& item : items) item.execute(live);
        check(retired_calls == 0 && current_calls == 1,
              "late stats target the current owner after live/replay teardown");
        live.unsubscribe_stats(key, &current_calls);
    }

    {
        StreamManager sm(1);
        int owner = 0;
        const StreamKey key{{"binancef", "sophusdt"}, Terminal::Stream::Orderbook, 0};
        check(!sm.refresh_orderbook(key, 10000), "unowned depth cannot be refreshed");
        sm.subscribe_direct(key, &owner);
        sent.clear();
        check(sm.refresh_orderbook(key, 10000), "broken depth requests a fresh seed");
        check(sent.size() == 2 && sent[0]["method"] == "unsubscribe" && sent[1]["method"] == "subscribe",
            "refresh restarts exactly the depth subscription");
        check(!sm.refresh_orderbook(key, 14999) && sent.size() == 2, "shared charts cannot cause a refresh storm");
        ready_state = 0;
        check(!sm.refresh_orderbook(key, 15000), "closed socket does not consume refresh cooldown");
        ready_state = 1;
        check(sm.refresh_orderbook(key, 15000), "recovery can retry after cooldown");
        sm.set_replay_mode(true);
        check(!sm.refresh_orderbook(key, 20000), "replay never requests live depth");
        sm.set_replay_mode(false);
        sm.pause_live_subscriptions();
        check(!sm.refresh_orderbook(key, 20000), "paused live subscriptions cannot be refreshed");
        sm.resume_live_subscriptions();
        sm.unsubscribe_direct(key, &owner);
        check(!sm.refresh_orderbook(key, 20000), "refresh does not retain released owners");
    }
    {
        StreamManager sm(1);
        TradeAtPriceAccumulator flow;
        const Terminal::Pair pair{"binancef", "sophusdt"};
        const StreamKey key{pair, Terminal::Stream::Trades, 0};
        flow.init(pair, sm, 0.01, true);
        sm.dispatch_trade_impl(key, {10, 2, 1000, true});
        sm.dispatch_trade_impl(key, {10, 2, 1000, true});
        sm.dispatch_trade_impl(key, {10.01, 3, 2000, false});
        flow.advance_to(1000);
        check(flow.total_delta() == 4 && flow.total_trades() == 2, "as-of CVD preserves equal records and excludes future sells");
        sm.dispatch_trade_impl(key, {10.02, 5, 2500, true});
        check(flow.total_delta() == 4, "pause holds CVD while reception continues");
        flow.advance_to(2000);
        check(flow.total_delta() == 1 && flow.get(10.01)->sell_volume == 3, "stepping advances flow only to the chart clock");
        flow.advance_to(2500);
        check(flow.total_delta() == 6, "resume drains each record once");
        flow.advance_to(2500);
        check(flow.total_delta() == 6, "repeated render cannot double count");
        sm.dispatch_trade_impl(key, {10, 7, 301000, false});
        sm.dispatch_trade_impl(key, {10, 9, 302000, true});
        flow.advance_to(301000);
        check(flow.total_delta() == -7 && flow.total_trades() == 1, "5m reset uses market time");
        flow.advance_to(302000);
        check(flow.total_delta() == 2, "automatic reset preserves queued future trades");
        sm.dispatch_trade_impl(key, {10, 4, 602000, true});
        flow.advance_to(601000);
        check(flow.total_delta() == 0, "quiet market resets at the chart clock boundary");
        sm.dispatch_trade_impl(key, {10, 100, 400000, false});
        flow.advance_to(602000);
        check(flow.total_delta() == 4, "late records cannot leak across reset boundaries; future queue survives");
        flow.reset();
        check(flow.total_trades() == 0, "rewind resets accumulated flow");
        sm.dispatch_trade_impl(key, {10, 1, 500, true});
        flow.advance_to(500);
        check(flow.total_delta() == 1, "rewound market clock can start again");
        for (int i = 0; i < 20001; ++i) sm.dispatch_trade_impl(key, {10, 1, 1000 + i, true});
        check(flow.total_delta() == 1, "backlog overflow cannot mutate paused totals");
        flow.advance_to(22000);
        check(flow.total_trades() == 1 && flow.reset_after_gap(), "overflow restarts and explicitly marks incomplete cumulative coverage");
    }

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
