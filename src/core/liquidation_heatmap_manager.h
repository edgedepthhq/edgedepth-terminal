#pragma once

#include "types/types.h"
#include "rendering/shader_heatmap_renderer.h"
#include "pb/messages.pb.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

struct LiqHeatmapKey {
    std::string exchange;
    std::string symbol;

    bool operator<(const LiqHeatmapKey& other) const {
        if (exchange != other.exchange) return exchange < other.exchange;
        return symbol < other.symbol;
    }
};

class LiquidationHeatmapManager {
public:
    LiquidationHeatmapManager() = default;

    // ─── Profile mode (current snapshot) ─────────────────────────
    void apply_update(const Terminal::Pair& pair, const pb::LiquidationHeatmapUpdate& update_pb);
    const Terminal::LiquidationHeatmapSnapshot* get_snapshot(const Terminal::Pair& pair) const;

    // ─── HL census layer ("Liq Levels HL", P2e) ──────────────────
    // REAL open positions' liq levels from the HL clearinghouseState census
    // (stream 34), keyed by UNDERLYING ({"hl","BTC"}). Same wire message as
    // the modelled snapshot but a SEPARATE store: an HL pair's modelled
    // heatmap and its census would otherwise collide on the same key, and
    // real-vs-modelled must never conflate. Straight field copy — no
    // obs-peak blending, timeline, or reach recompute (snapshot-only layer);
    // flow_intensity carries coverage_frac verbatim (0 must stay 0).
    void apply_census_update(const Terminal::Pair& pair, const pb::LiquidationHeatmapUpdate& update_pb);
    const Terminal::LiquidationHeatmapSnapshot* get_census_snapshot(const Terminal::Pair& pair) const;

    // ─── Timeline mode (historical GPU heatmap) ──────────────────
    // Called from message_handler for each historical snapshot in the batch
    void apply_timeline_snapshot(const Terminal::Pair& pair, const pb::LiquidationHeatmapUpdate& update_pb);
    // Called for live updates to finalize the current column
    void finalize_timeline_column(const Terminal::Pair& pair, const pb::LiquidationHeatmapUpdate& update_pb);
    // Get the shader renderer for rendering
    ShaderHeatmapRenderer* get_timeline_reconstructor(const Terminal::Pair& pair);
    bool has_timeline_data(const Terminal::Pair& pair) const;

    // ─── Discrete historical band tracks (MMT-style line render) ──
    // Aggregates the per-minute timeline (raw_timeline_protos_) into ONE track per price
    // level: when it first appeared, when it was last significant, and its peak USD — for
    // the enabled leverage tiers only. This is the data behind the discrete band-lines
    // (NOT the GPU field). Cached; rebuilt when proto count or leverage mask changes.
    struct BandTrack { double price = 0; int64_t first_ms = 0; int64_t last_ms = 0; int64_t consume_ms = 0; double peak_usd = 0; double last_usd = 0; };
    const std::vector<BandTrack>& get_band_history(const Terminal::Pair& pair, uint8_t mask);

    // ─── Observed @forceOrder events (WS4 "Observed" chart layer) ─────────
    // Discrete REAL liquidations, time-ordered per symbol — the fact layer
    // next to the estimated Field. Fed by the live LIQUIDATIONS stream; in
    // replay by the archive bundle / the liquidation_events DB seed (same
    // STREAM_LIQUIDATIONS payload). Bounded per symbol (oldest dropped).
    void add_observed_event(const Terminal::Pair& pair, const Terminal::Liquidation& liq);
    const std::vector<Terminal::Liquidation>& observed_events(const Terminal::Pair& pair) const;
    // Replay rewind (<<): drop events past the rewind target — they are the
    // replay's "future" now and would paint markers ahead of the playback head.
    void trim_observed_after(int64_t cutoff_ms);

    bool has_data(const Terminal::Pair& pair) const;
    void clear(const Terminal::Pair& pair);
    void clear_all();

    // Mark all timeline renderers for a symbol dirty (forces rebuild after batch load)
    void mark_timeline_dirty(const Terminal::Pair& pair);

    // Per-leverage filtering: bit 0=5x, 1=10x, 2=25x, 3=50x, 4=75x, 5=100x
    void set_leverage_mask(uint8_t mask);
    uint8_t get_leverage_mask() const { return leverage_mask_; }

