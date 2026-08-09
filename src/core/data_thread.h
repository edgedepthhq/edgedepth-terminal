#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// data_thread.h — Offload ZSTD + protobuf processing to a dedicated pthread
//
// Architecture (Option B):
//   Main thread (browser):
//     WS callback → copies raw bytes into InboundQueue (fast, <1μs)
//     main_loop top → data_thread.drain_results(ctx)
//       → dispatches queued trades/candles/stats to widgets
//       → swap_buffers() for orderbook (already exists)
//     main_loop     → update() + render() widgets as normal
//
//   Data thread (pthread):
//     Drains InboundQueue → ZSTD decompress → protobuf parse
//     → Orderbook updates: writes directly to write_buf (mutex-protected)
//     → Trades/candles/stats/etc: queues into PendingDispatches
//     → Heatmap/pattern/debug: writes directly (mutex if needed)
//
// Why queue dispatches instead of calling StreamManager directly?
//   Widget callbacks mutate widget-local state (trade buffers, candle arrays).
//   Widgets read that state in update()/render() on the main thread.
//   Concurrent writes from data thread + reads from main thread = data race.
//   Queueing ensures all widget state mutations happen on the main thread.
// ═══════════════════════════════════════════════════════════════════════════════

#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <pthread.h>
#include "message_context.h"
#include "stream_handler.h"

// ─── Inbound queue: raw WS bytes from main thread → data thread ─────────────

struct RawMessage {
    std::vector<uint8_t> data;
};

class InboundQueue {
public:
    void push(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(RawMessage{{data, data + len}});
    }

    void drain(std::vector<RawMessage>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
        // After swap, queue_ has out's old contents (empty if caller cleared it)
    }

private:
    std::mutex mutex_;
    std::vector<RawMessage> queue_;
};

// ─── Pending dispatches: parsed data queued for main thread dispatch ─────────
// These are type-erased dispatch calls that the main thread executes.
// Each one captures the StreamKey + data needed to call dispatch_* on StreamManager.

struct PendingDispatch {
    // Type-erased callable — captures everything needed for dispatch
    std::function<void(StreamManager&)> execute;
};

class DispatchQueue {
public:
    void push(PendingDispatch&& dispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(dispatch));
    }

    void drain(std::vector<PendingDispatch>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
    }

private:
    std::mutex mutex_;
    std::vector<PendingDispatch> queue_;
};

// ─── DataThread: the pthread that does the heavy lifting ─────────────────────

class DataThread {
public:
    DataThread() = default;
    ~DataThread() { stop(); }

    DataThread(const DataThread&) = delete;
    DataThread& operator=(const DataThread&) = delete;

    // Call from main thread after managers are constructed
    void start(const MessageContext& ctx);

    // Call from main thread during shutdown
    void stop();

    // Called from WS callback on main thread — just enqueues raw bytes
    void enqueue(const uint8_t* data, size_t len) {
        inbound_.push(data, len);
    }

    // Called from main thread at top of each frame — drains queued
    // dispatches and executes them on the main thread where it's safe
    // to touch widget state.
    //
    // budget_ms > 0: stop executing once the budget is spent; the remainder
    // stays in carry_ (order preserved) and resumes next frame. This converts
    // ingest storms (replay catch-up, reconnect bursts) from one 10-30ms frame
    // into a few budget-bounded frames. Returns the number executed.
    size_t drain_dispatches(StreamManager& stream_mgr, double budget_ms = 0.0);

    // Dispatches still queued locally after a budgeted drain (diagnostics).
    size_t carry_backlog() const { return carry_.size() - carry_pos_; }

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Expose dispatch queue for MessageHandler to push into
    DispatchQueue& dispatch_queue() { return dispatches_; }

private:
    static void* thread_func(void* arg);
    void run();

    InboundQueue inbound_;
    DispatchQueue dispatches_;
    // Main-thread carry-over for budgeted drains: executed front-to-back;
    // refilled from dispatches_ only when fully consumed (preserves order).
    std::vector<PendingDispatch> carry_;
    size_t carry_pos_ = 0;
    MessageContext ctx_{};
    pthread_t thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
};

