#pragma once
#include "core/realtime_history.h"

class TradeAtPriceAccumulator;

// Chart-owned, main-thread frame. The DOM consumes the same immutable sampled
// book and absolute screen transform after the chart renders, never a live read.
struct RealtimeDOMFrame {
    RealtimeDepthHistory::SamplePtr book;
    const TradeAtPriceAccumulator* flow = nullptr;
    int frame = -1;
    int64_t clock_ms = 0;
    double price_min = 0, price_max = 0;
    float top = 0, bottom = 0;
    bool synchronized = false, paused = false, replay = false;

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
