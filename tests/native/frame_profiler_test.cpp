#include "../../src/types/frame_profiler.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_u64(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %llu, want %llu\n", what,
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
        ++failures;
    }
}

void expect_near(double got, double want, double tolerance, const char* what) {
    if (!std::isfinite(got) || std::fabs(got - want) > tolerance) {
        std::fprintf(stderr, "FAIL %s: got %.9f, want %.9f\n", what, got, want);
        ++failures;
    }
}

const FrameProfiler::DisplayRow* find_row(const FrameProfiler::Snapshot& snapshot,
                                          const char* name) {
    for (int i = 0; i < snapshot.section_count; ++i) {
        const auto& row = snapshot.sections[static_cast<std::size_t>(i)];
        if (row.name != nullptr && std::strcmp(row.name, name) == 0) return &row;
    }
    return nullptr;
}

void test_empty_and_partial_tracker() {
    FrameTimeTracker tracker;
    const auto empty = tracker.compute();
    expect_u64(empty.sample_count, 0, "empty sample count");
    expect_near(empty.avg, 0.0, 0.0, "empty average");
    expect_near(empty.p50, 0.0, 0.0, "empty p50");
    expect_near(empty.p95, 0.0, 0.0, "empty p95");
    expect_near(empty.p99, 0.0, 0.0, "empty p99");
    expect_near(empty.max, 0.0, 0.0, "empty max");

    for (int value = 20; value >= 1; --value) tracker.record(value);
    const auto partial = tracker.compute();
    expect_u64(partial.sample_count, 20, "partial sample count");
    expect_near(partial.avg, 10.5, 1e-12, "partial average");
    // Nearest-rank semantics: ceil(P * N), then convert to zero-based index.
    expect_near(partial.p50, 10.0, 0.0, "nearest-rank p50");
    expect_near(partial.p95, 19.0, 0.0, "nearest-rank p95");
    expect_near(partial.p99, 20.0, 0.0, "nearest-rank p99");
    expect_near(partial.max, 20.0, 0.0, "partial max");

    const std::size_t before_invalid = tracker.count();
    expect_true(!tracker.record(std::numeric_limits<double>::quiet_NaN()), "reject NaN");
    expect_true(!tracker.record(std::numeric_limits<double>::infinity()), "reject infinity");
    expect_true(!tracker.record(-1.0), "reject negative duration");
    expect_u64(tracker.count(), before_invalid, "invalid samples do not change count");
}

void test_ring_wraparound() {
    FrameTimeTracker tracker;
    for (int value = 1; value <= 305; ++value) tracker.record(value);

    const auto wrapped = tracker.compute();
    expect_u64(tracker.count(), FrameTimeTracker::HISTORY_SIZE, "wrapped tracker count");
    expect_u64(tracker.write_pos(), 5, "wrapped write position");
    expect_near(wrapped.avg, 155.5, 1e-12, "wrapped average");
    expect_near(wrapped.p50, 155.0, 0.0, "wrapped p50");
    expect_near(wrapped.p95, 290.0, 0.0, "wrapped p95");
    expect_near(wrapped.p99, 302.0, 0.0, "wrapped p99");
    expect_near(wrapped.max, 305.0, 0.0, "wrapped max");
}

void test_profiler_cpu_snapshot_and_spikes() {
    using namespace std::chrono_literals;
    using Clock = FrameProfiler::Clock;
    const Clock::time_point start{};
    FrameProfiler profiler(start);
    profiler.set_spike_threshold_ms(5.0);

    expect_true(!profiler.spike_log(), "spike logging defaults off");

    profiler.add_count("Dispatch", 2);
    profiler.end_frame_at(1.0, start + 100ms);
    profiler.add_count("Dispatch", 3);
    profiler.end_frame_at(3.0, start + 200ms);
    profiler.end_frame_at(7.0, start + 300ms);
    profiler.end_frame_at(9.0, start + 1100ms);

    const auto first = profiler.snapshot();
    expect_true(first.ready, "first CPU snapshot ready");
    expect_u64(first.generation, 1, "first snapshot generation");
    expect_u64(first.frame_count, 4, "first snapshot frame count");
    expect_u64(first.cpu.sample_count, 4, "first CPU sample count");
    expect_near(first.window_seconds, 1.1, 1e-12, "first window seconds");
    expect_near(first.cpu.avg, 5.0, 1e-12, "CPU average");
    expect_near(first.cpu.p50, 3.0, 0.0, "CPU p50");
    expect_near(first.cpu.p95, 9.0, 0.0, "CPU p95");
    expect_near(first.cpu.p99, 9.0, 0.0, "CPU p99");
    expect_near(first.cpu.max, 9.0, 0.0, "CPU max");
    expect_u64(first.cpu_spikes, 2, "CPU spikes in window");
    expect_u64(first.total_cpu_spikes, 2, "CPU spikes total");

    const auto* dispatch = find_row(first, "Dispatch");
    expect_true(dispatch != nullptr, "dispatch count row exists");
    if (dispatch != nullptr) {
        expect_u64(dispatch->count_total, 5, "dispatch window count");
        expect_near(dispatch->count_per_frame, 1.25, 1e-12,
                    "dispatch count per frame");
        expect_near(dispatch->avg_ms, 0.0, 0.0, "count-only row average");
        expect_near(dispatch->max_ms, 0.0, 0.0, "count-only row maximum");
    }

    profiler.end_frame_at(2.0, start + 1200ms);
    expect_u64(profiler.snapshot().generation, 1, "snapshot stable before rollover");
    profiler.end_frame_at(4.0, start + 2200ms);

    const auto second = profiler.snapshot();
    expect_u64(second.generation, 2, "second snapshot generation");
    expect_u64(second.cpu.sample_count, 2, "second CPU sample count");
    expect_near(second.cpu.avg, 3.0, 1e-12, "second CPU average");
    expect_near(second.cpu.p50, 2.0, 0.0, "second CPU p50");
    expect_near(second.cpu.max, 4.0, 0.0, "second CPU max");
    expect_u64(second.cpu_spikes, 0, "second CPU spike count reset");
    expect_u64(second.total_cpu_spikes, 2, "total CPU spike count retained");
}

}  // namespace

int main() {
    test_empty_and_partial_tracker();
    test_ring_wraparound();
    test_profiler_cpu_snapshot_and_spikes();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("frame_profiler_test: all assertions passed\n");
    return 0;
}
