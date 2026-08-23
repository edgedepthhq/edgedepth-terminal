// ═══════════════════════════════════════════════════════════════════════════════
// dispatch_drain_test.cpp - native pin on the time-budgeted dispatch drain.
//
// This is the mechanism the terminal's ingest design rests on. The data thread
// decompresses and parses off the main thread, but it may not touch widget
// state, so every widget-visible mutation is queued as a PendingDispatch and
// executed on the main thread at the top of the frame. Executing the whole
// backlog there is what used to cause the classic frame dip on a replay
// catch-up or a reconnect burst, so the drain is given a 3 ms budget and the
// remainder rides over to the next frame.
//
// Two properties make that safe, and neither is visible from a screenshot:
//
//   ORDER SURVIVES THE BOUNDARY. Work that spilled over resumes exactly where
//   it stopped, ahead of anything queued since. If the carry buffer were
//   refilled eagerly, a burst would interleave with newer messages and widgets
//   would apply a book delta before the snapshot it belongs to.
//
//   THE DRAIN ALWAYS PROGRESSES. The budget is checked every 16 dispatches, so
//   it is a soft ceiling: it overshoots by under a stride, and no budget however
//   small can produce a frame that executes nothing and lets the backlog grow
//   without bound.
//
// The clock is injected, so nothing here sleeps or reads wall time. The
// dispatches are the REAL PendingDispatch type driven through the REAL
// DispatchQueue: data_queues.h only forward-declares StreamManager, and this
// file supplies a stand-in for it, so the plumbing under test is the shipping
// plumbing rather than a mock of it.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/data_queues.h"
#include "core/dispatch_drain.h"

#include <cstddef>
#include <cstdio>
#include <vector>

// The queue stores std::function<void(StreamManager&)> and never touches the
// object, so the forward declaration in data_queues.h is the whole contract.
class StreamManager {};

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_size(std::size_t got, std::size_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %zu, want %zu\n", what, got, want);
        ++failures;
    }
}

// A stand-in for DataThread's main-thread half: the same carry buffer, the same
// counters, the same call into dispatch_drain::run, with a clock that advances
// only when a dispatch runs.
class Harness {
public:
    // Each executed dispatch costs this much simulated time.
    void set_cost_ms(double cost) { cost_ms_ = cost; }

    void push(int id) {
        queue_.push(PendingDispatch{[this, id](StreamManager&) {
            executed_.push_back(id);
            clock_ms_ += cost_ms_;
        }});
    }

    // Byte for byte what DataThread::drain_dispatches does, minus the real clock.
    std::size_t drain(double budget_ms) {
        const std::size_t executed = dispatch_drain::run(
            carry_, carry_pos_, budget_ms,
            [this](std::vector<PendingDispatch>& out) {
                queue_.drain(out);
                counters_.set(BacklogQueue::Carry, out.size());
            },
            [this](PendingDispatch& dispatch) { dispatch.execute(stream_); },
            [this] { return clock_ms_; });
        counters_.set(BacklogQueue::Carry, carry_.size() - carry_pos_);
        return executed;
    }

    std::size_t backlog() const { return carry_.size() - carry_pos_; }
    const std::vector<int>& executed() const { return executed_; }
    QueueBacklogSnapshot metrics() const { return counters_.snapshot(); }
    double clock_ms() const { return clock_ms_; }

private:
    QueueBacklogCounters counters_;
    DispatchQueue queue_{counters_};
    std::vector<PendingDispatch> carry_;
    std::size_t carry_pos_ = 0;
    std::vector<int> executed_;
    StreamManager stream_;
    double clock_ms_ = 0.0;
    double cost_ms_ = 0.0;
};

// The executed ids must be exactly 0,1,2,...,n-1 with nothing dropped,
// duplicated or reordered.
void expect_executed_in_order(const Harness& h, std::size_t n, const char* what) {
    if (h.executed().size() != n) {
        std::fprintf(stderr, "FAIL %s: executed %zu dispatches, want %zu\n", what,
                     h.executed().size(), n);
        ++failures;
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (h.executed()[i] != static_cast<int>(i)) {
            std::fprintf(stderr, "FAIL %s: position %zu holds dispatch %d, want %zu\n", what, i,
                         h.executed()[i], i);
            ++failures;
            return;
        }
    }
}

