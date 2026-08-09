#include "stream_handler.h"
#include "core/data_thread.h"

using json = nlohmann::json;

// Pack replay request hook (Hot Replay Path B) — consulted by REPLAY-mode
// managers before the WebSocket; see stream_handler.h.
std::function<bool(const std::string&)> StreamManager::pack_request_hook_;

void StreamManager::send_message(const std::string& message) const {
    // Pack replay: the engine owns every replay-context request (candle
    // backfills etc.) — nothing may leak to the box from a pack session.
    if (replay_mode_ && pack_request_hook_ && pack_request_hook_(message)) {
        return;
    }
    if (ws_ <= 0) return;
    // Guard: only send when socket is actually OPEN (readyState == 1)
    // is_connected_ on WebSocketClient is set in on_open, but StreamManager
    // holds the raw handle and can be called before on_open fires.
    unsigned short ready_state = 0;
    emscripten_websocket_get_ready_state(ws_, &ready_state);
    if (ready_state != 1) {  // 1 = OPEN
        return;
    }
    emscripten_websocket_send_utf8_text(ws_, message.c_str());
}
// void StreamManager::send_message(const std::string& message) const {
//     if (ws_ == 0) {
//         printf("Error: Cannot send message, WebSocket not connected (handle = 0)\n");
//         return;
//     }
//     emscripten_websocket_send_utf8_text(ws_, message.c_str());
//     printf("Sent message: %s\n", message.c_str());
// }

void StreamManager::subscribe_trades(StreamKey key, StreamHandler<Terminal::Trade> handler) {
    if (trade_subs_.find(key) == trade_subs_.end()) {
        send_subscribe(key);
    }
    trade_subs_[key].push_back(handler);
}

void StreamManager::subscribe_orderbook(const StreamKey& key) {
    if (const auto it = orderbook_subs_.find(key); it == orderbook_subs_.end()) {
        // Insert empty vector just to track refcount
        orderbook_subs_[key] = {};
        send_subscribe(key);
    }
}

// void StreamManager::subscribe_orderbook(StreamKey key, StreamHandler<Terminal::Orderbook> handler) {
//     if (orderbook_subs_.find(key) == orderbook_subs_.end()) {
//         send_subscribe(key);
//     }
//     orderbook_subs_[key].push_back(handler);
// }

void StreamManager::subscribe_candles(const StreamKey& key, StreamHandler<Terminal::Candle> handler) {
    // Only send server-side subscribe if neither candle_subs_ nor candle_batch_subs_
    // have registered for this key yet. subscribe_candles_batch() may have already
    // sent the subscribe — sending it twice causes the server to create duplicate
    // NATS consumers, doubling every candle/stat/volume message on this stream.
    if (candle_subs_.find(key) == candle_subs_.end() &&
        candle_batch_subs_.find(key) == candle_batch_subs_.end()) {
        send_subscribe(key);
    }
    candle_subs_[key].push_back(handler);
}

void StreamManager::subscribe_candles_batch(const StreamKey& key, const StreamBatchHandler<Terminal::Candle> handler) {
    candle_batch_subs_[key].push_back(handler);
    // Only send subscribe if this is the FIRST subscription to this key
    if (candle_subs_.find(key) == candle_subs_.end() && candle_batch_subs_[key].size() == 1) {
        send_subscribe(key);  // Only subscribe once
    }
}

void StreamManager::subscribe_stats(const StreamKey& key, StreamHandler<Terminal::Stat> handler) {
    if (stats_subs_.find(key) == stats_subs_.end()) {
        send_subscribe(key);
    }
    stats_subs_[key].push_back(handler);
}

void StreamManager::subscribe_volume(const StreamKey& key, StreamHandler<Terminal::Volume> handler) {
    // Volume stream is already subscribed by default — don't re-subscribe.
    // Just register the callback for dispatch.
    volume_subs_[key].push_back(handler);
}

