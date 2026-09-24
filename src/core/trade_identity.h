#pragma once
// Bounded recent-trade identity for at-least-once live feeds.
//
// Native string identities (Bybit execution IDs) can be re-delivered by the
// publisher after a crash or a broker restart, beyond any broker duplicate
// window. Each market keeps the most recent identities it has admitted and
// refuses a repeat. Binance aggregate IDs are handled by the archive join and
// are not routed through here; a trade without a native ID is always admitted.
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_set>

class RecentTradeIdentities {
public:
    explicit RecentTradeIdentities(size_t capacity = 65536) : capacity_(capacity) {}

    // True when the identity is new (admit it); false when it was seen recently.
    bool admit(const std::string& native_id) {
        if (native_id.empty()) return true;
        if (!seen_.insert(native_id).second) { ++rejected_; return false; }
        order_.push_back(native_id);
        while (order_.size() > capacity_) {
            seen_.erase(order_.front());
            order_.pop_front();
        }
        return true;
    }
    size_t size() const { return order_.size(); }
    size_t rejected() const { return rejected_; }
    void clear() { seen_.clear(); order_.clear(); }

private:
    size_t capacity_;
    size_t rejected_ = 0;
    std::unordered_set<std::string> seen_;
    std::deque<std::string> order_;
};