void test_empty_and_unbudgeted_drains() {
    Harness idle;
    expect_size(idle.drain(3.0), 0, "an empty queue executes nothing");
    expect_size(idle.backlog(), 0, "an empty queue leaves no backlog");
    expect_size(idle.metrics().carry.current, 0, "an empty drain reports no carry");

    // budget <= 0 is the unbudgeted path, used by callers that want the whole
    // backlog now (shutdown, replay seek). Even an expensive batch drains fully.
    Harness all;
    all.set_cost_ms(5.0);
    for (int i = 0; i < 100; ++i) all.push(i);
    expect_size(all.drain(0.0), 100, "a zero budget drains everything");
    expect_size(all.backlog(), 0, "a full drain leaves nothing behind");
    expect_executed_in_order(all, 100, "a full drain preserves order");

    Harness negative;
    negative.set_cost_ms(5.0);
    for (int i = 0; i < 100; ++i) negative.push(i);
    expect_size(negative.drain(-1.0), 100, "a negative budget also drains everything");

    // A clock that never advances can never exhaust a positive budget, so the
    // batch drains in one call rather than stalling at the first check.
    Harness frozen;
    frozen.set_cost_ms(0.0);
    for (int i = 0; i < 100; ++i) frozen.push(i);
    expect_size(frozen.drain(3.0), 100, "a free dispatch never spends the budget");
    expect_size(frozen.backlog(), 0, "and leaves no backlog");
}

void test_budget_stops_the_drain_at_a_stride_boundary() {
    // 0.05 ms per dispatch against the terminal's real 3 ms budget. The check
    // fires at 16, 32, 48, 64; the first one to reach 3 ms is 64 (3.2 ms).
    Harness h;
    h.set_cost_ms(0.05);
    for (int i = 0; i < 500; ++i) h.push(i);

    expect_size(h.drain(3.0), 64, "the drain stops at the first stride past the budget");
    expect_size(h.backlog(), 500 - 64, "the remainder is carried, not dropped");
    expect_size(h.metrics().carry.current, 500 - 64, "the carry counter reports the remainder");

    // Soft ceiling: it overshoots, but by less than one stride's worth of work.
    expect_true(h.clock_ms() >= 3.0, "the budget was actually spent");
    expect_true(h.clock_ms() < 3.0 + dispatch_drain::kBudgetCheckStride * 0.05,
                "the overshoot stays under one stride");

    // A dispatch expensive enough to blow the budget on its own still gets a
    // whole stride, because the budget is not checked more often than that.
    Harness pricey;
    pricey.set_cost_ms(10.0);
    for (int i = 0; i < 500; ++i) pricey.push(i);
    expect_size(pricey.drain(3.0), dispatch_drain::kBudgetCheckStride,
                "an over-budget batch still executes exactly one stride");

    // And the liveness floor: no budget, however small, produces a frame that
    // executes nothing. A drain that could return 0 with work queued would let
    // the backlog grow forever.
    Harness starved;
    starved.set_cost_ms(1000.0);
    for (int i = 0; i < 500; ++i) starved.push(i);
    expect_size(starved.drain(1e-9), dispatch_drain::kBudgetCheckStride,
                "even an unreachable budget makes a stride of progress");

    // A batch shorter than one stride is never truncated by the budget at all.
    Harness tiny;
    tiny.set_cost_ms(1000.0);
    for (int i = 0; i < 10; ++i) tiny.push(i);
    expect_size(tiny.drain(1e-9), 10, "a sub-stride batch drains whole");
    expect_size(tiny.backlog(), 0, "and leaves nothing carried");
}

void test_a_storm_drains_over_several_frames_in_order() {
    // 500 dispatches at 0.05 ms is 25 ms of work: eight frames at the 3 ms
    // budget rather than one 25 ms stall. Nothing may be lost or reordered.
    Harness h;
    h.set_cost_ms(0.05);
    for (int i = 0; i < 500; ++i) h.push(i);

    int frames = 0;
    std::size_t total = 0;
    while (h.backlog() > 0 || total < 500) {
        const std::size_t n = h.drain(3.0);
        if (n == 0) break;
        total += n;
        ++frames;
        expect_true(frames < 100, "the storm terminates");
    }

    expect_size(total, 500, "every queued dispatch eventually runs");
    expect_executed_in_order(h, 500, "order survives every budget boundary");
    expect_size(h.backlog(), 0, "the storm fully clears");
    expect_true(frames > 1, "the storm really did span several frames");
    expect_true(static_cast<std::size_t>(frames) <=
                    500 / dispatch_drain::kBudgetCheckStride + 2,
                "and did not degenerate into one dispatch per frame");
    expect_size(h.metrics().carry.current, 0, "the carry counter returns to zero");
    // The high-water mark survives, which is what the diagnostics panel plots.
    expect_true(h.metrics().carry.high_water > 0, "the carry high-water mark is recorded");
}