void StreamManager::unsubscribe_volume(const StreamKey& key, void* widget_ptr) {
    unsubscribe_impl(volume_subs_, key, widget_ptr);
}

void StreamManager::unsubscribe_trades(const StreamKey &key, void* widget_ptr) {
    unsubscribe_impl(trade_subs_, key, widget_ptr);
}

void StreamManager::unsubscribe_candles(const StreamKey &key, void* widget_ptr) {
    bool removed = false;

    if (auto it = candle_subs_.find(key); it != candle_subs_.end()) {
        auto& handlers = it->second;
        const size_t before = handlers.size();
        handlers.erase(
            std::remove_if(handlers.begin(), handlers.end(),
                [widget_ptr](const StreamHandler<Terminal::Candle>& h) {
                    return h.widget_ptr == widget_ptr;
                }),
            handlers.end());
        removed = removed || handlers.size() != before;
        if (handlers.empty()) candle_subs_.erase(it);
    }

    if (auto it = candle_batch_subs_.find(key); it != candle_batch_subs_.end()) {
        auto& handlers = it->second;
        const size_t before = handlers.size();
        handlers.erase(
            std::remove_if(handlers.begin(), handlers.end(),
                [widget_ptr](const StreamBatchHandler<Terminal::Candle>& h) {
                    return h.widget_ptr == widget_ptr;
                }),
            handlers.end());
        removed = removed || handlers.size() != before;
        if (handlers.empty()) candle_batch_subs_.erase(it);
    }

    // A candle stream has two client callback maps but only one server-side
    // subscription. Release it only after both maps have lost this key.
    if (removed && candle_subs_.find(key) == candle_subs_.end() &&
        candle_batch_subs_.find(key) == candle_batch_subs_.end()) {
        send_unsubscribe(key);
    }
}

void StreamManager::unsubscribe_orderbook(const StreamKey &key, void* widget_ptr) {
    unsubscribe_impl(orderbook_subs_, key, widget_ptr);
}

void StreamManager::unsubscribe_stats(const StreamKey &key, void* widget_ptr) {
    unsubscribe_impl(stats_subs_, key, widget_ptr);
}

// WS4 Observed markers — discrete @forceOrder liquidation events. Live comes
// from the LIQUIDATIONS NATS stream (numeric stream id 5 in the subscribe
// JSON); replay comes from the archive bundle / the liquidation_events DB
// seed, delivered through the same STREAM_LIQUIDATIONS payload path.
void StreamManager::subscribe_liquidations(const StreamKey& key,
                                           StreamHandler<Terminal::Liquidation> handler) {
    if (liquidation_subs_.find(key) == liquidation_subs_.end()) {
        send_subscribe(key);
    }
    liquidation_subs_[key].push_back(handler);
}

void StreamManager::unsubscribe_liquidations(const StreamKey& key, void* widget_ptr) {
    unsubscribe_impl(liquidation_subs_, key, widget_ptr);
}

void StreamManager::subscribe_patterns(
    const StreamKey& key,
    StreamHandler<Terminal::PatternOverlay> handler) {
    if (pattern_subs_.find(key) == pattern_subs_.end()) {
        send_subscribe(key);
    }
    pattern_subs_[key].push_back(handler);
}

void StreamManager::unsubscribe_patterns(const StreamKey& key, void* widget_ptr) {
    unsubscribe_impl(pattern_subs_, key, widget_ptr);
}

void StreamManager::request_historical_candles(const Terminal::Pair& pair, int64_t timeframe, size_t count, int64_t end_time) const {
    json request = {
        {"method", "get_historical_candles"},
        {"data", {
                {"pair", {
                    {"exchange", pair.exchange},
                    {"symbol", pair.symbol}
                }},
                {"timeframe", timeframe},
                {"count", count}
        }}
    };
    if (end_time > 0) {
        request["data"]["end_time"] = end_time;
    }
    send_message(request.dump());
}

