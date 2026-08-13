// ═══════════════════════════════════════════════════════════════════════════════
// data_thread.cpp - Dedicated pthread for ZSTD + protobuf processing
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/data_thread.h"
#include "core/message_handler.h"
#include <cstdio>
#include <chrono>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void DataThread::start(const MessageContext& ctx) {
    if (running_.load()) {
        return;
    }

    ctx_ = ctx;
    should_stop_.store(false);
    running_.store(true);

    int rc = pthread_create(&thread_, nullptr, thread_func, this);
    if (rc != 0) {
        running_.store(false);
        return;
    }
}

void DataThread::stop() {
    if (!running_.load()) return;

    should_stop_.store(true);
    pthread_join(thread_, nullptr);
    running_.store(false);
}

// ─── Thread entry point ──────────────────────────────────────────────────────

void* DataThread::thread_func(void* arg) {
    auto* self = static_cast<DataThread*>(arg);
    self->run();
    return nullptr;
}

// ─── Main processing loop ────────────────────────────────────────────────────

void DataThread::run() {
    std::vector<RawMessage> batch;
    batch.reserve(256);  // Avoid reallocs for typical bursts

    while (!should_stop_.load(std::memory_order_relaxed)) {
        // Drain all pending raw messages in one lock acquisition
        batch.clear();
        inbound_.drain(batch);

        if (batch.empty()) {
            // No work - yield briefly to avoid busy-spinning.
            // 500μs is a good balance: fast enough to not add perceptible
            // latency, slow enough to not waste CPU.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        // Process each message: ZSTD decompress → protobuf parse → route
        for (const auto& msg : batch) {
            if (should_stop_.load(std::memory_order_relaxed)) break;

            std::string data(
                reinterpret_cast<const char*>(msg.data.data()),
                msg.data.size()
            );
            MessageHandler::handle_message(data, ctx_);
        }
    }
}

// ─── Main-thread drain ───────────────────────────────────────────────────────

size_t DataThread::drain_dispatches(StreamManager& stream_mgr, double budget_ms) {
    // Refill the carry buffer only when the previous batch is fully executed -
    // dispatch order must be preserved across budget boundaries.
    if (carry_pos_ >= carry_.size()) {
        carry_.clear();
        carry_pos_ = 0;
        dispatches_.drain(carry_);
        backlog_counters_.set(BacklogQueue::Carry, carry_.size());
    }
    if (carry_pos_ >= carry_.size()) {
        backlog_counters_.set(BacklogQueue::Carry, 0);
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    size_t executed = 0;

    while (carry_pos_ < carry_.size()) {
        carry_[carry_pos_++].execute(stream_mgr);
        ++executed;
        // Amortize the clock read - check the budget every 16 dispatches.
        if (budget_ms > 0.0 && (executed & 15u) == 0) {
            const double spent = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (spent >= budget_ms) break;
        }
    }

    if (carry_pos_ >= carry_.size()) {
        carry_.clear();
        carry_pos_ = 0;
    }
    backlog_counters_.set(BacklogQueue::Carry, carry_.size() - carry_pos_);
    return executed;
}