void test_work_queued_mid_storm_waits_its_turn() {
    // THE ordering invariant. A budget boundary must not let newer messages
    // overtake the batch already in flight: the carry buffer is refilled only
    // once it is fully consumed.
    Harness h;
    h.set_cost_ms(0.05);
    for (int i = 0; i < 200; ++i) h.push(i);

    const std::size_t first = h.drain(3.0);
    expect_true(first > 0 && first < 200, "the first frame drains part of the batch");
    const std::size_t remaining = h.backlog();
    expect_size(remaining, 200 - first, "the rest is carried");

    // The data thread keeps working while the main thread renders. These arrive
    // AFTER the batch in flight and must run after all of it.
    for (int i = 200; i < 400; ++i) h.push(i);

    // The producer queue is filling while the carry is still being worked
    // through: both show up in the combined backlog, and they are distinct.
    const auto mid = h.metrics();
    expect_size(mid.carry.current, remaining, "the in-flight batch is counted as carry");
    expect_size(mid.pending_dispatch.current, 200, "the newly queued work is counted separately");
    expect_size(mid.total.current, remaining + 200, "the total backlog spans both");

    // Drain to completion. Nothing from the second wave may appear before the
    // first wave is finished.
    for (int frame = 0; frame < 200 && h.executed().size() < 400; ++frame) {
        if (h.drain(3.0) == 0) break;
    }
    expect_executed_in_order(h, 400, "the second wave runs strictly after the first");
    expect_size(h.backlog(), 0, "everything eventually clears");
    expect_size(h.metrics().total.current, 0, "and the backlog counters return to zero");
}

void test_a_dispatch_that_queues_more_work_does_not_run_it_this_frame() {
    // Message handling can enqueue follow-up work. That work belongs to the NEXT
    // refill, not to the batch currently being executed - otherwise a dispatch
    // that queues a dispatch could extend a frame without bound and defeat the
    // budget entirely.
    Harness h;
    h.set_cost_ms(0.0);
    for (int i = 0; i < 5; ++i) h.push(i);

    expect_size(h.drain(3.0), 5, "the first batch drains");
    expect_size(h.executed().size(), 5, "and only the first batch ran");

    // Queue a follow-up wave and confirm it needs its own drain call.
    for (int i = 5; i < 10; ++i) h.push(i);
    expect_size(h.executed().size(), 5, "queueing alone executes nothing");
    expect_size(h.drain(3.0), 5, "the follow-up wave drains on the next call");
    expect_executed_in_order(h, 10, "and runs after everything before it");
}

void test_carry_accounting_matches_what_ran() {
    // carry_backlog() drives the DispBacklog diagnostic. It must equal what is
    // actually left, at every step, or the panel misreports the ingest health.
    Harness h;
    h.set_cost_ms(0.05);
    const std::size_t queued = 300;
    for (int i = 0; i < static_cast<int>(queued); ++i) h.push(i);

    std::size_t ran = 0;
    for (int frame = 0; frame < 100; ++frame) {
        const std::size_t n = h.drain(3.0);
        if (n == 0) break;
        ran += n;
        expect_size(h.backlog(), queued - ran, "the backlog is exactly what has not run");
        expect_size(h.metrics().carry.current, h.backlog(),
                    "the carry counter tracks the backlog");
        expect_size(h.executed().size(), ran, "the executed count tracks the return value");
    }
    expect_size(ran, queued, "the whole queue ran");
    expect_size(h.backlog(), 0, "and the backlog is empty");

    // A drain on an exhausted queue is a no-op, not an underflow. carry_backlog
    // is an unsigned subtraction, so an off-by-one here would read as ~1.8e19.
    expect_size(h.drain(3.0), 0, "draining an empty queue executes nothing");
    expect_size(h.backlog(), 0, "and does not underflow the backlog");
    expect_size(h.metrics().carry.current, 0, "nor the carry counter");
}

}  // namespace

int main() {
    test_empty_and_unbudgeted_drains();
    test_budget_stops_the_drain_at_a_stride_boundary();
    test_a_storm_drains_over_several_frames_in_order();
    test_work_queued_mid_storm_waits_its_turn();
    test_a_dispatch_that_queues_more_work_does_not_run_it_this_frame();
    test_carry_accounting_matches_what_ran();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("dispatch_drain_test: all assertions passed\n");
    return 0;
}
