#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "queue_metrics.h"

class StreamManager;

struct RawMessage {
    std::vector<std::uint8_t> data;
};

class InboundQueue {
public:
    explicit InboundQueue(QueueBacklogCounters& counters) : counters_(counters) {}

    void push(const std::uint8_t* data, std::size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(RawMessage{{data, data + len}});
        counters_.set(BacklogQueue::Inbound, queue_.size());
    }

    void drain(std::vector<RawMessage>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
        // queue_ now owns out's former contents. The worker clears out before
        // each drain, but recording queue_.size() also remains correct if a
        // different caller swaps a non-empty vector.
        counters_.set(BacklogQueue::Inbound, queue_.size());
    }

private:
    QueueBacklogCounters& counters_;
    std::mutex mutex_;
    std::vector<RawMessage> queue_;
};

struct PendingDispatch {
    std::function<void(StreamManager&)> execute;
};

// Bounded: the main thread drains it once per presented frame, and a hidden
// tab presents nothing (rAF stops), so an unbounded queue would grow for as
// long as the window stayed behind another one. When full, the OLDEST quarter
// is dropped in one erase and counted; the newest events, which are the ones
// that matter when the window comes back, always get through. Depth is not
// affected (the book is written directly, not queued); dropped trades and
// candle updates leave a gap the caller repairs with a recent-candle refetch.
// The dropped entries are the OLDEST, so the gap starts where the window
// stopped draining, not where the queue first overflowed: a repair sized from
// the first drop only re-fetched the retained newest entries and left the hole.
class DispatchQueue {
public:
    static constexpr std::size_t kMaxPending = 65536;

    explicit DispatchQueue(QueueBacklogCounters& counters) : counters_(counters) {}

    void push(PendingDispatch&& dispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxPending) {
            const std::size_t drop = kMaxPending / 4;
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(drop));
            dropped_ += drop;
            // Everything queued arrived after the last drain, so that is the
            // earliest a dropped entry can date from.
            if (gap_start_ms_ <= 0.0) gap_start_ms_ = last_drain_ms_ > 0.0 ? last_drain_ms_ : steady_ms();
        }
        queue_.push_back(std::move(dispatch));
        counters_.set(BacklogQueue::PendingDispatch, queue_.size());
    }

    void drain(std::vector<PendingDispatch>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
        last_drain_ms_ = steady_ms();
        counters_.set(BacklogQueue::PendingDispatch, queue_.size());
    }

    static double steady_ms() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Drops since the previous call and the earliest time a dropped entry can
    // date from (steady_ms clock); returns false when nothing was dropped.
    bool take_drops(std::uint64_t& dropped, double& gap_start_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped = dropped_;
        gap_start_ms = gap_start_ms_;
        dropped_total_ += dropped_;
        dropped_ = 0;
        gap_start_ms_ = 0.0;
        return dropped > 0;
    }

    std::uint64_t dropped_total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_total_ + dropped_;
    }

private:
    QueueBacklogCounters& counters_;
    mutable std::mutex mutex_;
    std::vector<PendingDispatch> queue_;
    std::uint64_t dropped_ = 0;        // since the last take_drops
    std::uint64_t dropped_total_ = 0;  // lifetime, for the perf panel
    double gap_start_ms_ = 0.0;
    double last_drain_ms_ = 0.0;
};
