#include "liquidation_heatmap_manager.h"
#include "reach_math.h"
#include <algorithm>
#include <cmath>

// Use shared reach_math:: namespace for GBM functions.
// These aliases keep call sites unchanged:
using reach_math::gbm_reach_probability;
using reach_math::compute_reach_for_band;

// ═══════════════════════════════════════════════════════════════════
// Profile mode (current snapshot)
// ═══════════════════════════════════════════════════════════════════

void LiquidationHeatmapManager::apply_update(
    const Terminal::Pair& pair,
    const pb::LiquidationHeatmapUpdate& update_pb)
{
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto& snapshot = snapshots_[key];

    snapshot.timestamp_ms = update_pb.timestamp_ms();
    snapshot.mark_price = update_pb.mark_price();
    snapshot.total_long_risk_usd = update_pb.total_long_risk_usd();
    snapshot.total_short_risk_usd = update_pb.total_short_risk_usd();
    snapshot.net_risk_bias = update_pb.net_risk_bias();
    snapshot.band_width_pct = update_pb.band_width_pct();
    snapshot.flow_intensity = update_pb.flow_intensity() > 0.001 ? update_pb.flow_intensity() : 1.0;
    snapshot.bayesian_uncertainty = update_pb.bayesian_uncertainty();
    snapshot.hawkes_branching_ratio = update_pb.hawkes_branching_ratio();
    snapshot.hawkes_severity = update_pb.hawkes_severity();

    const int band_count = update_pb.price_mid_size();
    snapshot.bands.resize(band_count);

    for (int i = 0; i < band_count; ++i) {
        auto& band = snapshot.bands[i];
        band.price_mid     = update_pb.price_mid(i);
        band.obs_long_usd  = (i < update_pb.obs_long_usd_size())  ? update_pb.obs_long_usd(i)  : 0.0;
        band.obs_short_usd = (i < update_pb.obs_short_usd_size()) ? update_pb.obs_short_usd(i) : 0.0;
        band.est_long_usd  = (i < update_pb.est_long_usd_size())  ? update_pb.est_long_usd(i)  : 0.0;
        band.est_short_usd = (i < update_pb.est_short_usd_size()) ? update_pb.est_short_usd(i) : 0.0;
        band.intensity     = (i < update_pb.intensity_size())      ? update_pb.intensity(i)      : 0.0;
        band.reach_prob    = (i < update_pb.reach_prob_size())     ? update_pb.reach_prob(i)     : 1.0;
        band.cascade_prob  = (i < update_pb.cascade_prob_size())   ? update_pb.cascade_prob(i)   : 0.0;
        // Per-leverage tier breakdown
        band.est_5x_usd   = (i < update_pb.est_5x_usd_size())    ? update_pb.est_5x_usd(i)    : 0.0;
        band.est_10x_usd  = (i < update_pb.est_10x_usd_size())   ? update_pb.est_10x_usd(i)   : 0.0;
        band.est_25x_usd  = (i < update_pb.est_25x_usd_size())   ? update_pb.est_25x_usd(i)   : 0.0;
        band.est_50x_usd  = (i < update_pb.est_50x_usd_size())   ? update_pb.est_50x_usd(i)   : 0.0;
        band.est_75x_usd  = (i < update_pb.est_75x_usd_size())   ? update_pb.est_75x_usd(i)   : 0.0;
        band.est_100x_usd = (i < update_pb.est_100x_usd_size())  ? update_pb.est_100x_usd(i)  : 0.0;
        // Stop-loss density
        band.long_stop_density  = (i < update_pb.long_stop_density_size())  ? update_pb.long_stop_density(i)  : 0.0;
        band.short_stop_density = (i < update_pb.short_stop_density_size()) ? update_pb.short_stop_density(i) : 0.0;

        // Accumulate observed peaks - use max(server, local) so observed
        // liquidations don't visually disappear when mark price shifts
        if (band.obs_long_usd > 0.0 || band.obs_short_usd > 0.0) {
            const double bw = snapshot.band_width_pct / 100.0 * snapshot.mark_price;
            const int64_t price_bucket = (bw > 0) ? static_cast<int64_t>(band.price_mid / bw) : 0;
            auto& peak = obs_peaks_[key][price_bucket];
            peak.obs_long_usd = std::max(peak.obs_long_usd, band.obs_long_usd);
            peak.obs_short_usd = std::max(peak.obs_short_usd, band.obs_short_usd);
            peak.last_seen_ms = snapshot.timestamp_ms;
        }
    }

    // Merge accumulated observed peaks back into the snapshot.
    // For each band, if our local peak tracker has a higher observed value
    // at that price (from a previous update before mark price shifted),
    // use the higher value so observed data persists visually.
    {
        const double bw = snapshot.band_width_pct / 100.0 * snapshot.mark_price;
        auto& peaks = obs_peaks_[key];
        const int64_t now_ms = snapshot.timestamp_ms;

        for (auto& band : snapshot.bands) {
            const int64_t price_bucket = (bw > 0) ? static_cast<int64_t>(band.price_mid / bw) : 0;
            auto it = peaks.find(price_bucket);
            if (it != peaks.end()) {
                auto& peak = it->second;
                // Apply client-side decay: 3-minute half-life for visual persistence
                // This is faster than the backend's 2-hour decay because we want the
                // visual to track the server's authoritative values relatively quickly,
                // while still smoothing out the "jump" during mark price shifts.
                const double age_ms = static_cast<double>(now_ms - peak.last_seen_ms);
                const double half_life_ms = 180000.0; // 3 minutes
                const double decay = std::exp(-0.693 * age_ms / half_life_ms);
                const double decayed_long = peak.obs_long_usd * decay;
                const double decayed_short = peak.obs_short_usd * decay;

                // Use max of server value and our decayed peak
                band.obs_long_usd = std::max(band.obs_long_usd, decayed_long);
                band.obs_short_usd = std::max(band.obs_short_usd, decayed_short);

                // Clean up old peaks (>10 minutes)
                if (age_ms > 600000.0) {
                    peaks.erase(it);
                }
            }
        }
    }
    // Also update timeline with live data
    finalize_timeline_column(pair, update_pb);

    // Phase 2a: Recompute reach_timeline_ from CURRENT mark price.
    // This creates a dynamic "cone" that moves with price - bands near current
    // mark are bright, far away are dim, regardless of when they were created.
    // Throttled to >0.1% mark movement to avoid per-tick recompute.
    {
        const double mark = update_pb.mark_price();
        if (mark > 0.0) {
            auto& last_mark = last_reach_mark_[key];
            bool should_recompute = false;
            if (last_mark <= 0.0) {
                // First live update after load - recompute all historical data
                // from current mark to create the initial cone effect.
                last_mark = mark;
                should_recompute = true;
            } else {
                const double pct_move = std::abs((mark - last_mark) / last_mark);
                if (pct_move > 0.001) { // >0.1% move
                    should_recompute = true;
                    last_mark = mark;
                }
            }
            if (should_recompute) {
                auto* recon = get_timeline_reconstructor(pair);
                if (recon && recon->get_reach_modulation()) {
                    // Use sigma=1.20 (BTC-level) as default; 4h horizon
                    recon->recompute_reach_from_mark(mark, 1.20, 4.0);
                }
            }
        }
    }
}

