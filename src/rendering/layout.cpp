#include "layout.h"
#include "theme.h"
#include "imgui_internal.h"
#include "../core/education_boot.h"
#include <algorithm>

bool LayoutManager::is_initialized = false;
float LayoutManager::top_reserve = 0.0f;
float LayoutManager::bottom_reserve = 0.0f;
float LayoutManager::status_reserve = 0.0f;
float LayoutManager::left_reserve = 0.0f;
std::string LayoutManager::pending_exchange;
std::string LayoutManager::pending_symbol;

void LayoutManager::setup_default_layout(const std::string& exchange, const std::string& symbol) {
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    ImGuiID dock_main = dockspace_id;

    // Chart dock identity is TF-independent (after "###") so the chart stays docked
    // when its timeframe changes. Must match the id ChartWidget builds in its title.
    std::string chart_name  = "Chart " + exchange + " " + symbol + "###chart_" + exchange + "_" + symbol;
    std::string dom_name    = "DOM " + exchange + " " + symbol;
    std::string trades_name = "T " + exchange + " " + symbol;

    if (EducationBoot::instance().is_embedded()) {
        // Lesson/Studio layout: NO Watchlist, NO Depth. Chart fills the center;
        // the right column is the Order Flow read - DOM ladder on top, tape below.
        //
        // Size the right column in PIXELS (~RIGHTCOL_W) rather than a flat ratio.
        // The DOM ladder carries ~272px of FIXED columns (BUYS/PRICE/SELLS/DELTA)
        // under ImGuiTableFlags_NoHostExtendX, so a flat 0.26 ratio clipped its
        // rightmost columns + the tape's TIME column on narrower canvases (a laptop,
        // or the lesson with its left rail eating width). Clamp to a sane band so it
        // can't swallow the chart on small screens or look lost on huge ones.
        const float vw = ImGui::GetMainViewport()->Size.x;
        const float right_ratio =
            std::clamp(Theme::Layout::RIGHTCOL_W / std::max(vw, 1.0f), 0.22f, 0.42f);
        ImGuiID dock_right;
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, &dock_right, &dock_main);

        // DOM over TAPE at a ~63/37 split (DOM ~15% taller, tape ~15% shorter than the
        // old 56/44) - the ladder is the focus of the read, the tape is the single
        // order-flow scan column under it.
        ImGuiID dock_right_trades;
        ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.365f, &dock_right_trades, &dock_right);

        ImGui::DockBuilderDockWindow(chart_name.c_str(), dock_main);
        ImGui::DockBuilderDockWindow(dom_name.c_str(), dock_right);
        ImGui::DockBuilderDockWindow(trades_name.c_str(), dock_right_trades);

        ImGui::DockBuilderFinish(dockspace_id);
        is_initialized = true;
        return;
    }

    // Watchlist sidebar - fixed-ish WATCHLIST_W via ratio of viewport width
    const float vw = ImGui::GetMainViewport()->Size.x;
    const float wl_ratio = std::clamp(Theme::Layout::WATCHLIST_W / std::max(vw, 1.0f), 0.10f, 0.30f);
    ImGuiID dock_left;
    ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, wl_ratio, &dock_left, &dock_main);

    // Right column = the Order Flow read only: DOM ladder on top, trade tape
    // below. (Phase E dropped the separate Depth from the default dock; it stays
    // available via +Widget.) Sized in PIXELS (~RIGHTCOL_W) so the ladder's fixed
    // columns can't clip; clamped so it can't swallow the chart or look lost.
    const float right_ratio =
        std::clamp(Theme::Layout::RIGHTCOL_W / std::max(vw, 1.0f), 0.22f, 0.40f);
    ImGuiID dock_right;
    ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, &dock_right, &dock_main);

    // DOM over TAPE at a ~63/37 split (DOM ~15% taller, tape ~15% shorter than the old 56/44).
    ImGuiID dock_right_trades;
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.365f, &dock_right_trades, &dock_right);

    ImGui::DockBuilderDockWindow("Watchlist", dock_left);
    ImGui::DockBuilderDockWindow(chart_name.c_str(), dock_main);
    ImGui::DockBuilderDockWindow(dom_name.c_str(), dock_right);
    ImGui::DockBuilderDockWindow(trades_name.c_str(), dock_right_trades);

    ImGui::DockBuilderFinish(dockspace_id);
    is_initialized = true;
}

void LayoutManager::render_dockspace(const std::function<void()>& menu_callback,
                                      const std::string& exchange,
                                      const std::string& symbol) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + left_reserve,
                                   viewport->Pos.y + top_reserve));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x - left_reserve,
                                    viewport->Size.y - top_reserve - bottom_reserve
                                                     - status_reserve));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    if (!is_initialized) {
        const bool has_pending_pair = !pending_exchange.empty() && !pending_symbol.empty();
        setup_default_layout(
            has_pending_pair ? pending_exchange : exchange,
            has_pending_pair ? pending_symbol : symbol);
        pending_exchange.clear();
        pending_symbol.clear();
    }

    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    if (menu_callback) {
        menu_callback();
    }

    ImGui::End();
}

void LayoutManager::reset_layout() {
    is_initialized = false;
    pending_exchange.clear();
    pending_symbol.clear();
}

void LayoutManager::reset_layout_for(
    const std::string& exchange, const std::string& symbol) {
    is_initialized = false;
    pending_exchange = exchange;
    pending_symbol = symbol;
}
