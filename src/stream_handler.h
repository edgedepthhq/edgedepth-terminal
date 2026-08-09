#pragma once
#include <functional>
#include <map>
#include <emscripten/websocket.h>

#include "types/types.h"

template<typename T>
struct StreamHandler {
    void *widget_ptr;
    void (*callback)(void* ptr, const T& data);
};

template<typename T>
struct StreamBatchHandler {
    void *widget_ptr;
    void (*callback)(void* ptr, std::span<const T> batch);
};

struct StreamKey {
    Terminal::Pair pair;
    Terminal::Stream stream_type;
    int64_t timeframe;

    bool operator<(const StreamKey& other) const {
        if (pair.exchange != other.pair.exchange) return pair.exchange < other.pair.exchange;
        if (pair.symbol != other.pair.symbol) return pair.symbol < other.pair.symbol;
        if (stream_type != other.stream_type) return stream_type < other.stream_type;
        return timeframe < other.timeframe;
    }
};

class DispatchQueue;

class StreamManager {
public:
    StreamManager(EMSCRIPTEN_WEBSOCKET_T ws) : ws_(ws) {}

    void update_websocket_handle(EMSCRIPTEN_WEBSOCKET_T ws);

    // When true, send_subscribe/send_unsubscribe are no-ops.
    // Replay data arrives from the Replayer actor, not live NATS consumers.
    void set_replay_mode(bool v) { replay_mode_ = v; }

    // Pack replay (Hot Replay Path B): while a .edpack session is active, the
    // PackReplayEngine installs this hook and REPLAY-mode StreamManagers offer
    // it every outgoing JSON request (get_historical_candles etc.) BEFORE the
    // WebSocket. Returning true consumes the request — the engine serves it
    // from the pack's baked seeds, so a pack replay never touches the box.
    // Live-mode managers never consult it. One replay session at a time, so a
    // single static hook is sufficient.
    static void set_pack_request_hook(std::function<bool(const std::string&)> hook) {
        pack_request_hook_ = std::move(hook);
    }

    // Threading: when set, dispatch_* methods queue instead of calling
    // widget callbacks directly. Call drain_dispatches() on main thread.
    void set_dispatch_queue(DispatchQueue* q) { dispatch_queue_ = q; }

    void subscribe_trades(StreamKey key, StreamHandler<Terminal::Trade> handler);
    void subscribe_candles(const StreamKey& key, StreamHandler<Terminal::Candle> handler);
    void subscribe_candles_batch(const StreamKey& key, StreamBatchHandler<Terminal::Candle> handler);
    void subscribe_volume(const StreamKey& key, StreamHandler<Terminal::Volume> handler);
    void subscribe_stats(const StreamKey& key, StreamHandler<Terminal::Stat> handler);
    void subscribe_liquidations(const StreamKey& key, StreamHandler<Terminal::Liquidation> handler);
    void subscribe_patterns(const StreamKey& key, StreamHandler<Terminal::PatternOverlay> handler);
    void subscribe_orderbook(const StreamKey& key); // , StreamHandler<Terminal::Orderbook> handler

    void unsubscribe_trades(const StreamKey &key, void *widget_ptr);
    // Removes both single-candle and historical-batch callbacks for the key.
    // CandleManager registers both as one logical subscription.
    void unsubscribe_candles(const StreamKey &key, void *widget_ptr);
    void unsubscribe_volume(const StreamKey& key, void* widget_ptr);
    void unsubscribe_stats(const StreamKey &key, void *widget_ptr);
    void unsubscribe_liquidations(const StreamKey& key, void* widget_ptr);
    void unsubscribe_patterns(const StreamKey& key, void* widget_ptr);
    void unsubscribe_orderbook(const StreamKey &key, void *widget_ptr);


    void dispatch_trade(const StreamKey& key, const Terminal::Trade& trade);
    void dispatch_candle(const StreamKey& key, const Terminal::Candle& candle);
    void dispatch_candles(const StreamKey& key, std::span<const Terminal::Candle> batch);
    void dispatch_volume(const StreamKey& key, const Terminal::Volume& volume);
    void dispatch_stat(const StreamKey& key, const Terminal::Stat& stat);
    void dispatch_liquidation(const StreamKey& key, const Terminal::Liquidation& liq);
    void dispatch_pattern(const StreamKey& key, const Terminal::PatternOverlay& pattern);

