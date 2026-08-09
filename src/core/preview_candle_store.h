#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// preview_candle_store.h - replay scrub-preview candles (ghost render source)
//
// Owns the FULL replay window's candles at a preview timeframe, fetched once per
// replay session (get_replay_preview_candles) and re-fetched when the chart TF
// changes. This is the ONE sanctioned future-leak in the client: the data is
// read ONLY by the chart's ghost scrub-preview pass and the transport hover
// chip - never by CandleManager, indicators, or the liq layers. Batches arrive
// on the dedicated STREAM_REPLAY_PREVIEW_CANDLES id (33), so they cannot be
// dispatched into the main candle series by construction.
//
// Lifetime: owned by the replay DataContext (never exists in live mode) and
// dies with it on replay stop. Threading: writes land on the main thread (the
// replay MessageContext routes inline; the queued branch in message_handler
// covers the threaded case), reads happen during render. SoA mirrors
// CandleManager's cache so the ghost pass can reuse the same binary-search +
// culling pattern with zero per-frame allocation.
// ═══════════════════════════════════════════════════════════════════════════════

#include <vector>
#include <cstdint>
#include "../types/types.h"

class StreamManager;

class PreviewCandleStore {
public:
    // Replace contents with a served batch. timeframe_sec is the SERVED
    // timeframe from the response envelope: the backend may have snapped the
    // requested TF up to fit its batch cap, and the served value is authoritative.
    void apply_batch(int64_t timeframe_sec, std::vector<Terminal::Candle>&& batch);

    void clear();

    // Ask the box for the preview batch at (near) the chart's timeframe.
    // No-op when a fetch for the same TF is already in flight or served.
    void request(StreamManager& streams, const Terminal::Pair& pair, int64_t timeframe_sec);

    bool ready() const { return !timestamps_.empty(); }
    int64_t timeframe_sec() const { return timeframe_sec_; }

    const std::vector<double>& timestamps() const { return timestamps_; }
    const std::vector<double>& opens()      const { return opens_; }
    const std::vector<double>& highs()      const { return highs_; }
    const std::vector<double>& lows()       const { return lows_; }
    const std::vector<double>& closes()     const { return closes_; }

    // Close price at-or-before ts_ms (hover chip). False when not covered.
    bool close_at(int64_t ts_ms, double& out) const;

    // Min low / max high across candles overlapping [t0_ms, t1_ms] (preview
    // y-fit). False when the range has no preview candles.
    bool minmax_in_range(double t0_ms, double t1_ms, double& lo, double& hi) const;

private:
    int64_t timeframe_sec_ = 0;    // served TF (0 = nothing served yet)
    int64_t requested_tf_sec_ = 0; // last requested TF (in-flight dedupe)
    std::vector<double> timestamps_;
    std::vector<double> opens_;
    std::vector<double> highs_;
    std::vector<double> lows_;
    std::vector<double> closes_;
};
