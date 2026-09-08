#pragma once
#include "types/types.h"
#include <cmath>
#include <deque>

// Independent top-of-book observations. Never mutate sequenced depth with these.
class RealtimeQuotes {
public:
    void append(const Terminal::BookTicker& q) {
        if (q.timestamp_ms <= 0 || !std::isfinite(q.best_bid) || !std::isfinite(q.best_ask) ||
            !(q.best_bid > 0 && q.best_ask > q.best_bid) ||
            !std::isfinite(q.best_bid_qty) || !std::isfinite(q.best_ask_qty) ||
            q.best_bid_qty < 0 || q.best_ask_qty < 0) return;
        if (!quotes_.empty() && q.timestamp_ms < quotes_.back().timestamp_ms) return;
        quotes_.push_back(q);
        while (quotes_.size() > 8192 || quotes_.front().timestamp_ms <= q.timestamp_ms - 120000)
            quotes_.pop_front();
    }
    Terminal::BookTicker at(int64_t clock) const {
        for (auto it = quotes_.rbegin(); it != quotes_.rend(); ++it)
            if (it->timestamp_ms <= clock)
                return clock - it->timestamp_ms <= 15000 ? *it : Terminal::BookTicker{};
        return {};
    }
    void clear() { quotes_.clear(); }
private:
    std::deque<Terminal::BookTicker> quotes_;
};
