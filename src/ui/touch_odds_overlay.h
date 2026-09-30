#pragma once
#include "types/types.h"
#include "core/touch_zones.h"
#include <cstdint>
// Heatmap V2 touch odds and the P9 cascade tier on live Binance charts, admin preview.
// Data comes only from the web host's window.__EDGEDEPTH_REACH__ (see core/touch_odds.h).
namespace touch_odds_overlay {
// True once the web host has published (it mounts the bridge for admins only).
bool available(const Terminal::Pair& pair);
// End of the current forecast window for this pair, or 0; the chart reserves future margin to it.
int64_t window_end(const Terminal::Pair& pair, int64_t clock);
void menu(const Terminal::Pair& pair, bool& enabled);
void render(const Terminal::Pair& pair, int64_t clock, const char* price_fmt, double live_price, bool replay = false);
// Calibrated 1h/4h/24h touch odds on the strongest scenario-map zones (both layers on).
void zone_labels(const Terminal::Pair& pair, int64_t clock, double live_price, const char* price_fmt,
                 const std::vector<touch_zones::Zone>& zones, bool replay = false);
}
