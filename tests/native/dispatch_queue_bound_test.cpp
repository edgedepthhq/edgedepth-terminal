// The dispatch queue is bounded: a hidden window stops draining it, so it
// must drop its OLDEST entries, keep the newest, count what it dropped and
// date the gap from the last drain so the caller's repair reaches back to it.
#include "core/data_queues.h"

#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}

int main() {
    QueueBacklogCounters counters;
    DispatchQueue queue(counters);
    std::uint64_t dropped = 0;
    double gap_start = 0.0;
    check(!queue.take_drops(dropped, gap_start), "nothing dropped while empty");

    // The window last drained here, then stopped (hidden: rAF never fires).
    std::vector<PendingDispatch> out;
    queue.drain(out);
    const double drained_at = DispatchQueue::steady_ms();

    const std::size_t total = DispatchQueue::kMaxPending + 10;
    for (std::size_t i = 0; i < total; ++i) {
        queue.push(PendingDispatch{[i](StreamManager&) { (void)i; }});
    }
    check(counters.snapshot().pending_dispatch.current <= DispatchQueue::kMaxPending,
          "size stays within the bound");
    check(queue.take_drops(dropped, gap_start), "overflow is reported");
    check(dropped == DispatchQueue::kMaxPending / 4, "one quarter is dropped per overflow");
    // The dropped entries are the oldest ones, queued right after that drain.
    // Dating the gap from the first overflow instead sized the repair to the
    // retained newest entries and never reached the hole.
    check(gap_start > 0.0 && gap_start <= drained_at, "the gap is dated from the last drain");
    check(queue.dropped_total() == DispatchQueue::kMaxPending / 4, "lifetime total counts the drop");
    check(!queue.take_drops(dropped, gap_start), "drops are reported once");

    // The survivors are the newest: draining yields total - dropped entries.
    queue.drain(out);
    check(out.size() == total - DispatchQueue::kMaxPending / 4, "newest entries survive");
    check(counters.snapshot().pending_dispatch.current == 0, "drain empties the queue");
    std::puts("dispatch queue bound passed");
    return 0;
}
