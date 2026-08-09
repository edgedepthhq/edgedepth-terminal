#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// indicator_series.h — IndicatorSeriesManager (the shared SeriesCache)
//
// Indicators V1 §5.6: the client-side cache for stream-backed indicator
// series — per (symbol, series) sorted vectors keyed by absolute ms,
// dedup on equal timestamp (replace), trim_after for replay rewinds,
// clear_all on seek. The observed_events semantics, generalized.
//
// VPIN is the pathfinder occupant; hawkes/cfti/flow-deviation add their
// typed stores here in S5–S7. Stored points are the FINALIZED grain
// (parity grain rule): every VPINStateUpdate is a volume-bucket
// completion, so live/replay/history/archive all converge on the same
// series of record.
//
// Threading: written ONLY on the main thread (message_handler routes
// VPIN frames through the DispatchQueue, same as heatmap/pattern/debug),
// read by widgets during render. No locking needed.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Series {

    // One finalized VPIN volume-bucket print (vpin_buckets grain).
    struct VPINPoint {
        int64_t ts_ms      = 0;   // bucket end time (volume-clock print)
        float   vpin       = 0.0f;
        float   imbalance  = 0.0f;
        float   hmm_conf   = 0.0f;
        int16_t hmm_state  = -1;  // -1 = threshold / pre-M1 era (no HMM)
        int8_t  regime     = 0;   // 0 Normal · 1 Elevated · 2 High · 3 Critical
    };

    // Map the wire regime string to the strip/color index. Unknown → 0.
    int8_t regime_index(const char* regime);

} // namespace Series

class IndicatorSeriesManager {
public:
    // Insert one point, keeping the vector sorted by ts_ms. Equal timestamp
    // replaces in place (idempotent re-delivery — replay rewind redispatch,
    // history overlapping the live stream).
    void add_vpin(const std::string& symbol, const Series::VPINPoint& p);

    // Bulk insert (historical batch). Points are expected ascending but the
    // path tolerates disorder (falls back to per-point insert).
    void add_vpin_batch(const std::string& symbol,
                        const Series::VPINPoint* pts, size_t n);

    // Render-side read. Never null; empty vector when no data.
    const std::vector<Series::VPINPoint>& vpin(const std::string& symbol) const;

    // Bumped on every mutation of the symbol's series — widgets compare to
    // repopulate their render caches only when something actually changed.
    uint64_t vpin_revision(const std::string& symbol) const;

    // Oldest point's ts_ms (0 = no data). Drives the chart's
    // chunk-until-covered history loop (the 5000-count response cap means
    // one request never guarantees full window coverage).
    int64_t vpin_oldest_ts(const std::string& symbol) const;

    // Replay hygiene — mirrors LiquidationHeatmapManager::observed_events:
    //   rewind (<<)  → trim_after(cutoff): points past the target are the
    //                  replay's future now.
    //   seek         → clear_all(): backend re-seeds the full window.
    void trim_after(int64_t cutoff_ms);
    void clear_all();

private:
    struct VPINSeries {
        std::vector<Series::VPINPoint> points;
        uint64_t revision = 0;
    };
    std::unordered_map<std::string, VPINSeries> vpin_;
    static const std::vector<Series::VPINPoint> kEmptyVPIN;
};