const Terminal::LiquidationHeatmapSnapshot*
LiquidationHeatmapManager::get_snapshot(const Terminal::Pair& pair) const {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    const auto it = snapshots_.find(key);
    return (it != snapshots_.end()) ? &it->second : nullptr;
}

// ═══════════════════════════════════════════════════════════════════
// HL census layer ("Liq Levels HL", P2e) - real-position snapshots
// ═══════════════════════════════════════════════════════════════════

void LiquidationHeatmapManager::apply_census_update(
    const Terminal::Pair& pair,
    const pb::LiquidationHeatmapUpdate& update_pb)
{
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto& snapshot = census_snapshots_[key];

    snapshot.timestamp_ms = update_pb.timestamp_ms();
    snapshot.mark_price = update_pb.mark_price();
    snapshot.total_long_risk_usd = update_pb.total_long_risk_usd();
    snapshot.total_short_risk_usd = update_pb.total_short_risk_usd();
    snapshot.net_risk_bias = update_pb.net_risk_bias();
    snapshot.band_width_pct = update_pb.band_width_pct();
    // Census reuse: flow_intensity carries coverage_frac (0..1). Verbatim -
    // apply_update's >0.001→1.0 defaulting would turn "no coverage" into "full".
    snapshot.flow_intensity = update_pb.flow_intensity();

    const int band_count = update_pb.price_mid_size();
    snapshot.bands.resize(band_count);
    for (int i = 0; i < band_count; ++i) {
        auto& band = snapshot.bands[i];
        band.price_mid     = update_pb.price_mid(i);
        band.est_long_usd  = (i < update_pb.est_long_usd_size())  ? update_pb.est_long_usd(i)  : 0.0;
        band.est_short_usd = (i < update_pb.est_short_usd_size()) ? update_pb.est_short_usd(i) : 0.0;
        band.intensity     = (i < update_pb.intensity_size())     ? update_pb.intensity(i)     : 0.0;
        // REAL leverage tiers (not an assumption cohort): the LIQ LEV chips
        // filter on the actual leverage of the underlying positions.
        band.est_5x_usd   = (i < update_pb.est_5x_usd_size())   ? update_pb.est_5x_usd(i)   : 0.0;
        band.est_10x_usd  = (i < update_pb.est_10x_usd_size())  ? update_pb.est_10x_usd(i)  : 0.0;
        band.est_25x_usd  = (i < update_pb.est_25x_usd_size())  ? update_pb.est_25x_usd(i)  : 0.0;
        band.est_50x_usd  = (i < update_pb.est_50x_usd_size())  ? update_pb.est_50x_usd(i)  : 0.0;
        band.est_75x_usd  = (i < update_pb.est_75x_usd_size())  ? update_pb.est_75x_usd(i)  : 0.0;
        band.est_100x_usd = (i < update_pb.est_100x_usd_size()) ? update_pb.est_100x_usd(i) : 0.0;
    }
}

