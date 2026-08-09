#pragma once
#include "indicator_base.h"
#include <memory>
#include <vector>
#include <typeinfo>

namespace Indicators {

    class IndicatorManager {
    public:
        IndicatorManager() = default;

        void add_indicator(std::unique_ptr<IndicatorBase> indicator);
        // Get indicator by index
        IndicatorBase* get_indicator(size_t index);
        // Get indicator by type (type-safe!)
        template<typename T>
        T* get_indicator_of_type() {
            for (auto& indicator : indicators) {
                if (auto* typed = dynamic_cast<T*>(indicator.get())) {
                    return typed;
                }
            }
            return nullptr;
        }

        void update_all();
        void clear_all();

        template<typename T>
        bool has_indicator_of_type() const {
            for (const auto& indicator : indicators) {
                if (dynamic_cast<T*>(indicator.get())) {
                    return true;
                }
            }
            return false;
        }

        void remove_indicator(size_t index);

        template<typename T>
        bool remove_indicator_of_type() {
            for (size_t i = 0; i < indicators.size(); ++i) {
                if (dynamic_cast<T*>(indicators[i].get())) {
                    indicators.erase(indicators.begin() + i);
                    return true;
                }
            }
            return false;
        }
        // Render the tabbed indicator pane (design: .indi-pane — 31px header
        // with tabs + live value + collapse/close, single plot below).
        void render_tabbed(double x_min, double x_max, void* chart_widget,
                           void* crosshair_state_ptr);

        // Height the pane wants this frame: 0 (no indicators / closed),
        // PANEL_HEADER_H (collapsed), or header + N stacked plots.
        float pane_height() const;
        bool pane_expanded() const;   // true when the pane shows a plot (time axis lives there)
        void show_pane() { pane_hidden_ = false; }

        // Get number of indicators
        size_t count() const { return indicators.size(); }
        // Count only visible indicators
        size_t visible_count() const {
            size_t n = 0;
            for (const auto& ind : indicators)
                if (ind->is_visible()) ++n;
            return n;
        }
        // Calculate total height needed
        float get_total_height_ratio() const;

    private:
        // Indices of indicators to draw as stacked plots this frame:
        // every pinned indicator, plus the active tab if it isn't pinned.
        // Returns at least one entry when a plot should show.
        std::vector<int> stacked_plot_indices() const;

        std::vector<std::unique_ptr<IndicatorBase>> indicators;
        std::vector<bool> pinned_;     // parallel to indicators; survives reorder via resize
        int  active_tab_ = 0;
        bool collapsed_ = false;
        bool pane_hidden_ = false;
        // F2 pilot: index of the indicator whose settings popover is open
        // (right-click tab → Settings…). -1 = none. pending_ arms the
        // one-shot ImGui::OpenPopup on the frame the menu item fired.
        int  settings_open_idx_ = -1;
        bool settings_popup_pending_ = false;
    };

} // namespace Indicators