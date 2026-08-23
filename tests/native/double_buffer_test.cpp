// ═══════════════════════════════════════════════════════════════════════════════
// double_buffer_test.cpp - native pin on the order book's publish-on-frame buffer.
//
// Every order book in the terminal is a DoubleBuffered<Terminal::Orderbook>. The
// data thread mutates the write side as depth deltas arrive; the main thread
// publishes once at the top of the frame and every widget reads the read side
// for the rest of it. That is what makes the DOM, the order book ladder and the
// heatmap agree with each other: they are not merely reading the same object,
// they are reading an object that CANNOT change while they read it.
//
// The failure this design exists to prevent is invisible in a screenshot and
// impossible to reproduce on demand: a pointer swap, or a read of the live write
// buffer, gives a widget a book that is half-way through a delta. Levels appear
// and vanish between two widgets in the same frame, and the bug reads as "the
// DOM flickers sometimes".
//
// So the assertions here are about VISIBILITY, not about copying:
//   - a write is not observable until the next publish, however many arrive;
//   - the read side is byte-stable across an entire frame;
//   - a clean buffer costs nothing (the dirty flag is what keeps forty idle
//     watchlist symbols off the frame budget);
//   - and under a real concurrent writer, the reader never observes a torn
//     state, which is the property the mutex is actually there for.
//
// Terminal::Orderbook itself needs protobuf and cannot be built here, so the
// buffer is instantiated over a stand-in payload carrying the same invariant a
// book does: a set of fields that are only ever mutually consistent.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/double_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_i64(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what,
                     static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

// A stand-in for Terminal::Orderbook. What matters is that it has an internal
// consistency rule a torn read would break: the levels always sum to `total`,
// and every level carries the same `revision` as the header.
struct Book {
    int64_t revision = 0;
    int64_t total = 0;
    std::vector<int64_t> levels;

    // Rewrite the whole book to a new revision. Mid-way through this the object
    // is inconsistent, which is exactly what the buffer must never publish.
    void rewrite(int64_t rev, std::size_t level_count) {
        revision = rev;
        levels.assign(level_count, rev);
        total = static_cast<int64_t>(level_count) * rev;
    }

    bool consistent() const {
        int64_t sum = 0;
        for (const int64_t lvl : levels) {
            if (lvl != revision) return false;
            sum += lvl;
        }
        return sum == total;
    }
};

void test_a_write_is_invisible_until_the_next_publish() {
    DoubleBuffered<Book> db;
    expect_true(!db.dirty.load(), "a fresh buffer is clean");
    expect_i64(db.read_buf.revision, 0, "a fresh read side is empty");

    // The producer writes. Nothing the consumer can see has changed yet.
    db.write_buf.rewrite(1, 4);
    db.mark_dirty();
    expect_true(db.dirty.load(), "a write marks the buffer dirty");
    expect_i64(db.read_buf.revision, 0, "the read side has not moved yet");
    expect_i64(db.write_buf.revision, 1, "but the write side has");

    // Publishing is the one moment the read side changes.
    expect_true(db.publish(), "publishing a dirty buffer reports that it copied");
    expect_i64(db.read_buf.revision, 1, "the read side now carries the write");
    expect_i64(db.read_buf.total, 4, "and the whole payload came across");
    expect_true(db.read_buf.consistent(), "the published book is internally consistent");
    expect_true(!db.dirty.load(), "publishing clears the dirty flag");

    // Publishing again with nothing new is a no-op. This is what keeps idle
    // symbols off the frame budget.
    expect_true(!db.publish(), "publishing a clean buffer reports no copy");
    expect_i64(db.read_buf.revision, 1, "and leaves the read side alone");
    expect_true(!db.publish(), "and stays a no-op on the next frame too");
}

void test_a_burst_of_writes_publishes_once_as_the_latest_state() {
    DoubleBuffered<Book> db;

    // Fifty depth deltas land during one frame. The consumer sees none of them
    // until the frame boundary, and then sees the LAST one, not the first and
    // not some blend.
    for (int64_t rev = 1; rev <= 50; ++rev) {
        db.write_buf.rewrite(rev, 3);
        db.mark_dirty();
        expect_i64(db.read_buf.revision, 0, "no intermediate write reaches the read side");
    }

    expect_true(db.publish(), "the burst publishes once");
    expect_i64(db.read_buf.revision, 50, "the read side lands on the latest state");
    expect_true(db.read_buf.consistent(), "and that state is coherent");

    // The read side is genuinely a copy, not a view: mutating the write side
    // afterwards must not reach through into it.
    db.write_buf.rewrite(99, 3);
    expect_i64(db.read_buf.revision, 50, "the read side is a copy, not an alias");
    expect_i64(db.read_buf.levels[0], 50, "including its level data");
}

