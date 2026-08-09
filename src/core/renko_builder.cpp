#include "core/renko_builder.h"

#include <algorithm>
#include <cmath>

namespace {

// Advance one Renko step for a single price observation. Mutates the running
// (open, close, dir) state and appends any emitted bricks to `out`. Shared by
// build() (committed state) and fold_building() (a copy of the state), so the
// live building brick and the finalized bricks use IDENTICAL rules.
//
// Traditional Renko:
//   - continuation needs +1 brick_size past the last close,
//   - a reversal needs 2x against the trend; the first opposite brick opens at
//     the PRIOR brick's open (close -/+ B), which is the characteristic 2-brick
//     turn. From an unseeded state (dir == 0) the first move is a plain 1x step
//     that sets the direction.
void renko_step(double& open, double& close, int& dir,
                double B, double price, int64_t t,
                std::vector<RenkoBrick>& out, size_t cap) {
    if (B <= 0.0) return;
    for (;;) {
        if (out.size() >= cap) return;
        if (dir >= 0) {
            // Up continuation (also the seed's first up move when dir == 0).
            if (price >= close + B) {
                const double o = close, c = close + B;
                out.push_back({true, o, c, t});
                open = o; close = c; dir = 1;
                continue;
            }
            // Reversal down: 2x when trending up, 1x from the unseeded seed.
            const double rev_thresh = (dir > 0) ? (close - 2.0 * B) : (close - B);
            if (price <= rev_thresh) {
                const double base = (dir > 0) ? (close - B) : close;  // prior brick's open
                const double o = base, c = base - B;
                out.push_back({false, o, c, t});
                open = o; close = c; dir = -1;
                continue;
            }
            return;
        } else {
            // Down continuation.
            if (price <= close - B) {
                const double o = close, c = close - B;
                out.push_back({false, o, c, t});
                open = o; close = c; dir = -1;
                continue;
            }
            // Reversal up: needs 2x against the down trend.
            if (price >= close + 2.0 * B) {
                const double base = close + B;  // prior down brick's open
                const double o = base, c = base + B;
                out.push_back({true, o, c, t});
                open = o; close = c; dir = 1;
                continue;
            }
            return;
        }
    }
}

}  // namespace

void RenkoBuilder::build(const std::vector<double>& closes,
                         const std::vector<int64_t>& close_times,
                         double brick_size) {
    const size_t n = std::min(closes.size(), close_times.size());

    // Can we APPEND (fold only the candles newer than the ones already consumed)?
    // Requires: same brick size, already seeded, at least as many candles as
    // consumed, and the last-consumed candle still sitting at its old index with
    // its old timestamp (so the tail was extended, not shifted by a prepend or
    // reset). A mutation to an already-consumed candle's CLOSE keeps its
    // timestamp, so it passes this check and is intentionally left un-recomputed.
    const bool can_append = seeded_ && brick_size == brick_size_ &&
                            consumed_ > 0 && n >= consumed_ &&
                            close_times[consumed_ - 1] == last_time_;

    if (!can_append) {
        // Full (re)build - brick size change, seek/timeframe reload, or prepend.
        bricks_.clear();
        building_.clear();
        brick_size_ = brick_size;
        open_ = close_ = 0.0;
        dir_ = 0;
        seeded_ = false;
        consumed_ = 0;
        last_time_ = 0;
        if (brick_size <= 0.0 || n == 0) return;
        // Seed the first brick level on a brick_size grid so brick price levels
        // are stable across history-load prepends (every level is an exact
        // multiple of brick_size regardless of which candle happens to be first).
        open_ = close_ = std::round(closes[0] / brick_size) * brick_size;
        seeded_ = true;
    }

    // Fold the not-yet-consumed candles onto the committed sequence (append-only).
    for (size_t i = consumed_; i < n; ++i) {
        renko_step(open_, close_, dir_, brick_size_, closes[i], close_times[i],
                   bricks_, MAX_BRICKS);
        consumed_  = i + 1;
        last_time_ = close_times[i];
        if (bricks_.size() >= MAX_BRICKS) return;
    }
    consumed_  = n;
    if (n > 0) last_time_ = close_times[n - 1];
}

void RenkoBuilder::fold_building(double price, int64_t time_ms) {
    building_.clear();
    if (!seeded_ || brick_size_ <= 0.0) return;
    // Run the stepper from a COPY of the committed terminal state so provisional
    // bricks never mutate the committed sequence.
    double o = open_, c = close_;
    int d = dir_;
    renko_step(o, c, d, brick_size_, price, time_ms, building_, MAX_BRICKS);
}

void RenkoBuilder::clear() {
    bricks_.clear();
    building_.clear();
    brick_size_ = 0.0;
    open_ = close_ = 0.0;
    dir_ = 0;
    seeded_ = false;
    consumed_ = 0;
    last_time_ = 0;
}

int RenkoBuilder::auto_brick_ticks(double reference_price, double tick_size) {
    if (reference_price <= 0.0 || tick_size <= 0.0) return 0;
    // Target a brick ~0.15% of price (mid of the 0.1-0.25% band in the plan).
    constexpr double kBrickFraction = 0.0015;
    const double raw_ticks = (reference_price * kBrickFraction) / tick_size;
    if (raw_ticks <= 1.0) return 1;
    // Round to a nice 1/2/5 x 10^n tick count for a legible, stable default.
    const double mag  = std::pow(10.0, std::floor(std::log10(raw_ticks)));
    const double norm = raw_ticks / mag;  // [1, 10)
    const double nice = (norm < 1.5) ? 1.0 : (norm < 3.5) ? 2.0
                      : (norm < 7.5) ? 5.0 : 10.0;
    const int ticks = static_cast<int>(std::llround(nice * mag));
    return std::max(1, ticks);
}
