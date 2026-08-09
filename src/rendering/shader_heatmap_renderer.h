#pragma once

#include <GLES3/gl3.h>
#include <array>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>

#include "imgui.h"
#include "implot.h"
#include "pb/messages.pb.h"

/**
 * ShaderHeatmapRenderer — GPU ring buffer + custom fragment shader.
 *
 * Replaces HeatmapReconstructorGPU. Each instance owns a ring buffer data
 * texture (R32F) and metadata texture (RGBA32F). Rendering is done via
 * ImDrawList::AddCallback which injects a custom shader into ImPlot's
 * draw list. Scroll/zoom changes only uniforms — zero CPU work.
 *
 * Used by both HeatmapManager (orderbook) and LiquidationHeatmapManager.
 * Shared GPU resources (shader, VAO, colormaps) live in ShaderHeatmapResources.
 */
class ShaderHeatmapRenderer {
public:
    ShaderHeatmapRenderer();
    ~ShaderHeatmapRenderer();

    ShaderHeatmapRenderer(const ShaderHeatmapRenderer&) = delete;
    ShaderHeatmapRenderer& operator=(const ShaderHeatmapRenderer&) = delete;

    // ── Data input ──────────────────────────────────────────────────
    /// Process a historical or live-finalized snapshot. Uploads one column to GPU.
    bool process_snapshot(const pb::HeatmapSnapshot& snapshot_pb);

    /// Upload reach_prob data for a column matching timestamp_ms.
    /// Called by LiquidationHeatmapManager after process_snapshot/finalize_column.
    /// price_reach_map: price → reach_prob (0.0-1.0)
    void upload_reach_data(int64_t timestamp_ms,
                           const std::unordered_map<double, float>& price_reach_map);

    /// Phase 2a: Recompute ALL reach_timeline_ values using current_mark as reference.
    /// Creates a dynamic "cone" that moves with price — bands near mark are bright,
    /// far away are dim. Only re-uploads the reach texture (no data rebuild).
    /// Called by LiquidationHeatmapManager when mark_price moves >0.1%.
    void recompute_reach_from_mark(double current_mark, double sigma, double time_hours);

    /// Update the live (rightmost) column with current orderbook data.
    void update_live_column(int64_t timestamp_ms,
                            const std::unordered_map<double, float>& price_qty_map);

    /// Finalize a column (server-authoritative, prevents live overwrite).
    void finalize_column(int64_t timestamp_ms,
                         const std::unordered_map<double, float>& price_qty_map);

    void clear();
    void mark_dirty();

    // ── Rendering ───────────────────────────────────────────────────
    /// Main render call — injects shader callback into ImPlot draw list.
    /// Must be called between ImPlot::BeginPlot() and EndPlot().
    void render_cells(int64_t candle_timeframe_ms,
                      float sensitivity = 1.0f,
                      bool show_labels = true);

    // ── Configuration ───────────────────────────────────────────────
    enum class ColormapType { Orderbook, Liquidation };
    void set_colormap_type(ColormapType type);
    void set_bucket_multiplier(int multiplier);
    int get_bucket_multiplier() const { return bucket_multiplier_; }

    void set_time_offset_ms(int64_t offset) { time_offset_ms_ = offset; }
    int64_t get_time_offset_ms() const { return time_offset_ms_; }

    void set_opacity(float opacity);
    float get_opacity() const { return opacity_; }

    void set_smooth_mode(bool smooth) { smooth_mode_ = smooth; }
    void set_extend_levels(bool extend) { extend_levels_ = extend; }
    bool get_extend_levels() const { return extend_levels_; }

    /// Set GL_LINEAR filtering on data texture for continuous "heat cloud" look.
    /// GL_NEAREST (default) gives crisp discrete cells for orderbook.
    /// GL_LINEAR interpolates between rows/columns for smooth liq heatmap.
    void set_linear_filtering(bool linear);

    /// Phase 2a: Enable GPU-side reach-probability opacity modulation.
    /// When enabled, the fragment shader samples reach_texture_ and applies
    /// smoothstep(0.1, 0.5, reach_prob) to alpha. Bands with <10% reach
    /// become transparent, 10-50% are dimmed proportionally.
    void set_reach_modulation(bool enable) { use_reach_modulation_ = enable; }
    bool get_reach_modulation() const { return use_reach_modulation_; }

    void set_color_low(float low) { color_low_ = low; }
    void set_color_peak(float peak) { color_peak_ = peak; }
    float get_color_low() const { return color_low_; }
    float get_color_peak() const { return color_peak_; }

