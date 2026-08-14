#include "core/candle_manager.h"
#include "types/frame_profiler.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <ranges>

// Construction / Destruction

CandleManager::CandleManager(
    const Terminal::Pair& pair,
    int64_t timeframe_seconds,
    StreamManager& stream_mgr)
    : pair_(pair)
    , timeframe_seconds_(timeframe_seconds)
    , stream_mgr_(stream_mgr)
    , candle_stream_key_{pair, Terminal::Stream::Candles, timeframe_seconds}
    , trade_stream_key_{pair, Terminal::Stream::Trades, 0}
{

}

CandleManager::~CandleManager() {
    unsubscribe();
}

void CandleManager::subscribe() {
    StreamHandler<Terminal::Candle> live_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Candle& c) {
            static_cast<CandleManager*>(ptr)->handle_candle(c);
        }
    };
    StreamBatchHandler<Terminal::Candle> batch_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, std::span<const Terminal::Candle> batch) {
            static_cast<CandleManager*>(ptr)->handle_candle_batch(batch);
        }
    };
    StreamHandler<Terminal::Trade> trade_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Trade& t) {
            static_cast<CandleManager*>(ptr)->handle_trade(t);
        }
    };
    stream_mgr_.subscribe_trades(trade_stream_key_, trade_handler);
    stream_mgr_.subscribe_candles_batch(candle_stream_key_, batch_handler);
    stream_mgr_.subscribe_candles(candle_stream_key_, live_handler);
}

void CandleManager::unsubscribe() {
    stream_mgr_.unsubscribe_candles(candle_stream_key_, this);
    stream_mgr_.unsubscribe_trades(trade_stream_key_, this);
}


void CandleManager::initial_load() {
    if (initial_load_complete_ || is_loading_) return;
    // Subscribe to streams now that WebSocket is connected
    subscribe();
    is_loading_ = true;
    preload_count_ = INITIAL_PRELOAD_CANDLES;
    stream_mgr_.request_historical_candles(pair_, timeframe_seconds_, preload_count_);
}

void CandleManager::request_historical(size_t count, int64_t end_time_ms) {
    is_loading_ = true;
    if (end_time_ms > 0) {
        stream_mgr_.request_historical_candles(pair_, timeframe_seconds_, count, end_time_ms);
    } else {
        stream_mgr_.request_historical_candles(pair_, timeframe_seconds_, count);
    }
}

void CandleManager::update(double visible_x_min, double visible_x_max) {
    // Time-based candle finalization - live mode only.
    // During replay, candle period transitions are driven by trade/candle
    // handlers detecting timestamp changes. Wall clock is meaningless for
    // replay candles (always in the past → instant finalization every frame).
    if (has_current_candle_ && replay_start_time_ms_ == 0) {
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const int64_t candle_end_ms = current_candle_.timestamp_ms + (timeframe_seconds_ * 1000);
        if (now_ms >= candle_end_ms) {
            finalize_current_candle();
        }
    }
    // Auto-load more candles when scrolled near the left edge
    if (initial_load_complete_ && !candles_.empty() && visible_x_max > 0) {
        check_and_load_more(visible_x_min);
    }
}

void CandleManager::change_timeframe(const int64_t new_timeframe_seconds) {
    if (new_timeframe_seconds == timeframe_seconds_) return;
    // Unsubscribe old
    stream_mgr_.unsubscribe_candles(candle_stream_key_, this);
    stream_mgr_.unsubscribe_trades(trade_stream_key_, this);
    // Clear all state
    candles_.clear();
    has_current_candle_ = false;
    cache_dirty_ = true;
    cached_timestamps_.clear();
    cached_opens_.clear();
    cached_closes_.clear();
    cached_lows_.clear();
    cached_highs_.clear();
    cached_volumes_.clear();
    cached_vbuys_.clear();
    cached_vsells_.clear();
    oldest_candle_timestamp_ = 0;
    last_close_price_ = 0.0;
    follow_live_ = true;
    initial_load_complete_ = false;
    is_loading_ = false;
    // Update keys
    timeframe_seconds_ = new_timeframe_seconds;
    candle_stream_key_ = StreamKey{pair_, Terminal::Stream::Candles, timeframe_seconds_};
    // Re-subscribe
    StreamHandler<Terminal::Candle> live_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Candle& c) {
            static_cast<CandleManager*>(ptr)->handle_candle(c);
        }
    };
    StreamBatchHandler<Terminal::Candle> batch_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, std::span<const Terminal::Candle> batch) {
            static_cast<CandleManager*>(ptr)->handle_candle_batch(batch);
        }
    };
    StreamHandler<Terminal::Trade> trade_handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Trade& t) {
            static_cast<CandleManager*>(ptr)->handle_trade(t);
        }
    };
    stream_mgr_.subscribe_trades(trade_stream_key_, trade_handler);
    stream_mgr_.subscribe_candles_batch(candle_stream_key_, batch_handler);
    stream_mgr_.subscribe_candles(candle_stream_key_, live_handler);
    // Trigger reload - replay mode requests candles ending at replay start time
    is_loading_ = true;
    if (replay_start_time_ms_ > 0) {
        // Use the latest replay position, not the start time. Otherwise
        // candles built during the replay session are lost on TF switch.
        // Add one period buffer to cover alignment rounding from the old TF
        // (e.g., replay_latest_time_ms_ may be 5m-aligned while actual
        // playback is mid-period at the new TF).
        int64_t end_time = (replay_latest_time_ms_ > replay_start_time_ms_)
            ? replay_latest_time_ms_ + (new_timeframe_seconds * 1000)
            : replay_start_time_ms_;
        stream_mgr_.request_candles_before(pair_, timeframe_seconds_, end_time, preload_count_);
    } else {
        stream_mgr_.request_historical_candles(pair_, timeframe_seconds_, preload_count_);
    }
}

