#pragma once

#include <cstdint>

/**
 * Shared colormap functions used by ShaderHeatmapRenderer
 * and ShaderHeatmapResources. Extracted to avoid code duplication.
 */
namespace HeatmapColormap {

enum class Type { Orderbook, Liquidation, LiquidationWarm };

// Liquidation-field colormap selection (Tweaks panel: Ember/Inferno/Magma/Viridis).
// Routes every Type::Liquidation apply()/LUT build — consumers don't change.
// Ember-K is the default (2026-07-02 redesign): a hand ramp with a
// long black→violet toe and a rare orange→yellow ignition top. Stop table =
// the design system's liq-heatmap-colormap.json.
enum class LiqMap : uint8_t { Ember, Viridis, Magma, Inferno };
LiqMap  liq_map();
void    set_liq_map(LiqMap m);   // bumps generation()
// Increments whenever the selected map changes; consumers caching LUTs
// compare against this to know when to rebuild.
uint32_t generation();

/// Apply colormap to a normalized value t ∈ [0,1] → RGB
void apply(Type type, float t,
           uint8_t& r, uint8_t& g, uint8_t& b);

/// Build a 256-entry packed RGBA LUT for fast texture generation.
/// opacity: global alpha multiplier [0,1]
void build_packed_lut(Type type, float opacity,
                      uint32_t* out_lut256);

/// Build a 256-entry ImGui color LUT (for labels).
void build_lut(Type type, uint32_t* out_lut256);

} // namespace HeatmapColormap
