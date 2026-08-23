#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// dispatch_drain.h - the time-budgeted, order-preserving drain rule.
//
// Extracted from DataThread::drain_dispatches so the rule can be exercised on
// its own. The policy is small but load bearing, and both halves of it are easy
// to break by accident:
//
//   ORDER. The carry buffer is refilled from the producer queue ONLY when the
//   previous batch is fully consumed. A drain that spilled over its budget
//   resumes at the exact dispatch it stopped on, ahead of anything the data
//   thread queued in the meantime. Refilling eagerly would reorder widget state
//   mutations, which is the one thing the queue exists to prevent.
//
//   PROGRESS. The budget is checked every kBudgetCheckStride dispatches, not
//   every dispatch: during an ingest storm the clock read cost more than the
//   dispatches did. So the budget is a SOFT ceiling that overshoots by up to
//   one stride, and, more importantly, a drain always executes at least one
//   stride (or the whole remaining batch, if it is shorter). A budget so small
//   that no dispatch ran would leave the backlog to grow forever.
//
// The clock is a parameter rather than a call to steady_clock, which is what
// makes the rule testable without sleeping.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <vector>

namespace dispatch_drain {

// Dispatches executed between budget checks. Must stay a power of two only in
// the sense that it is the granularity callers reason about; the check itself
// is a plain modulo.
inline constexpr std::size_t kBudgetCheckStride = 16;

// Execute from `carry` starting at `carry_pos`, stopping once `budget_ms` of
// injected-clock time has been spent. Returns the number executed.
//
//   carry / carry_pos  caller-owned persistent state (the partially consumed
//                      batch and how far into it we have got)
//   budget_ms          <= 0 means unbounded: drain the whole batch
//   refill(out)        fill `out` with the next batch; called only when the
//                      previous batch is fully consumed
//   execute(item)      run one item
//   now_ms()           milliseconds from any fixed epoch; only differences are
//                      used, so the epoch itself is irrelevant
template <typename Item, typename Refill, typename Execute, typename NowMs>
std::size_t run(std::vector<Item>& carry, std::size_t& carry_pos, double budget_ms,
                Refill&& refill, Execute&& execute, NowMs&& now_ms) {
    if (carry_pos >= carry.size()) {
        carry.clear();
        carry_pos = 0;
        refill(carry);
    }
    if (carry_pos >= carry.size()) {
        carry.clear();
        carry_pos = 0;
        return 0;
    }

    const double t0 = now_ms();
    std::size_t executed = 0;

    while (carry_pos < carry.size()) {
        execute(carry[carry_pos++]);
        ++executed;
        // Amortize the clock read - check the budget every stride dispatches.
        if (budget_ms > 0.0 && (executed % kBudgetCheckStride) == 0) {
            if (now_ms() - t0 >= budget_ms) break;
        }
    }

    if (carry_pos >= carry.size()) {
        carry.clear();
        carry_pos = 0;
    }
    return executed;
}

}  // namespace dispatch_drain
