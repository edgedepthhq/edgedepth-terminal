#pragma once
#include "types/types.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// Main-thread capture, shared by charts through RealtimeArchive. Camera state
// and display filters never enter publication. A closed group is immutable.
class RealtimeBubbleHistory {
public:
    static constexpr int64_t interval_ms = 100, settle_ms = 100, retention_ms = 1800000;
    static constexpr size_t groups_per_bin = 8, max_pending_groups = 65536;
    struct Bubble {
        int64_t timestamp_ms = 0; // fixed midpoint of the 100ms window, not a fill time
        double price = 0, buy = 0, sell = 0;
        uint64_t count = 0;
        double value() const { return buy + sell; }
        bool buy_led() const { return buy >= sell; }
        double minor() const { return std::min(buy, sell) / value(); }
    };
    size_t filtered_groups = 0;
    uint64_t late_records = 0, approximate_records = 0, overflow_records = 0;

    void append(const Terminal::Trade& t, int64_t arrival_ms) {
        if (t.timestamp_ms <= 0 || !(t.price > 0) || !(t.qty > 0) ||
            !std::isfinite(t.price * t.qty)) return;
        const uint64_t count = t.summary_count ? t.summary_count : 1;
        // A side/time summary spanning prices cannot supply an exact-price dot.
        if (t.summary_count && (!(t.summary_low > 0) || t.summary_low != t.summary_high)) {
            approximate_records += count; return;
        }
        const int64_t bin = t.timestamp_ms / interval_ms * interval_ms;
        if (bin + interval_ms <= clock_ms_ - retention_ms) return;
        if (published_.contains(bin)) { late_records += count; return; }
        const double price = t.summary_count ? t.summary_low : t.price;
        const double value = price * t.qty;
        if (!std::isfinite(value)) return;
        auto it = pending_.find(bin);
        if (pending_groups_ >= max_pending_groups &&
            (it == pending_.end() || !it->second.prices.contains(price))) {
            overflow_records += count; return;
        }
        auto& pending = pending_[bin];
        auto [group, inserted] = pending.prices.try_emplace(price);
        if (inserted) {
            ++pending_groups_;
            group->second.timestamp_ms = bin + interval_ms / 2;
            group->second.price = price;
        }
        auto& bubble = group->second;
        if (!std::isfinite(bubble.value() + value)) { overflow_records += count; return; }
        (t.is_buy ? bubble.buy : bubble.sell) += value;
        bubble.count += count;
        pending.last_arrival_ms = std::max(pending.last_arrival_ms, arrival_ms);
    }

    void advance(int64_t clock_ms) {
        // Small replay clock corrections are as-of queries, not a new traversal.
        // The source's explicit clear/seek owns reset().
        clock_ms_ = std::max(clock_ms_, clock_ms);
        const int64_t expired = (clock_ms_ - retention_ms) / interval_ms * interval_ms;
        while (!published_.empty() && published_.begin()->first < expired) {
            size_ -= published_.begin()->second.bubbles.size();
            published_.erase(published_.begin());
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            const auto& pending = it->second;
            if (it->first < expired) {
                pending_groups_ -= pending.prices.size(); it = pending_.erase(it); continue;
            }
            if (it->first + interval_ms > clock_ms || pending.last_arrival_ms + settle_ms > clock_ms) {
                ++it; continue;
            }
            Publication publication;
            publication.at_ms = clock_ms;
            for (const auto& [price, bubble] : pending.prices)
                if (bubble.value() > 0) publication.bubbles.push_back(bubble);
            auto& bubbles = publication.bubbles;
            if (bubbles.size() > groups_per_bin) {
                filtered_groups += bubbles.size() - groups_per_bin;
                std::nth_element(bubbles.begin(), bubbles.begin() + groups_per_bin, bubbles.end(),
                    [](const Bubble& a, const Bubble& b) {
                        return a.value() != b.value() ? a.value() > b.value() : a.price < b.price;
                    });
                bubbles.resize(groups_per_bin);
            }
            std::sort(bubbles.begin(), bubbles.end(),
                [](const Bubble& a, const Bubble& b) { return a.price < b.price; });
            size_ += bubbles.size();
            published_.emplace(it->first, std::move(publication));
            pending_groups_ -= pending.prices.size(); it = pending_.erase(it);
        }
    }

    template<class Emit>
    void for_each(int64_t from, int64_t to, int64_t as_of, Emit&& emit) const {
        for (auto it = published_.lower_bound(from / interval_ms * interval_ms);
             it != published_.end() && it->first <= to; ++it) {
            if (it->second.at_ms > as_of) continue; // paused charts cannot see later publications
            for (const auto& bubble : it->second.bubbles)
                if (bubble.timestamp_ms >= from && bubble.timestamp_ms <= to) emit(bubble);
        }
    }
    size_t size() const { return size_; }
    void reset() { *this = {}; }
private:
    struct Pending { std::map<double, Bubble> prices; int64_t last_arrival_ms = 0; };
    struct Publication { std::vector<Bubble> bubbles; int64_t at_ms = 0; };
    std::map<int64_t, Pending> pending_;
    std::map<int64_t, Publication> published_;
    size_t pending_groups_ = 0, size_ = 0;
    int64_t clock_ms_ = 0;
};