void CandleManager::reset_for_seek(int64_t seek_time_ms) {
    // Clear all candle data
    candles_.clear();
    has_current_candle_ = false;
    // Drop the recent-tick ring buffer: a seek jumps to a new time, so the old
    // ticks are stale for the Line chart (candle-close fallback covers the gap
    // until fresh replay trades refill it).
    clear_ticks();
    cache_dirty_ = true;
    cached_timestamps_.clear();
    cached_opens_.clear();
    cached_closes_.clear();
    cached_lows_.clear();
    cached_highs_.clear();
    cached_volumes_.clear();
    cached_vbuys_.clear();
    cached_vsells_.clear();
    oldest_candle_timestamp_ = 0;
    last_close_price_ = 0.0;
    follow_live_ = true;
    replay_latest_time_ms_ = 0;

    // Update replay time to the seek position
    replay_start_time_ms_ = seek_time_ms;

    // Keep initial_load_complete_ = true so incoming replay data isn't dropped
    // while we wait for the historical batch. The batch will just prepend when
    // it arrives (handle_candle_batch skips the pop-as-building-candle logic
    // when initial_load_complete_ is already true).
    is_loading_ = true;

    stream_mgr_.request_candles_before(pair_, timeframe_seconds_, seek_time_ms, preload_count_);
}

void CandleManager::trim_candles_after(int64_t cutoff_ms) {
    // Remove the building candle if it's past the cutoff
    if (has_current_candle_ && current_candle_.timestamp_ms > cutoff_ms) {
        has_current_candle_ = false;
    }

    // Remove finalized candles past the cutoff (they're sorted by time)
    while (!candles_.empty() && candles_.back().timestamp_ms > cutoff_ms) {
        candles_.pop_back();
    }

    // If the building candle survived (its bucket starts at or before the
    // cutoff), truncate its OHLC. The candle may contain price data from
    // AFTER the cutoff time (e.g., rewinding from 10:49:45 to 10:49:00 -
    // the 10:49 candle's wick includes the spike at :45s). Reset to open
    // so incoming replay data rebuilds the correct OHLC.
    if (has_current_candle_ && current_candle_.timestamp_ms <= cutoff_ms) {
        current_candle_.close = current_candle_.open;
        current_candle_.high = current_candle_.open;
        current_candle_.low = current_candle_.open;
        current_candle_.volume = 0.0;
        current_candle_.vbuy = 0.0;
        current_candle_.vsell = 0.0;
        current_candle_.tbuy = 0;
        current_candle_.tsell = 0;
    }

    // If the building candle was removed, promote the last finalized candle
    if (!has_current_candle_ && !candles_.empty()) {
        current_candle_ = candles_.back();
        candles_.pop_back();
        has_current_candle_ = true;

        // Same OHLC reset - this candle's bucket is at or before the cutoff
        // but it was finalized with a full minute of data.
        current_candle_.close = current_candle_.open;
        current_candle_.high = current_candle_.open;
        current_candle_.low = current_candle_.open;
        current_candle_.volume = 0.0;
        current_candle_.vbuy = 0.0;
        current_candle_.vsell = 0.0;
        current_candle_.tbuy = 0;
        current_candle_.tsell = 0;
    }

    // Update tracking
    if (!candles_.empty()) {
        last_close_price_ = candles_.back().close;
    }
    replay_latest_time_ms_ = cutoff_ms;
    cache_dirty_ = true;
    follow_live_ = true;
}

