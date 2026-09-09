#pragma once
#include "imgui.h"
#include <string>
#include <functional>

class Widget;

class LayoutManager {
public:
    static bool vertical_siblings(const Widget& first, const Widget& second);
    static void setup_default_layout(const std::string& exchange, const std::string& symbol);
    static void render_dockspace(const std::function<void()>& menu_callback,
                                  const std::string& exchange = "binancef",
                                  const std::string& symbol = "btcusdt");
    static void reset_layout();
    static void restore_layout_for(const std::string& exchange, const std::string& symbol);
    static void reset_layout_for(const std::string& exchange, const std::string& symbol);
    static bool is_initialized;

    // True when the dock tree currently in place was built for this pair.
    //
    // setup_default_layout docks by EXACT title string ("DOM <ex> <sym>",
    // "T <ex> <sym>", "Chart ...###chart_<ex>_<sym>"), so a layout built for one
    // symbol has no node for another symbol's windows and they come up floating.
    // Callers that swap the symbol under a live layout must ask this rather than
    // whether a chart merely exists, which is symbol-blind.
    static bool layout_matches(const std::string& exchange, const std::string& symbol);
    static float top_reserve;     // Pixels reserved at top (topbar + statsbar)
    static float bottom_reserve;  // Pixels reserved at bottom (e.g., replay control bar)
    static float status_reserve;  // Pixels reserved for the bottom status bar (telemetry);
                                  // stacked UNDER bottom_reserve so the replay bar sits above it
    static float left_reserve;    // Pixels reserved at the left edge (drawing-tools rail)

private:
    static std::string pending_exchange;
    static std::string pending_symbol;
    static std::string layout_exchange;
    static std::string layout_symbol;
};
