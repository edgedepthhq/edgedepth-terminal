#pragma once

#include <GLES3/gl3.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

/**
 * LiqFieldTextureRenderer — WS2: the dense projection Field as ONE LUT-shaded
 * textured quad per frame (replaces the per-rect immediate-mode path when the
 * liq_field_use_texture_ A/B toggle is on; the rect path stays as fallback).
 *
 * Design:
 *  - R8 2D texture: x = candle column, y = ABSOLUTE log-price bucket row.
 *    Texels store t = ln(f/lo)/ln(hi/lo) clamped 0..1 — the PRE-gamma log-mapped
 *    intensity. Gamma, noise floor, alpha floor/pow, opacity and the colormap
 *    stay LIVE shader uniforms (Tweaks/Settings changes don't re-upload;
 *    Low/Peak/bps/half-life already invalidate the segment cache upstream).
 *  - Rasterized ONCE per Field-cache rebuild from liq_field_segs_ (the cache
 *    stays the source of truth — this class renders it, never recomputes it).
 *    Column resolution reproduces no-overlap: the depositing candle's own
 *    column and the consuming candle's own column stay dark (the rect path's
 *    tf*0.35 sub-candle edge offsets become whole-column boundaries — flagged
 *    parity risk, A/B at high zoom).
 *  - Per frame: ONE glTexSubImage2D of the live column (building-candle carve)
 *    plus the 1xH standing-fuel texture that drives the in-shader cascade.
 *  - Cascade is procedural in-shader right of the live-edge seam:
 *    len = span*(0.15 + 0.85*tv_row), alpha = history * cascade_alpha with a
 *    LINEAR fade over the final fade_frac (12%) of the projection — the design
 *    target that retires the interim stepped tail.
 *  - GL_LINEAR sampling on the R8 texture gives the vertical feather; x is
 *    snapped to the column texel center in-shader so deposit/consume edges
 *    stay crisp (no horizontal smear across the no-overlap boundaries).
 *    At sub-pixel candles (>1 col/px) the history branch switches to a MAX
 *    over the fragment's column footprint on a stride grid anchored to
 *    ABSOLUTE column indices (≤8 taps) — scroll-invariant, so extreme
 *    zoom-out can't shimmer (the OB shader's <0.3px/col bail was rejected:
 *    a 10-day 5m view is already ~0.55px/col and the Field must survive it).
 *
 * The proven plumbing (ImDrawList::AddCallback GL injection, ClipRect ∩ data
 * extent scissor tightening, bufferless fullscreen triangle) follows
 * ShaderHeatmapRenderer, but this class owns its own tiny program/VAO/LUT so
 * the OB-depth heatmap path is untouched.
 *
 * Owner: ChartWidget. All methods main (GL) thread, between BeginPlot/EndPlot
 * for render(). ensure() returning false => caller uses the rect path.
 */

// Shared segment POD — the Field cache element (was a ChartWidget nested type;
// moved here so the rasterizer can consume the cache directly).
struct LiqFieldSeg {
    float   price_lo;   // bucket-center price of the run's lowest bucket
    float   price_hi;   // bucket-center price of the run's highest bucket
    float   intensity;  // age-decayed anomaly-weighted fuel sum (PINNED statistic)
    int64_t start_ms;   // depositing candle timestamp
    int64_t end_ms;     // consuming candle timestamp, or pending sentinel below
};

class LiqFieldTextureRenderer {
public:
    // Sentinel end_ms for a still-standing (never consumed) segment.
    static constexpr int64_t kSegPending = std::numeric_limits<int64_t>::max();

    // The texture is a buffered window around the visible candle range, not a
    // newest-N cache. Panning beyond the guard band re-rasterizes from the full
    // CPU segment cache, so loaded history is unbounded while GPU/CPU raster
    // memory stays fixed. 8192 columns comfortably covers the chart's zoom cap
    // plus a large pan guard; worst-case R8 storage is 8192*4096 = 32 MiB.
    static constexpr int kMaxCols = 8192;
    static constexpr int kMaxRows = 4096;   // wider log ranges fall back to the rect path
    static constexpr float kMaxProjectionCols = 24.0f;

    LiqFieldTextureRenderer() = default;
    ~LiqFieldTextureRenderer();
    LiqFieldTextureRenderer(const LiqFieldTextureRenderer&) = delete;
    LiqFieldTextureRenderer& operator=(const LiqFieldTextureRenderer&) = delete;

    // ── Raster inputs (stable between Field-cache rebuilds) ─────────────────
    struct RasterParams {
        uint32_t rebuild_gen = 0;  // ChartWidget's Field-cache rebuild counter — the raster key
        double   lbw = 0.0;        // log-price bucket width (liq_field_bw_)
        float    norm_lo = 0.0f;   // dual-percentile log-map clips (liq_field_norm_lo_/hi_)
        float    norm_hi = 0.0f;
        int64_t  tf_ms = 0;        // candle timeframe
        int64_t  first_ms = 0;     // first loaded candle timestamp
        int64_t  latest_ms = 0;    // latest candle (building included) at raster time
        double   view_min_ms = 0.0; // current plot range; selects the buffered window
        double   view_max_ms = 0.0;
    };

