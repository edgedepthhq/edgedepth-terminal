#include "shader_heatmap_resources.h"
#include "core/heatmap_colormap.h"
#include <cstdio>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// GLSL ES 3.0 Shader Sources
// ═══════════════════════════════════════════════════════════════════════════════

static const char* kVertexShaderSrc = R"(#version 300 es
// Fullscreen triangle — no vertex buffer needed.
// gl_VertexID 0,1,2 → covers entire clip space.
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFragmentShaderSrc = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

uniform sampler2D u_data;      // R32F ring buffer, RING_SIZE × MAX_ROWS
uniform sampler2D u_meta;      // RGBA32F per-column, RING_SIZE × 1
uniform sampler2D u_colormap;  // RGBA8 color LUT, 256 × 1 (cool/cyan for liq)
uniform sampler2D u_colormap_warm; // RGBA8 warm color LUT, 256 × 1 (amber for liq)
uniform sampler2D u_reach_data;    // R32F reach_prob texture (Phase 2a)

uniform vec2 u_plot_origin;     // Bottom-left of plot area in window pixels
uniform vec2 u_plot_size;       // Plot area size in pixels (after framebuffer scale)

// Viewport mapping (from ImPlot::GetPlotLimits)
uniform float u_viewport_time_min;  // Left edge (seconds since epoch)
uniform float u_viewport_time_max;  // Right edge
uniform float u_viewport_price_min; // Bottom price
uniform float u_viewport_price_max; // Top price

// Ring buffer state
uniform float u_data_time_start;   // Timestamp of oldest valid column (seconds)
uniform float u_time_step;         // Seconds per column
uniform int u_ring_start;          // Ring index of oldest valid column
uniform int u_ring_count;          // Number of valid columns
uniform int u_ring_size;           // 4096
uniform int u_max_rows;            // 1024

// Rendering params
uniform float u_bucket_size;       // Native price bucket size
uniform int u_bucket_multiplier;   // Display coarsening factor
uniform float u_sensitivity;       // Brightness (orderbook mode)
uniform float u_max_qty;           // Global max value (orderbook normalization)
uniform float u_color_low;         // Noise floor (liq mode)
uniform float u_color_peak;        // Max intensity (liq mode)
uniform int u_mode;                // 0=orderbook, 1=liquidation
uniform float u_opacity;           // Global alpha multiplier
uniform int u_use_reach;           // Phase 2a: 1=modulate alpha by reach_prob

out vec4 fragColor;

