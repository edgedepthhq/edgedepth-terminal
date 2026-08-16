#pragma once

/*
 * Liquidation projection Field: the dense candle x leverage backdrop.
 *
 * Owns the whole Field subsystem that used to live inside ChartWidget: the
 * cached time x price segment build, its rebuild signature, every tuning knob
 * the settings panel binds to, and both render paths (the GPU texture quad and
 * the immediate-mode rect fallback).
 *
 * The cache is the source of truth and is shared. render() draws it, and the
 * Liq Profile marginal reads the same segments through segments() + the
 * norm/bucket accessors, so the profile keeps working when the Field layer
 * itself is toggled off.
 *
 * PINNED STATISTIC. rebuild() computes age-decayed anomaly-weighted deposit
 * sums on a log-price grid, then maps them through a dual-percentile log
 * normalization. Those MECHANICS are fixed. Change knob VALUES, never the
 * mechanics: the normalization is adaptive, so reshaping the intensity
 * distribution makes the map re-normalize and repaints the entire field.
 *
 * The caller decides whether the Field is allowed to draw at all. Chart types
 * with no time axis (TPO, Renko) must not call render().
 */

#include <cstdint>
#include <vector>

#include "core/app_context.h"
#include "rendering/liq_field_texture_renderer.h"

class LiqFieldRenderer {
public:
    // Tunables. Grouped in one struct so the settings panel can bind directly
    // to the members and so the knob set is greppable in one place.
    //
    // Knobs marked BAKED are consumed while building the cache, so changing one
    // has no effect until the cache is rebuilt. The settings panel must call
    // invalidate() after touching them. The rest are read at render time and
    // take effect on the next frame.
    struct Knobs {
        // Render-time look.
        float gamma       = 2.2f;    // shaping applied to the LOG-mapped t
        float opacity     = 0.90f;   // global alpha multiplier
        float floor_t     = 0.02f;   // rendered-intensity noise floor
        float alpha_floor = 0.08f;   // alpha = opacity*(floor + (1-floor)*t^pow)
        float alpha_pow   = 1.5f;
        float min_row_px  = 2.0f;    // minimum painted row height in px
        bool  extend      = true;    // forward cascade past the live edge
        float cascade_alpha = 0.78f; // projection alpha vs the history alpha
        bool  use_texture = true;    // A/B: GPU quad, false = rect fallback

        // BAKED into the cache.
        int   kernel     = 0;        // deposit kernel half-width in buckets
        float bps        = 6.0f;     // bucket height, bps of its own price
        float lo_pct     = 0.30f;    // intensity LOW clip percentile
        float hi_pct     = 0.9985f;  // intensity PEAK clip percentile
        float wcap       = 6.0f;     // cap on per-candle relative-volume weight
        float wbase      = 0.7f;     // anomaly baseline for the excess term
        float wfloor     = 0.15f;    // dim floor trace, near tiers only
        float halflife_h = 16.0f;    // age-decay half-life in hours, <=0 off
    };

    // Max buckets fused into one segment by the build-time merge pass. Also
    // bounds how far below the viewport a Y-culling scan has to start, which is
    // why the Profile marginal needs it too.
    static constexpr int kMaxMergeRun = 64;

    // Sentinel end_ms for a still-standing (never consumed) segment.
    static constexpr int64_t kSegPending = LiqFieldTextureRenderer::kSegPending;

    explicit LiqFieldRenderer(const AppContext& ctx) : ctx_(ctx) {}

    // Rebuild the cache if its inputs changed (closed-candle set, leverage
    // mask, timeframe). Cheap no-op when nothing changed. Safe to call more
    // than once per frame: both the Field and the Profile call it.
    void ensure_cache();

    // Force the next ensure_cache() to rebuild. Call after changing a BAKED
    // knob.
    void invalidate() { sig_ts_ = -1; }

    // Draw the Field. Assumes an open ImPlot plot and that the caller has
    // already established the chart type permits time overlays.
    void render();

    // Read access for the Liq Profile marginal, which is the price-marginal of
    // this same segment cache and inherits its fixed log map rather than
    // computing its own.
    [[nodiscard]] const std::vector<LiqFieldSeg>& segments() const { return segs_; }
    [[nodiscard]] double bucket_width_log() const { return bw_; }
    [[nodiscard]] float  norm_lo() const { return norm_lo_; }
    [[nodiscard]] float  norm_hi() const { return norm_hi_; }

    [[nodiscard]] Knobs&       knobs()       { return knobs_; }
    [[nodiscard]] const Knobs& knobs() const { return knobs_; }

private:
    void rebuild(uint8_t lmask, int64_t tf_ms);
    bool render_textured(int64_t tf_ms);

    const AppContext& ctx_;
    Knobs knobs_;

    // Cache: segments sorted ascending by price_lo so the renderer can binary
    // search the visible Y band.
    std::vector<LiqFieldSeg> segs_;
    double bw_       = 0.0;   // bucket width in LOG price, grid = exp(k*bw)
    float  max_mag_  = 0.0f;  // global max magnitude, reference only
    float  norm_lo_  = 0.0f;  // LOG-map lower clip = p(lo_pct) of intensities
    float  norm_hi_  = 0.0f;  // LOG-map upper clip = p(hi_pct) of intensities

    // Cache signature: rebuild when any of these move.
    int64_t sig_ts_   = -1;   // last CLOSED candle timestamp
    size_t  sig_n_    = 0;    // closed candle count
    uint8_t sig_mask_ = 0;    // leverage mask
    int64_t sig_tf_   = 0;    // timeframe in ms

    LiqFieldTextureRenderer tex_;
    uint32_t rebuild_gen_ = 0;   // bumped on rebuild, keys the texture re-raster
};
