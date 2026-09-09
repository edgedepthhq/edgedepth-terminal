#pragma once
#include "core/realtime_history.h"
#include <cmath>

class TradeAtPriceAccumulator;

// Chart-owned, main-thread frame. The DOM consumes the same immutable sampled
// book and absolute screen transform after the chart renders, never a live read.
struct RealtimeDOMFrame {
    RealtimeDepthHistory::SamplePtr book;
    Terminal::BookTicker quote{};
    const TradeAtPriceAccumulator* flow = nullptr;
    int frame = -1;
    int64_t clock_ms = 0;
    double price_min = 0, price_max = 0;
    float top = 0, bottom = 0;
    double native_tick = 0;
    int bucket_ticks = 1;
    bool automatic_grouping = false;

    double bucket_size() const { return native_tick * bucket_ticks; }
    int64_t bucket_index(double price) const {
        // Same native-tick boundary tolerance as RT heatmap column building.
        return int64_t(std::floor((price / native_tick + 1e-7) / bucket_ticks));
    }
    double bucket_center(int64_t index) const {
        return (double(index) + 0.5) * bucket_size();
    }
    bool synchronized = false, paused = false, replay = false;

    bool native_quote() const { return quote.timestamp_ms > 0; }
    double bid() const { return native_quote() ? quote.best_bid : book->bid; }
    double ask() const { return native_quote() ? quote.best_ask : book->ask; }

    bool projected(int current_frame) const {
        return frame == current_frame && bottom > top && price_max > price_min;
    }
    bool fresh() const {
        return synchronized && book && book->timestamp_ms <= clock_ms &&
            clock_ms - book->timestamp_ms <= 15000;
    }
    float price_y(double price) const {
        return float(bottom - (price - price_min) / (price_max - price_min) * (bottom - top));
    }
};
