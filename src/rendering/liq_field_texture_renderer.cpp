#include "liq_field_texture_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "implot.h"
#include "core/heatmap_colormap.h"

// ═══════════════════════════════════════════════════════════════════════════
// Shaders — GLSL ES 3.0. The fragment shader is the whole point of WS2:
// sample R8 (pre-gamma tl) → gamma → noise floor → LUT → design alpha curve,
// with the forward cascade computed procedurally right of the live-edge seam.
// ═══════════════════════════════════════════════════════════════════════════

static const char* kLiqFieldVS = R"(#version 300 es
// Bufferless fullscreen triangle (same trick as ShaderHeatmapResources).
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kLiqFieldFS = R"(#version 300 es
precision highp float;
precision highp int;

uniform sampler2D u_field;     // R8 WxH: x = candle column, y = log-price row; PRE-gamma tl
uniform sampler2D u_standing;  // R8 1xH: standing tl per row, live carve applied (cascade)
uniform sampler2D u_colormap;  // 256x1 RGBA8 — RGB only; alpha is computed here

uniform vec2  u_plot_origin;   // plot bottom-left, GL framebuffer px
uniform vec2  u_plot_size;     // plot size, GL framebuffer px
uniform float u_view_col_min;  // viewport left/right edges in texture-column coords
uniform float u_view_col_max;
uniform float u_view_price_min;
uniform float u_view_price_max;
uniform float u_inv_lbw;       // 1 / log-price bucket width
uniform float u_row0;          // bucket index of row 0 (row r holds bucket u_row0 + r)
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_live_col;      // CURRENT live column index (may be raster live + 1)
uniform float u_seam_u;        // live edge (seam) position in plot-uv x
uniform int   u_extend;        // 1 = cascade length ∝ strength; 0 = fixed magnet
uniform float u_magnet_frac;   // cascade length fraction of (right − seam) when !u_extend
uniform float u_max_projection_frac; // maximum cascade length fraction of (right - seam)
uniform float u_gamma;         // live style knobs — no texture re-upload on change
uniform float u_floor;
uniform float u_alpha_floor;
uniform float u_alpha_pow;
uniform float u_opacity;
uniform float u_cascade_alpha; // ×0.78 vs history (design `cascade`)
uniform float u_fade_frac;     // LINEAR fade over the final 12% of the projection
uniform float u_min_row_px;    // rect-path emit_rect row clamp (min painted row px)

out vec4 fragColor;

// Row-COVERAGE sampling (round 3 — the "tidy" fix). LINEAR filtering read as
// mush: a lone row's value bell-curved away from its center, so thin rows
// rendered dimmer/thinner than the rect path's hard full-alpha slabs and the
// cascade rows blurred together. Instead: fetch the ±2 nearest rows exactly
// (texelFetch) and let each PAINT over the fragment the way emit_rect did —
// full strength inside max(native+1px, u_min_row_px px) about its center,
// with only a 0.5px smoothstep feather at the boundary (the design feather).
// Returns vec2(value, coverage). Coverage modulates ALPHA only — the colour
// keeps the row's true LUT hue, exactly like the rect path's hard edges.
vec2 row_cover(sampler2D tex, int c, float rc, float row_px) {
    float h_px = max(0.5 * (row_px + 1.0), 0.5 * u_min_row_px);
    int r0 = int(floor(rc + 0.5));
    float best_v = 0.0, best_cov = 0.0, best_s = 0.0;
    for (int dr = -2; dr <= 2; ++dr) {
        int r = r0 + dr;
        if (r < 0 || r >= int(u_tex_h)) continue;
        float v = texelFetch(tex, ivec2(c, r), 0).r;
        if (v <= 0.0) continue;
        float d_px = abs(rc - float(r)) * row_px;   // distance from row center, px
        float cov = 1.0 - smoothstep(h_px - 0.5, h_px + 0.5, d_px);
        float s = v * cov;
        if (s > best_s) { best_s = s; best_v = v; best_cov = cov; }
    }
    return vec2(best_v, best_cov);
}