    // Replay time cutoff: during replay, don't render/sync columns past this timestamp.
    // 0 = disabled (live mode). Set to replay interpolated time during active replay.
    void set_replay_cutoff_ms(int64_t ms) {
        if (ms != replay_cutoff_ms_) {
            replay_cutoff_ms_ = ms;
            gpu_dirty_ = true;  // Force re-sync to clip at new cutoff
        }
    }
    int64_t get_replay_cutoff_ms() const { return replay_cutoff_ms_; }

    // ── Queries ─────────────────────────────────────────────────────
    bool has_data() const { return !timeline_.empty(); }
    size_t get_snapshot_count() const { return timeline_.size(); }
    GLuint get_texture_id() const { return data_texture_; }

    float get_value_at_price_and_time(double price, int64_t time_ms) const;

    int64_t get_min_time() const;
    int64_t get_max_time() const;
    double get_min_price() const;
    double get_max_price() const;
    double get_native_bucket_size() const { return native_bucket_size_; }
    double get_display_bucket_size() const { return native_bucket_size_ * bucket_multiplier_; }
    float get_max_qty() const { return global_max_qty_; }

    float get_data_mean() const { return zscore_mean_; }
    float get_data_stddev() const { return zscore_stddev_; }

    struct ViewportStats {
        float mean = 0.0f;
        float stddev = 1.0f;
        int count = 0;
    };
    ViewportStats get_viewport_stats(double price_min, double price_max) const;

    // Cached viewport stats — avoids iterating all timeline entries every frame
    mutable ViewportStats cached_vp_stats_;
    mutable double cached_vp_price_min_ = 0.0;
    mutable double cached_vp_price_max_ = 0.0;
    mutable int cached_vp_timeline_size_ = 0;

    struct PerfStats {
        int64_t last_upload_us = 0;
        int columns_uploaded_last_frame = 0;
        int total_valid_columns = 0;
    };
    const PerfStats& get_perf_stats() const { return perf_stats_; }

private:
    // ── GPU resources (per-instance) ────────────────────────────────
    GLuint data_texture_ = 0;   // R32F, RING_SIZE × MAX_ROWS
    GLuint reach_texture_ = 0;  // R32F, RING_SIZE × MAX_ROWS — reach_prob per cell (Phase 2a)
    GLuint meta_texture_ = 0;   // RGBA32F, RING_SIZE × 1

    void create_textures();
    void destroy_textures();

    // ── Ring buffer state ───────────────────────────────────────────
    static constexpr int RING_SIZE = 8192;
    static constexpr int MAX_ROWS = 1024;

    // Sequential ring buffer — rebuilt from sorted timeline_ on data change.
    int ring_count_ = 0;        // Number of valid columns currently in ring
    int snapshot_count_ = 0;    // Total snapshots ever processed (for time_step detection)
    int64_t first_seen_ts_ = 0; // First snapshot ts (for time_step detection)
    bool gpu_dirty_ = true;     // Set when timeline_ changes; cleared after GPU sync
    int64_t last_sync_ms_ = 0;  // Debounce: minimum interval between full syncs
    static constexpr int64_t SYNC_DEBOUNCE_MS = 500;  // Don't rebuild more than every 500ms

    // Per-column CPU-side metadata
    struct ColumnMeta {
        int64_t timestamp_ms = 0;
        double price_min = 0.0;
        double price_step = 0.0;  // bucket_size
        int num_rows = 0;
        float max_value = 0.0f;
        bool finalized = false;
    };
    std::array<ColumnMeta, RING_SIZE> column_meta_{};

    // CPU-side data for tooltip/label lookups (dual-write with GPU)
    std::map<int64_t, std::unordered_map<double, float>> timeline_;
    static constexpr size_t MAX_TIMELINE_ENTRIES = 5000;
    void evict_oldest_timeline();

    // Phase 2a: CPU-side reach_prob data, keyed by timestamp → price → reach_prob.
    // Uploaded to reach_texture_ during sync_gpu_from_timeline() alongside data.
    std::map<int64_t, std::unordered_map<double, float>> reach_timeline_;

    // Global price center: all columns use the same price_min to avoid
    // per-column waviness. Updated during sync_gpu_from_timeline().
    double global_price_center_ = 0.0;
    double global_price_min_ = 0.0;  // center - (MAX_ROWS/2) * bucket_size

    // ── Column building helpers ─────────────────────────────────────
    // Temp buffer for building a column before GPU upload
    std::vector<float> column_build_buf_;  // MAX_ROWS
    std::vector<float> reach_build_buf_;   // MAX_ROWS — reach_prob per row (Phase 2a)

    // Staging buffer for batch meta upload (avoids thousands of glTexSubImage2D calls)
    // Data is uploaded per-column directly from column_build_buf_ (cache-friendly).
    std::vector<float> meta_staging_;   // RING_SIZE * 4 — entire meta texture