void StreamManager::request_candles_before(
    const Terminal::Pair& pair, int64_t timeframe,
    int64_t before_timestamp_ms, size_t count) const
{
    json request = {
        {"method", "get_historical_candles"},
        {"data", {
            {"pair", {
                {"exchange", pair.exchange},
                {"symbol", pair.symbol}
            }},
            {"timeframe", timeframe},
            {"count", count},
            {"end_time", before_timestamp_ms}
        }}
    };
    send_message(request.dump());
}

void StreamManager::request_replay_preview_candles(const Terminal::Pair& pair, int64_t timeframe) const {
    json request = {
        {"method", "get_replay_preview_candles"},
        {"data", {
            {"pair", {
                {"exchange", pair.exchange},
                {"symbol", pair.symbol}
            }},
            {"timeframe", timeframe}
        }}
    };
    send_message(request.dump());
}

void StreamManager::request_historical_oi(const Terminal::Pair& pair, int64_t timeframe, int count) const {
    json request = {
        {"method", "get_historical_oi"},
        {"data", {
            {"pair", {
                {"exchange", pair.exchange},
                {"symbol", pair.symbol}
            }},
            {"timeframe", timeframe},
            {"count", count}
        }}
    };
    send_message(request.dump());
}

void StreamManager::request_historical_funding(const Terminal::Pair& pair, int64_t timeframe, int count) const {
    json request = {
        {"method", "get_historical_funding"},
        {"data", {
            {"pair", {
                {"exchange", pair.exchange},
                {"symbol", pair.symbol}
            }},
            {"timeframe", timeframe},
            {"count", count}
        }}
    };
    send_message(request.dump());
}

void StreamManager::request_historical_vpin(const Terminal::Pair& pair, int64_t start_ms,
                                            int64_t end_ms, int count) const {
    json request = {
        {"method", "get_historical_vpin"},
        {"data", {
            {"pair", {
                {"exchange", pair.exchange},
                {"symbol", pair.symbol}
            }},
            {"count", count}
        }}
    };
    // F3 absolute range — only send what's set; the server defaults +
    // replay-clamps the rest (no future leak during replay sessions).
    if (start_ms > 0) request["data"]["start_ms"] = start_ms;
    if (end_ms > 0)   request["data"]["end_ms"] = end_ms;
    send_message(request.dump());
}

void StreamManager::request_historical_heatmap(const Terminal::Pair &pair, const std::string &mode, int64_t start_time_ms, int64_t end_time_ms, int64_t timeframe) const {
    json request = {
        {"method", "get_historical_heatmap"},
        {"data", {
                {"pair", {
                    {"exchange", pair.exchange},
                    {"symbol", pair.symbol}
                }},
                {"mode", mode},
                {"timeframe", timeframe},
                {"start_time", start_time_ms},
                {"end_time", end_time_ms}
        }}
    };
    send_message(request.dump());
}

void StreamManager::request_historical_liq_heatmap(
    const Terminal::Pair& pair,
    int64_t start_time_ms,
    int64_t end_time_ms,
    int64_t timeframe) const {
    json request = {
        {"method", "get_historical_liq_heatmap"},
        {"data", {
                {"pair", {
                    {"exchange", pair.exchange},
                    {"symbol", pair.symbol}
                }},
                {"timeframe", timeframe},
                {"start_time", start_time_ms},
                {"end_time", end_time_ms}
        }}
    };
    send_message(request.dump());
}


template<typename T>
void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<T>>> &subs,
    StreamKey key,
    void* widget_ptr) {
    auto it = subs.find(key);
    if (it == subs.end()) return;

    auto& handlers = it->second;
    handlers.erase(
        std::remove_if(handlers.begin(), handlers.end(),
            [widget_ptr](const StreamHandler<T>& h) {
                return h.widget_ptr == widget_ptr;
            }),
            handlers.end()
    );
    if (handlers.empty()) {
        send_unsubscribe(key);
        subs.erase(it);
    }
}

