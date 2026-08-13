#pragma once

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

class DispatchQueue {
public:
    explicit DispatchQueue(QueueBacklogCounters& counters) : counters_(counters) {}

    void push(PendingDispatch&& dispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(dispatch));
        counters_.set(BacklogQueue::PendingDispatch, queue_.size());
    }

    void drain(std::vector<PendingDispatch>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
        counters_.set(BacklogQueue::PendingDispatch, queue_.size());
    }

private:
    QueueBacklogCounters& counters_;
    std::mutex mutex_;
    std::vector<PendingDispatch> queue_;
};