    // Internal dispatch implementations (called directly or from queued lambdas)
    void dispatch_trade_impl(const StreamKey& key, const Terminal::Trade& trade);
    void dispatch_candle_impl(const StreamKey& key, const Terminal::Candle& candle);
    void dispatch_candles_impl(const StreamKey& key, std::span<const Terminal::Candle> batch);
    void dispatch_volume_impl(const StreamKey& key, const Terminal::Volume& volume);
    void dispatch_stat_impl(const StreamKey& key, const Terminal::Stat& stat);
    void dispatch_liquidation_impl(const StreamKey& key, const Terminal::Liquidation& liq);
    void dispatch_pattern_impl(const StreamKey& key, const Terminal::PatternOverlay& pattern);

    // Smart candle loading (for auto-loading on scroll)
    void request_historical_candles(
        const Terminal::Pair& pair,
        int64_t timeframe,
        size_t count,
        int64_t end_time_ms = 0  // 0 = now
    ) const;

    void request_historical_heatmap(
        const Terminal::Pair& pair,
        const std::string& mode,
        int64_t start_time_ms = 0,
        int64_t end_time_ms = 0.,
        int64_t timeframe = 60
    ) const;

    void request_historical_liq_heatmap(
        const Terminal::Pair& pair,
        int64_t start_time_ms,
        int64_t end_time_ms,
        int64_t timeframe
    ) const;

    void request_candles_before(const Terminal::Pair &pair, int64_t timeframe, int64_t before_timestamp_ms,
                                size_t count) const;

    // Scrub-preview candles (replay only): asks the box for the FULL replay
    // window at (near) this timeframe. The server resolves the range from its
    // own session state and answers on STREAM_REPLAY_PREVIEW_CANDLES; outside
    // a replay session it answers nothing. In pack mode the request is
    // swallowed by the pack hook (pack ghost support is a follow-up).
    void request_replay_preview_candles(const Terminal::Pair& pair, int64_t timeframe) const;

    void request_historical_oi(const Terminal::Pair& pair, int64_t timeframe, int count) const;
    void request_historical_funding(const Terminal::Pair& pair, int64_t timeframe, int count) const;
    // Indicators V1 (F3 absolute range): vpin_buckets history. start_ms/end_ms
    // are absolute unix ms (0 = server default; replay sessions clamp end to
    // the playback head server-side). No timeframe — bucket grain is canonical.
    void request_historical_vpin(const Terminal::Pair& pair, int64_t start_ms,
                                 int64_t end_ms, int count) const;

    void send_message(const std::string& message) const;
    void send_subscribe(const StreamKey& key) const;
    void send_unsubscribe(const StreamKey& key) const;

    // ─── Replay lifecycle ────────────────────────────────────────────────
    // Sends unsubscribe for ALL active server-side subscriptions.
    // Client-side callback maps are preserved so we can re-subscribe later.
    void pause_live_subscriptions();
    // Re-sends subscribe for every key that was active before pause.
    void resume_live_subscriptions();

private:
    EMSCRIPTEN_WEBSOCKET_T ws_;
    DispatchQueue* dispatch_queue_ = nullptr;
    bool replay_mode_ = false;  // When true, no server-side subscribe/unsubscribe
    bool live_subscriptions_paused_ = false;
    static std::function<bool(const std::string&)> pack_request_hook_;

    std::map<StreamKey, std::vector<StreamHandler<Terminal::Trade>>> trade_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Candle>>> candle_subs_;
    std::map<StreamKey, std::vector<StreamBatchHandler<Terminal::Candle>>> candle_batch_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Volume>>> volume_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Stat>>> stats_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Liquidation>>> liquidation_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::PatternOverlay>>> pattern_subs_;
    std::map<StreamKey, std::vector<StreamHandler<Terminal::Orderbook>>> orderbook_subs_;
    template<typename T>
    void unsubscribe_impl(std::map<StreamKey, std::vector<StreamHandler<T>>>& subs, StreamKey key, void* widget_ptr);
};