void test_the_read_side_is_stable_for_a_whole_frame() {
    DoubleBuffered<Book> db;
    db.write_buf.rewrite(7, 5);
    db.mark_dirty();
    db.publish();

    // Simulate a frame: several widgets read the same book while the data thread
    // keeps writing. Every reader in the frame must agree, both with each other
    // and with what the frame started with.
    const Book at_frame_start = db.read_buf;
    for (int widget = 0; widget < 8; ++widget) {
        db.write_buf.rewrite(100 + widget, 5);
        db.mark_dirty();

        const Book& seen = db.read_buf;
        expect_i64(seen.revision, at_frame_start.revision, "every widget sees the same revision");
        expect_i64(seen.total, at_frame_start.total, "and the same totals");
        expect_true(seen.levels == at_frame_start.levels, "and the same levels");
        expect_true(seen.consistent(), "and a consistent book");
    }

    // Only the next frame boundary moves it, and it moves to the latest state.
    expect_true(db.publish(), "the next frame publishes the accumulated writes");
    expect_i64(db.read_buf.revision, 107, "the new frame sees the latest state");
}

void test_dirty_is_the_only_thing_that_triggers_a_copy() {
    DoubleBuffered<Book> db;

    // A write that never marks dirty does not publish. That is the contract the
    // producer has to keep, and pinning it here is what makes a forgotten
    // mark_dirty() at a new write site show up as a stale panel, not a mystery.
    db.write_buf.rewrite(3, 2);
    expect_true(!db.publish(), "an unmarked write does not publish");
    expect_i64(db.read_buf.revision, 0, "and the read side stays empty");

    db.mark_dirty();
    expect_true(db.publish(), "marking it makes the same write publish");
    expect_i64(db.read_buf.revision, 3, "and the read side catches up");

    // mark_dirty is idempotent: marking repeatedly still costs exactly one copy.
    db.mark_dirty();
    db.mark_dirty();
    db.mark_dirty();
    expect_true(db.publish(), "the first publish after marking copies");
    expect_true(!db.publish(), "repeated marks do not queue extra copies");
}

void test_a_concurrent_writer_never_publishes_a_torn_book() {
    // The property the mutex is for. A writer thread rewrites the book as fast
    // as it can, spending real time in an inconsistent intermediate state; the
    // consumer publishes and reads. If publish() ever copied a half-written
    // book, or if the read side were an alias for the write side, `consistent()`
    // would fail here.
    //
    // Deterministic in the sense that matters: fixed iteration counts, no
    // sleeps, no clock reads, and an assertion that must hold on every possible
    // interleaving rather than on a particular one.
    DoubleBuffered<Book> db;
    std::atomic<bool> stop{false};
    // Run until this many REAL publishes have been observed, rather than for a
    // fixed number of frames: a fixed frame count can finish before the writer
    // thread has even started, and then the test asserts nothing at all.
    constexpr int kTargetPublishes = 2000;
    // Bounded so a writer that never runs fails the test instead of hanging it.
    constexpr long long kMaxSpins = 50'000'000LL;
    constexpr std::size_t kLevels = 64;

    std::thread writer([&db, &stop] {
        for (int64_t rev = 1; !stop.load(std::memory_order_relaxed); ++rev) {
            std::lock_guard<std::mutex> lock(db.write_mutex);
            // Deliberately leave the book inconsistent part-way through, the way
            // a real depth delta does between erasing a level and inserting one.
            db.write_buf.revision = rev;
            db.write_buf.total = 0;
            db.write_buf.levels.assign(kLevels, rev);
            db.write_buf.total = static_cast<int64_t>(kLevels) * rev;
            db.mark_dirty();
        }
    });

    int torn = 0;
    int published = 0;
    int64_t last_revision = 0;
    long long spins = 0;
    bool went_backwards = false;
    while (published < kTargetPublishes && spins < kMaxSpins) {
        ++spins;
        if (db.publish()) ++published;
        const Book& seen = db.read_buf;
        if (!seen.consistent()) ++torn;
        // The published revision may skip (the consumer only sees the latest at
        // each boundary) but it must never go BACKWARDS.
        if (seen.revision < last_revision) {
            went_backwards = true;
            break;
        }
        last_revision = seen.revision;
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    if (went_backwards) {
        std::fprintf(stderr, "FAIL published revision went backwards: %lld then %lld\n",
                     static_cast<long long>(last_revision),
                     static_cast<long long>(db.read_buf.revision));
        ++failures;
    }
    expect_true(torn == 0, "no frame ever observed a torn book");
    expect_true(published >= kTargetPublishes,
                "the concurrency test observed the publishes it set out to");
    expect_true(db.read_buf.consistent(), "the final published book is consistent");

    // One last publish after the writer has stopped settles on the writer's
    // final state, which must itself be coherent.
    db.mark_dirty();
    db.publish();
    expect_true(db.read_buf.consistent(), "the settled book is consistent");
    expect_i64(db.read_buf.revision, db.write_buf.revision, "and matches the final write");
}

}  // namespace

int main() {
    test_a_write_is_invisible_until_the_next_publish();
    test_a_burst_of_writes_publishes_once_as_the_latest_state();
    test_the_read_side_is_stable_for_a_whole_frame();
    test_dirty_is_the_only_thing_that_triggers_a_copy();
    test_a_concurrent_writer_never_publishes_a_torn_book();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("double_buffer_test: all assertions passed\n");
    return 0;
}
