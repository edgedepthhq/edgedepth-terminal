#pragma once

#include "types/types.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>

// Observations, not reconstructed pre-join history. The orderbook owner calls
// this under its write lock after applying a complete depth event. Tickers and
// trades never advance the depth clock. One actual observation per 100ms bin.
class RealtimeDepthHistory {
public:
    static constexpr int64_t interval_ms = 100;
    static constexpr int64_t retention_ms = 300000;
    static constexpr size_t max_levels_per_side = 512;
    static constexpr size_t max_samples = retention_ms / interval_ms;
    struct Sample {
        uint64_t serial = 0;
        bool segment_start = false;
        int64_t timestamp_ms = 0;
        double bid = 0, ask = 0;
        std::vector<Terminal::BookEntry> levels;
    };
    using SamplePtr = std::shared_ptr<const Sample>;

    void seed() { if (!valid_) segment_start_ = true; valid_ = true; first_delta_ = true; }
    void interrupt() { valid_ = false; }
    bool valid() const { return valid_; }
    void check_delta(int64_t prior, int64_t first, int64_t last, int64_t previous) {
        if (!valid_ || last <= prior) return;
        const bool bridge = first_delta_ && first > 0 && first <= prior + 1 && last >= prior;
        if (prior == 0 || (previous != prior && !bridge)) valid_ = false;
        first_delta_ = false;
    }
    void observe(const Terminal::Orderbook& book, int64_t timestamp_ms) {
        if (!valid_ || timestamp_ms <= 0 || !book.is_synchronized()) return;
        const double bid = book.bids.begin()->first, ask = book.asks.begin()->first;
        if (!(bid > 0 && ask > bid) || !std::isfinite(bid) || !std::isfinite(ask)) { valid_ = false; return; }
        if (!samples_.empty() && timestamp_ms / interval_ms <=
            samples_.back()->timestamp_ms / interval_ms) return;
        auto sample = std::make_shared<Sample>();
        sample->serial = ++serial_;
        sample->segment_start = segment_start_;
        segment_start_ = false;
        sample->timestamp_ms = timestamp_ms;
        sample->bid = bid; sample->ask = ask;
        sample->levels.reserve(max_levels_per_side * 2);
        for (const auto& [price, qty] : book.bids) {
            if (sample->levels.size() == max_levels_per_side) break;
            sample->levels.push_back({price, qty});
        }
        size_t count = 0;
        for (const auto& [price, qty] : book.asks) {
            if (count++ == max_levels_per_side) break;
            sample->levels.push_back({price, qty});
        }
        samples_.push_back(std::move(sample));
        while (samples_.size() > max_samples ||
            (!samples_.empty() && samples_.front()->timestamp_ms <= timestamp_ms - retention_ms))
            samples_.pop_front();
    }
    void copy_since(uint64_t serial, std::vector<SamplePtr>& out) const {
        out.clear();
        const auto first = std::upper_bound(samples_.begin(), samples_.end(), serial,
            [](uint64_t value, const SamplePtr& sample) { return value < sample->serial; });
        out.insert(out.end(), first, samples_.end());
    }
    void trim_after(int64_t cutoff) {
        while (!samples_.empty() && samples_.back()->timestamp_ms > cutoff) samples_.pop_back();
        interrupt(); // A restored seed must re-establish depth continuity.
    }
private:
    bool valid_ = false, first_delta_ = false, segment_start_ = true;
    uint64_t serial_ = 0;
    std::deque<SamplePtr> samples_;
};

// Preserve wire multiplicity: no identity exists in the trade protocol, so
// equal timestamp/price/quantity records must not be guessed to be duplicates.
// One append per CandleManager callback, regardless of the number of charts.
class RealtimeTradeHistory {
public:
    static constexpr size_t max_trades = 20000;
    void append(const Terminal::Trade& trade) {
        if (trade.timestamp_ms <= 0 || !(trade.price > 0) || !(trade.qty > 0) ||
            !std::isfinite(trade.price) || !std::isfinite(trade.qty)) return;
        auto it = std::upper_bound(trades_.begin(), trades_.end(), trade.timestamp_ms,
            [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
        trades_.insert(it, trade);
        const int64_t cutoff = trades_.back().timestamp_ms - RealtimeDepthHistory::retention_ms;
        while (trades_.size() > max_trades || trades_.front().timestamp_ms <= cutoff) trades_.pop_front();
    }
    void trim_after(int64_t cutoff) {
        while (!trades_.empty() && trades_.back().timestamp_ms > cutoff) trades_.pop_back();
    }
    void clear() { trades_.clear(); }
    const std::deque<Terminal::Trade>& trades() const { return trades_; }
private:
    std::deque<Terminal::Trade> trades_;
};

// A bounded, as-of market scale shared by live, paused and replay rendering.
// The viewport never enters these statistics. Warm-up can settle once a useful
// sample exists; thereafter explicit recalibration is required to resize history.
class RealtimeBubbleScale {
public:
    double minimum() const { return minimum_; }
    bool settled() const { return settled_; }
    void update(const std::deque<Terminal::Trade>& trades, int64_t clock_ms) {
        if (clock_ms < clock_ms_) { minimum_ = 0; settled_ = false; }
        if (settled_) { clock_ms_ = clock_ms; return; }
        if (minimum_ > 0 && clock_ms - clock_ms_ < 5000) return;
        std::vector<double> values;
        values.reserve(trades.size());
        for (const auto& trade : trades) {
            if (trade.timestamp_ms > clock_ms) break;
            if (trade.timestamp_ms <= clock_ms - 60000) continue;
            const double value = trade.price * trade.qty;
            if (value > 0 && std::isfinite(value)) values.push_back(value);
        }
        if (!values.empty()) {
            const size_t index = (values.size() - 1) * 3 / 4;
            std::nth_element(values.begin(), values.begin() + index, values.end());
            minimum_ = values[index];
            settled_ = values.size() >= 32;
        }
        clock_ms_ = clock_ms;
    }
private:
    double minimum_ = 0;
    int64_t clock_ms_ = 0;
    bool settled_ = false;
};
