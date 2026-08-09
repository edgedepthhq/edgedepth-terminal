#include "shader_heatmap_renderer.h"
#include "shader_heatmap_resources.h"
#include "core/heatmap_colormap.h"
#include "core/reach_math.h"
#include "implot_internal.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>

// ═══════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════════

ShaderHeatmapRenderer::ShaderHeatmapRenderer() {
    column_build_buf_.resize(MAX_ROWS, 0.0f);
    reach_build_buf_.resize(MAX_ROWS, 0.0f);
    prev_column_carry_.resize(MAX_ROWS, 0.0f);
    meta_staging_.resize(static_cast<size_t>(RING_SIZE) * 4, 0.0f);
    create_textures();
}

ShaderHeatmapRenderer::~ShaderHeatmapRenderer() {
    destroy_textures();
}

void ShaderHeatmapRenderer::create_textures() {
    // Data texture: R32F, RING_SIZE × MAX_ROWS
    // Zero-initialize to prevent WebGL "lazy initialization" warnings
    // when doing partial glTexSubImage2D uploads before full init.
    const size_t data_size = static_cast<size_t>(RING_SIZE) * MAX_ROWS;
    std::vector<float> zero_data(data_size, 0.0f);

    glGenTextures(1, &data_texture_);
    glBindTexture(GL_TEXTURE_2D, data_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, RING_SIZE, MAX_ROWS, 0,
                 GL_RED, GL_FLOAT, zero_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Reach-probability texture: R32F, RING_SIZE × MAX_ROWS (Phase 2a)
    // Stores per-cell reach_prob [0,1] from Model A. Sampled by fragment shader
    // to modulate opacity: bands with low reach probability fade out.
    glGenTextures(1, &reach_texture_);
    glBindTexture(GL_TEXTURE_2D, reach_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, RING_SIZE, MAX_ROWS, 0,
                 GL_RED, GL_FLOAT, zero_data.data());
    // Use GL_LINEAR to match data_texture_ filtering for smooth reach gradients
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Metadata texture: RGBA32F, RING_SIZE × 1
    // Per-column: R=price_min, G=num_rows, B=max_value, A=flags
    std::vector<float> zero_meta(static_cast<size_t>(RING_SIZE) * 4, 0.0f);

    glGenTextures(1, &meta_texture_);
    glBindTexture(GL_TEXTURE_2D, meta_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, RING_SIZE, 1, 0,
                 GL_RGBA, GL_FLOAT, zero_meta.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderHeatmapRenderer::destroy_textures() {
    if (data_texture_) { glDeleteTextures(1, &data_texture_); data_texture_ = 0; }
    if (reach_texture_) { glDeleteTextures(1, &reach_texture_); reach_texture_ = 0; }
    if (meta_texture_) { glDeleteTextures(1, &meta_texture_); meta_texture_ = 0; }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Data Input — process_snapshot
// ═══════════════════════════════════════════════════════════════════════════════

bool ShaderHeatmapRenderer::process_snapshot(const pb::HeatmapSnapshot& snapshot_pb) {
    const int64_t ts = snapshot_pb.timestamp_ms();
    const double bucket_size = snapshot_pb.bucket_size();
    if (bucket_size <= 0) return false;
    native_bucket_size_ = bucket_size;

    // Detect time step from first two snapshots
    if (snapshot_count_ == 0) {
        first_seen_ts_ = ts;
    } else if (snapshot_count_ == 1) {
        const int64_t gap = std::abs(ts - first_seen_ts_);
        if (gap > 0) {
            if (gap >= 2700000) time_step_ms_ = 3600000;
            else if (gap >= 600000) time_step_ms_ = 900000;
            else if (gap >= 180000) time_step_ms_ = 300000;
            else time_step_ms_ = 60000;
        }
    }
    snapshot_count_++;

    // Build price→qty map from protobuf
    std::unordered_map<double, float> price_qty_map;
    for (int i = 0; i < snapshot_pb.prices_size(); ++i) {
        const double raw_price = snapshot_pb.prices(i);
        const float total_qty = static_cast<float>(
            snapshot_pb.bid_qty(i) + snapshot_pb.ask_qty(i));
        if (std::abs(total_qty) < 0.001f) continue;
        const double bp = std::floor(raw_price / bucket_size) * bucket_size;
        price_qty_map[bp] += total_qty;
    }
    if (price_qty_map.empty()) return false;

    // CPU-only: store in sorted timeline. GPU sync deferred to render_cells.
    timeline_[ts] = std::move(price_qty_map);
    evict_oldest_timeline();
    gpu_dirty_ = true;

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Data Input — upload_reach_data (Phase 2a)
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::upload_reach_data(
    int64_t timestamp_ms,
    const std::unordered_map<double, float>& price_reach_map)
{
    if (price_reach_map.empty()) return;

    // Always store CPU-side for sync_gpu_from_timeline() batch rebuilds.
    reach_timeline_[timestamp_ms] = price_reach_map;
    while (reach_timeline_.size() > MAX_TIMELINE_ENTRIES) {
        reach_timeline_.erase(reach_timeline_.begin());
    }

    // If the column already exists in the ring (live/finalize path),
    // upload directly to GPU now. Otherwise the batch sync will handle it.
    if (native_bucket_size_ <= 0 || !reach_texture_) return;
    const int col = find_column_for_time(timestamp_ms);
    if (col < 0 || col >= RING_SIZE) return;
    const auto& meta = column_meta_[col];
    if (meta.num_rows <= 0) return;
    const double price_min = meta.price_min;

    std::memset(reach_build_buf_.data(), 0, MAX_ROWS * sizeof(float));
    for (const auto& [price, rp] : price_reach_map) {
        const int row = static_cast<int>(
            std::floor((price - price_min) / native_bucket_size_));
        if (row >= 0 && row < MAX_ROWS) {
            reach_build_buf_[row] = std::clamp(rp, 0.0f, 1.0f);
        }
    }
    // Spread ±2 rows Gaussian
    static constexpr int kSR = 2;
    static constexpr float kRW[] = {0.61f, 0.14f};
    std::vector<float> rspread(MAX_ROWS, 0.0f);
    for (int r = 0; r < MAX_ROWS; ++r) {
        const float v = reach_build_buf_[r];
        if (v < 0.01f) continue;
        for (int d = 1; d <= kSR; ++d) {
            const float nv = v * kRW[d - 1];
            for (int s : {-1, 1}) {
                const int nr = r + s * d;
                if (nr >= 0 && nr < MAX_ROWS && nv > rspread[nr])
                    rspread[nr] = nv;
            }
        }
    }
    for (int r = 0; r < MAX_ROWS; ++r) {
        if (rspread[r] > reach_build_buf_[r])
            reach_build_buf_[r] = rspread[r];
    }
    upload_reach_column(col);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2a — Recompute all reach values from current mark price
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::recompute_reach_from_mark(
    double current_mark, double sigma, double time_hours)
{
    if (reach_timeline_.empty() || native_bucket_size_ <= 0 || !reach_texture_) return;
    if (current_mark <= 0.0) return;

    // Iterate every column in reach_timeline_ and recompute reach values
    // based on distance from current_mark (not the column's historical mark).
    for (auto& [ts, reach_map] : reach_timeline_) {
        for (auto& [price, rp] : reach_map) {
            const double dist_pct = std::abs((price - current_mark) / current_mark * 100.0);
            rp = static_cast<float>(reach_math::gbm_reach_probability(dist_pct, sigma, time_hours));
        }

        // Re-upload this column's reach data directly to GPU.
        // No gpu_dirty_ — we only touch the reach texture, not data.
        const int col = find_column_for_time(ts);
        if (col < 0 || col >= RING_SIZE) continue;
        const auto& meta = column_meta_[col];
        if (meta.num_rows <= 0) continue;
        const double price_min = meta.price_min;

        std::memset(reach_build_buf_.data(), 0, MAX_ROWS * sizeof(float));
        for (const auto& [price, rp] : reach_map) {
            const int row = static_cast<int>(
                std::floor((price - price_min) / native_bucket_size_));
            if (row >= 0 && row < MAX_ROWS) {
                reach_build_buf_[row] = std::clamp(rp, 0.0f, 1.0f);
            }
        }

        // Apply ±2 Gaussian spread (same kernel as upload_reach_data)
        static constexpr int kSR = 2;
        static constexpr float kRW[] = {0.61f, 0.14f};
        // Use a local scratch buffer for spread (can't use column_build_buf_ safely
        // since sync_gpu_from_timeline might be running on the same frame path)
        float spread_buf[MAX_ROWS];
        std::memset(spread_buf, 0, MAX_ROWS * sizeof(float));
        for (int r = 0; r < MAX_ROWS; ++r) {
            const float v = reach_build_buf_[r];
            if (v < 0.01f) continue;
            for (int d = 1; d <= kSR; ++d) {
                const float nv = v * kRW[d - 1];
                for (int s : {-1, 1}) {
                    const int nr = r + s * d;
                    if (nr >= 0 && nr < MAX_ROWS && nv > spread_buf[nr])
                        spread_buf[nr] = nv;
                }
            }
        }
        for (int r = 0; r < MAX_ROWS; ++r) {
            if (spread_buf[r] > reach_build_buf_[r])
                reach_build_buf_[r] = spread_buf[r];
        }

        upload_reach_column(col);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Data Input — update_live_column / finalize_column
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::update_live_column(
    int64_t timestamp_ms,
    const std::unordered_map<double, float>& price_qty_map)
{
    if (timeline_.empty() || native_bucket_size_ <= 0) return;
    if (price_qty_map.empty()) return;

    // Live column goes at the end of the current ring
    const int target_col = (ring_count_ > 0) ? ring_count_ - 1 : 0;
    if (target_col >= RING_SIZE) return;
    live_ring_col_ = target_col;
    live_timestamp_ms_ = timestamp_ms;

    // Center the MAX_ROWS window around the live data's midpoint
    double raw_pmin = std::numeric_limits<double>::max();
    double raw_pmax = std::numeric_limits<double>::lowest();
    for (const auto& [p, q] : price_qty_map) {
        raw_pmin = std::min(raw_pmin, p);
        raw_pmax = std::max(raw_pmax, p);
    }
    const double mid_price = (raw_pmin + raw_pmax) * 0.5;
    const double half_window = (MAX_ROWS / 2) * native_bucket_size_;
    const double pmin = std::floor(
        (mid_price - half_window) / native_bucket_size_) * native_bucket_size_;

    const int num_rows = build_column(price_qty_map, pmin,
                                      native_bucket_size_, false);
    if (num_rows <= 0) return;

    float col_max = 0.0f;
    for (int i = 0; i < num_rows; ++i) {
        float av = std::abs(column_build_buf_[i]);
        if (av > col_max) col_max = av;
    }
    if (col_max > global_max_qty_) global_max_qty_ = col_max;

    // Upload — flags=2.0 for live column
    upload_column(target_col, num_rows, pmin, native_bucket_size_, col_max, 2.0f);

    auto& meta = column_meta_[target_col];
    meta.timestamp_ms = timestamp_ms;
    meta.price_min = pmin;
    meta.price_step = native_bucket_size_;
    meta.num_rows = num_rows;
    meta.max_value = col_max;
    meta.finalized = false;
}

void ShaderHeatmapRenderer::finalize_column(
    int64_t timestamp_ms,
    const std::unordered_map<double, float>& price_qty_map)
{
    if (native_bucket_size_ <= 0 || price_qty_map.empty()) return;

    // Store in timeline (CPU-side for tooltips/labels)
    timeline_[timestamp_ms] = price_qty_map;
    evict_oldest_timeline();

    // Direct GPU upload of this single column instead of marking the entire
    // ring dirty (which would trigger a full rebuild of all columns).
    // This is critical for FPS: finalize is called every few seconds for live data.
    if (ring_count_ > 0 && time_step_ms_ > 0) {
        const int64_t oldest_ts = timeline_.begin()->first;
        const int col = static_cast<int>((timestamp_ms - oldest_ts) / time_step_ms_);
        if (col >= 0 && col < RING_SIZE) {
            // Center this column (same logic as sync_gpu_from_timeline)
            double snap_pmin = std::numeric_limits<double>::max();
            double snap_pmax = std::numeric_limits<double>::lowest();
            for (const auto& [p, q] : price_qty_map) {
                if (p < snap_pmin) snap_pmin = p;
                if (p > snap_pmax) snap_pmax = p;
            }
            const double mid = (snap_pmin + snap_pmax) * 0.5;
            const double hw = (MAX_ROWS / 2) * native_bucket_size_;
            const double pmin = std::floor((mid - hw) / native_bucket_size_) * native_bucket_size_;

            const bool spread = (colormap_type_ == ColormapType::Liquidation);
            const int num_rows = build_column(price_qty_map, pmin, native_bucket_size_, spread);
            if (num_rows > 0) {
                float col_max = 0.0f;
                for (int i = 0; i < num_rows; ++i) {
                    float av = std::abs(column_build_buf_[i]);
                    if (av > col_max) col_max = av;
                }
                if (col_max > global_max_qty_) global_max_qty_ = col_max;
                upload_column(col, num_rows, pmin, native_bucket_size_, col_max, 1.0f);

                auto& meta = column_meta_[col];
                meta.timestamp_ms = timestamp_ms;
                meta.price_min = pmin;
                meta.price_step = native_bucket_size_;
                meta.num_rows = num_rows;
                meta.max_value = col_max;
                meta.finalized = true;

                if (col >= ring_count_) ring_count_ = col + 1;
            }
            return;  // Successfully uploaded — no need for full rebuild
        }
    }

    // Fallback: column doesn't fit in current ring — need full rebuild
    gpu_dirty_ = true;
    live_ring_col_ = -1;
    live_timestamp_ms_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Column Building Helpers
// ═══════════════════════════════════════════════════════════════════════════════

int ShaderHeatmapRenderer::build_column(
    const std::unordered_map<double, float>& price_qty_map,
    double price_min, double bucket_size, bool apply_spread)
{
    // Clear build buffer
    std::memset(column_build_buf_.data(), 0, MAX_ROWS * sizeof(float));

    int max_row_used = 0;
    for (const auto& [price, qty] : price_qty_map) {
        const int row = static_cast<int>(
            std::floor((price - price_min) / bucket_size));
        if (row >= 0 && row < MAX_ROWS) {
            column_build_buf_[row] += qty;
            max_row_used = std::max(max_row_used, row);
        }
    }

    const int num_rows = max_row_used + 1;

    // V7: ±2 rows Gaussian spread (σ=1.0) + shader bilinear interpolation.
    if (apply_spread && num_rows > 0) {
        apply_liq_spread(num_rows);
    }

    return num_rows;
}

void ShaderHeatmapRenderer::apply_liq_spread(int num_rows) {
    // V7: Spread values ±2 rows with Gaussian kernel (σ=1.0) to fill sub-band gaps.
    // Combined with shader-side bilinear interpolation, this creates smooth
    // continuous gradients. Wider spread (±4) was too aggressive — it created
    // uniform amber fog. ±2 keeps clusters focused with soft edges.
    // Weights: exp(-d²/(2*1.0²)) for d=1..2
    static constexpr int kSpreadRadius = 2;
    static constexpr float kWeights[] = {0.61f, 0.14f};
    // Work on a copy to avoid cascading
    std::vector<float> spread(num_rows + 2 * kSpreadRadius, 0.0f);
    const int limit = std::min(num_rows, MAX_ROWS);
    for (int r = 0; r < limit; ++r) {
        const float v = column_build_buf_[r];
        if (std::abs(v) < 0.05f) continue;
        const float sign = (v < 0.0f) ? -1.0f : 1.0f;
        for (int d = 1; d <= kSpreadRadius; ++d) {
            const float nv = std::abs(v) * kWeights[d - 1];
            for (int s : {-1, 1}) {
                const int nr = r + s * d;
                if (nr >= 0 && nr < limit) {
                    const float sv = sign * nv;
                    if (std::abs(sv) > std::abs(spread[nr])) {
                        spread[nr] = sv;
                    }
                }
            }
        }
    }
    // Merge: take the value with larger absolute magnitude
    for (int r = 0; r < limit; ++r) {
        if (std::abs(spread[r]) > std::abs(column_build_buf_[r])) {
            column_build_buf_[r] = spread[r];
        }
    }
}

void ShaderHeatmapRenderer::apply_extend_levels(int num_rows) {
    // Forward-fill from previous column: if current row is empty,
    // carry from prev_column_carry_ with decay.
    // V2: Fast decay (0.90) — bands persist visibly for ~20 columns
    // (~20 minutes at 1-min publish), creating horizontal persistence
    // like MMT without permanent stripes. 0.90^20 = 0.12, 0.90^30 = 0.04.
    static constexpr float kDecay = 0.90f;
    const int limit = std::min(num_rows, MAX_ROWS);
    for (int r = 0; r < limit; ++r) {
        if (column_build_buf_[r] > 0.001f) {
            // Real data — update carry
            prev_column_carry_[r] = column_build_buf_[r];
        } else if (prev_column_carry_[r] > 0.001f) {
            // Empty — forward-fill with decayed carry
            prev_column_carry_[r] *= kDecay;
            column_build_buf_[r] = prev_column_carry_[r];
        }
    }
}

void ShaderHeatmapRenderer::upload_column(
    int ring_col, int num_rows,
    double price_min, double bucket_size,
    float max_value, float flags)
{
    // Upload data column (1 × num_rows, padded to MAX_ROWS)
    glBindTexture(GL_TEXTURE_2D, data_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    ring_col, 0,       // xoffset, yoffset
                    1, MAX_ROWS,       // width=1, height=MAX_ROWS
                    GL_RED, GL_FLOAT,
                    column_build_buf_.data());

    // Upload metadata for this column
    float meta[4] = {
        static_cast<float>(price_min),
        static_cast<float>(num_rows),
        max_value,
        flags  // 0=empty, 1=valid, 2=live
    };
    glBindTexture(GL_TEXTURE_2D, meta_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    ring_col, 0,
                    1, 1,
                    GL_RGBA, GL_FLOAT,
                    meta);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderHeatmapRenderer::upload_reach_column(int ring_col) {
    // Upload reach_build_buf_ to reach_texture_ at the same column position.
    // Called after upload_column() for liq heatmap data.
    if (!reach_texture_) return;
    glBindTexture(GL_TEXTURE_2D, reach_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    ring_col, 0,
                    1, MAX_ROWS,
                    GL_RED, GL_FLOAT,
                    reach_build_buf_.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Ring Buffer Management
// ═══════════════════════════════════════════════════════════════════════════════

// advance_ring_head removed — columns are time-indexed, not sequential

// ═══════════════════════════════════════════════════════════════════════════════
// sync_gpu_from_timeline — Rebuild entire ring buffer from sorted timeline_
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::sync_gpu_from_timeline() {
    if (timeline_.empty()) {
        ring_count_ = 0;
        gpu_dirty_ = false;
        return;
    }

    const bool apply_spread = (colormap_type_ == ColormapType::Liquidation);
    global_max_qty_ = 0.01f;
    std::fill(prev_column_carry_.begin(), prev_column_carry_.end(), 0.0f);
    column_meta_.fill(ColumnMeta{});

    // Detect time_step from first two entries if not already detected
    if (timeline_.size() >= 2) {
        auto it = timeline_.begin();
        const int64_t t0 = it->first; ++it;
        const int64_t t1 = it->first;
        const int64_t gap = t1 - t0;
        if (gap > 0) {
            if (gap >= 2700000) time_step_ms_ = 3600000;
            else if (gap >= 600000) time_step_ms_ = 900000;
            else if (gap >= 180000) time_step_ms_ = 300000;
            else time_step_ms_ = 60000;
        }
    }

    // Time-indexed placement: each column goes to its computed ring position
    // so the shader's col_offset = time / time_step maps correctly.
    // Gaps in data produce empty columns (zero-initialized, flags=0 → discard).
    const int64_t oldest_ts = timeline_.begin()->first;
    const int64_t newest_ts = timeline_.rbegin()->first;
    int64_t total_span = newest_ts - oldest_ts;

    // If data spans more columns than RING_SIZE, increase time_step to fit
    if (time_step_ms_ > 0 && total_span / time_step_ms_ >= RING_SIZE) {
        time_step_ms_ = (total_span / (RING_SIZE - 2)) + 1;
        // Round up to a clean interval
        if (time_step_ms_ > 1800000) time_step_ms_ = 3600000;
        else if (time_step_ms_ > 600000) time_step_ms_ = 900000;
        else if (time_step_ms_ > 180000) time_step_ms_ = 300000;
        else time_step_ms_ = std::max(time_step_ms_, static_cast<int64_t>(60000));
    }

    // ── Build all columns into CPU staging buffers ──
    // Only zero meta staging (64KB). Data staging doesn't need clearing because
    // the shader checks meta flags before reading data — gap columns have
    // flags=0 and get discarded regardless of stale data values.
    std::memset(meta_staging_.data(), 0,
                static_cast<size_t>(RING_SIZE) * 4 * sizeof(float));

    int highest_col = -1;

    for (const auto& [ts, price_qty_map] : timeline_) {
        // Replay cutoff: skip columns past the current playback position
        if (replay_cutoff_ms_ > 0 && ts > replay_cutoff_ms_) break;  // timeline_ is sorted

        const int col = static_cast<int>((ts - oldest_ts) / time_step_ms_);
        if (col < 0 || col >= RING_SIZE) continue;

        // Per-column centering: each column centers around its own data midpoint.
        // This handles large price movements over time (e.g., BTC $87k→$75k).
        // Adjacent columns have nearly identical midpoints so the per-column
        // price_min metadata stays consistent — no visible waviness.
        double snap_pmin = std::numeric_limits<double>::max();
        double snap_pmax = std::numeric_limits<double>::lowest();
        for (const auto& [p, q] : price_qty_map) {
            if (p < snap_pmin) snap_pmin = p;
            if (p > snap_pmax) snap_pmax = p;
        }
        if (snap_pmin == std::numeric_limits<double>::max()) continue;

        const double mid_price = (snap_pmin + snap_pmax) * 0.5;
        const double half_win = (MAX_ROWS / 2) * native_bucket_size_;

        // "Sticky" centering: keep price_min stable across columns to avoid
        // stair-stepping at large bucket sizes ($50, $100). Only shift when
        // the midpoint drifts beyond 25% of the window from current center.
        double col_price_min;
        if (highest_col >= 0) {
            // Use the previous column's price_min as baseline
            const double prev_pmin = column_meta_[highest_col].price_min;
            const double prev_center = prev_pmin + half_win;
            const double drift = std::abs(mid_price - prev_center);
            const double threshold = half_win * 0.25;  // 25% of window
            if (drift < threshold) {
                col_price_min = prev_pmin;  // Keep same price_min
            } else {
                // Re-center, snapped to bucket grid
                col_price_min = std::floor(
                    (mid_price - half_win) / native_bucket_size_) * native_bucket_size_;
            }
        } else {
            // First column: snap to bucket grid
            col_price_min = std::floor(
                (mid_price - half_win) / native_bucket_size_) * native_bucket_size_;
        }

        // Build float column
        const int num_rows = build_column(price_qty_map, col_price_min,
                                          native_bucket_size_, apply_spread);
        if (num_rows <= 0) continue;

        // Apply extend_levels (prev_column_carry_ is maintained across iterations)
        if (extend_levels_ && colormap_type_ == ColormapType::Liquidation) {
            apply_extend_levels(num_rows);
        }

        // Find column max (use abs for directional liq data)
        float col_max = 0.0f;
        for (int i = 0; i < num_rows; ++i) {
            float av = std::abs(column_build_buf_[i]);
            if (av > col_max) col_max = av;
        }
        if (col_max > global_max_qty_) global_max_qty_ = col_max;

        // Direct per-column GL upload from column_build_buf_ (contiguous, cache-friendly).
        // The old staging buffer approach required strided writes (data_staging_[r * RING_SIZE + col])
        // which were cache-hostile — 4M iterations for 4000 columns.
        upload_column(col, num_rows, col_price_min, native_bucket_size_, col_max, 1.0f);

        // Phase 2a: Build + upload reach_prob column at the same ring position.
        // Uses the same col_price_min and native_bucket_size_ as the data column.
        {
            auto reach_it = reach_timeline_.find(ts);
            if (reach_it != reach_timeline_.end() && !reach_it->second.empty()) {
                std::memset(reach_build_buf_.data(), 0, MAX_ROWS * sizeof(float));
                for (const auto& [price, rp] : reach_it->second) {
                    const int row = static_cast<int>(
                        std::floor((price - col_price_min) / native_bucket_size_));
                    if (row >= 0 && row < MAX_ROWS) {
                        reach_build_buf_[row] = std::clamp(rp, 0.0f, 1.0f);
                    }
                }
                // Spread reach values ±2 rows with same Gaussian as data
                // Reuse column_build_buf_ as scratch (data already uploaded above)
                static constexpr int kSR = 2;
                static constexpr float kRW[] = {0.61f, 0.14f};
                std::memset(column_build_buf_.data(), 0, MAX_ROWS * sizeof(float));
                for (int r = 0; r < MAX_ROWS; ++r) {
                    const float v = reach_build_buf_[r];
                    if (v < 0.01f) continue;
                    for (int d = 1; d <= kSR; ++d) {
                        const float nv = v * kRW[d - 1];
                        for (int s : {-1, 1}) {
                            const int nr = r + s * d;
                            if (nr >= 0 && nr < MAX_ROWS && nv > column_build_buf_[nr])
                                column_build_buf_[nr] = nv;
                        }
                    }
                }
                for (int r = 0; r < MAX_ROWS; ++r) {
                    if (column_build_buf_[r] > reach_build_buf_[r])
                        reach_build_buf_[r] = column_build_buf_[r];
                }
                upload_reach_column(col);
            } else {
                // No reach data for this timestamp — fill with 1.0 (fully visible).
                // This ensures bands without ML data aren't hidden when reach modulation is on.
                std::fill(reach_build_buf_.begin(), reach_build_buf_.begin() + MAX_ROWS, 1.0f);
                upload_reach_column(col);
            }
        }

        // Write metadata into staging buffer
        const size_t meta_offset = static_cast<size_t>(col) * 4;
        meta_staging_[meta_offset + 0] = static_cast<float>(col_price_min);
        meta_staging_[meta_offset + 1] = static_cast<float>(num_rows);
        meta_staging_[meta_offset + 2] = col_max;
        meta_staging_[meta_offset + 3] = 1.0f;  // flags: valid

        // Store CPU-side metadata
        auto& meta = column_meta_[col];
        meta.timestamp_ms = ts;
        meta.price_min = col_price_min;
        meta.price_step = native_bucket_size_;
        meta.num_rows = num_rows;
        meta.max_value = col_max;
        meta.finalized = true;

        if (col > highest_col) highest_col = col;
    }

    // ── Batch upload metadata only (data was uploaded per-column above) ──
    const int upload_cols = (highest_col >= 0) ? highest_col + 1 : 0;
    if (upload_cols > 0) {
        glBindTexture(GL_TEXTURE_2D, meta_texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        upload_cols, 1,
                        GL_RGBA, GL_FLOAT, meta_staging_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    ring_count_ = upload_cols;
    gpu_dirty_ = false;
}

int ShaderHeatmapRenderer::find_column_for_time(int64_t timestamp_ms) const {
    if (ring_count_ == 0 || time_step_ms_ <= 0) return -1;
    if (timeline_.empty()) return -1;
    const int64_t oldest = timeline_.begin()->first;
    const int col = static_cast<int>((timestamp_ms - oldest) / time_step_ms_);
    if (col < 0 || col >= ring_count_) return -1;
    return col;
}

int ShaderHeatmapRenderer::time_to_column_offset(int64_t timestamp_ms) const {
    return find_column_for_time(timestamp_ms);
}

void ShaderHeatmapRenderer::evict_oldest_timeline() {
    while (timeline_.size() > MAX_TIMELINE_ENTRIES) {
        const auto ts = timeline_.begin()->first;
        timeline_.erase(timeline_.begin());
        reach_timeline_.erase(ts);  // Keep reach_timeline_ in sync
    }
}

void ShaderHeatmapRenderer::clear() {
    ring_count_ = 0;
    snapshot_count_ = 0;
    first_seen_ts_ = 0;
    live_ring_col_ = -1;
    live_timestamp_ms_ = 0;
    gpu_dirty_ = true;
    global_max_qty_ = 0.01f;
    global_price_center_ = 0.0;
    global_price_min_ = 0.0;
    last_sync_ms_ = 0;
    cached_vp_timeline_size_ = 0;
    cached_vp_stats_ = ViewportStats{};
    zscore_mean_ = 0.0f;
    zscore_stddev_ = 1.0f;
    timeline_.clear();
    reach_timeline_.clear();
    column_meta_.fill(ColumnMeta{});
    std::fill(prev_column_carry_.begin(), prev_column_carry_.end(), 0.0f);

    // Clear GPU textures — zero out metadata to mark all columns empty
    std::vector<float> zero_meta(RING_SIZE * 4, 0.0f);
    glBindTexture(GL_TEXTURE_2D, meta_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, RING_SIZE, 1,
                    GL_RGBA, GL_FLOAT, zero_meta.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Phase 2a: Clear reach texture (zero = no reach data → smoothstep(0.1,0.5,0)=0 → transparent)
    // Only clear metadata row worth of columns; full clear is too expensive.
    // The shader checks meta flags first anyway, so stale reach data in empty columns is harmless.
}

void ShaderHeatmapRenderer::mark_dirty() {
    // For the shader renderer, mark_dirty means we need to re-process data.
    // In practice this is called when leverage mask changes — the manager
    // clears us and re-processes cached protos via process_snapshot.
}

// ═══════════════════════════════════════════════════════════════════════════════
// Rendering — Phase 3: The GL Callback
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::render_cells(
    int64_t candle_timeframe_ms,
    float sensitivity,
    bool show_labels)
{
    if (timeline_.empty() || !data_texture_ || !meta_texture_) return;

    auto& res = ShaderHeatmapResources::instance();
    if (!res.is_initialized()) return;

    // Sync GPU ring buffer from sorted timeline_ if data changed.
    // Debounce: during batch loads (scroll-back), hundreds of process_snapshot
    // calls set gpu_dirty_ over multiple frames. Without debounce, each frame
    // triggers a full rebuild of all columns (e.g., 4000 liq columns).
    if (gpu_dirty_) {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (ring_count_ == 0 || (now_ms - last_sync_ms_) >= SYNC_DEBOUNCE_MS) {
            sync_gpu_from_timeline();
            last_sync_ms_ = now_ms;
        }
    }
    if (ring_count_ == 0) return;

    // Align time_step with candle timeframe to prevent column width drift.
    // The auto-detected time_step may be slightly off (e.g., 299800ms vs 300000ms)
    // due to irregular snapshot timing. Snap to the candle timeframe when close.
    if (candle_timeframe_ms > 0 && time_step_ms_ > 0) {
        const double ratio = static_cast<double>(candle_timeframe_ms) / time_step_ms_;
        if (ratio > 0.8 && ratio < 1.2) {
            time_step_ms_ = candle_timeframe_ms;
        }
    }

    current_candle_timeframe_ms_ = candle_timeframe_ms;

    // Get viewport from ImPlot
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();

    // Sub-pixel column check: if data columns are smaller than 1 pixel each,
    // the shader produces aliasing artifacts (diagonal lines). Skip rendering.
    {
        const double viewport_time_span = limits.X.Max - limits.X.Min;
        const double data_cols_in_view = viewport_time_span /
            static_cast<double>(time_step_ms_);
        const double pixels_per_col = plot_size.x / data_cols_in_view;
        if (pixels_per_col < 0.3) return;  // Sub-pixel columns: avoid aliasing artifacts
    }

    // Get framebuffer scale for HiDPI
    ImDrawData* dd = ImGui::GetDrawData();
    const float fb_scale_x = dd ? dd->FramebufferScale.x : 1.0f;
    const float fb_scale_y = dd ? dd->FramebufferScale.y : 1.0f;

    // Populate render uniforms — these persist until RenderDrawData()
    auto& u = render_uniforms_;
    u.data_tex = data_texture_;
    u.meta_tex = meta_texture_;
    u.colormap_tex = (colormap_type_ == ColormapType::Orderbook)
        ? res.colormap_orderbook() : res.colormap_liquidation();
    u.colormap_warm_tex = res.colormap_liquidation_warm();
    u.reach_tex = reach_texture_;  // Phase 2a

    // Store screen-space plot area from ImPlot (exact data area bounds)
    // These are converted to GL framebuffer coords in the callback
    u.plot_pos_screen[0] = plot_pos.x;
    u.plot_pos_screen[1] = plot_pos.y;
    u.plot_size_screen[0] = plot_size.x;
    u.plot_size_screen[1] = plot_size.y;

    // Time as seconds — use oldest timestamp as reference epoch.
    // Use timeline_ directly (more robust than column_meta_[0] with time-indexed ring)
    const int64_t oldest_ts = timeline_.begin()->first;
    u.viewport_time_min = static_cast<float>(
        (limits.X.Min - static_cast<double>(oldest_ts)) / 1000.0);
    u.viewport_time_max = static_cast<float>(
        (limits.X.Max - static_cast<double>(oldest_ts)) / 1000.0);
    u.viewport_price_min = static_cast<float>(limits.Y.Min);
    u.viewport_price_max = static_cast<float>(limits.Y.Max);

    // Sequential ring buffer — column 0 = oldest, column ring_count-1 = newest.
    u.time_step = static_cast<float>(time_step_ms_) / 1000.0f;
    // Center each column on its bucket timestamp. Candles are center-anchored on T
    // (plot_candles draws timestamps[i] ± half_width); a left-anchored column spanning
    // [T, T+step] sat half a candle to the RIGHT of its candle. Shifting the column
    // origin back half a step makes column i cover [T-step/2, T+step/2], centered on
    // T = oldest_ts + i*step — aligned with the candle.
    u.data_time_start = -0.5f * u.time_step;
    u.ring_start = 0;
    u.ring_count = ring_count_;
    u.ring_size = RING_SIZE;
    u.max_rows = MAX_ROWS;

    u.bucket_size = static_cast<float>(native_bucket_size_);
    u.bucket_multiplier = bucket_multiplier_;
    u.sensitivity = sensitivity;
    u.max_qty = global_max_qty_;
    u.color_low = color_low_;
    u.color_peak = color_peak_;
    u.mode = (colormap_type_ == ColormapType::Liquidation) ? 1 : 0;
    u.opacity = opacity_;
    u.use_reach = (use_reach_modulation_ && reach_texture_) ? 1 : 0;

    // Compute data-extent bounding box in screen coords for tight scissor.
    // This prevents the shader from running on fragments outside the data range,
    // which is the main FPS cost (entire plot area vs just the data strip).
    {
        // Columns are now center-anchored (see data_time_start) → the first/last column
        // extends half a step either side of its bucket time, so widen the scissor to match.
        const double data_time_min = static_cast<double>(get_min_time()) - time_step_ms_ * 0.5;
        const double data_time_max = static_cast<double>(get_max_time()) + time_step_ms_ * 0.5;
        const double data_price_min = get_min_price();
        const double data_price_max = get_max_price();

        // Clamp to viewport (no point rendering outside visible area)
        const double vis_t_min = std::max(data_time_min, limits.X.Min);
        const double vis_t_max = std::min(data_time_max, limits.X.Max);
        const double vis_p_min = std::max(data_price_min, limits.Y.Min);
        const double vis_p_max = std::min(data_price_max, limits.Y.Max);

        // Convert to screen pixels (ImGui coords, Y-down)
        const ImVec2 tl = ImPlot::PlotToPixels(vis_t_min, vis_p_max); // top-left
        const ImVec2 br = ImPlot::PlotToPixels(vis_t_max, vis_p_min); // bottom-right
        u.data_screen_min[0] = tl.x;
        u.data_screen_min[1] = tl.y;
        u.data_screen_max[0] = br.x;
        u.data_screen_max[1] = br.y;
    }

    // Inject custom shader draw callback into ImPlot's draw list
    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    dl->AddCallback(gl_render_callback, &render_uniforms_);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    ImPlot::PopPlotClipRect();

    // CPU-side labels when zoomed in
    if (show_labels) {
        const double display_bucket = native_bucket_size_ * bucket_multiplier_;
        const int vis_cols = static_cast<int>(
            (limits.X.Max - limits.X.Min) / candle_timeframe_ms) + 1;
        const int vis_rows = static_cast<int>(
            (limits.Y.Max - limits.Y.Min) / display_bucket) + 1;
        if (vis_cols * vis_rows < 1000) {
            render_labels(candle_timeframe_ms, sensitivity);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// GL Render Callback — called by ImGui during RenderDrawData()
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::gl_render_callback(
    const ImDrawList* /*parent*/, const ImDrawCmd* cmd)
{
    auto* u = static_cast<RenderUniforms*>(cmd->UserCallbackData);
    if (!u || !u->data_tex || !u->meta_tex || !u->colormap_tex) return;

    auto& res = ShaderHeatmapResources::instance();
    if (!res.is_initialized()) return;

    // Get framebuffer info for Y-flip
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd) return;
    const float fb_scale_x = dd->FramebufferScale.x;
    const float fb_scale_y = dd->FramebufferScale.y;
    const float fb_h = dd->DisplaySize.y * fb_scale_y;

    // Apply scissor from ImGui clip rect, tightened to data extent.
    // The data-extent bbox was computed in render_cells() using ImPlot::PlotToPixels().
    // Intersect it with the ClipRect to get the tightest possible scissor.
    // This is the main FPS optimization: shader only runs where data exists.
    const float clip_x0 = (cmd->ClipRect.x - dd->DisplayPos.x) * fb_scale_x;
    const float clip_y0 = (cmd->ClipRect.y - dd->DisplayPos.y) * fb_scale_y;
    const float clip_x1 = (cmd->ClipRect.z - dd->DisplayPos.x) * fb_scale_x;
    const float clip_y1 = (cmd->ClipRect.w - dd->DisplayPos.y) * fb_scale_y;

    // Data extent in framebuffer coords (screen-space Y-down → GL Y-up via fb_h)
    const float data_x0 = (u->data_screen_min[0] - dd->DisplayPos.x) * fb_scale_x;
    const float data_y0 = (u->data_screen_min[1] - dd->DisplayPos.y) * fb_scale_y;
    const float data_x1 = (u->data_screen_max[0] - dd->DisplayPos.x) * fb_scale_x;
    const float data_y1 = (u->data_screen_max[1] - dd->DisplayPos.y) * fb_scale_y;

    // Intersect ClipRect with data extent (all in screen-space FB coords, Y-down)
    const float ix0 = std::max(clip_x0, data_x0);
    const float iy0 = std::max(clip_y0, data_y0);
    const float ix1 = std::min(clip_x1, data_x1);
    const float iy1 = std::min(clip_y1, data_y1);

    if (ix0 >= ix1 || iy0 >= iy1) return;  // No intersection — skip draw entirely

    // Convert to GL scissor (Y-flipped)
    const float sx = ix0;
    const float sy = fb_h - iy1;    // bottom edge in GL coords
    const float sw = ix1 - ix0;
    const float sh = iy1 - iy0;
    glScissor(static_cast<int>(sx), static_cast<int>(sy),
              static_cast<int>(sw), static_cast<int>(sh));

    // Compute plot origin/size from ImPlot's exact data area (not ClipRect).
    // ClipRect can differ by padding/axis borders/AA fringe, causing Y-offset.
    const float px = (u->plot_pos_screen[0] - dd->DisplayPos.x) * fb_scale_x;
    const float py = (u->plot_pos_screen[1] - dd->DisplayPos.y) * fb_scale_y;
    const float pw = u->plot_size_screen[0] * fb_scale_x;
    const float ph = u->plot_size_screen[1] * fb_scale_y;
    // Convert to GL coords: origin = bottom-left (Y-up from window bottom)
    const float plot_gl_x = px;
    const float plot_gl_y = fb_h - (py + ph);

    // Do NOT change glViewport — keep ImGui's full-framebuffer viewport.
    // The scissor clips to the plot area. The shader uses u_plot_origin
    // to map gl_FragCoord (window coords) to plot-local [0,1] UV.

    // Bind our custom shader
    glUseProgram(res.shader_program());

    const auto& loc = res.uniforms();

    // Bind textures to units
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, u->data_tex);
    glUniform1i(loc.u_data, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, u->meta_tex);
    glUniform1i(loc.u_meta, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, u->colormap_tex);
    glUniform1i(loc.u_colormap, 2);

    // Bind warm colormap for directional liquidation rendering (texture unit 3)
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, u->colormap_warm_tex);
    glUniform1i(loc.u_colormap_warm, 3);

    // Phase 2a: Bind reach-probability texture (unit 4) + enable flag
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, u->reach_tex);
    glUniform1i(loc.u_reach_data, 4);
    glUniform1i(loc.u_use_reach, u->use_reach);

    // Set uniforms — plot_origin is the bottom-left corner in GL window pixels
    glUniform2f(loc.u_plot_origin, plot_gl_x, plot_gl_y);
    glUniform2f(loc.u_plot_size, pw, ph);
    glUniform1f(loc.u_viewport_time_min, u->viewport_time_min);
    glUniform1f(loc.u_viewport_time_max, u->viewport_time_max);
    glUniform1f(loc.u_viewport_price_min, u->viewport_price_min);
    glUniform1f(loc.u_viewport_price_max, u->viewport_price_max);
    glUniform1f(loc.u_data_time_start, u->data_time_start);
    glUniform1f(loc.u_time_step, u->time_step);
    glUniform1i(loc.u_ring_start, u->ring_start);
    glUniform1i(loc.u_ring_count, u->ring_count);
    glUniform1i(loc.u_ring_size, u->ring_size);
    glUniform1i(loc.u_max_rows, u->max_rows);
    glUniform1f(loc.u_bucket_size, u->bucket_size);
    glUniform1i(loc.u_bucket_multiplier, u->bucket_multiplier);
    glUniform1f(loc.u_sensitivity, u->sensitivity);
    glUniform1f(loc.u_max_qty, u->max_qty);
    glUniform1f(loc.u_color_low, u->color_low);
    glUniform1f(loc.u_color_peak, u->color_peak);
    glUniform1i(loc.u_mode, u->mode);
    glUniform1f(loc.u_opacity, u->opacity);

    // Draw fullscreen triangle (3 vertices, bufferless via gl_VertexID)
    glBindVertexArray(res.empty_vao());
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore active texture unit to 0 (ImGui expects this)
    glActiveTexture(GL_TEXTURE0);
    // Note: ImDrawCallback_ResetRenderState follows this callback
    // and will restore ImGui's full GL state (shader, VAO, viewport, etc.)
}

// ═══════════════════════════════════════════════════════════════════════════════
// CPU-side Label Rendering
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::render_labels(
    int64_t candle_timeframe_ms, float sensitivity) const
{
    if (timeline_.empty() || native_bucket_size_ <= 0) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const double display_bucket = native_bucket_size_ * bucket_multiplier_;

    // Check cell size in pixels — only render if cells are large enough
    const ImVec2 p1 = ImPlot::PlotToPixels(0, 0);
    const ImVec2 p2 = ImPlot::PlotToPixels(
        static_cast<double>(candle_timeframe_ms), display_bucket);
    const float cell_w = std::abs(p2.x - p1.x);
    const float cell_h = std::abs(p2.y - p1.y);
    if (cell_w < 40.0f || cell_h < 16.0f) return;

    ImPlot::PushPlotClipRect();
    int labels_rendered = 0;
    constexpr int MAX_LABELS = 500;

    const int64_t first_col = (static_cast<int64_t>(limits.X.Min) /
        candle_timeframe_ms) * candle_timeframe_ms;
    const int64_t last_col = (static_cast<int64_t>(limits.X.Max) /
        candle_timeframe_ms + 1) * candle_timeframe_ms;
    const double first_row = std::floor(limits.Y.Min / display_bucket) * display_bucket;
    const double last_row = std::ceil(limits.Y.Max / display_bucket) * display_bucket;

    for (int64_t col_time = first_col;
         col_time <= last_col && labels_rendered < MAX_LABELS;
         col_time += candle_timeframe_ms)
    {
        // Find data near this timestamp in timeline_
        auto it = timeline_.lower_bound(col_time - candle_timeframe_ms / 2);
        if (it == timeline_.end()) continue;
        if (std::abs(it->first - col_time) > candle_timeframe_ms) continue;

        for (double row_price = first_row;
             row_price <= last_row && labels_rendered < MAX_LABELS;
             row_price += display_bucket)
        {
            // Sum values in this display bucket
            float qty = 0.0f;
            for (const auto& [p, q] : it->second) {
                const double dp = std::floor(p / display_bucket) * display_bucket;
                if (std::abs(dp - row_price) < display_bucket * 0.5) {
                    qty += q;
                }
            }
            if (qty < 0.01f) continue;

            const double cx = static_cast<double>(col_time);
            const double cy = row_price + display_bucket * 0.5;
            const ImVec2 center_px = ImPlot::PlotToPixels(cx, cy);

            char label[16];
            if (qty >= 1000.0f) snprintf(label, sizeof(label), "%.1fk", qty / 1000.0f);
            else if (qty >= 100.0f) snprintf(label, sizeof(label), "%.0f", qty);
            else if (qty >= 10.0f) snprintf(label, sizeof(label), "%.1f", qty);
            else snprintf(label, sizeof(label), "%.2f", qty);

            const ImVec2 text_size = ImGui::CalcTextSize(label);
            if (text_size.x > cell_w * 0.95f || text_size.y > cell_h * 0.9f) continue;

            const ImVec2 text_pos(center_px.x - text_size.x * 0.5f,
                                  center_px.y - text_size.y * 0.5f);
            const float normalized = (global_max_qty_ > 0.001f)
                ? qty / global_max_qty_ : 0.0f;
            const ImU32 text_color = (normalized > 0.5f)
                ? IM_COL32(0, 0, 0, 230) : IM_COL32(255, 255, 255, 230);

            draw_list->AddText(text_pos, text_color, label);
            labels_rendered++;
        }
    }
    ImPlot::PopPlotClipRect();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Query Methods
// ═══════════════════════════════════════════════════════════════════════════════

int64_t ShaderHeatmapRenderer::get_min_time() const {
    return timeline_.empty() ? 0 : timeline_.begin()->first;
}

int64_t ShaderHeatmapRenderer::get_max_time() const {
    return timeline_.empty() ? 0 : timeline_.rbegin()->first;
}

double ShaderHeatmapRenderer::get_min_price() const {
    if (ring_count_ == 0) return 0.0;
    double pmin = std::numeric_limits<double>::max();
    for (int i = 0; i < ring_count_; ++i) {
        const auto& m = column_meta_[i];
        if (m.num_rows > 0 && m.price_min < pmin) pmin = m.price_min;
    }
    return pmin;
}

double ShaderHeatmapRenderer::get_max_price() const {
    if (ring_count_ == 0) return 0.0;
    double pmax = std::numeric_limits<double>::lowest();
    for (int i = 0; i < ring_count_; ++i) {
        const auto& m = column_meta_[i];
        if (m.num_rows > 0) {
            const double top = m.price_min + m.num_rows * m.price_step;
            if (top > pmax) pmax = top;
        }
    }
    return pmax;
}

float ShaderHeatmapRenderer::get_value_at_price_and_time(
    double price, int64_t time_ms) const
{
    // Look up from CPU-side timeline_ (no GPU readback)
    auto it = timeline_.lower_bound(time_ms - time_step_ms_ / 2);
    if (it == timeline_.end()) return 0.0f;
    if (std::abs(it->first - time_ms) > time_step_ms_) return 0.0f;

    const double bucket = native_bucket_size_ * bucket_multiplier_;
    const double bp = std::floor(price / bucket) * bucket;
    float total = 0.0f;
    for (const auto& [p, q] : it->second) {
        const double dp = std::floor(p / bucket) * bucket;
        if (std::abs(dp - bp) < bucket * 0.5) total += q;
    }
    return total;
}

ShaderHeatmapRenderer::ViewportStats
ShaderHeatmapRenderer::get_viewport_stats(double price_min, double price_max) const
{
    if (timeline_.empty()) return ViewportStats{};

    // Cache: only recompute when viewport changes >10% or data changes.
    // This avoids iterating all 4000+ timeline entries every frame —
    // the #1 FPS bottleneck for the liq heatmap.
    const double range = price_max - price_min;
    const double shift = std::abs(price_min - cached_vp_price_min_) +
                         std::abs(price_max - cached_vp_price_max_);
    const bool viewport_changed = (range > 0.001) && (shift / range > 0.1);
    const bool data_changed = (static_cast<int>(timeline_.size()) != cached_vp_timeline_size_);

    if (!viewport_changed && !data_changed && cached_vp_stats_.count > 0) {
        return cached_vp_stats_;
    }

    double sum = 0.0, sum_sq = 0.0;
    int count = 0;
    for (const auto& [ts, pmap] : timeline_) {
        for (const auto& [p, q] : pmap) {
            if (p >= price_min && p <= price_max) {
                const float aq = std::abs(q);
                if (aq > 0.001f) {
                    sum += aq;
                    sum_sq += static_cast<double>(aq) * aq;
                    count++;
                }
            }
        }
    }

    ViewportStats stats;
    if (count > 0) {
        stats.mean = static_cast<float>(sum / count);
        const float var = (count > 1)
            ? static_cast<float>((sum_sq - sum * sum / count) / (count - 1))
            : 1.0f;
        stats.stddev = std::sqrt(std::max(var, 0.0001f));
        stats.count = count;
    }

    // Update cache
    cached_vp_stats_ = stats;
    cached_vp_price_min_ = price_min;
    cached_vp_price_max_ = price_max;
    cached_vp_timeline_size_ = static_cast<int>(timeline_.size());

    return stats;
    return stats;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Configuration Setters
// ═══════════════════════════════════════════════════════════════════════════════

void ShaderHeatmapRenderer::set_colormap_type(ColormapType type) {
    colormap_type_ = type;
}

void ShaderHeatmapRenderer::set_bucket_multiplier(int multiplier) {
    bucket_multiplier_ = std::max(1, multiplier);
}

void ShaderHeatmapRenderer::set_opacity(float opacity) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    // Rebuild the colormap texture with new opacity baked in for alpha ramp
    auto& res = ShaderHeatmapResources::instance();
    if (res.is_initialized()) {
        const int type = (colormap_type_ == ColormapType::Liquidation) ? 1 : 0;
        res.rebuild_colormap(type, opacity_);
    }
}

void ShaderHeatmapRenderer::set_linear_filtering(bool linear) {
    if (linear == linear_filtering_) return;
    linear_filtering_ = linear;
    if (!data_texture_) return;
    GLenum filter = linear ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, data_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindTexture(GL_TEXTURE_2D, 0);
}
