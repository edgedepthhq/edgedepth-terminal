#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "imgui.h"
#include "implot.h"

/**
 * PriceProfileRenderer - shared sidebar histogram renderer.
 *
 * Used by both the Liquidation Heatmap profile and the Volume Profile.
 * Each feature builds a vector of ProfileBand from its own data model,
 * then hands it to this renderer for drawing.
 *
 * Rendering layers (back to front):
 *   1. Full-width background wash (optional, subtle tint)
 *   2. Primary bar (right-aligned solid rect)
 *   3. Secondary bar (inner overlay, e.g. observed liq or buy volume)
 *   4. Edge highlight (outline for special bands)
 *   5. Labels (right-aligned text with shadow)
 *   6. Price markers (POC/VAH/VAL or mark price lines)
 *   7. Tooltip on hover
 */

namespace Profile {

struct Color {
    uint8_t r, g, b;
};

struct ProfileBand {
    double price_mid;
    double band_width;          // price height of this band

    float  primary_value;       // normalized 0-1, sizes the main bar
    Color  primary_color;       // main bar color
    float  primary_alpha;       // base alpha for main bar (0-1)

    float  secondary_value;     // normalized 0-1, sizes inner overlay bar (0 = none)
    Color  secondary_color;     // inner bar color
    float  secondary_alpha;     // alpha for inner bar

    bool   show_edge;           // draw outline highlight
    Color  edge_color;
    float  edge_alpha;

    // Label (nullptr = no label)
    const char* label;          // short label text, caller owns lifetime
    char   label_buf[64];       // storage for formatted label

    // Background wash
    float  wash_alpha;          // 0 = no wash, >0 = subtle full-width tint

    // Tooltip data (opaque to renderer, shown on hover)
    bool   has_tooltip;
    int    source_index = -1;   // index into the original data source (for tooltip lookup)
};

struct PriceMarker {
    double price;
    ImU32  color;
    const char* label;          // "POC", "VAH", "mark", etc.
    bool   dashed;
    char   label_buf[16];
};

struct ProfileConfig {
    float  max_bar_width_pct = 0.25f;   // max bar as fraction of plot width
    bool   right_aligned = true;         // bars grow from right edge
    bool   draw_wash = true;             // draw background tint
    float  label_min_band_height = 12.0f; // min pixels to show labels
};

// Main render function - call inside ImPlot::BeginPlot/EndPlot
void render_profile(
    ImDrawList* draw_list,
    const ImPlotRect& limits,
    const ImVec2& plot_pos,
    const ImVec2& plot_size,
    const std::vector<ProfileBand>& bands,
    const std::vector<PriceMarker>& markers,
    const ProfileConfig& config = {}
);

// Tooltip callback type - called when a band is hovered
using TooltipCallback = void(*)(const ProfileBand& band, int band_index, void* user_data);

void render_profile_with_tooltip(
    ImDrawList* draw_list,
    const ImPlotRect& limits,
    const ImVec2& plot_pos,
    const ImVec2& plot_size,
    const std::vector<ProfileBand>& bands,
    const std::vector<PriceMarker>& markers,
    const ProfileConfig& config,
    TooltipCallback tooltip_cb,
    void* tooltip_user_data
);

} // namespace Profile