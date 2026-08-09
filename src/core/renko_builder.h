#pragma once

#include <vector>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════════════
// RenkoBuilder — price-driven bricks from the candle CLOSE series
//
// Renko abandons time: X = brick ordinal, price drives the chart. A brick is a
// fixed-size step; a reversal needs 2x the brick size (traditional Renko). This
// builder is a PURE transform over the close series + a resolved brick size, so
// it is deterministic and replay-identical (the platform's replay/lesson/pack
// invariants depend on that).
//
// Committed bricks build from CLOSED candles only (rebuilt when the closed set /
// brick size / timeframe change). The live building candle folds in per frame
// via fold_building() into a separate provisional list, so the rightmost forming
// brick can appear/disappear as price moves without dirtying the committed cache.
// ═══════════════════════════════════════════════════════════════════════════════

struct RenkoBrick {
    bool     up;            // true = up brick (close = open + size), false = down
    double   open;          // brick open price (on the brick_size grid)
    double   close;         // brick close price = open +/- brick_size
    int64_t  close_time_ms; // source candle timestamp — the non-uniform axis label
};

class RenkoBuilder {
public:
    // Hard cap so a pathological (tiny) brick size on a huge move cannot blow up
    // memory or stall the frame. ~ the candle cap order of magnitude.
    static constexpr size_t MAX_BRICKS = 200000;

    // Build/extend the committed brick vector from parallel (closes, close_times)
    // and a resolved brick size (price units). APPEND-ONLY: bricks already
    // published from earlier candles are NEVER recomputed, so a later mutation to
    // an already-consumed candle's close (which the client does during replay:
    // finalize-replace, late trades, 15s-tick merges) can never un-print a brick.
    // Only candles NEWER than the last consumed one are folded on. A full rebuild
    // happens only when the brick size changes, on a seek/timeframe reload (the
    // last-consumed candle's TIMESTAMP no longer matches), or a history prepend.
    // close-based, traditional 2x reversal, seed snapped to a brick_size grid.
    void build(const std::vector<double>& closes,
               const std::vector<int64_t>& close_times,
               double brick_size);

    // Provisional bricks from the live price on top of the committed terminal
    // state, WITHOUT mutating the committed sequence. Cleared + refilled each
    // call — a retrace simply drops the provisional brick next frame.
    void fold_building(double price, int64_t time_ms);

    const std::vector<RenkoBrick>& bricks()   const { return bricks_; }    // committed
    const std::vector<RenkoBrick>& building() const { return building_; }  // provisional
    double brick_size() const { return brick_size_; }
    size_t total_count() const { return bricks_.size() + building_.size(); }

    // Brick at a global index (0 .. total_count()-1), spanning committed then
    // provisional. Caller must bounds-check via total_count().
    const RenkoBrick& at(size_t i) const {
        return (i < bricks_.size()) ? bricks_[i] : building_[i - bricks_.size()];
    }

    void clear();

    // ── Auto brick-size heuristic (price units) ────────────────────────────
    // Target a brick ~0.15% of price, expressed in ticks, rounded to a nice
    // 1/2/5 x 10^n count so the default is legible and stable. Returns the
    // resolved TICK count (multiply by tick_size for price). 0 if inputs invalid
    // (no price yet) so the caller can defer resolving until candles load.
    static int auto_brick_ticks(double reference_price, double tick_size);

private:
    std::vector<RenkoBrick> bricks_;
    std::vector<RenkoBrick> building_;
    double brick_size_ = 0.0;
    // Committed terminal state (the last emitted brick's open/close level + the
    // current direction: +1 up, -1 down, 0 = unseeded). fold_building() runs the
    // same stepper from a COPY of this.
    double open_  = 0.0;
    double close_ = 0.0;
    int    dir_   = 0;
    bool   seeded_ = false;
    // Append-only bookkeeping: how many candles have been folded into bricks_,
    // and the timestamp of the last one. A later call that still ends on that
    // same timestamp appends only the newer candles (and ignores any mutation to
    // the already-consumed tail); a different timestamp / fewer candles / a new
    // brick size triggers a full rebuild.
    size_t  consumed_  = 0;
    int64_t last_time_ = 0;
};