    // ── Per-frame inputs (cheap; live edge + carve + style uniforms) ────────
    struct FrameParams {
        int64_t   latest_ms = 0;       // current latest candle ts → live column
        long long carve_klo = 0;       // building-candle carve band, bucket indices
        long long carve_khi = -1;      //   (khi < klo = no carve)
        int   extend = 1;              // liq_field_extend_
        float magnet_frac = 0.06f;     // plot-width fraction of the extend-off magnet
        float max_projection_cols = kMaxProjectionCols;  // forward cascade cap in candle columns
        float gamma = 2.2f;            // liq_field_gamma_
        float floor_t = 0.02f;         // liq_field_floor_
        float alpha_floor = 0.08f;     // liq_field_alpha_floor_
        float alpha_pow = 1.5f;        // liq_field_alpha_pow_
        float opacity = 0.90f;         // liq_field_opacity_
        float cascade_alpha = 0.78f;   // liq_cascade_alpha_
        float fade_frac = 0.12f;       // linear fade over the final 12% (design)
        float min_row_px = 2.0f;       // liq_field_min_row_px_ — rect-path row clamp parity
    };

    /// Re-rasterize liq_field_segs_ into the R8 texture if rebuild_gen changed.
    /// Returns false when this path can't serve (rows > kMaxRows, degenerate
    /// params, GL/shader init failed) — caller falls back to the rect path.
    bool ensure(const std::vector<LiqFieldSeg>& segs, const RasterParams& p);

    /// Per-frame: upload the live column (carve applied) + the standing-fuel
    /// 1xH texture. Call after ensure() returned true, before render().
    void update_live(const FrameParams& f);

    /// Inject the draw callback into the current ImPlot plot's draw list.
    /// Must be called between BeginPlot()/EndPlot(). Also computes the seam /
    /// scissor from ImPlot state; returns the seam x in screen px (for the
    /// caller's 1px seam line), or <0 when nothing was drawn.
    float render(const FrameParams& f);

    bool ready() const { return raster_ok_; }

private:
    // ── GL objects (owned; independent of ShaderHeatmapResources) ───────────
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint field_tex_ = 0;      // R8 alloc_w_ × rows_
    GLuint standing_tex_ = 0;   // R8 1 × rows_
    GLuint colormap_tex_ = 0;   // 256×1 RGBA8, RGB used (rebuilt on LiqMap generation)
    bool   gl_init_tried_ = false;
    bool   gl_init_ok_ = false;
    GLint  max_texture_size_ = 0;
    uint32_t lut_generation_ = 0xFFFFFFFFu;

    bool init_gl();             // compile program, create VAO + LUT (once)
    void rebuild_lut_if_stale();
    void destroy_textures();

    // ── Raster state ────────────────────────────────────────────────────────
    bool     raster_ok_ = false;
    uint32_t raster_gen_ = 0xFFFFFFFFu;
    int64_t  raster_key_t0_ms_ = std::numeric_limits<int64_t>::min();
    int64_t  raster_key_tf_ms_ = 0;
    int      raster_key_w_ = 0;
    double   lbw_ = 0.0;
    int64_t  tf_ms_ = 0;
    int64_t  t0_ms_ = 0;        // column 0 timestamp
    int      alloc_w_ = 0;      // texture width (cols incl. +1 live headroom)
    int      rows_ = 0;         // texture height
    long long k_min_ = 0;       // bucket index of row 0
    int      live_col_raster_ = 0;  // live column at raster time

    std::vector<uint8_t> buf_;          // CPU copy of the full raster (column restore)
    std::vector<uint8_t> standing_tl_;  // per-row pre-gamma tl of PENDING fuel (no carve)
    std::vector<uint8_t> from_live_;    // 1 = deposited by the raster-time live candle
    std::vector<uint8_t> col_scratch_;  // per-frame live-column upload buffer
    std::vector<uint8_t> standing_scratch_;  // per-frame standing upload buffer
    int last_live_col_uploaded_ = -1;   // restore raster content when the live col advances

    // Nearest column for a candle timestamp (negative-safe; candles are tf-aligned).
    int col_of(int64_t ms) const {
        return static_cast<int>(std::llround(
            static_cast<double>(ms - t0_ms_) / static_cast<double>(tf_ms_)));
    }

    // ── Render callback payload (member — must outlive the draw list; the
    //    callback receives `this` and reads uniforms_/locs_/GL handles) ──────
    struct Uniforms {
        float plot_pos_screen[2] = {};   // ImGui screen coords (Y-down)
        float plot_size_screen[2] = {};
        float data_screen_min[2] = {};   // scissor tightening box
        float data_screen_max[2] = {};
        float view_col_min = 0, view_col_max = 0;
        float view_price_min = 0, view_price_max = 0;
        float inv_lbw = 0, row0 = 0;
        float tex_w = 0, tex_h = 0;
        float live_col = 0;
        float seam_u = 1.0f;
        int   extend = 1;
        float magnet_frac = 0.06f;
        float max_projection_frac = 0.0f;
        float gamma = 2.2f, floor_t = 0.02f;
        float alpha_floor = 0.08f, alpha_pow = 1.5f, opacity = 0.90f;
        float cascade_alpha = 0.78f, fade_frac = 0.12f;
        float min_row_px = 2.0f;
    };
    Uniforms uniforms_;

    struct UniformLocs {
        GLint field = -1, standing = -1, colormap = -1;
        GLint plot_origin = -1, plot_size = -1;
        GLint view_col_min = -1, view_col_max = -1;
        GLint view_price_min = -1, view_price_max = -1;
        GLint inv_lbw = -1, row0 = -1, tex_w = -1, tex_h = -1;
        GLint live_col = -1, seam_u = -1, extend = -1, magnet_frac = -1;
        GLint max_projection_frac = -1;
        GLint gamma = -1, floor_t = -1;
        GLint alpha_floor = -1, alpha_pow = -1, opacity = -1;
        GLint cascade_alpha = -1, fade_frac = -1;
        GLint min_row_px = -1;
    };
    UniformLocs locs_;

    static void gl_render_callback(const struct ImDrawList* parent,
                                   const struct ImDrawCmd* cmd);
};