    // Forward-fill carry state for extend_levels (liq heatmap)
    std::vector<float> prev_column_carry_;  // MAX_ROWS

    /// Build a float column from a price→qty map. Returns num_rows used.
    int build_column(const std::unordered_map<double, float>& price_qty_map,
                     double price_min, double bucket_size,
                     bool apply_spread);

    /// Upload column_build_buf_ to the GPU ring buffer at ring_head_.
    void upload_column(int ring_col, int num_rows,
                       double price_min, double bucket_size,
                       float max_value, float flags);

    /// Upload reach_build_buf_ to the reach texture at the same ring column.
    void upload_reach_column(int ring_col);

    /// Apply ±2 row Gaussian spread for liquidation data (in column_build_buf_).
    void apply_liq_spread(int num_rows);

    /// Apply extend_levels forward-fill from prev_column_carry_.
    void apply_extend_levels(int num_rows);

    // ── Ring buffer management ──────────────────────────────────────
    /// Rebuild the entire GPU ring buffer from sorted timeline_ data.
    /// Called from render_cells() when gpu_dirty_ is set.
    void sync_gpu_from_timeline();

    /// Find the ring buffer column index for a given timestamp.
    int find_column_for_time(int64_t timestamp_ms) const;
    int time_to_column_offset(int64_t timestamp_ms) const;

    int64_t time_step_ms_ = 300000; // Detected snapshot interval (default 5min)

    // ── Render callback data ────────────────────────────────────────
    // Must persist until RenderDrawData() — stored as member.
    struct RenderUniforms {
        GLuint data_tex = 0;
        GLuint reach_tex = 0;   // Phase 2a: reach_prob R32F texture
        GLuint meta_tex = 0;
        GLuint colormap_tex = 0;
        GLuint colormap_warm_tex = 0;  // Warm colormap for directional liq heatmap
        float plot_size[2] = {};
        float plot_pos_screen[2] = {};    // ImPlot::GetPlotPos() in screen coords
        float plot_size_screen[2] = {};   // ImPlot::GetPlotSize() in screen coords
        // Data-extent bounding box in screen coords (Y-down ImGui convention)
        // Used to tighten the scissor rect so the shader only runs on
        // fragments where data actually exists, not the entire plot area.
        float data_screen_min[2] = {};    // top-left of data extent
        float data_screen_max[2] = {};    // bottom-right of data extent
        float viewport_time_min = 0, viewport_time_max = 0;
        float viewport_price_min = 0, viewport_price_max = 0;
        float data_time_start = 0;
        float time_step = 0;
        int ring_start = 0, ring_count = 0;
        int ring_size = RING_SIZE, max_rows = MAX_ROWS;
        float bucket_size = 0;
        int bucket_multiplier = 1;
        float sensitivity = 1.0f;
        float max_qty = 1.0f;
        float color_low = 0, color_peak = 1.0f;
        int mode = 0;  // 0=orderbook, 1=liquidation
        float opacity = 1.0f;
        int use_reach = 0;  // Phase 2a: 1 = modulate alpha by reach_prob texture
    };
    RenderUniforms render_uniforms_;

    /// The GL callback invoked by ImDrawList during RenderDrawData.
    static void gl_render_callback(const ImDrawList* parent, const ImDrawCmd* cmd);

    /// CPU-side label rendering (when zoomed in).
    void render_labels(int64_t candle_timeframe_ms, float sensitivity) const;

    // ── Configuration state ─────────────────────────────────────────
    ColormapType colormap_type_ = ColormapType::Orderbook;
    double native_bucket_size_ = 0.0;
    int bucket_multiplier_ = 1;
    int64_t time_offset_ms_ = 0;
    float opacity_ = 1.0f;
    bool smooth_mode_ = false;
    bool extend_levels_ = false;
    bool linear_filtering_ = false;  // GL_LINEAR for continuous heat cloud (liq heatmap)
    bool use_reach_modulation_ = false; // Phase 2a: GPU-side reach_prob opacity modulation
    float color_low_ = 0.0f;
    float color_peak_ = 100000.0f;

    // Replay time cutoff: during replay, don't render cells past this timestamp.
    // 0 = disabled (live mode). >0 = clip rendering to this time.
    int64_t replay_cutoff_ms_ = 0;

    // ── Stats ───────────────────────────────────────────────────────
    float global_max_qty_ = 0.01f;
    float zscore_mean_ = 0.0f;
    float zscore_stddev_ = 1.0f;

    // Live column tracking
    int live_ring_col_ = -1;
    int64_t live_timestamp_ms_ = 0;

    PerfStats perf_stats_{};

    // Detected candle timeframe (for interval detection)
    int64_t current_candle_timeframe_ms_ = 0;
};