const Terminal::LiquidationHeatmapSnapshot*
LiquidationHeatmapManager::get_census_snapshot(const Terminal::Pair& pair) const {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    const auto it = census_snapshots_.find(key);
    return (it != census_snapshots_.end()) ? &it->second : nullptr;
}

bool LiquidationHeatmapManager::has_data(const Terminal::Pair& pair) const {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    return snapshots_.contains(key);
}

void LiquidationHeatmapManager::clear(const Terminal::Pair& pair) {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    snapshots_.erase(key);
    census_snapshots_.erase(key);
    obs_peaks_.erase(key);
    raw_timeline_protos_.erase(key);
    last_reach_mark_.erase(key);
    observed_events_.erase(key);
    if (auto it = timeline_heatmaps_.find(key); it != timeline_heatmaps_.end()) {
        it->second->clear();
    }
}

void LiquidationHeatmapManager::clear_all() {
    snapshots_.clear();
    census_snapshots_.clear();
    obs_peaks_.clear();
    raw_timeline_protos_.clear();
    timeline_heatmaps_.clear();
    last_reach_mark_.clear();
    observed_events_.clear();
}

// ═══════════════════════════════════════════════════════════════════
// WS4 Observed markers - discrete @forceOrder event store
// ═══════════════════════════════════════════════════════════════════

void LiquidationHeatmapManager::add_observed_event(const Terminal::Pair& pair,
                                                   const Terminal::Liquidation& liq) {
    if (liq.timestamp_ms <= 0) return;
    auto& evs = observed_events_[LiqHeatmapKey{pair.exchange, pair.symbol}];
    // Near-monotonic input: append, or short back-scan insert to keep sorted
    // (out-of-order arrivals are rare and shallow - reconnect races, replay
    // buffer boundaries). Renderer binary-searches on timestamp_ms.
    // Exact-duplicate skip makes replay rewind re-delivery idempotent (the
    // history buffer / backend may re-stream the trimmed window).
    auto is_dup_around = [&](std::vector<Terminal::Liquidation>::iterator pos) {
        for (auto it = pos; it != evs.begin();) {
            --it;
            if (it->timestamp_ms != liq.timestamp_ms) break;
            if (it->price == liq.price && it->qty == liq.qty && it->is_buy == liq.is_buy)
                return true;
        }
        for (auto it = pos; it != evs.end() && it->timestamp_ms == liq.timestamp_ms; ++it) {
            if (it->price == liq.price && it->qty == liq.qty && it->is_buy == liq.is_buy)
                return true;
        }
        return false;
    };
    if (evs.empty() || liq.timestamp_ms > evs.back().timestamp_ms) {
        evs.push_back(liq);
    } else {
        auto it = std::upper_bound(
            evs.begin(), evs.end(), liq.timestamp_ms,
            [](int64_t ts, const Terminal::Liquidation& e) { return ts < e.timestamp_ms; });
        if (is_dup_around(it)) return;
        evs.insert(it, liq);
    }
    if (evs.size() > kMaxObservedEvents) {
        // Amortized: shed the oldest 10% in one erase instead of one-per-insert.
        evs.erase(evs.begin(), evs.begin() + static_cast<long>(kMaxObservedEvents / 10));
    }
}