void StreamManager::dispatch_trade(const StreamKey& key, const Terminal::Trade& trade) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, trade](StreamManager& sm) {
            sm.dispatch_trade_impl(key, trade);
        }});
        return;
    }
    dispatch_trade_impl(key, trade);
}

void StreamManager::dispatch_trade_impl(const StreamKey& key, const Terminal::Trade& trade) {
    if (auto it = trade_subs_.find(key); it != trade_subs_.end()) {
        for (auto&[widget_ptr, callback] : it->second) {
            callback(widget_ptr, trade);
        }
    }
}

void StreamManager::dispatch_candle(const StreamKey& key, const Terminal::Candle& candle) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, candle](StreamManager& sm) {
            sm.dispatch_candle_impl(key, candle);
        }});
        return;
    }
    dispatch_candle_impl(key, candle);
}

void StreamManager::dispatch_candle_impl(const StreamKey& key, const Terminal::Candle& candle) {
    if (const auto it = candle_subs_.find(key); it != candle_subs_.end()) {
        for (auto&[widget_ptr, callback] : it->second) {
            callback(widget_ptr, candle);
        }
    } else {
        static int miss_count = 0;
        if (++miss_count <= 3) {
        }
    }
}

void StreamManager::dispatch_candles(const StreamKey& key, const std::span<const Terminal::Candle> batch) {
    if (dispatch_queue_) {
        // Must copy the span data since it won't outlive this call
        std::vector<Terminal::Candle> batch_copy(batch.begin(), batch.end());
        dispatch_queue_->push({[key, bc = std::move(batch_copy)](StreamManager& sm) {
            sm.dispatch_candles_impl(key, std::span<const Terminal::Candle>(bc));
        }});
        return;
    }
    dispatch_candles_impl(key, batch);
}

void StreamManager::dispatch_candles_impl(const StreamKey& key, const std::span<const Terminal::Candle> batch) {
    if (const auto it = candle_batch_subs_.find(key); it != candle_batch_subs_.end()) {
        for (auto& [widget_ptr, callback] : it->second) {
            callback(widget_ptr, batch);
        }
    }
}

void StreamManager::dispatch_volume(const StreamKey& key, const Terminal::Volume& volume) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, volume](StreamManager& sm) {
            sm.dispatch_volume_impl(key, volume);
        }});
        return;
    }
    dispatch_volume_impl(key, volume);
}

void StreamManager::dispatch_volume_impl(const StreamKey& key, const Terminal::Volume& volume) {
    if (const auto it = volume_subs_.find(key); it != volume_subs_.end()) {
        for (auto&[widget_ptr, callback] : it->second) {
            callback(widget_ptr, volume);
        }
    }
}

void StreamManager::dispatch_stat(const StreamKey& key, const Terminal::Stat& stat) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, stat](StreamManager& sm) {
            sm.dispatch_stat_impl(key, stat);
        }});
        return;
    }
    dispatch_stat_impl(key, stat);
}

void StreamManager::dispatch_stat_impl(const StreamKey& key, const Terminal::Stat& stat) {
    if (const auto it = stats_subs_.find(key); it != stats_subs_.end()) {
        for (auto&[widget_ptr, callback] : it->second) {
            callback(widget_ptr, stat);
        }
    }
}

void StreamManager::dispatch_liquidation(const StreamKey& key, const Terminal::Liquidation& liq) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, liq](StreamManager& sm) {
            sm.dispatch_liquidation_impl(key, liq);
        }});
        return;
    }
    dispatch_liquidation_impl(key, liq);
}

void StreamManager::dispatch_liquidation_impl(const StreamKey& key, const Terminal::Liquidation& liq) {
    if (const auto it = liquidation_subs_.find(key); it != liquidation_subs_.end()) {
        for (auto&[widget_ptr, callback] : it->second) {
            callback(widget_ptr, liq);
        }
    }
}

