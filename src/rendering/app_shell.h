#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// app_shell.h — Fixed chrome: topbar (44px) + stats strip (33px)
//
// Ports the .topbar and .statsbar chrome from the design system. Rendered as
// fixed, undockable ImGui windows pinned to the top of the viewport; the
// dockspace below is offset by total_height(). The replay transport (bottom)
// stays owned by ReplayManager.
//
// Data: mark/funding/OI/liqs come from a tiny always-on stats subscription
// for the active symbol; 24h price/change/volume come from TickerManager
// (global ticker24h stream, subscribed once at init).
// ═══════════════════════════════════════════════════════════════════════════════
#include <memory>
#include <string>
#include <vector>

#include "core/app_context.h"
#include "types/types.h"

class Widget;
class ChartWidget;

namespace AppShell {

    // TOPBAR_H + STATSBAR_H — dockspace top offset
    float total_height();

    // Favourites timeframe bar + grouped dropdown, moved out of the top bar in
    // v2 3b so the chart toolbar can host it (matches the /terminal?event= chrome).
    // Draws at the current ImGui cursor; returns the bar width in px.
    float render_chart_tf(ChartWidget* chart);

    // Call once after managers exist and the WS is connected; subscribes the
    // shell's stats handler for the active symbol + the global ticker24h feed.
    void init(const AppContext& ctx, const Terminal::Pair& active_pair);

    // Render both bars. Call every frame before the dockspace.
    // Needs the widget list because the "+ Widget" menu creates widgets and
    // the symbol picker popup is owned by the shell now.
    void render(std::vector<std::unique_ptr<Widget>>& widgets,
                const AppContext& ctx);

    // Draw ONLY the bottom telemetry bar (symbol · WS · FPS · frame · UTC), no
    // topbar/statsbar. For the embedded event/demo chromes that suppress the
    // native shell but still want the live status strip. symbol_lc = active
    // symbol (lowercase); ws_ok drives the connection pill.
    void render_statusbar(const AppContext& ctx, const std::string& symbol_lc, bool ws_ok);
}