size_t CandleManager::visible_candles_for_timeframe() const {
    if (timeframe_seconds_ >= 86400) return 60;   // 1d: 2 months
    if (timeframe_seconds_ >= 14400) return 84;   // 4h: 2 weeks
    if (timeframe_seconds_ >= 3600)  return 72;   // 1h: 3 days
    if (timeframe_seconds_ >= 900)   return 96;   // 15m: 1 day
    if (timeframe_seconds_ >= 300)   return 144;  // 5m: 12 hours
    return 180;                                     // 1m: 3 hours
}

// Trade → Candle Building

void CandleManager::handle_trade(const Terminal::Trade& trade) {
    if (!initial_load_complete_) return;  // Don't build candles until batch arrives
    // After a seek, suppress trade-based candle building until the historical
    // batch arrives. Without this, trades create a building candle at the seek
    // target before the batch, then the batch collides with it - producing
    // doubled candles with wrong open/high/low.
    if (is_loading_) return;
    // Track latest replay time for timeframe-change batch requests
    if (replay_start_time_ms_ > 0 && trade.timestamp_ms > replay_latest_time_ms_) {
        replay_latest_time_ms_ = trade.timestamp_ms;
    }
    static int replay_trade_count = 0;
    if (replay_start_time_ms_ > 0 && ++replay_trade_count <= 10) {
    }
    // Feed the recent-tick ring buffer BEFORE building the candle so the Line
    // chart moves tick-by-tick (live and replay). Timeframe-independent.
    append_tick(trade.timestamp_ms, trade.price);
    build_candle_from_trade(trade);
}

