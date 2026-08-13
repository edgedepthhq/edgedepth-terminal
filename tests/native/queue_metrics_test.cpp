#include "../../src/core/data_queues.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect_u64(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %llu, want %llu\n", what,
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
        ++failures;
    }
}

void test_inbound_vector_swaps() {
    QueueBacklogCounters counters;
    InboundQueue inbound(counters);
    const std::uint8_t payload[] = {1, 2, 3};

    inbound.push(payload, sizeof(payload));
    inbound.push(payload, sizeof(payload));
    inbound.push(payload, sizeof(payload));
    auto snapshot = counters.snapshot();
    expect_u64(snapshot.inbound.current, 3, "inbound current after pushes");
    expect_u64(snapshot.inbound.high_water, 3, "inbound high-water after pushes");
    expect_u64(snapshot.total.current, 3, "total current after inbound pushes");

    std::vector<RawMessage> batch;
    inbound.drain(batch);
    snapshot = counters.snapshot();
    expect_u64(batch.size(), 3, "inbound drained batch size");
    expect_u64(snapshot.inbound.current, 0, "inbound current after empty swap");
    expect_u64(snapshot.inbound.high_water, 3, "inbound high-water survives drain");

    inbound.push(payload, sizeof(payload));
    inbound.push(payload, sizeof(payload));
    // Swapping a non-empty destination puts its former contents back into the
    // producer vector. The counter must report that actual post-swap size.
    inbound.drain(batch);
    snapshot = counters.snapshot();
    expect_u64(batch.size(), 2, "non-empty destination receives producer batch");
    expect_u64(snapshot.inbound.current, 3, "post-swap inbound current is exact");

    batch.clear();
    inbound.drain(batch);
    snapshot = counters.snapshot();
    expect_u64(batch.size(), 3, "swapped-back batch drains again");
    expect_u64(snapshot.inbound.current, 0, "inbound returns to zero");
}

void test_dispatch_carry_and_total_high_water() {
    QueueBacklogCounters counters;
    DispatchQueue dispatches(counters);

    for (int i = 0; i < 4; ++i) dispatches.push(PendingDispatch{});
    auto snapshot = counters.snapshot();
    expect_u64(snapshot.pending_dispatch.current, 4, "pending dispatch current");
    expect_u64(snapshot.pending_dispatch.high_water, 4, "pending dispatch high-water");

    std::vector<PendingDispatch> carry;
    dispatches.drain(carry);
    counters.set(BacklogQueue::Carry, carry.size());
    snapshot = counters.snapshot();
    expect_u64(snapshot.pending_dispatch.current, 0, "pending zero after transfer");
    expect_u64(snapshot.carry.current, 4, "carry current after transfer");
    expect_u64(snapshot.total.current, 4, "total preserved across transfer");

    for (int i = 0; i < 3; ++i) dispatches.push(PendingDispatch{});
    snapshot = counters.snapshot();
    expect_u64(snapshot.pending_dispatch.current, 3, "producer pending during carry");
    expect_u64(snapshot.carry.current, 4, "carry remains visible");
    expect_u64(snapshot.total.current, 7, "combined total backlog");
    expect_u64(snapshot.total.high_water, 7, "combined total high-water");

    // A budgeted drain advances within the same vector; only the remaining
    // dispatches count as main-thread carry.
    counters.set(BacklogQueue::Carry, 2);
    snapshot = counters.snapshot();
    expect_u64(snapshot.total.current, 5, "total after partial carry drain");
    expect_u64(snapshot.carry.high_water, 4, "carry high-water survives partial drain");

    counters.set(BacklogQueue::Carry, 0);
    carry.clear();
    dispatches.drain(carry);
    counters.set(BacklogQueue::Carry, carry.size());
    snapshot = counters.snapshot();
    expect_u64(snapshot.pending_dispatch.current, 0, "pending zero after next transfer");
    expect_u64(snapshot.carry.current, 3, "next producer batch becomes carry");
    expect_u64(snapshot.total.current, 3, "total after next transfer");
    expect_u64(snapshot.total.high_water, 7, "total high-water remains monotonic");

    counters.set(BacklogQueue::Carry, 0);
    snapshot = counters.snapshot();
    expect_u64(snapshot.total.current, 0, "all queues return to zero");
    expect_u64(snapshot.total.high_water, 7, "zero does not reset total high-water");
}

}  // namespace

int main() {
    test_inbound_vector_swaps();
    test_dispatch_carry_and_total_high_water();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("queue_metrics_test: all assertions passed\n");
    return 0;
}