void main() {
    // Map fragment position to [0,1] UV within the plot area.
    // gl_FragCoord is in WINDOW coordinates — subtract the plot origin
    // to get position relative to the plot's bottom-left corner.
    vec2 uv = (gl_FragCoord.xy - u_plot_origin) / u_plot_size;

    // Map UV to plot coordinates
    float time = mix(u_viewport_time_min, u_viewport_time_max, uv.x);
    // Y: gl_FragCoord.y=0 is bottom in GL, which is price_min
    float price = mix(u_viewport_price_min, u_viewport_price_max, uv.y);

    // Map time → data column offset from the oldest column
    float col_offset_f = (time - u_data_time_start) / u_time_step;
    int col_offset = int(floor(col_offset_f));

    // Bounds check: is this time within our data range?
    if (col_offset < 0 || col_offset >= u_ring_count) discard;

    // Map to ring buffer texture column
    int tex_col = (u_ring_start + col_offset) % u_ring_size;

    // Read per-column metadata:
    // R = price_min, G = num_valid_rows, B = max_value, A = flags
    vec4 meta = texelFetch(u_meta, ivec2(tex_col, 0), 0);
    float col_price_min = meta.r;
    int col_num_rows = int(meta.g);
    // float col_max_value = meta.b;  // Available for per-column normalization
    float col_flags = meta.a;

    if (col_flags < 0.5) discard; // Empty column (flags == 0)
    if (col_num_rows < 1) discard;

    // Map price → row index within this column
    float display_bucket = u_bucket_size * float(u_bucket_multiplier);
    int base_row = int(floor((price - col_price_min) / u_bucket_size));

    // Early out: if the row is completely outside this column's data range,
    // skip the aggregation loop entirely. Major FPS win for sparse data
    // (liq heatmap) where most of the plot height has no data.
    int agg_base = (base_row / u_bucket_multiplier) * u_bucket_multiplier;
    if (agg_base + u_bucket_multiplier <= 0 || agg_base >= col_num_rows) discard;

    // ── VALUE SAMPLING ──────────────────────────────────────────────
    float value = 0.0;

    if (u_mode == 1) {
        // ── LIQUIDATION: band mode — snap to the aggregated bucket ──
        // Band-based render: take the max |value| within the aggregated bucket
        // with NO cross-band interpolation, so each price band renders as a
        // distinct horizontal level (matches the band design). The soft glow
        // comes from the alpha smoothstep + power curve below, not from smearing
        // adjacent bands into a continuous cloud. (Previous V7 lerped between
        // buckets with a cubic smoothstep — that produced the diffuse cloud.)
        for (int d = 0; d < u_bucket_multiplier; d++) {
            int r = agg_base + d;
            if (r >= 0 && r < col_num_rows && r < u_max_rows) {
                float v = abs(texelFetch(u_data, ivec2(tex_col, r), 0).r);
                value = max(value, v);
            }
        }
    } else {
        // ── ORDERBOOK: Integer snap, sum aggregation (crisp discrete levels) ──
        for (int d = 0; d < u_bucket_multiplier; d++) {
            int r = agg_base + d;
            if (r >= 0 && r < col_num_rows && r < u_max_rows) {
                float v = texelFetch(u_data, ivec2(tex_col, r), 0).r;
                value += v;
            }
        }
    }

    // ── NORMALIZATION ───────────────────────────────────────────────
    float t;
    if (u_mode == 0) {
        // Orderbook: max_qty normalization
        if (u_max_qty < 0.0001) discard;
        t = clamp(value * u_sensitivity / u_max_qty, 0.0, 1.0);
    } else {
        // Liquidation: low/peak normalization (value already abs from interp above)
        if (value < u_color_low) discard;
        float range = u_color_peak - u_color_low;
        if (range < 0.0001) discard;
        t = clamp((value - u_color_low) / range, 0.0, 1.0);
    }

    // Discard threshold: liquidation mode discards sub-visible fragments.
    float discard_threshold = (u_mode == 1) ? 0.07 : 0.004;  // V9b: 0.07 keeps the dim dense background (MMT look) with bands still bright; tuned in the offline render harness. (0.15 carved to distinct-bands-no-background; 0.02 = full cloud.)
    if (t < discard_threshold) discard;

    // ── POWER CURVE (liq only) ──────────────────────────────────────
    // V7d: Gentle contrast — pow(t, 1.3) lets purple/magenta mid-range show
    // as visible halos while still differentiating peaks from background.
    // t^1.3: 0.1→0.05, 0.2→0.13, 0.3→0.22, 0.5→0.41, 0.8→0.74
    if (u_mode == 1) {
        t = pow(t, 1.3);
    }

    // ── COLORMAP LOOKUP ─────────────────────────────────────────────
    // V7: Unified inferno colormap for both directions — always use cool LUT.
    fragColor = texture(u_colormap, vec2(t, 0.5));

    // V7: Smooth alpha fade for liquidation — creates glowing halo effect.
    // The LUT alpha handles the base curve; this smoothstep adds an extra
    // gradient at the lowest values for seamless fade into the background.
    if (u_mode == 1) {
        float smooth_alpha = smoothstep(0.0, 0.15, t);
        fragColor.a *= smooth_alpha;
    }

    // Phase 2a: Reach-probability opacity modulation.
    // Bands with low reach probability fade out, creating a "cone" of
    // visibility around the mark price. pow(reach, 2.5) sharpens the
    // falloff — GBM analytical values bunch in 20-60% range for visible
    // bands, the power curve spreads them: 0.5→0.18, 0.3→0.05, 0.2→0.02.
    if (u_use_reach == 1 && u_mode == 1) {
        float reach = texelFetch(u_reach_data, ivec2(tex_col, base_row), 0).r;
        float shaped = pow(clamp(reach, 0.0, 1.0), 2.5);
        fragColor.a *= smoothstep(0.02, 0.4, shaped);
    }

    fragColor.a *= u_opacity;
}
)";

// ═══════════════════════════════════════════════════════════════════════════════
// Implementation
// ═══════════════════════════════════════════════════════════════════════════════

ShaderHeatmapResources& ShaderHeatmapResources::instance() {
    static ShaderHeatmapResources s_instance;
    return s_instance;
}

