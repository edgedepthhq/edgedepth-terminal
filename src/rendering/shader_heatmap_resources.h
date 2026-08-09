#pragma once

#include <GLES3/gl3.h>
#include <cstdint>

/**
 * ShaderHeatmapResources — Shared GPU resources for all shader heatmap instances.
 *
 * Owns the shader program, empty VAO, and colormap textures. Compiled once at
 * startup and shared across all ShaderHeatmapRenderer instances (OB + liq,
 * multiple symbols).
 *
 * Call init() once after GL context is ready. Call destroy() at shutdown.
 * All methods must be called from the main (GL) thread.
 */
class ShaderHeatmapResources {
public:
    static ShaderHeatmapResources& instance();

    /// Initialize all GPU resources. Call once after GL context creation.
    bool init();

    /// Release all GPU resources. Call at shutdown.
    void destroy();

    bool is_initialized() const { return initialized_; }

    // ── Shared GPU handles ──
    GLuint shader_program() const { return program_; }
    GLuint empty_vao() const { return vao_; }
    GLuint colormap_orderbook() const { return colormap_ob_; }
    GLuint colormap_liquidation() const { return colormap_liq_; }
    GLuint colormap_liquidation_warm() const { return colormap_liq_warm_; }

    /// Rebuild a colormap texture (e.g., after opacity change).
    /// type: 0=orderbook, 1=liquidation
    void rebuild_colormap(int type, float opacity);

    // ── Cached uniform locations ──
    struct Uniforms {
        GLint u_data = -1;
        GLint u_meta = -1;
        GLint u_colormap = -1;
        GLint u_colormap_warm = -1;  // Warm colormap for directional liq heatmap
        GLint u_plot_origin = -1;
        GLint u_plot_size = -1;
        GLint u_viewport_time_min = -1;
        GLint u_viewport_time_max = -1;
        GLint u_viewport_price_min = -1;
        GLint u_viewport_price_max = -1;
        GLint u_data_time_start = -1;
        GLint u_time_step = -1;
        GLint u_ring_start = -1;
        GLint u_ring_count = -1;
        GLint u_ring_size = -1;
        GLint u_max_rows = -1;
        GLint u_bucket_size = -1;
        GLint u_bucket_multiplier = -1;
        GLint u_sensitivity = -1;
        GLint u_max_qty = -1;
        GLint u_color_low = -1;
        GLint u_color_peak = -1;
        GLint u_mode = -1;
        GLint u_opacity = -1;
        GLint u_reach_data = -1;   // Phase 2a: reach_prob R32F texture sampler
        GLint u_use_reach = -1;    // Phase 2a: 0=off, 1=modulate alpha by reach_prob
    };
    const Uniforms& uniforms() const { return uniforms_; }

private:
    ShaderHeatmapResources() = default;
    ~ShaderHeatmapResources() = default;
    ShaderHeatmapResources(const ShaderHeatmapResources&) = delete;
    ShaderHeatmapResources& operator=(const ShaderHeatmapResources&) = delete;

    bool compile_shader();
    void cache_uniforms();
    void build_colormap_texture(GLuint& tex, int type, float opacity);
    static bool check_shader(GLuint shader, const char* name);
    static bool check_program(GLuint program);

    bool initialized_ = false;
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint colormap_ob_ = 0;       // Orderbook colormap 256×1 RGBA8
    GLuint colormap_liq_ = 0;      // Liquidation cool colormap 256×1 RGBA8
    GLuint colormap_liq_warm_ = 0; // Liquidation warm colormap 256×1 RGBA8
    Uniforms uniforms_{};
};