// Recent-tick ring buffer (Line chart). Appends one raw trade point and, once a
// slack accumulates, drops the stale front in ONE batched erase so the hot path
// stays push_back (amortized O(1), reused capacity) with no per-trade shift.
// Bounded by BOTH count (MAX_TICKS) and age (MAX_TICK_AGE_MS from the newest).
void CandleManager::append_tick(int64_t time_ms, double price) {
    tick_times_.push_back(time_ms);
    tick_prices_.push_back(price);

    const size_t n = tick_times_.size();
    // Count how many front entries are droppable: over the count cap, or older
    // than the age window relative to the newest tick.
    const int64_t newest = time_ms;
    const int64_t cutoff = newest - MAX_TICK_AGE_MS;
    size_t drop = 0;
    if (n > MAX_TICKS) drop = n - MAX_TICKS;             // count cap
    while (drop < n && tick_times_[drop] < cutoff) drop++; // age cap (front is oldest)

    // Only compact once a slack builds up, so erase runs at most ~1/512 trades.
    if (drop >= kTickTrimSlack) {
        tick_times_.erase(tick_times_.begin(),
                          tick_times_.begin() + static_cast<std::ptrdiff_t>(drop));
        tick_prices_.erase(tick_prices_.begin(),
                           tick_prices_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

void CandleManager::clear_ticks() {
    // Keep capacity so a symbol/seek reset does not re-thrash the allocator.
    tick_times_.clear();
    tick_prices_.clear();
}

void CandleManager::build_candle_from_trade(const Terminal::Trade& trade) {
    const int64_t candle_ts = get_candle_timestamp(trade.timestamp_ms);

    // During replay, reject trades for periods BEFORE the building candle.
    // Out-of-order replay data would create candles behind the playback head,
    // causing the building candle to appear as second-to-last.
    if (replay_start_time_ms_ > 0 && has_current_candle_ &&
        candle_ts < current_candle_.timestamp_ms) {
        // Try to update an existing finalized candle instead
        for (auto it = candles_.rbegin(); it != candles_.rend(); ++it) {
            if (it->timestamp_ms == candle_ts) {
                it->high = std::max(it->high, trade.price);
                it->low = std::min(it->low, trade.price);
                it->close = trade.price;
                it->volume += trade.qty;
                if (trade.is_buy) { it->vbuy += trade.qty; it->tbuy++; }
                else { it->vsell += trade.qty; it->tsell++; }
                mark_dirty();
                return;
            }
        }
        return;  // No matching candle - drop the out-of-order trade
    }

    // Finalize previous candle if we've moved to a new period
    if (has_current_candle_ && current_candle_.timestamp_ms != candle_ts) {
        finalize_current_candle();
    }
    if (!has_current_candle_ || current_candle_.timestamp_ms != candle_ts) {
        // If this period was already finalized by the server, update the
        // finalized candle in-place instead of creating a duplicate building
        // candle. Prevents two candles rendering at the same x position.
        if (!candles_.empty() && candles_.back().timestamp_ms == candle_ts) {
            auto& back = candles_.back();
            back.high = std::max(back.high, trade.price);
            back.low = std::min(back.low, trade.price);
            back.close = trade.price;
            back.volume += trade.qty;
            if (trade.is_buy) { back.vbuy += trade.qty; back.tbuy++; }
            else { back.vsell += trade.qty; back.tsell++; }
            mark_dirty();
            return;
        }
        // Determine open price: continuity from last finalized candle
        double open_price;
        if (!candles_.empty()) {
            open_price = candles_.back().close;
        } else if (has_current_candle_) {
            open_price = current_candle_.close;
        } else {
            open_price = trade.price;
        }
        current_candle_ = Terminal::Candle{
            .timeframe = timeframe_seconds_,
            .open = open_price,
            .high = std::max(open_price, trade.price),
            .low = std::min(open_price, trade.price),
            .close = trade.price,
            .volume = trade.qty,
            .vbuy = trade.is_buy ? trade.qty : 0.0,
            .vsell = trade.is_buy ? 0.0 : trade.qty,
            .tbuy = trade.is_buy ? 1 : 0,
            .tsell = trade.is_buy ? 0 : 1,
            .timestamp_ms = candle_ts,
            .final = false
        };
        has_current_candle_ = true;
    } else {
        // Update existing building candle
        current_candle_.high = std::max(current_candle_.high, trade.price);
        current_candle_.low = std::min(current_candle_.low, trade.price);
        current_candle_.close = trade.price;
        current_candle_.volume += trade.qty;
        if (trade.is_buy) {
            current_candle_.vbuy += trade.qty;
            current_candle_.tbuy++;
        } else {
            current_candle_.vsell += trade.qty;
            current_candle_.tsell++;
        }
    }
}

void CandleManager::finalize_current_candle() {
    if (!has_current_candle_) return;

    // Enforce OHLC invariants before finalizing
    current_candle_.high = std::max({current_candle_.open, current_candle_.close, current_candle_.high});
    current_candle_.low = std::min({current_candle_.open, current_candle_.close, current_candle_.low});

    // Skip if server already finalized this timestamp
    if (!candles_.empty() && candles_.back().timestamp_ms == current_candle_.timestamp_ms) {
        if (replay_start_time_ms_ > 0) {
            // During replay, REPLACE the existing candle with the building one
            // (the building candle has been updated by trades and 15s ticks)
            candles_.back() = current_candle_;
            mark_dirty();
        }
        has_current_candle_ = false;
        return;
    }
    // Never break the sorted-timestamp invariant. candles_ is binary searched
    // by timestamp by the chart, the candle-derived liquidation field and the
    // scrub preview, and one out-of-order entry at the back drags x_max
    // backwards and renders an empty plot. The equal case returned above, so
    // reaching here with a building candle no newer than the back means
    // something upstream sent a candle that should never have been accepted.
    // Drop it rather than corrupt the series.
    if (!candles_.empty() && current_candle_.timestamp_ms <= candles_.back().timestamp_ms) {
        has_current_candle_ = false;
        return;
    }
    candles_.push_back(current_candle_);
    has_current_candle_ = false;
    mark_dirty();
    // Trim if over limit
    if (candles_.size() > MAX_CANDLES * 1.1) {
        const size_t to_remove = candles_.size() - MAX_CANDLES;
        candles_.erase(candles_.begin(), candles_.begin() + static_cast<std::ptrdiff_t>(to_remove));
        mark_dirty();
    }
    if (!candles_.empty()) {
        oldest_candle_timestamp_ = candles_.front().timestamp_ms;
    }
}

int64_t CandleManager::get_candle_timestamp(int64_t trade_timestamp_ms) const {
    const int64_t seconds = trade_timestamp_ms / 1000;
    const int64_t candle_seconds = (seconds / timeframe_seconds_) * timeframe_seconds_;
    return candle_seconds * 1000;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Server Candle Updates
// ═══════════════════════════════════════════════════════════════════════════════

void CandleManager::handle_candle(const Terminal::Candle& candle) {
    // A candle with no timestamp, or with no price at all, is not a candle.
    // This is the exact shape a wire mismatch produces: proto3 skips fields it
    // cannot match as unknown, the parse still succeeds, and every field comes
    // back at its default. Such a candle aligns to bucket 0, becomes the
    // building candle, gets finalized at the BACK of the series, and takes the
    // chart's x_max to 0 against a real epoch x_min. The plot then renders
    // empty and the OHLC readout prints zeros, and nothing recovers it except a
    // timeframe change, because later zero frames are absorbed by the
    // back-merge guard below. Refusing it here is cheap and local.
    if (candle.timestamp_ms <= 0) {
        return;
    }
    if (candle.open == 0.0 && candle.high == 0.0 &&
        candle.low == 0.0 && candle.close == 0.0) {
        return;
    }

    if (!initial_load_complete_) {
        return;
    }

    // After a seek, suppress streaming candle messages until the historical
    // batch arrives. Without this, streaming ticks from the new consumer
    // create a building candle before the batch, then the batch arrives and
    // collides - producing doubled candles and mixed green/red bodies.
    if (is_loading_) {
        return;
    }

    // During drip-feed after rewind, suppress server candle messages.
    // They carry accumulated OHLCV from the original 15s tick period which
    // includes price data from AFTER the rewind target. Trades alone are
    // sufficient to build candles incrementally.
    if (suppress_candle_messages_) {
        return;
    }

    // Align candle timestamp to our timeframe boundary.
    // Replay sends raw 15s ticks with 15s-aligned timestamps but timeframe=300
    // in the WSPayload envelope. Snapping to the 5m boundary ensures all ticks
    // within one period merge into a single candle instead of creating duplicates.
    Terminal::Candle aligned = candle;
    const int64_t tf_ms = timeframe_seconds_ * 1000;
    if (tf_ms > 0) {
        aligned.timestamp_ms = (candle.timestamp_ms / tf_ms) * tf_ms;
    }

    // During replay, raw 15s candle ticks are ALL marked final=true (NATS stores
    // the final state of each 15s tick). But we're aggregating into 5m candles,
    // so treat them as partial updates. The candle only truly finalizes when the
    // next 5m period begins (timestamp rolls). This keeps the building candle
    // alive between 15s ticks so trades can update it tick-by-tick.
    if (replay_start_time_ms_ > 0) {
        aligned.final = false;
        // Track latest replay time for timeframe-change batch requests
        if (aligned.timestamp_ms > replay_latest_time_ms_) {
            replay_latest_time_ms_ = aligned.timestamp_ms;
        }
    }

    static int candle_count = 0;
    if (++candle_count <= 5) {
    }

    // During replay, reject ticks for periods BEFORE the building candle.
    // Out-of-order data would displace the building candle from rightmost position.
    if (replay_start_time_ms_ > 0 && has_current_candle_ &&
        aligned.timestamp_ms < current_candle_.timestamp_ms) {
        return;  // Drop out-of-order replay tick
    }

    if (!aligned.final) {
        // LIVE coexistence guard (2026-06-28, §3C - higher-TF price-follow bug):
        // If this period is ALREADY a finalized candle (the wall-clock finalizer in update()
        // closed it at the TF boundary, or a server final landed) and it is NOT the current
        // building candle, do NOT spawn a building candle at that same timestamp. A
        // finalized+building pair at one ts makes plot_candles skip the building candle
        // (already_in_finalized), so the STALE finalized candle renders while the live price
        // tag (= building close) floats above it - the "5m/15m building candle goes invisible /
        // close not pinned / price mid-body" bug. 1m hid it because next-period trades recreate
        // the building candle almost instantly; on 5m/15m the client boundary races the server's
        // late partials and next-period trades are sparser, so it lingers. build_candle_from_trade
        // already guards this for trades; mirror it here for server partials. Merge H/L/volume
        // into the finalized candle in place (trades keep its close tracking live) and return.
        if (replay_start_time_ms_ == 0 && !candles_.empty() &&
            candles_.back().timestamp_ms == aligned.timestamp_ms &&
            !(has_current_candle_ && current_candle_.timestamp_ms == aligned.timestamp_ms)) {
            auto& back = candles_.back();
            back.high   = std::max(back.high, aligned.high);
            back.low    = std::min(back.low,  aligned.low);
            // Keep the finalized close (trade tape / server-final authoritative); a late
            // partial's aggregated close lags the tape. Enrich H/L/volume only.
            back.volume = std::max(back.volume, aligned.volume);
            back.vbuy   = std::max(back.vbuy,   aligned.vbuy);
            back.vsell  = std::max(back.vsell,  aligned.vsell);
            back.tbuy   = std::max(back.tbuy,   aligned.tbuy);
            back.tsell  = std::max(back.tsell,  aligned.tsell);
            back.high   = std::max({back.open, back.close, back.high});
            back.low    = std::min({back.open, back.close, back.low});
            mark_dirty();
            return;
        }
        // Partial update - merge with building candle
        if (has_current_candle_ && current_candle_.timestamp_ms == aligned.timestamp_ms) {
            // During replay, keep the continuity-fixed open from the first tick.
            // In live mode, server is authoritative for open price.
            if (replay_start_time_ms_ == 0) {
                current_candle_.open = aligned.open;
            }
            current_candle_.high = std::max(current_candle_.high, aligned.high);
            current_candle_.low = std::min(current_candle_.low, aligned.low);
            // Close: in LIVE mode the trade tape is the real-time price. The server's
            // partial candle for an aggregated TF (5m/15m/1h/…) carries a close built
            // from completed sub-candles, so it LAGS the tape - adopting it here
            // stalled the building candle below price on every TF above 1m (1m looked
            // fine only because its server close already equals the latest trade).
            // Let trades own the live close (build_candle_from_trade); take the server
            // close only during replay, where there's no live tape to drive it.
            if (replay_start_time_ms_ > 0) {
                current_candle_.close = aligned.close;
            }
            current_candle_.volume = aligned.volume;
            current_candle_.vbuy = aligned.vbuy;
            current_candle_.vsell = aligned.vsell;
            current_candle_.tbuy = aligned.tbuy;
            current_candle_.tsell = aligned.tsell;
            // Enforce OHLC invariants - prevents mixed red/green rendering
            // when continuity-open diverges from tick data
            current_candle_.high = std::max({current_candle_.open, current_candle_.close, current_candle_.high});
            current_candle_.low = std::min({current_candle_.open, current_candle_.close, current_candle_.low});
        } else {
            // Different period than the building candle. (BEHIND ticks were already
            // dropped above, so with a building candle present this tick is AHEAD.)
            //
            // During REPLAY, do NOT advance the building candle on a server tick
            // alone. The 15s candle-tick stream and the trade stream are delivered
            // independently and the tick stream often races a beat ahead, so a tick
            // for period T+1 can arrive before T's last trades. Advancing here would
            // finalize the still-active T and make T+1 the building candle - then T's
            // trailing trades land via the out-of-order path and keep updating T,
            // which now renders 2nd-from-last, while the premature T+1 sits (near-)
            // empty at the rightmost. That's the "1m candle updates the 2nd-from-last
            // until the minute closes" bug. Let TRADES drive period advancement (they
            // do, in build_candle_from_trade); drop the ahead-tick. A matching trade
            // will create the new period and a same-period tick then enriches it.
            // (Replay targets are dense-trade symbols, so every period gets trades.)
            if (replay_start_time_ms_ > 0 && has_current_candle_) {
                return;
            }
            // No building candle yet (or live mode): the tick establishes/advances it.
            if (has_current_candle_ && current_candle_.timestamp_ms != aligned.timestamp_ms) {
                finalize_current_candle();
            }
            current_candle_ = aligned;
            has_current_candle_ = true;
            // During REPLAY only: fix open for continuity with previous candle's close.
            // In live mode, server's open is authoritative - don't override.
            if (replay_start_time_ms_ > 0 && !candles_.empty()) {
                current_candle_.open = candles_.back().close;
                current_candle_.high = std::max(current_candle_.high, current_candle_.open);
                current_candle_.low = std::min(current_candle_.low, current_candle_.open);
            }
        }
        return;
    }

    // Final candle from server
    // Clear live candle if it matches
    if (has_current_candle_ && current_candle_.timestamp_ms == aligned.timestamp_ms) {
        has_current_candle_ = false;
    }

    // Find and replace existing candle with this timestamp (may not be at back
    // if trade-finalization pushed a newer candle before server's final arrived)
    bool replaced = false;
    if (!candles_.empty()) {
        // Check back first (common case)
        if (candles_.back().timestamp_ms == aligned.timestamp_ms) {
            candles_.back() = aligned;
            replaced = true;
        } else if (candles_.size() >= 2) {
            // Check second-to-last (race: trade for T+1 finalized before server sent final for T)
            auto& second_last = candles_[candles_.size() - 2];
            if (second_last.timestamp_ms == aligned.timestamp_ms) {
                second_last = aligned;
                replaced = true;
            }
        }
    }
    if (!replaced) {
        candles_.push_back(aligned);
    }

    last_close_price_ = aligned.close;

    // Fix next building candle's open for continuity
    if (has_current_candle_ && !candles_.empty()) {
        const int64_t expected_next_ts = aligned.timestamp_ms + (timeframe_seconds_ * 1000);
        if (current_candle_.timestamp_ms == expected_next_ts) {
            current_candle_.open = aligned.close;
            current_candle_.high = std::max(current_candle_.high, current_candle_.open);
            current_candle_.low = std::min(current_candle_.low, current_candle_.open);
        }
    }

    mark_dirty();

    // Cleanup
    if (candles_.size() > MAX_CANDLES * 1.1) {
        const size_t to_remove = candles_.size() - MAX_CANDLES;
        candles_.erase(candles_.begin(), candles_.begin() + static_cast<std::ptrdiff_t>(to_remove));
        mark_dirty();
    }
    if (!candles_.empty()) {
        oldest_candle_timestamp_ = candles_.front().timestamp_ms;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Historical Batch
// ═══════════════════════════════════════════════════════════════════════════════

void CandleManager::handle_candle_batch(std::span<const Terminal::Candle> batch) {
    if (batch.empty()) {
        is_loading_ = false;
        return;
    }
    // Backend sends oldest → newest. Historical requests can race a TF
    // switch or overlap a previous scroll request, so prepend only timestamps
    // strictly older than the current front. Blindly prepending a repeated
    // batch creates [old..new, old..new], violating the sorted-timestamp
    // invariant used by chart binary searches and the candle-derived liq field.
    const bool had_candles = !candles_.empty();
    const int64_t prepend_before = had_candles
        ? candles_.front().timestamp_ms
        : std::numeric_limits<int64_t>::max();
    int64_t last_inserted_ts = std::numeric_limits<int64_t>::max();
    size_t inserted = 0;
    for (const auto& it : std::ranges::reverse_view(batch)) {
        if (it.timestamp_ms >= prepend_before || it.timestamp_ms == last_inserted_ts) continue;
        candles_.push_front(it);
        last_inserted_ts = it.timestamp_ms;
        ++inserted;
    }
    if (inserted == 0) {
        is_loading_ = false;
        return;
    }
    // Trim excess from FRONT (oldest prepended data), not back (newest live data).
    // The old pop_back() approach was wrong - it removed the most recent candles
    // when historical data was loaded, causing "recent data gets cut" on zoom-out.
    while (candles_.size() > MAX_CANDLES) {
        candles_.pop_front();
    }
    oldest_candle_timestamp_ = candles_.front().timestamp_ms;
    if (!had_candles) {
        last_close_price_ = batch.back().close;
    }
    if (!initial_load_complete_) {
        initial_load_complete_ = true;

        // The last candle in the batch may be the current (incomplete) period.
        // Pop it out and make it the building candle so trades/ticks merge into
        // it instead of creating a duplicate.
        //
        // REPLAY MODE: Always pop unconditionally. The batch loads candles up to
        // the current replay position; 15s ticks will arrive for that period or
        // later. If we leave the last batch candle finalized, the first 15s tick
        // creates a building candle with open = candles_.back().close (the SAME
        // period's historical close, not the PREVIOUS period's close) - wrong.
        //
        // LIVE MODE: Only pop if wall clock is still within the candle's period
        // (the candle hasn't finished yet).
        if (!candles_.empty()) {
            bool should_pop = false;
            if (replay_start_time_ms_ > 0) {
                // Replay: always pop the last batch candle
                should_pop = true;
            } else {
                // Live: check if the period is still open
                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                const int64_t candle_end_ms = candles_.back().timestamp_ms + (timeframe_seconds_ * 1000);
                should_pop = (now_ms < candle_end_ms);
            }
            if (should_pop) {
                const auto& last = candles_.back();
                if (has_current_candle_ &&
                    current_candle_.timestamp_ms == last.timestamp_ms) {
                    current_candle_.open   = last.open;
                    current_candle_.high   = std::max(current_candle_.high, last.high);
                    current_candle_.low    = std::min(current_candle_.low, last.low);
                    current_candle_.volume = std::max(current_candle_.volume, last.volume);
                    current_candle_.vbuy   = std::max(current_candle_.vbuy, last.vbuy);
                    current_candle_.vsell  = std::max(current_candle_.vsell, last.vsell);
                } else {
                    current_candle_ = last;
                    has_current_candle_ = true;
                }
                // During replay, the batch candle has FULL historical H/L/close
                // for the entire period - data that hasn't been replayed yet.
                // Reset to just the open so ticks progressively fill in H/L/close.
                // Without this, the building candle renders with the full period's
                // range immediately (giant candle covering the entire 5m/1m move).
                if (replay_start_time_ms_ > 0) {
                    current_candle_.high   = current_candle_.open;
                    current_candle_.low    = current_candle_.open;
                    current_candle_.close  = current_candle_.open;
                    current_candle_.volume = 0;
                    current_candle_.vbuy   = 0;
                    current_candle_.vsell  = 0;
                    current_candle_.tbuy   = 0;
                    current_candle_.tsell  = 0;
                }
                candles_.pop_back();
                mark_dirty();
            }
        }
    }

    is_loading_ = false;
    mark_dirty();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Auto-Load (Scroll)
// ═══════════════════════════════════════════════════════════════════════════════

bool CandleManager::should_load_more(double visible_x_min) const {
    if (is_loading_ || candles_.empty()) return false;
    if (visible_x_min <= 0) return false;
    const double oldest_time = static_cast<double>(oldest_candle_timestamp_);
    const double distance = visible_x_min - oldest_time;
    // Load if we're within 30% of visible range from the oldest candle, or past it
    // Use a fixed trigger based on timeframe
    const double trigger = static_cast<double>(timeframe_seconds_) * 1000.0 *
                           static_cast<double>(visible_candles_for_timeframe()) * 0.3;

    return (distance >= 0 && distance < trigger) || (distance < 0);
}

void CandleManager::check_and_load_more(double visible_x_min) {
    if (!initial_load_complete_ || is_loading_ || candles_.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_load_time_).count();
    if (elapsed < LOAD_COOLDOWN_MS) return;

    if (should_load_more(visible_x_min)) {
        is_loading_ = true;
        last_load_time_ = now;
        stream_mgr_.request_historical_candles(
            pair_, timeframe_seconds_,
            scroll_load_count_,
            oldest_candle_timestamp_ - 1
        );
    }
}

// SoA Cache Rebuild

void CandleManager::rebuild_cache() const {
    // Runs at most once/frame (lazy, via ensure_cache) but copies the WHOLE
    // deque - cost scales with loaded candles. Section shows up in the perf
    // overlay when replay 15s-tick merges keep dirtying the cache.
    ProfileScope _ps("SoARebuild");
    const size_t n = candles_.size();

    cached_timestamps_.clear();
    cached_opens_.clear();
    cached_highs_.clear();
    cached_lows_.clear();
    cached_closes_.clear();
    cached_volumes_.clear();
    cached_vbuys_.clear();
    cached_vsells_.clear();

    cached_timestamps_.reserve(n);
    cached_opens_.reserve(n);
    cached_highs_.reserve(n);
    cached_lows_.reserve(n);
    cached_closes_.reserve(n);
    cached_volumes_.reserve(n);
    cached_vbuys_.reserve(n);
    cached_vsells_.reserve(n);

    for (const auto& c : candles_) {
        cached_timestamps_.push_back(static_cast<double>(c.timestamp_ms));
        cached_opens_.push_back(c.open);
        cached_highs_.push_back(c.high);
        cached_lows_.push_back(c.low);
        cached_closes_.push_back(c.close);
        cached_volumes_.push_back(c.volume);
        cached_vbuys_.push_back(c.vbuy);
        cached_vsells_.push_back(c.vsell);
    }

    // ── Heikin Ashi derived SoA (recursive over the real OHLC) ──────────────
    // HA_close = (O+H+L+C)/4; HA_open seeds at (O0+C0)/2 then rolls forward as
    // (HA_open[i-1]+HA_close[i-1])/2; HA_high/low fold in the HA body. Path-
    // dependent from the oldest loaded candle, so a scroll-load prepend repaints
    // the left edge slightly (matches every platform) - automatic via this
    // full rebuild under cache_dirty_.
    cached_ha_opens_.clear();
    cached_ha_highs_.clear();
    cached_ha_lows_.clear();
    cached_ha_closes_.clear();
    cached_ha_opens_.reserve(n);
    cached_ha_highs_.reserve(n);
    cached_ha_lows_.reserve(n);
    cached_ha_closes_.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const double o = cached_opens_[i];
        const double h = cached_highs_[i];
        const double l = cached_lows_[i];
        const double c = cached_closes_[i];
        const double ha_close = (o + h + l + c) * 0.25;
        const double ha_open  = (i == 0)
            ? (o + c) * 0.5
            : (cached_ha_opens_[i - 1] + cached_ha_closes_[i - 1]) * 0.5;
        const double ha_high = std::max({h, ha_open, ha_close});
        const double ha_low  = std::min({l, ha_open, ha_close});
        cached_ha_opens_.push_back(ha_open);
        cached_ha_highs_.push_back(ha_high);
        cached_ha_lows_.push_back(ha_low);
        cached_ha_closes_.push_back(ha_close);
    }

    cache_dirty_ = false;
}