bool ShaderHeatmapResources::init() {
    if (initialized_) return true;

    if (!compile_shader()) {
        return false;
    }
    cache_uniforms();

    // Create empty VAO (required by WebGL2 even for bufferless rendering)
    glGenVertexArrays(1, &vao_);

    // Build colormap textures from HeatmapColormap (reuse existing LUT code)
    build_colormap_texture(colormap_ob_, 0, 1.0f);
    build_colormap_texture(colormap_liq_, 1, 1.0f);
    build_colormap_texture(colormap_liq_warm_, 2, 1.0f);

    initialized_ = true;
    return true;
}

void ShaderHeatmapResources::destroy() {
    if (!initialized_) return;
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (colormap_ob_) { glDeleteTextures(1, &colormap_ob_); colormap_ob_ = 0; }
    if (colormap_liq_) { glDeleteTextures(1, &colormap_liq_); colormap_liq_ = 0; }
    if (colormap_liq_warm_) { glDeleteTextures(1, &colormap_liq_warm_); colormap_liq_warm_ = 0; }
    initialized_ = false;
}

bool ShaderHeatmapResources::compile_shader() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(vs, 1, &kVertexShaderSrc, nullptr);
    glCompileShader(vs);
    if (!check_shader(vs, "Vertex")) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glShaderSource(fs, 1, &kFragmentShaderSrc, nullptr);
    glCompileShader(fs);
    if (!check_shader(fs, "Fragment")) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!check_program(program_)) {
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    return true;
}

bool ShaderHeatmapResources::check_shader(GLuint shader, const char* name) {
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        return false;
    }
    return true;
}

bool ShaderHeatmapResources::check_program(GLuint program) {
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetProgramInfoLog(program, sizeof(info), nullptr, info);
        return false;
    }
    return true;
}

void ShaderHeatmapResources::cache_uniforms() {
    auto loc = [this](const char* name) {
        return glGetUniformLocation(program_, name);
    };
    uniforms_.u_data = loc("u_data");
    uniforms_.u_meta = loc("u_meta");
    uniforms_.u_colormap = loc("u_colormap");
    uniforms_.u_colormap_warm = loc("u_colormap_warm");
    uniforms_.u_plot_origin = loc("u_plot_origin");
    uniforms_.u_plot_size = loc("u_plot_size");
    uniforms_.u_viewport_time_min = loc("u_viewport_time_min");
    uniforms_.u_viewport_time_max = loc("u_viewport_time_max");
    uniforms_.u_viewport_price_min = loc("u_viewport_price_min");
    uniforms_.u_viewport_price_max = loc("u_viewport_price_max");
    uniforms_.u_data_time_start = loc("u_data_time_start");
    uniforms_.u_time_step = loc("u_time_step");
    uniforms_.u_ring_start = loc("u_ring_start");
    uniforms_.u_ring_count = loc("u_ring_count");
    uniforms_.u_ring_size = loc("u_ring_size");
    uniforms_.u_max_rows = loc("u_max_rows");
    uniforms_.u_bucket_size = loc("u_bucket_size");
    uniforms_.u_bucket_multiplier = loc("u_bucket_multiplier");
    uniforms_.u_sensitivity = loc("u_sensitivity");
    uniforms_.u_max_qty = loc("u_max_qty");
    uniforms_.u_color_low = loc("u_color_low");
    uniforms_.u_color_peak = loc("u_color_peak");
    uniforms_.u_mode = loc("u_mode");
    uniforms_.u_opacity = loc("u_opacity");
    uniforms_.u_reach_data = loc("u_reach_data");
    uniforms_.u_use_reach = loc("u_use_reach");
}

void ShaderHeatmapResources::build_colormap_texture(GLuint& tex, int type, float opacity) {
    uint32_t lut[256];
    auto cm_type = (type == 0) ? HeatmapColormap::Type::Orderbook
                 : (type == 2) ? HeatmapColormap::Type::LiquidationWarm
                               : HeatmapColormap::Type::Liquidation;
    HeatmapColormap::build_packed_lut(cm_type, opacity, lut);

    if (tex == 0) {
        glGenTextures(1, &tex);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, lut);
    // LINEAR filtering gives smooth colormap interpolation between the 256 entries
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderHeatmapResources::rebuild_colormap(int type, float opacity) {
    if (type == 0) {
        build_colormap_texture(colormap_ob_, 0, opacity);
    } else if (type == 2) {
        build_colormap_texture(colormap_liq_warm_, 2, opacity);
    } else {
        build_colormap_texture(colormap_liq_, 1, opacity);
    }
}