void main() {
    vec2 uv = (gl_FragCoord.xy - u_plot_origin) / u_plot_size;
    float price = mix(u_view_price_min, u_view_price_max, uv.y);
    // Continuous row coordinate on the ABSOLUTE log grid: texel r center at
    // rc == r. log() is guarded so rc + its derivative are defined for EVERY
    // fragment BEFORE the first discard (derivative correctness), then the
    // price <= 0 fragments (zoomed-out linear axis under-range) are dropped.
    float rc = log(max(price, 1.0e-30)) * u_inv_lbw - u_row0;
    float rspan = fwidth(rc);   // rows per fragment (fragment price span in rows)
    if (price <= 0.0) discard;
    if (rc < -0.5 || rc > u_tex_h - 0.5) discard;
    float row_px = 1.0 / max(rspan, 1.0e-4);     // pixels per price row at this zoom

    vec2 vc;                    // (row value, row coverage)
    float cascade_mul = 1.0;
    if (uv.x > u_seam_u) {
        // ── Forward CASCADE: procedural projection of the standing fuel ──
        vc = row_cover(u_standing, 0, rc, row_px);
        float tvs = pow(clamp(vc.x, 0.0, 1.0), u_gamma);
        if (tvs < u_floor) discard;              // carved/empty rows cast nothing
        float rel = (uv.x - u_seam_u) / max(1.0 - u_seam_u, 1.0e-4);
        float len = (u_extend == 1)
            ? u_max_projection_frac * (0.15 + 0.85 * tvs)  // strength proportional to length
            : u_magnet_frac;                                // fixed forward magnet
        float ft = rel / max(len, 1.0e-4);
        if (ft > 1.0) discard;
        // Design target: flat, then LINEAR fade over the final u_fade_frac of len —
        // this retires the interim stepped tail (88/95/100% × 100/55/28%).
        float fade = (ft <= 1.0 - u_fade_frac) ? 1.0 : (1.0 - ft) / u_fade_frac;
        cascade_mul = u_cascade_alpha * fade;
    } else {
        // ── History + live column ──
        float col_f = mix(u_view_col_min, u_view_col_max, uv.x);
        // Columns per fragment — ANALYTIC (col_f is linear in gl_FragCoord.x),
        // no fwidth needed, so it's exact and control-flow-safe.
        float cpp = (u_view_col_max - u_view_col_min) / u_plot_size.x;
        float half_c = 0.5 * max(cpp, 0.0);
        if (col_f + half_c < -0.5 || col_f - half_c > u_live_col + 0.5) discard;
        int c_last = min(int(floor(u_live_col + 0.5)), int(u_tex_w) - 1);
        if (cpp <= 1.0) {
            // Zoomed in (≥1px per candle): column snaps to its texel — no
            // horizontal smear across the deposit/consume boundaries, the
            // no-overlap edges stay crisp.
            int c = clamp(int(floor(col_f + 0.5)), 0, int(u_tex_w) - 1);
            if (float(c) > u_live_col + 0.5) discard;
            vc = row_cover(u_field, c, rc, row_px);
        } else {
            // Extreme zoom-out (sub-pixel candles): point sampling would skip
            // columns and SHIMMER during scroll (the WS2 wiggle risk). Instead
            // take the MAX over the fragment's column footprint — "brightest
            // fuel in this pixel", the same painter semantics as the rows.
            // Taps sit on a stride grid anchored to ABSOLUTE column indices
            // (not the fragment), so the sampled set is scroll-invariant and
            // the field stays pinned. ≤8 taps; stride grows past 8 cols/px.
            int k = max(1, int(ceil(cpp / 8.0)));
            int cf_lo = int(floor(col_f - half_c + 0.5));
            int cf_hi = int(floor(col_f + half_c + 0.5));
            int c0 = max(cf_lo, 0);
            int c1 = min(cf_hi, c_last);
            c0 = (c0 / k) * k;              // snap down onto the absolute grid (c0 ≥ 0)
            if (c0 < cf_lo) c0 += k;        // keep taps inside the footprint
            float best_s = 0.0;
            vc = vec2(0.0);
            for (int c = c0, guard = 0; c <= c1 && guard < 12; c += k, ++guard) {
                vec2 s = row_cover(u_field, c, rc, row_px);
                float sc = s.x * s.y;
                if (sc > best_s) { best_s = sc; vc = s; }
            }
        }
    }
    if (vc.y <= 0.0) discard;                    // no row paints this fragment

    float tv = pow(clamp(vc.x, 0.0, 1.0), u_gamma);
    if (tv < u_floor) discard;                   // the field's rendered noise floor
    vec3 rgb = texture(u_colormap, vec2(tv, 0.5)).rgb;
    // Design alpha curve: alpha = opacity·(floor + (1−floor)·tv^pow); row
    // coverage feathers only the 0.5px boundary band.
    float a = u_opacity * (u_alpha_floor + (1.0 - u_alpha_floor) * pow(tv, u_alpha_pow));
    fragColor = vec4(rgb, a * cascade_mul * vc.y);
}
)";

