#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// double_buffer.h - publish-on-frame double buffering.
//
// Extracted from DoubleBufferedOrderbook so the mechanism can be tested without
// dragging in protobuf and the order book types. The contract:
//
//   The producer (data thread) mutates write_buf under write_mutex and calls
//   mark_dirty(). The consumer (main thread) reads read_buf, which changes at
//   EXACTLY ONE point per frame: the publish() call at the top of the frame,
//   before any widget runs. So every widget in a frame sees the same book, and
//   two widgets reading the same symbol can never disagree.
//
// Why copy instead of swapping pointers?
//
//   The write side is continuously mutated: an insert or erase per price level
//   on every depth update. A pointer swap would hand the consumer a buffer that
//   the very next WS callback starts mutating underneath it, so a widget
//   half-way through drawing the book would see levels appear and vanish inside
//   a single frame. Copying costs ~50us for a 1000-level book, which is inside
//   the frame budget, and buys a snapshot that is stable by construction.
//
// dirty is what keeps that cost off idle symbols: a watchlist of forty pairs
// only copies the handful that actually ticked this frame.
// ═══════════════════════════════════════════════════════════════════════════════

#include <atomic>
#include <mutex>

template <typename T>
struct DoubleBuffered {
    T write_buf;                     // mutated by the producer under write_mutex
    T read_buf;                      // read by the consumer, stable for a frame
    std::atomic<bool> dirty{false};  // set on any write, cleared on publish
    mutable std::mutex write_mutex;  // protects write_buf

    // Called by the producer after mutating write_buf, while still holding
    // write_mutex. Marks the buffer for the next publish.
    void mark_dirty() { dirty.store(true, std::memory_order_relaxed); }

    // Called once per frame on the consumer thread, before anything reads.
    // Copies write -> read if there is anything new. Returns whether it copied,
    // which is what lets a caller count how many books moved this frame.
    //
    // A clean buffer is skipped WITHOUT taking the lock, so an idle symbol never
    // contends with the data thread.
    bool publish() {
        if (!dirty.load(std::memory_order_relaxed)) return false;
        std::lock_guard<std::mutex> lock(write_mutex);
        read_buf = write_buf;
        dirty.store(false, std::memory_order_relaxed);
        return true;
    }
};
