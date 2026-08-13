#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class BacklogQueue : std::uint8_t {
    Inbound,
    PendingDispatch,
    Carry,
};

struct QueueDepthSnapshot {
    std::uint64_t current = 0;
    std::uint64_t high_water = 0;
};

struct QueueBacklogSnapshot {
    QueueDepthSnapshot inbound{};
    QueueDepthSnapshot pending_dispatch{};
    QueueDepthSnapshot carry{};
    QueueDepthSnapshot total{};
};

// Relaxed counters mirror queue sizes while each queue's existing mutex is
// already held. Reading a snapshot never locks either producer queue.
class QueueBacklogCounters {
public:
    void set(BacklogQueue queue, std::size_t size) {
        const auto value = static_cast<std::uint64_t>(size);
        Counter& selected = counter(queue);
        const std::uint64_t previous =
            selected.current.exchange(value, std::memory_order_relaxed);
        update_high_water(selected.high_water, value);

        std::uint64_t total_after = 0;
        if (value >= previous) {
            const std::uint64_t delta = value - previous;
            const std::uint64_t before =
                total_current_.fetch_add(delta, std::memory_order_relaxed);
            total_after = before + delta;
        } else {
            const std::uint64_t delta = previous - value;
            const std::uint64_t before =
                total_current_.fetch_sub(delta, std::memory_order_relaxed);
            total_after = before - delta;
        }
        update_high_water(total_high_water_, total_after);
    }

    [[nodiscard]] QueueBacklogSnapshot snapshot() const {
        QueueBacklogSnapshot result{};
        result.inbound = read(inbound_);
        result.pending_dispatch = read(pending_dispatch_);
        result.carry = read(carry_);

        result.total.current = saturating_add(
            saturating_add(result.inbound.current, result.pending_dispatch.current),
            result.carry.current);
        result.total.high_water = std::max(
            result.total.current,
            total_high_water_.load(std::memory_order_relaxed));
        return result;
    }

private:
    struct Counter {
        std::atomic<std::uint64_t> current{0};
        std::atomic<std::uint64_t> high_water{0};
    };

    static void update_high_water(std::atomic<std::uint64_t>& high_water,
                                  std::uint64_t value) {
        std::uint64_t observed = high_water.load(std::memory_order_relaxed);
        while (observed < value &&
               !high_water.compare_exchange_weak(observed, value,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
        }
    }

    static QueueDepthSnapshot read(const Counter& counter_value) {
        QueueDepthSnapshot result{
            .current = counter_value.current.load(std::memory_order_relaxed),
            .high_water = counter_value.high_water.load(std::memory_order_relaxed),
        };
        result.high_water = std::max(result.high_water, result.current);
        return result;
    }

    static std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) {
        const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
        return lhs > max - rhs ? max : lhs + rhs;
    }

    Counter& counter(BacklogQueue queue) {
        switch (queue) {
            case BacklogQueue::Inbound: return inbound_;
            case BacklogQueue::PendingDispatch: return pending_dispatch_;
            case BacklogQueue::Carry: return carry_;
        }
        return inbound_;
    }

    Counter inbound_{};
    Counter pending_dispatch_{};
    Counter carry_{};
    std::atomic<std::uint64_t> total_current_{0};
    std::atomic<std::uint64_t> total_high_water_{0};
};