// ═══════════════════════════════════════════════════════════════════════════
// GL init / teardown
// ═══════════════════════════════════════════════════════════════════════════

LiqFieldTextureRenderer::~LiqFieldTextureRenderer() {
    destroy_textures();
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (colormap_tex_) glDeleteTextures(1, &colormap_tex_);
}

void LiqFieldTextureRenderer::destroy_textures() {
    if (field_tex_) { glDeleteTextures(1, &field_tex_); field_tex_ = 0; }
    if (standing_tex_) { glDeleteTextures(1, &standing_tex_); standing_tex_ = 0; }
}

static bool lfq_check_shader(GLuint sh, const char* name) {
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[1024];
        glGetShaderInfoLog(sh, sizeof(info), nullptr, info);
        return false;
    }
    return true;
}

bool LiqFieldTextureRenderer::init_gl() {
    if (gl_init_tried_) return gl_init_ok_;
    gl_init_tried_ = true;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &kLiqFieldVS, nullptr);
    glCompileShader(vs);
    glShaderSource(fs, 1, &kLiqFieldFS, nullptr);
    glCompileShader(fs);
    if (!lfq_check_shader(vs, "LiqField vertex") || !lfq_check_shader(fs, "LiqField fragment")) {
        glDeleteShader(vs); glDeleteShader(fs);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[1024];
        glGetProgramInfoLog(program_, sizeof(info), nullptr, info);
        glDeleteProgram(program_); program_ = 0;
        return false;
    }

    auto loc = [this](const char* n) { return glGetUniformLocation(program_, n); };
    locs_.field = loc("u_field");
    locs_.standing = loc("u_standing");
    locs_.colormap = loc("u_colormap");
    locs_.plot_origin = loc("u_plot_origin");
    locs_.plot_size = loc("u_plot_size");
    locs_.view_col_min = loc("u_view_col_min");
    locs_.view_col_max = loc("u_view_col_max");
    locs_.view_price_min = loc("u_view_price_min");
    locs_.view_price_max = loc("u_view_price_max");
    locs_.inv_lbw = loc("u_inv_lbw");
    locs_.row0 = loc("u_row0");
    locs_.tex_w = loc("u_tex_w");
    locs_.tex_h = loc("u_tex_h");
    locs_.live_col = loc("u_live_col");
    locs_.seam_u = loc("u_seam_u");
    locs_.extend = loc("u_extend");
    locs_.magnet_frac = loc("u_magnet_frac");
    locs_.max_projection_frac = loc("u_max_projection_frac");
    locs_.gamma = loc("u_gamma");
    locs_.floor_t = loc("u_floor");
    locs_.alpha_floor = loc("u_alpha_floor");
    locs_.alpha_pow = loc("u_alpha_pow");
    locs_.opacity = loc("u_opacity");
    locs_.cascade_alpha = loc("u_cascade_alpha");
    locs_.fade_frac = loc("u_fade_frac");
    locs_.min_row_px = loc("u_min_row_px");

    glGenVertexArrays(1, &vao_);   // WebGL2 needs a bound VAO even bufferless
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);
    if (max_texture_size_ <= 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    gl_init_ok_ = true;
    return true;
}