void StreamManager::dispatch_pattern(const StreamKey& key, const Terminal::PatternOverlay& pattern) {
    if (dispatch_queue_) {
        dispatch_queue_->push({[key, pattern](StreamManager& sm) {
            sm.dispatch_pattern_impl(key, pattern);
        }});
        return;
    }
    dispatch_pattern_impl(key, pattern);
}

void StreamManager::dispatch_pattern_impl(
    const StreamKey& key,
    const Terminal::PatternOverlay& pattern) {
    if (const auto it = pattern_subs_.find(key); it != pattern_subs_.end()) {
        for (auto& [widget_ptr, callback] : it->second) {
            callback(widget_ptr, pattern);
        }
    }
}

void StreamManager::send_subscribe(const StreamKey& key) const {
    if (replay_mode_ || live_subscriptions_paused_) return;
    const auto msg = Terminal::create_subscribe_message(
        key.pair.exchange,
        key.pair.symbol,
        key.stream_type,
        key.timeframe
    );
    emscripten_websocket_send_utf8_text(ws_, msg.c_str());
}

void StreamManager::update_websocket_handle(EMSCRIPTEN_WEBSOCKET_T ws) {
    ws_ = ws;
    if (live_subscriptions_paused_) return;
    for (const auto& [key, handlers]: trade_subs_) {
        if (!handlers.empty()) {
            send_subscribe(key);
        }
    }
    for (const auto& [key, handlers] : orderbook_subs_) {
        if (!handlers.empty()) {
            send_subscribe(key);
        }
    }
    for (const auto& [key, handlers] : stats_subs_) {
        if (!handlers.empty()) {
            send_subscribe(key);
        }
    }
    for (const auto& [key, handlers] : liquidation_subs_) {
        if (!handlers.empty()) {
            send_subscribe(key);
        }
    }
    for (const auto& [key, handlers] : pattern_subs_) {
        if (!handlers.empty()) {
            send_subscribe(key);
        }
    }
}

void StreamManager::pause_live_subscriptions() {
    live_subscriptions_paused_ = true;
    int count = 0;
    auto unsub_map = [&](const auto& subs) {
        for (const auto& [key, handlers] : subs) {
            if (!handlers.empty()) {
                send_unsubscribe(key);
                count++;
            }
        }
    };
    unsub_map(trade_subs_);
    unsub_map(orderbook_subs_);
    unsub_map(candle_subs_);
    unsub_map(volume_subs_);
    unsub_map(stats_subs_);
    unsub_map(liquidation_subs_);
    unsub_map(pattern_subs_);
}

void StreamManager::resume_live_subscriptions() {
    live_subscriptions_paused_ = false;
    int count = 0;
    auto resub_map = [&](const auto& subs) {
        for (const auto& [key, handlers] : subs) {
            if (!handlers.empty()) {
                send_subscribe(key);
                count++;
            }
        }
    };
    resub_map(trade_subs_);
    resub_map(orderbook_subs_);
    resub_map(candle_subs_);
    resub_map(volume_subs_);
    resub_map(stats_subs_);
    resub_map(liquidation_subs_);
    resub_map(pattern_subs_);
}

void StreamManager::send_unsubscribe(const StreamKey& key) const {
    if (replay_mode_) return;  // Replay data comes from Replayer, not live NATS
    auto msg = Terminal::create_unsubscribe_message(
        key.pair.exchange,
        key.pair.symbol,
        key.stream_type,
        key.timeframe
    );
    emscripten_websocket_send_utf8_text(ws_, msg.c_str());
}

template void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Trade>>>&,
    StreamKey, void*);
template void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Orderbook>>>&,
    StreamKey, void*);
template void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Stat>>>&,
    StreamKey, void*);
template void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Candle>>>&,
    StreamKey, void*);
template void StreamManager::unsubscribe_impl(
    std::map<StreamKey, std::vector<StreamHandler<Terminal::PatternOverlay>>>&,
    StreamKey, void*);
