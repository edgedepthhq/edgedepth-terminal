#pragma once
#include "types/types.h"
#include <cmath>
#include <deque>

// Independent top-of-book observations. Never mutate sequenced depth with these.
class RealtimeQuotes {
public:
    // The as-of quote and, per side, the time its price has held since: the
    // first quote of the unbroken run at that price that ends at the as-of
    // quote. A ticker arrives on every top-of-book change, quantity included,
    // so the newest quote's own time is NOT when the price moved; a live step
    // drawn there slid right with every ticker and froze on screen.
    struct AsOf {
        Terminal::BookTicker quote{};
        int64_t bid_since_ms = 0, ask_since_ms = 0;
    };
    void append(const Terminal::BookTicker& q) {
        if (q.timestamp_ms <= 0 || !std::isfinite(q.best_bid) || !std::isfinite(q.best_ask) ||
            !(q.best_bid > 0 && q.best_ask > q.best_bid) ||
            !std::isfinite(q.best_bid_qty) || !std::isfinite(q.best_ask_qty) ||
            q.best_bid_qty < 0 || q.best_ask_qty < 0) return;
        if (!quotes_.empty() && q.timestamp_ms < quotes_.back().timestamp_ms) return;
        // Track the runs incrementally so the live lookup is O(1).
        if (quotes_.empty() || quotes_.back().best_bid != q.best_bid) bid_since_ = q.timestamp_ms;
        if (quotes_.empty() || quotes_.back().best_ask != q.best_ask) ask_since_ = q.timestamp_ms;
        quotes_.push_back(q);
        while (quotes_.size() > 8192 || quotes_.front().timestamp_ms <= q.timestamp_ms - 120000)
            quotes_.pop_front();
    }
    Terminal::BookTicker at(int64_t clock) const { return as_of(clock).quote; }
    AsOf as_of(int64_t clock) const {
        AsOf r;
        auto it = quotes_.rbegin();
        while (it != quotes_.rend() && it->timestamp_ms > clock) ++it;
        if (it == quotes_.rend() || clock - it->timestamp_ms > 15000) return r;
        r.quote = *it;
        if (it == quotes_.rbegin()) {
            r.bid_since_ms = bid_since_;
            r.ask_since_ms = ask_since_;
            return r;
        }
        // A lagging clock (replay) lands inside the deque: walk its run.
        r.bid_since_ms = r.ask_since_ms = it->timestamp_ms;
        bool bid = true, ask = true;
        for (auto j = std::next(it); j != quotes_.rend() && (bid || ask); ++j) {
            if (bid && j->best_bid == r.quote.best_bid) r.bid_since_ms = j->timestamp_ms; else bid = false;
            if (ask && j->best_ask == r.quote.best_ask) r.ask_since_ms = j->timestamp_ms; else ask = false;
        }
        return r;
    }
    void clear() { quotes_.clear(); bid_since_ = ask_since_ = 0; }
private:
    std::deque<Terminal::BookTicker> quotes_;
    int64_t bid_since_ = 0, ask_since_ = 0;
};