    // ML reach-prob weighting: if true, timeline values are scaled by reach_prob
    // This makes the ML model influence which bands appear prominent in the timeline
    void set_ml_weight_timeline(bool enable) { ml_weight_timeline_ = enable; }
    bool get_ml_weight_timeline() const { return ml_weight_timeline_; }

private:
    // Profile mode: latest snapshot per symbol
    std::map<LiqHeatmapKey, Terminal::LiquidationHeatmapSnapshot> snapshots_;

    // HL census layer: latest real-position snapshot per underlying ({"hl","BTC"}).
    std::map<LiqHeatmapKey, Terminal::LiquidationHeatmapSnapshot> census_snapshots_;

    // Observed liquidation peak tracker — accumulates observed USD values
    // across updates so they don't visually disappear when the server
    // recalculates with a new mark price. Uses max(server, local) blending
    // with gradual client-side decay.
    struct ObsPeak {
        double obs_long_usd = 0.0;
        double obs_short_usd = 0.0;
        int64_t last_seen_ms = 0;  // timestamp of last server update with this data
    };
    // Map of price_bucket → ObsPeak per symbol
    std::map<LiqHeatmapKey, std::unordered_map<int64_t, ObsPeak>> obs_peaks_;

    // WS4 Observed markers: time-ordered discrete events per symbol.
    // Stream order is near-monotonic; a short back-scan insert keeps the
    // vector sorted so the renderer can binary-search the visible window.
    std::map<LiqHeatmapKey, std::vector<Terminal::Liquidation>> observed_events_;
    static constexpr size_t kMaxObservedEvents = 50000;   // per symbol; ~28d of BTC

    // Timeline mode: GPU shader renderer per symbol
    std::map<LiqHeatmapKey, std::unique_ptr<ShaderHeatmapRenderer>> timeline_heatmaps_;
    ShaderHeatmapRenderer& get_or_create_timeline(const LiqHeatmapKey& key);

    // Raw proto cache: stored so we can re-process with different leverage mask
    // without re-fetching from server. Sorted by timestamp for ordered replay.
    std::map<LiqHeatmapKey, std::map<int64_t, pb::LiquidationHeatmapUpdate>> raw_timeline_protos_;
    static constexpr size_t kMaxCachedProtos = 4000; // Match backend's 4000 candle limit

    // Cache for get_band_history's return value — rebuilt from the latest snapshot's backend rails
    // list only when that snapshot's timestamp changes (~1/min), NOT every render frame.
    std::map<LiqHeatmapKey, std::vector<BandTrack>> band_history_cache_;
    std::map<LiqHeatmapKey, int64_t> band_history_ts_;   // latest snapshot ts the cache was built for
    std::map<LiqHeatmapKey, uint8_t> band_history_mask_; // leverage mask the cache was built for (rebuild on toggle)

    // Rebuild timeline GPU heatmap from cached raw protos with current settings
    void rebuild_timeline(const LiqHeatmapKey& key);

    // Convert LiquidationHeatmapUpdate → synthetic HeatmapSnapshot for GPU pipeline
    // leverage_mask: bitmask of enabled leverage tiers (bit 0=5x, 1=10x, 2=25x, 3=50x, 4=75x, 5=100x)
    // ml_weight: if true, scale values by reach_prob from ML model
    // flow_intensity: MMT-style per-snapshot brightness scaling (0.0–1.0)
    // Uses per-tier independent normalization via tier_maxes_.
    pb::HeatmapSnapshot convert_to_heatmap_snapshot(
        const pb::LiquidationHeatmapUpdate& update_pb,
        uint8_t leverage_mask = 0xF,
        bool ml_weight = false,
        float flow_intensity = 1.0f);

    uint8_t leverage_mask_ = 0x3C;  // Default: 25x+50x+75x+100x (matches chart_widget defaults)
    bool ml_weight_timeline_ = false;  // Scale timeline by reach_prob

    // Phase 2a: Per-symbol last mark price used for reach recompute throttling.
    // Reach texture is only recomputed when mark moves >0.1% from this value.
    std::map<LiqHeatmapKey, double> last_reach_mark_;

    // V3: Per-tier running max for independent normalization.
    // Each tier is sqrt-normalized against its OWN max, so toggling one tier
    // doesn't change the brightness of other tiers. Index: 0=5x, 1=10x,
    // 2=25x, 3=50x, 4=75x, 5=100x.
    double tier_maxes_[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
};
