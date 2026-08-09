#pragma once
#include "imgui.h"
#include <string>
#include <functional>

class LayoutManager {
public:
    static void setup_default_layout(const std::string& exchange, const std::string& symbol);
    static void render_dockspace(const std::function<void()>& menu_callback,
                                  const std::string& exchange = "binancef",
                                  const std::string& symbol = "btcusdt");
    static void reset_layout();
    static bool is_initialized;
    static float top_reserve;     // Pixels reserved at top (topbar + statsbar)
    static float bottom_reserve;  // Pixels reserved at bottom (e.g., replay control bar)
    static float status_reserve;  // Pixels reserved for the bottom status bar (telemetry);
                                  // stacked UNDER bottom_reserve so the replay bar sits above it
    static float left_reserve;    // Pixels reserved at the left edge (drawing-tools rail)
};