// Own 256x1 RGB LUT via HeatmapColormap::apply — the EXACT ramp the rect path
// uses CPU-side (routes Ember/Inferno/Magma/Viridis; generation() bumps on
// change). Deliberately NOT the shared ShaderHeatmapResources liq LUT: other
// consumers bake their own opacity into that one's alpha, and this shader
// wants pure RGB with alpha computed from the design curve.
void LiqFieldTextureRenderer::rebuild_lut_if_stale() {
    const uint32_t gen = HeatmapColormap::generation();
    if (colormap_tex_ && gen == lut_generation_) return;
    lut_generation_ = gen;
    uint8_t lut[256 * 4];
    for (int i = 0; i < 256; ++i) {
        uint8_t r, g, b;
        HeatmapColormap::apply(HeatmapColormap::Type::Liquidation,
                               static_cast<float>(i) / 255.0f, r, g, b);
        lut[i * 4 + 0] = r;
        lut[i * 4 + 1] = g;
        lut[i * 4 + 2] = b;
        lut[i * 4 + 3] = 255;
    }
    if (!colormap_tex_) glGenTextures(1, &colormap_tex_);
    glBindTexture(GL_TEXTURE_2D, colormap_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Rasterization — one pass over the segment cache per Field-cache rebuild.
// The cache (liq_field_segs_) stays the source of truth; this only bakes it.
// ═══════════════════════════════════════════════════════════════════════════

bool LiqFieldTextureRenderer::ensure(const std::vector<LiqFieldSeg>& segs,
                                     const RasterParams& p) {
    if (segs.empty() || p.lbw <= 0.0 || p.norm_hi <= p.norm_lo || p.norm_lo <= 0.0f ||
        p.tf_ms <= 0 || p.latest_ms <= 0 || p.first_ms <= 0) {
        return false;
    }
    if (!init_gl()) return false;

    // Pick a fixed-size buffered window around the visible candle range. The
    // segment cache spans every loaded candle, so moving this raster window is
    // lossless and avoids the old newest-N cliff when a historical page was
    // prepended. Keep one column of right headroom when anchored at live so a
    // new building candle can arrive before the next cache rebuild.
    const long long total_cols = (p.latest_ms - p.first_ms) / p.tf_ms + 1;
    if (total_cols <= 0) return false;
    const int max_cols = std::min(kMaxCols, static_cast<int>(max_texture_size_));
    if (max_cols < 2) return false;
    const int alloc_w = static_cast<int>(std::min<long long>(total_cols + 1, max_cols));
    const long long last_data_idx = total_cols - 1;
    const long long max_start_idx = std::max<long long>(0, total_cols - (alloc_w - 1));

    long long req_first_idx = last_data_idx;
    long long req_last_idx = last_data_idx;
    if (std::isfinite(p.view_min_ms) && std::isfinite(p.view_max_ms) &&
        p.view_max_ms > p.view_min_ms) {
        const double inv_tf = 1.0 / static_cast<double>(p.tf_ms);
        const long long raw_first = static_cast<long long>(std::floor(
            (p.view_min_ms - static_cast<double>(p.first_ms)) * inv_tf));
        const long long raw_last = static_cast<long long>(std::ceil(
            (p.view_max_ms - static_cast<double>(p.first_ms)) * inv_tf));
        if (raw_last >= 0 && raw_first <= last_data_idx) {
            req_first_idx = std::clamp(raw_first, 0LL, last_data_idx);
            req_last_idx = std::clamp(raw_last, req_first_idx, last_data_idx);
        }
    }

    // Do not re-raster on every pixel of a drag. A full visible-span guard
    // (bounded 256..1024 candles) keeps normal panning uniform-only; crossing
    // it recenters the 8192-column window once.
    const long long visible_cols = req_last_idx - req_first_idx + 1;
    const long long guard_cols = std::clamp<long long>(visible_cols, 256, 1024);
    const long long need_first = (req_first_idx == 0)
        ? 0 : std::max<long long>(0, req_first_idx - guard_cols);
    const long long need_last = (req_last_idx == last_data_idx)
        ? last_data_idx : std::min(last_data_idx, req_last_idx + guard_cols);

    if (raster_ok_ && p.rebuild_gen == raster_gen_ && p.tf_ms == raster_key_tf_ms_ &&
        alloc_w == raster_key_w_) {
        const long long current_start = (t0_ms_ - p.first_ms) / p.tf_ms;
        const long long current_end = current_start + alloc_w - 1;
        if (current_start <= need_first && current_end >= need_last) return true;
    }

    long long desired_start_idx = 0;
    if (alloc_w == max_cols) {
        if (req_last_idx == last_data_idx) {
            desired_start_idx = max_start_idx;
        } else if (req_first_idx == 0) {
            desired_start_idx = 0;
        } else {
            const long long midpoint = req_first_idx + visible_cols / 2;
            desired_start_idx = std::clamp(
                midpoint - static_cast<long long>(alloc_w - 1) / 2,
                0LL, max_start_idx);
        }
    }
    const int64_t desired_t0_ms =
        p.first_ms + desired_start_idx * p.tf_ms;

    const bool same_raster_key =
        p.rebuild_gen == raster_gen_ &&
        desired_t0_ms == raster_key_t0_ms_ &&
        p.tf_ms == raster_key_tf_ms_ &&
        alloc_w == raster_key_w_;
    if (same_raster_key) return raster_ok_;

    raster_gen_ = p.rebuild_gen;
    raster_key_t0_ms_ = desired_t0_ms;
    raster_key_tf_ms_ = p.tf_ms;
    raster_key_w_ = alloc_w;
    raster_ok_ = false;
    last_live_col_uploaded_ = -1;

    const double inv_lbw = 1.0 / p.lbw;

    // Row range from the price-sorted cache: k = llround(ln(price)/lbw) recovers
    // the exact bucket index from the stored float bucket-center price.
    const long long k_min = std::llround(std::log(static_cast<double>(segs.front().price_lo)) * inv_lbw);
    long long k_max = k_min;
    for (const auto& s : segs) {
        const long long k = std::llround(std::log(static_cast<double>(s.price_hi)) * inv_lbw);
        if (k > k_max) k_max = k;
    }
    const long long rows_ll = k_max - k_min + 1;
    if (rows_ll < 1 || rows_ll > std::min<long long>(kMaxRows, max_texture_size_)) {
        // TAIKO-class ranges >4096 rows at the current bps → rect path (§1 sizing gotcha).
        return false;
    }
    const int rows = static_cast<int>(rows_ll);

    tf_ms_ = p.tf_ms;
    t0_ms_ = desired_t0_ms;
    lbw_ = p.lbw;
    k_min_ = k_min;
    rows_ = rows;
    alloc_w_ = alloc_w;
    live_col_raster_ = col_of(p.latest_ms); // may be right of an older viewport window

    // CPU raster + standing state.
    buf_.assign(static_cast<size_t>(alloc_w) * rows, 0);
    standing_tl_.assign(static_cast<size_t>(rows), 0);
    from_live_.assign(static_cast<size_t>(rows), 0);
    col_scratch_.assign(static_cast<size_t>(rows), 0);
    standing_scratch_.assign(static_cast<size_t>(rows), 0);

    // Store t = ln(f/lo)/ln(hi/lo) PRE-gamma, quantized to R8. The merge pass
    // upstream already quantizes runs at 1/96 of this ramp, so 255 steps lose
    // nothing visible.
    const float inv_lr = 1.0f / std::log(p.norm_hi / p.norm_lo);
    auto quant_tl = [&](float f) -> uint8_t {
        if (f <= p.norm_lo) return 0;   // Intensity-Low clip → dark (rect: continue)
        const float tl = std::min(std::log(f / p.norm_lo) * inv_lr, 1.0f);
        const int q = static_cast<int>(std::lround(tl * 255.0f));
        return static_cast<uint8_t>(std::clamp(q, 0, 255));
    };

    for (const auto& s : segs) {
        const uint8_t v = quant_tl(s.intensity);
        const bool pending = (s.end_ms == kSegPending);
        const long long klo = std::llround(std::log(static_cast<double>(s.price_lo)) * inv_lbw);
        const long long khi = std::llround(std::log(static_cast<double>(s.price_hi)) * inv_lbw);
        const int r0 = static_cast<int>(std::clamp(klo - k_min, 0LL, static_cast<long long>(rows - 1)));
        const int r1 = static_cast<int>(std::clamp(khi - k_min, 0LL, static_cast<long long>(rows - 1)));

        // NO-OVERLAP column resolution: light [deposit_col+1 .. consume_col-1]
        // (pending → the live column). The depositing / consuming candles' own
        // columns stay dark — whole-column version of the rect path's ±tf*0.35
        // sub-candle edges (flagged A/B parity risk).
        const int dep_col = col_of(s.start_ms);
        int c0 = dep_col + 1;
        int c1 = pending ? live_col_raster_ : col_of(s.end_ms) - 1;

        if (pending) {
            // Standing fuel drives the cascade + the per-frame live column —
            // including fuel deposited BY the raster-time live candle (its own
            // column stays dark, but its projection must still cascade).
            for (int r = r0; r <= r1; ++r) {
                if (v > standing_tl_[static_cast<size_t>(r)])
                    standing_tl_[static_cast<size_t>(r)] = v;
                if (dep_col >= live_col_raster_)
                    from_live_[static_cast<size_t>(r)] = 1;
            }
        }
        if (v == 0) continue;
        c0 = std::max(c0, 0);
        c1 = std::min(c1, alloc_w - 1);
        if (c1 < c0) continue;   // next-candle churn collapses (rect: inter-candle sliver)
        for (int r = r0; r <= r1; ++r) {
            uint8_t* row = buf_.data() + static_cast<size_t>(r) * alloc_w;
            // Rows can't collide (a bucket holds one segment per time interval),
            // but max-write is cheap insurance against float→bucket edge ties.
            for (int c = c0; c <= c1; ++c)
                if (v > row[c]) row[c] = v;
        }
    }

    // Upload the 2D field texture (R8) + allocate the 1xH standing texture.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!field_tex_) glGenTextures(1, &field_tex_);
    glBindTexture(GL_TEXTURE_2D, field_tex_);
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, alloc_w, rows, 0, GL_RED, GL_UNSIGNED_BYTE, buf_.data());
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return false;
    }
    // GL_LINEAR = the vertical feather (§4); x is center-snapped in-shader.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!standing_tex_) glGenTextures(1, &standing_tex_);
    glBindTexture(GL_TEXTURE_2D, standing_tex_);
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, rows, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return false;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    raster_ok_ = true;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-frame live edge: ONE column SubImage + the standing 1xH texture.
// ═══════════════════════════════════════════════════════════════════════════

void LiqFieldTextureRenderer::update_live(const FrameParams& f) {
    if (!raster_ok_) return;
    const int live_col_raw = col_of(f.latest_ms);
    const bool live_in_window = live_col_raw >= 0 && live_col_raw < alloc_w_;
    const int live_col = std::clamp(live_col_raw, 0, alloc_w_ - 1);

    // The live column advanced past a previously-live one (building candle
    // appeared after raster): restore that column's raster content — it's
    // history now and must not keep a stale carve.
    if (last_live_col_uploaded_ >= 0 &&
        (!live_in_window || last_live_col_uploaded_ != live_col)) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, field_tex_);
        col_scratch_.resize(static_cast<size_t>(rows_));
        for (int r = 0; r < rows_; ++r)
            col_scratch_[static_cast<size_t>(r)] =
                buf_[static_cast<size_t>(r) * alloc_w_ + last_live_col_uploaded_];
        glTexSubImage2D(GL_TEXTURE_2D, 0, last_live_col_uploaded_, 0, 1, rows_,
                        GL_RED, GL_UNSIGNED_BYTE, col_scratch_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    // Live column = standing fuel with the building-candle carve applied; fuel
    // deposited by the raster-time live candle stays dark in its own column
    // (no-overlap) while it still cascades via the standing texture.
    const bool at_raster_live = (live_col_raw <= live_col_raster_);
    for (int r = 0; r < rows_; ++r) {
        const long long k = k_min_ + r;
        const bool carved = (f.carve_khi >= f.carve_klo &&
                             k >= f.carve_klo && k <= f.carve_khi);
        const uint8_t s = standing_tl_[static_cast<size_t>(r)];
        standing_scratch_[static_cast<size_t>(r)] = carved ? 0 : s;
        col_scratch_[static_cast<size_t>(r)] =
            (carved || (at_raster_live && from_live_[static_cast<size_t>(r)])) ? 0 : s;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (live_in_window) {
        glBindTexture(GL_TEXTURE_2D, field_tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, live_col, 0, 1, rows_,
                        GL_RED, GL_UNSIGNED_BYTE, col_scratch_.data());
    }
    glBindTexture(GL_TEXTURE_2D, standing_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, rows_,
                    GL_RED, GL_UNSIGNED_BYTE, standing_scratch_.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    last_live_col_uploaded_ = live_in_window ? live_col : -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// Render — ONE draw callback per frame; zoom/scroll is pure uniform math.
// ═══════════════════════════════════════════════════════════════════════════

float LiqFieldTextureRenderer::render(const FrameParams& f) {
    if (!raster_ok_ || f.latest_ms <= 0) return -1.0f;
    rebuild_lut_if_stale();

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) return -1.0f;
    // NOTE: y_min may be ≤ 0 on a zoomed-out linear axis (sub-cent movers) —
    // the shader discards price ≤ 0 per fragment, so only degenerate spans bail.
    if (lims.Y.Max <= lims.Y.Min) return -1.0f;

    const float x_left = plot_pos.x;
    const float x_right = plot_pos.x + plot_size.x;

    // Live edge (the seam): right edge of the live candle column — the same
    // latest_ms + tf/2 the rect path uses, clamped to the plot.
    const float x_live = std::clamp(
        ImPlot::PlotToPixels(ImPlotPoint(
            static_cast<double>(f.latest_ms) + static_cast<double>(tf_ms_) * 0.5,
            lims.Y.Min)).x,
        x_left, x_right);
    // Keep the future cascade stable in candle space. Without a column cap its
    // length was a fraction of all empty space to the right, so zooming out
    // stretched standing bands across days or weeks.
    const float x_projection_end = std::clamp(
        ImPlot::PlotToPixels(ImPlotPoint(
            static_cast<double>(f.latest_ms) + static_cast<double>(tf_ms_) *
                (0.5 + std::max(0.0f, f.max_projection_cols)),
            lims.Y.Min)).x,
        x_live, x_right);
    const float x_edge = std::min(
        x_projection_end, x_live + plot_size.x * f.magnet_frac);

    auto& u = uniforms_;
    u.plot_pos_screen[0] = plot_pos.x;
    u.plot_pos_screen[1] = plot_pos.y;
    u.plot_size_screen[0] = plot_size.x;
    u.plot_size_screen[1] = plot_size.y;

    // Viewport in texture-column coordinates (CPU doubles → float; magnitudes
    // stay ≤ ~8k so float precision holds — never raw ms in float).
    u.view_col_min = static_cast<float>((lims.X.Min - static_cast<double>(t0_ms_)) /
                                        static_cast<double>(tf_ms_));
    u.view_col_max = static_cast<float>((lims.X.Max - static_cast<double>(t0_ms_)) /
                                        static_cast<double>(tf_ms_));
    u.view_price_min = static_cast<float>(lims.Y.Min);
    u.view_price_max = static_cast<float>(lims.Y.Max);
    u.inv_lbw = static_cast<float>(1.0 / lbw_);
    u.row0 = static_cast<float>(k_min_);
    u.tex_w = static_cast<float>(alloc_w_);
    u.tex_h = static_cast<float>(rows_);
    u.live_col = static_cast<float>(std::clamp(col_of(f.latest_ms), 0, alloc_w_ - 1));
    u.seam_u = std::clamp((x_live - x_left) / plot_size.x, 0.0f, 1.0f);
    u.extend = f.extend;
    u.magnet_frac = (x_right - x_live > 1.0f)
        ? std::clamp((x_edge - x_live) / (x_right - x_live), 0.0f, 1.0f) : 0.0f;
    u.max_projection_frac = (x_right - x_live > 1.0f)
        ? std::clamp((x_projection_end - x_live) / (x_right - x_live), 0.0f, 1.0f)
        : 0.0f;
    u.gamma = std::max(0.1f, f.gamma);
    u.floor_t = std::max(0.0f, f.floor_t);
    u.alpha_floor = f.alpha_floor;
    u.alpha_pow = f.alpha_pow;
    u.opacity = std::clamp(f.opacity, 0.0f, 1.0f);
    u.cascade_alpha = std::clamp(f.cascade_alpha, 0.0f, 1.0f);
    u.fade_frac = std::clamp(f.fade_frac, 0.01f, 1.0f);
    u.min_row_px = std::max(1.0f, f.min_row_px);

    // Scissor tightening: data extent = [col0 left edge .. cascade max] ×
    // the texture's price range (exactly the rc ∈ [-0.5, rows-0.5] band).
    {
        const double data_t_min = static_cast<double>(t0_ms_) -
                                  static_cast<double>(tf_ms_) * 0.5;
        const double p_lo = std::exp((static_cast<double>(k_min_) - 0.5) * lbw_);
        const double p_hi = std::exp((static_cast<double>(k_min_) + rows_ - 0.5) * lbw_);
        const double vis_p_min = std::max(p_lo, lims.Y.Min);
        const double vis_p_max = std::min(p_hi, lims.Y.Max);
        if (vis_p_max <= vis_p_min) return -1.0f;
        const float data_x0 = std::max(x_left,
            ImPlot::PlotToPixels(ImPlotPoint(data_t_min, vis_p_min)).x);
        const float data_x1 = (f.extend == 1)
            ? std::min(x_right, x_projection_end + 1.0f)
            : std::min(x_right, x_edge + 1.0f);
        const ImVec2 tl_px = ImPlot::PlotToPixels(ImPlotPoint(data_t_min, vis_p_max));
        const ImVec2 br_px = ImPlot::PlotToPixels(ImPlotPoint(data_t_min, vis_p_min));
        u.data_screen_min[0] = data_x0;
        u.data_screen_min[1] = tl_px.y;     // top (higher price)
        u.data_screen_max[0] = data_x1;
        u.data_screen_max[1] = br_px.y;     // bottom
        if (u.data_screen_max[0] <= u.data_screen_min[0]) return -1.0f;
    }

    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    dl->AddCallback(gl_render_callback, this);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    ImPlot::PopPlotClipRect();
    return x_live;
}

void LiqFieldTextureRenderer::gl_render_callback(const ImDrawList* /*parent*/,
                                                 const ImDrawCmd* cmd) {
    auto* self = static_cast<LiqFieldTextureRenderer*>(cmd->UserCallbackData);
    if (!self || !self->raster_ok_ || !self->gl_init_ok_) return;
    const auto& u = self->uniforms_;
    const auto& loc = self->locs_;

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd) return;
    const float fb_scale_x = dd->FramebufferScale.x;
    const float fb_scale_y = dd->FramebufferScale.y;
    const float fb_h = dd->DisplaySize.y * fb_scale_y;

    // Scissor = ImGui ClipRect ∩ data extent (the OB heatmap's proven FPS
    // optimization — the shader only runs where the field can exist).
    const float clip_x0 = (cmd->ClipRect.x - dd->DisplayPos.x) * fb_scale_x;
    const float clip_y0 = (cmd->ClipRect.y - dd->DisplayPos.y) * fb_scale_y;
    const float clip_x1 = (cmd->ClipRect.z - dd->DisplayPos.x) * fb_scale_x;
    const float clip_y1 = (cmd->ClipRect.w - dd->DisplayPos.y) * fb_scale_y;
    const float data_x0 = (u.data_screen_min[0] - dd->DisplayPos.x) * fb_scale_x;
    const float data_y0 = (u.data_screen_min[1] - dd->DisplayPos.y) * fb_scale_y;
    const float data_x1 = (u.data_screen_max[0] - dd->DisplayPos.x) * fb_scale_x;
    const float data_y1 = (u.data_screen_max[1] - dd->DisplayPos.y) * fb_scale_y;
    const float ix0 = std::max(clip_x0, data_x0);
    const float iy0 = std::max(clip_y0, data_y0);
    const float ix1 = std::min(clip_x1, data_x1);
    const float iy1 = std::min(clip_y1, data_y1);
    if (ix0 >= ix1 || iy0 >= iy1) return;
    glScissor(static_cast<int>(ix0), static_cast<int>(fb_h - iy1),
              static_cast<int>(ix1 - ix0), static_cast<int>(iy1 - iy0));

    // Plot origin/size in GL framebuffer coords (bottom-left origin).
    const float px = (u.plot_pos_screen[0] - dd->DisplayPos.x) * fb_scale_x;
    const float py = (u.plot_pos_screen[1] - dd->DisplayPos.y) * fb_scale_y;
    const float pw = u.plot_size_screen[0] * fb_scale_x;
    const float ph = u.plot_size_screen[1] * fb_scale_y;

    glUseProgram(self->program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, self->field_tex_);
    glUniform1i(loc.field, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, self->standing_tex_);
    glUniform1i(loc.standing, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, self->colormap_tex_);
    glUniform1i(loc.colormap, 2);

    glUniform2f(loc.plot_origin, px, fb_h - (py + ph));
    glUniform2f(loc.plot_size, pw, ph);
    glUniform1f(loc.view_col_min, u.view_col_min);
    glUniform1f(loc.view_col_max, u.view_col_max);
    glUniform1f(loc.view_price_min, u.view_price_min);
    glUniform1f(loc.view_price_max, u.view_price_max);
    glUniform1f(loc.inv_lbw, u.inv_lbw);
    glUniform1f(loc.row0, u.row0);
    glUniform1f(loc.tex_w, u.tex_w);
    glUniform1f(loc.tex_h, u.tex_h);
    glUniform1f(loc.live_col, u.live_col);
    glUniform1f(loc.seam_u, u.seam_u);
    glUniform1i(loc.extend, u.extend);
    glUniform1f(loc.magnet_frac, u.magnet_frac);
    glUniform1f(loc.max_projection_frac, u.max_projection_frac);
    glUniform1f(loc.gamma, u.gamma);
    glUniform1f(loc.floor_t, u.floor_t);
    glUniform1f(loc.alpha_floor, u.alpha_floor);
    glUniform1f(loc.alpha_pow, u.alpha_pow);
    glUniform1f(loc.opacity, u.opacity);
    glUniform1f(loc.cascade_alpha, u.cascade_alpha);
    glUniform1f(loc.fade_frac, u.fade_frac);
    glUniform1f(loc.min_row_px, u.min_row_px);

    glBindVertexArray(self->vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    // ImDrawCallback_ResetRenderState follows and restores ImGui's GL state.
}