const std::vector<Terminal::Liquidation>&
LiquidationHeatmapManager::observed_events(const Terminal::Pair& pair) const {
    static const std::vector<Terminal::Liquidation> kEmpty;
    const auto it = observed_events_.find(LiqHeatmapKey{pair.exchange, pair.symbol});
    return it != observed_events_.end() ? it->second : kEmpty;
}

void LiquidationHeatmapManager::trim_observed_after(int64_t cutoff_ms) {
    for (auto& [key, evs] : observed_events_) {
        auto it = std::upper_bound(
            evs.begin(), evs.end(), cutoff_ms,
            [](int64_t ts, const Terminal::Liquidation& e) { return ts < e.timestamp_ms; });
        evs.erase(it, evs.end());
    }
}

// ═══════════════════════════════════════════════════════════════════
// Leverage mask change → rebuild timeline from cached raw protos
// ═══════════════════════════════════════════════════════════════════

void LiquidationHeatmapManager::set_leverage_mask(uint8_t mask) {
    if (mask == leverage_mask_) return;
    leverage_mask_ = mask;

    // Rebuild all timelines with new mask
    for (auto& [key, protos] : raw_timeline_protos_) {
        if (!protos.empty()) {
            rebuild_timeline(key);
        }
    }
}

void LiquidationHeatmapManager::rebuild_timeline(const LiqHeatmapKey& key) {
    // Clear existing GPU data
    auto it = timeline_heatmaps_.find(key);
    if (it != timeline_heatmaps_.end()) {
        it->second->clear();
    }

    // Reset per-tier maxes - they'll be recomputed from the proto replay
    for (int i = 0; i < 6; ++i) tier_maxes_[i] = 0.0;

    // Re-process all cached protos with current settings
    auto proto_it = raw_timeline_protos_.find(key);
    if (proto_it == raw_timeline_protos_.end()) return;

    auto& reconstructor = get_or_create_timeline(key);
    for (const auto& [ts, update_pb] : proto_it->second) {
        float fi = update_pb.flow_intensity() > 0.001
            ? static_cast<float>(update_pb.flow_intensity()) : 1.0f;
        pb::HeatmapSnapshot synthetic = convert_to_heatmap_snapshot(
            update_pb, leverage_mask_, ml_weight_timeline_, fi);
        reconstructor.process_snapshot(synthetic);

        // Phase 2a: Upload reach_prob for this column
        const int n = update_pb.price_mid_size();
        const int rn = update_pb.reach_prob_size();
        const double mark = update_pb.mark_price();
        const double bw = update_pb.band_width_pct() / 100.0 * mark;
        if (n > 0 && bw > 0) {
            std::unordered_map<double, float> reach_map;
            reach_map.reserve(n);
            for (int i = 0; i < n; ++i) {
                const double price = update_pb.price_mid(i);
                const double bucketed = std::floor(price / bw) * bw;
                const float proto_rp = (i < rn) ? static_cast<float>(update_pb.reach_prob(i)) : 0.0f;
                reach_map[bucketed] = compute_reach_for_band(price, mark, proto_rp, i < rn);
            }
            reconstructor.upload_reach_data(ts, reach_map);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Timeline mode (historical GPU heatmap)
// ═══════════════════════════════════════════════════════════════════

ShaderHeatmapRenderer& LiquidationHeatmapManager::get_or_create_timeline(const LiqHeatmapKey& key) {
    auto it = timeline_heatmaps_.find(key);
    if (it == timeline_heatmaps_.end()) {
        auto renderer = std::make_unique<ShaderHeatmapRenderer>();
        renderer->set_smooth_mode(false);
        renderer->set_colormap_type(ShaderHeatmapRenderer::ColormapType::Liquidation);
        // V8 render spike (2026-06-26): crisp MMT-style streaks on the discrete V5 band field.
        // Was GL_LINEAR "cloud" (blurry) + extend OFF; the discrete bands need GL_NEAREST to
        // read as crisp lines and extend-levels to draw them as continuous forward streaks.
        renderer->set_extend_levels(true);      // forward-fill levels into persistent horizontal streaks
        renderer->set_linear_filtering(false);  // GL_NEAREST - crisp discrete cells (was GL_LINEAR blur)
        it = timeline_heatmaps_.emplace(key, std::move(renderer)).first;
    }
    return *it->second;
}

void LiquidationHeatmapManager::apply_timeline_snapshot(
    const Terminal::Pair& pair,
    const pb::LiquidationHeatmapUpdate& update_pb)
{
    const LiqHeatmapKey key{pair.exchange, pair.symbol};

    // Cache raw proto for leverage mask rebuild
    auto& protos = raw_timeline_protos_[key];
    protos[update_pb.timestamp_ms()] = update_pb;
    // Evict oldest if over limit
    while (protos.size() > kMaxCachedProtos) {
        protos.erase(protos.begin());
    }

    auto& reconstructor = get_or_create_timeline(key);

    // Extract flow intensity (MMT-style per-snapshot brightness)
    float fi = update_pb.flow_intensity() > 0.001 ? static_cast<float>(update_pb.flow_intensity()) : 1.0f;

    // Convert liq update → synthetic HeatmapSnapshot → feed to GPU pipeline
    pb::HeatmapSnapshot synthetic = convert_to_heatmap_snapshot(update_pb, leverage_mask_, ml_weight_timeline_, fi);
    reconstructor.process_snapshot(synthetic);

    // Phase 2a: Upload reach_prob data to the parallel reach texture.
    // This is done AFTER process_snapshot so the column exists in the ring buffer.
    {
        const int n = update_pb.price_mid_size();
        const int rn = update_pb.reach_prob_size();
        const double mark = update_pb.mark_price();
        std::unordered_map<double, float> reach_map;
        reach_map.reserve(n);
        const double band_width = update_pb.band_width_pct() / 100.0 * mark;
        for (int i = 0; i < n; ++i) {
            const double price = update_pb.price_mid(i);
            const double bucketed = (band_width > 0)
                ? std::floor(price / band_width) * band_width : price;
            const float proto_rp = (i < rn) ? static_cast<float>(update_pb.reach_prob(i)) : 0.0f;
            reach_map[bucketed] = compute_reach_for_band(price, mark, proto_rp, i < rn);
        }
        if (!reach_map.empty()) {
            reconstructor.upload_reach_data(update_pb.timestamp_ms(), reach_map);
        }
    }
}

void LiquidationHeatmapManager::finalize_timeline_column(
    const Terminal::Pair& pair,
    const pb::LiquidationHeatmapUpdate& update_pb)
{
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    // Cache the live proto too, so get_band_history (discrete bands) and mask-rebuild
    // include live updates, not just the historical batch.
    {
        auto& protos = raw_timeline_protos_[key];
        protos[update_pb.timestamp_ms()] = update_pb;
        while (protos.size() > kMaxCachedProtos) protos.erase(protos.begin());
    }
    auto& reconstructor = get_or_create_timeline(key);
    // For the very first snapshot, use apply_timeline_snapshot path
    // (process_snapshot) which initializes the reconstructor's grid.
    // finalize_column requires an existing grid with >=2 snapshots.
    if (reconstructor.get_snapshot_count() < 2) {
        // Bootstrap: convert to synthetic HeatmapSnapshot and process
        float fi = update_pb.flow_intensity() > 0.001 ? static_cast<float>(update_pb.flow_intensity()) : 1.0f;
        pb::HeatmapSnapshot synthetic = convert_to_heatmap_snapshot(update_pb, leverage_mask_, ml_weight_timeline_, fi);
        reconstructor.process_snapshot(synthetic);
        // Fall through to reach upload below
    } else {
    // Convert to price→qty map using same power-law as historical data
    const double band_width = update_pb.band_width_pct() / 100.0 * update_pb.mark_price();
    if (band_width <= 0) return;
    // Flow intensity (MMT-style per-snapshot brightness)
    const float fi = update_pb.flow_intensity() > 0.001 ? static_cast<float>(update_pb.flow_intensity()) : 1.0f;
    std::unordered_map<double, float> price_qty_map;
    const int n = update_pb.price_mid_size();
    price_qty_map.reserve(n);
    // Check if per-leverage data is available
    const bool has_per_leverage = (update_pb.est_25x_usd_size() == n);
    for (int i = 0; i < n; ++i) {
        const double price = update_pb.price_mid(i);

        // V3: Per-tier independent normalization.
        // Each tier is sqrt-normalized against its OWN running max, then the
        // normalized [0-1] contributions are summed. This means toggling a
        // tier is purely additive - it adds/removes that tier's bands without
        // changing the brightness of other tiers. Matches MMT's behavior.
        //
        // Without this, toggling 5x/10x would change the global max and
        // renormalize ALL bands, making the whole chart shift brightness.
        double norm_value = 0.0;

        if (has_per_leverage) {
            // Process each enabled tier independently
            struct TierData { double usd; int tier_idx; };
            TierData tiers[] = {
                { (leverage_mask_ & 0x01) ? update_pb.est_5x_usd(i)   : 0.0, 0 },
                { (leverage_mask_ & 0x02) ? update_pb.est_10x_usd(i)  : 0.0, 1 },
                { (leverage_mask_ & 0x04) ? update_pb.est_25x_usd(i)  : 0.0, 2 },
                { (leverage_mask_ & 0x08) ? update_pb.est_50x_usd(i)  : 0.0, 3 },
                { (leverage_mask_ & 0x10) ? update_pb.est_75x_usd(i)  : 0.0, 4 },
                { (leverage_mask_ & 0x20) ? update_pb.est_100x_usd(i) : 0.0, 5 },
            };
            // V9: Absolute log-USD brightness (replaces per-tier running-max
            // normalization, which crushed the dense field to the few dominant
            // bands - the rails-only look; see LIQ_MAP_PLAN render reframe). Sum the
            // enabled tiers' USD at this band, then log-scale against the $500 floor
            // so the dim background survives and the dominant levels stay bright.
            // No running max → stable, doesn't shift brightness frame-to-frame.
            double band_usd = 0.0;
            for (const auto& t : tiers) {
                if (t.usd > 500.0) band_usd += t.usd;
            }
            if (band_usd > 500.0) {
                norm_value += std::log10(band_usd / 500.0);
            }
        } else {
            // Fallback: no per-tier data, use combined
            double est_usd = 0.0;
            if (i < update_pb.est_long_usd_size())  est_usd += update_pb.est_long_usd(i);
            if (i < update_pb.est_short_usd_size()) est_usd += update_pb.est_short_usd(i);
            norm_value = (est_usd > 500.0) ? std::sqrt(est_usd) : 0.0;
        }

        // ML reach-prob weighting
        if (ml_weight_timeline_ && norm_value > 0.001 && i < update_pb.reach_prob_size()) {
            const double reach = std::max(update_pb.reach_prob(i), 0.05);
            norm_value *= std::sqrt(reach);
        }
        norm_value *= fi;
        const double bucketed = std::floor(price / band_width) * band_width;
        if (norm_value > 0.001) {
            // Directional encoding: negative values = long liq zones (below mark)
            // The shader uses sign(value) to choose warm vs cool colormap
            if (price < update_pb.mark_price()) {
                price_qty_map[bucketed] -= static_cast<float>(norm_value);
            } else {
                price_qty_map[bucketed] += static_cast<float>(norm_value);
            }
        }
    }
    reconstructor.finalize_column(update_pb.timestamp_ms(), price_qty_map);
    } // end else (snapshot_count >= 2)

    // Phase 2a: Upload reach_prob to reach texture (both bootstrap and finalize paths)
    {
        const int n = update_pb.price_mid_size();
        const int rn = update_pb.reach_prob_size();
        const double mark = update_pb.mark_price();
        const double band_width_r = update_pb.band_width_pct() / 100.0 * mark;
        if (n > 0 && band_width_r > 0) {
            std::unordered_map<double, float> reach_map;
            reach_map.reserve(n);
            for (int i = 0; i < n; ++i) {
                const double price = update_pb.price_mid(i);
                const double bucketed = std::floor(price / band_width_r) * band_width_r;
                const float proto_rp = (i < rn) ? static_cast<float>(update_pb.reach_prob(i)) : 0.0f;
                reach_map[bucketed] = compute_reach_for_band(price, mark, proto_rp, i < rn);
            }
            reconstructor.upload_reach_data(update_pb.timestamp_ms(), reach_map);
        }
    }
}

ShaderHeatmapRenderer* LiquidationHeatmapManager::get_timeline_reconstructor(const Terminal::Pair& pair) {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto it = timeline_heatmaps_.find(key);
    return (it != timeline_heatmaps_.end()) ? it->second.get() : nullptr;
}

bool LiquidationHeatmapManager::has_timeline_data(const Terminal::Pair& pair) const {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto it = timeline_heatmaps_.find(key);
    return it != timeline_heatmaps_.end() && it->second->has_data();
}

const std::vector<LiquidationHeatmapManager::BandTrack>&
LiquidationHeatmapManager::get_band_history(const Terminal::Pair& pair, uint8_t mask) {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto& cache = band_history_cache_[key];
    const auto pit = raw_timeline_protos_.find(key);
    if (pit == raw_timeline_protos_.end() || pit->second.empty()) { cache.clear(); return cache; }

    // The backend RailTracker ships the AUTHORITATIVE rail lifecycle on the latest snapshot's
    // `rails` list: each rail is one episode at a (side, price) level, formed_ms → consumed_ms
    // (the first LAST-PRICE wick-through; 0 = still pending/standing). Each rail also carries a
    // per-leverage peak breakdown (peak_5x..100x); we sum ONLY the tiers enabled in `mask` so the
    // LIQ LEV toggle isolates e.g. the fragile near-price 100x cluster from the structural low-lev
    // levels. The list is cumulative, so the latest snapshot holds the full picture.
    const pb::LiquidationHeatmapUpdate& latest = pit->second.rbegin()->second;
    const int64_t latest_ts = latest.timestamp_ms();
    // Rebuild when a NEW snapshot arrives (rails change ~1/min) OR the leverage mask changes - both
    // alter per-rail magnitude/visibility, so a stale cache would silently ignore a toggle.
    if (latest_ts != 0 && band_history_ts_[key] == latest_ts && band_history_mask_[key] == mask) return cache;
    band_history_ts_[key] = latest_ts;
    band_history_mask_[key] = mask;
    cache.clear();
    cache.reserve(latest.rails_size());
    for (int i = 0; i < latest.rails_size(); ++i) {
        const pb::LiqRail& r = latest.rails(i);
        if (r.price() <= 0.0) continue;
        // Sum the enabled leverage tiers (mask bits 0x01=5x .. 0x20=100x). If the snapshot predates
        // per-tier rails (all tier fields 0 - e.g. older/archive baked data), fall back to the
        // blended peak_usd so rails still show (unfiltered) rather than vanishing.
        const double tier_all = r.peak_5x_usd() + r.peak_10x_usd() + r.peak_25x_usd()
                              + r.peak_50x_usd() + r.peak_75x_usd() + r.peak_100x_usd();
        double peak;
        if (tier_all <= 0.0) {
            peak = r.peak_usd();                       // no per-tier info → blended fallback
        } else {
            peak = 0.0;
            if (mask & 0x01) peak += r.peak_5x_usd();
            if (mask & 0x02) peak += r.peak_10x_usd();
            if (mask & 0x04) peak += r.peak_25x_usd();
            if (mask & 0x08) peak += r.peak_50x_usd();
            if (mask & 0x10) peak += r.peak_75x_usd();
            if (mask & 0x20) peak += r.peak_100x_usd();
            if (peak <= 0.0) continue;                 // per-tier present but none enabled → drop rail
        }
        BandTrack b;
        b.price      = r.price();
        b.first_ms   = r.formed_ms();
        b.consume_ms = r.consumed_ms();   // 0 = pending (still standing → draws to the live edge)
        b.last_ms    = latest_ts;
        b.peak_usd   = peak;
        b.last_usd   = peak;
        cache.push_back(b);
    }
    return cache;
}

void LiquidationHeatmapManager::mark_timeline_dirty(const Terminal::Pair& pair) {
    const LiqHeatmapKey key{pair.exchange, pair.symbol};
    auto it = timeline_heatmaps_.find(key);
    if (it != timeline_heatmaps_.end()) {
        it->second->mark_dirty();
    }
}

// Convert LiquidationHeatmapUpdate → synthetic HeatmapSnapshot
// so we can reuse the ShaderHeatmapRenderer pipeline.
// leverage_mask: bitmask of enabled tiers (bit 0=10x, 1=25x, 2=50x, 3=100x)
pb::HeatmapSnapshot LiquidationHeatmapManager::convert_to_heatmap_snapshot(
    const pb::LiquidationHeatmapUpdate& update_pb,
    uint8_t leverage_mask,
    bool ml_weight,
    float flow_intensity)
{
    pb::HeatmapSnapshot snapshot;
    snapshot.set_timestamp_ms(update_pb.timestamp_ms());
    snapshot.set_symbol(update_pb.pair().symbol());
    snapshot.set_exchange(update_pb.pair().exchange());

    const double mark = update_pb.mark_price();
    const double band_width = update_pb.band_width_pct() / 100.0 * mark;
    snapshot.set_bucket_size(band_width > 0 ? band_width : 1.0);
    snapshot.set_tick_size(band_width > 0 ? band_width : 1.0);

    // All tiers unchecked = empty snapshot (matches MMT: nothing visible)
    if (leverage_mask == 0) return snapshot;

    const int n = update_pb.price_mid_size();

    // Check if per-leverage data is available (use 25x as probe - always populated)
    const bool has_per_leverage = (update_pb.est_25x_usd_size() == n);

    // Track price range
    double p_min = 1e18, p_max = -1e18;

    for (int i = 0; i < n; ++i) {
        const double price = update_pb.price_mid(i);
        snapshot.add_prices(price);

        // Observed liquidations (always included with 3x boost for visibility)
        double obs_usd = 0.0;
        if (i < update_pb.obs_long_usd_size())  obs_usd += update_pb.obs_long_usd(i);
        if (i < update_pb.obs_short_usd_size()) obs_usd += update_pb.obs_short_usd(i);

        // Estimated: always use per-leverage filtering when available.
        // This ensures toggling tiers only affects bands at those tiers'
        // price distances - never changes values of other tiers' bands.
        // Fallback to combined totals only for old protos without per-tier data.
        // V3: Per-tier independent normalization (same as live path)
        double norm_value = 0.0;
        if (has_per_leverage) {
            struct TierData { double usd; int tier_idx; };
            TierData tiers[] = {
                { (leverage_mask & 0x01) ? update_pb.est_5x_usd(i)   : 0.0, 0 },
                { (leverage_mask & 0x02) ? update_pb.est_10x_usd(i)  : 0.0, 1 },
                { (leverage_mask & 0x04) ? update_pb.est_25x_usd(i)  : 0.0, 2 },
                { (leverage_mask & 0x08) ? update_pb.est_50x_usd(i)  : 0.0, 3 },
                { (leverage_mask & 0x10) ? update_pb.est_75x_usd(i)  : 0.0, 4 },
                { (leverage_mask & 0x20) ? update_pb.est_100x_usd(i) : 0.0, 5 },
            };
            // V9: Absolute log-USD brightness (replaces the per-tier decaying-max
            // + pow(0.6) normalization, which compressed the dense field to the few
            // dominant bands - the rails-only look). Sum the enabled tiers' USD at this
            // band, then log-scale against the $500 floor: dim background survives,
            // dominant levels stay bright, no running-max twitch. Kept identical to the
            // live finalize_column path so live == timeline by construction.
            double band_usd = 0.0;
            for (const auto& t : tiers) {
                if (t.usd > 500.0) band_usd += t.usd;
            }
            if (band_usd > 500.0) {
                norm_value += std::log10(band_usd / 500.0);
            }
        } else {
            double est_usd = 0.0;
            if (i < update_pb.est_long_usd_size())  est_usd += update_pb.est_long_usd(i);
            if (i < update_pb.est_short_usd_size()) est_usd += update_pb.est_short_usd(i);
            norm_value = (est_usd > 500.0) ? std::sqrt(est_usd) : 0.0;
        }

        // ML reach-prob weighting
        if (ml_weight && norm_value > 0.001 && i < update_pb.reach_prob_size()) {
            const double reach = std::max(update_pb.reach_prob(i), 0.05);
            norm_value *= std::sqrt(reach);
        }
        norm_value *= flow_intensity;
        // V6: Unified colormap - no directional encoding. Both long and short
        // liq use the same positive values and same colormap. Direction is
        // conveyed by position relative to mark price, not by color.
        snapshot.add_bid_qty(norm_value);
        snapshot.add_ask_qty(0.0);
        if (price < p_min) p_min = price;
        if (price > p_max) p_max = price;
    }

    snapshot.set_price_min(p_min);
    snapshot.set_price_max(p_max + band_width);
    snapshot.set_mode("liq");

    return snapshot;
}
