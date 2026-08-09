#include "volume_indicator.h"
#include "rendering/theme.h"
#include <algorithm>

#include "ui/chart_widget.h"

namespace Indicators {
    void VolumeIndicator::add_bar(int64_t time, double volume, bool bullish) {
        if (!bars.empty() && bars.back().time == time) {
            bars.back().volume = volume;
            bars.back().bullish = bullish;
            return;
        }
        bars.emplace_back(time, volume, bullish);
        std::ranges::sort(bars,
                          [](const VolumeBar& a, const VolumeBar& b) {
                              return a.time < b.time;
                          });
    }
    void VolumeIndicator::clear() {
        bars.clear();
        times.clear();
        volumes.clear();
        colors.clear();
    }

    void VolumeIndicator::set_current_bar(const int64_t time, const double volume, const bool bullish) {
        building_bar_ = VolumeBar{time, volume, bullish};
    }

    void VolumeIndicator::update() {
        rebuild_cache();
    }

    void VolumeIndicator::rebuild_cache() {
        const size_t n = bars.size();
        times.resize(n);
        volumes.resize(n);
        colors.resize(n);
        for (size_t i = 0; i < n; ++i) {
            times[i] = static_cast<double>(bars[i].time);
            volumes[i] = bars[i].volume;
            colors[i] = bars[i].bullish ?
                Theme::get_buy_color_u32(200) :
                Theme::get_sell_color_u32(200);
        }
    }

    void VolumeIndicator::get_y_limits(double x_min, double x_max, double& y_min, double& y_max) const {
        y_min = 0.0;  // Volume always starts at 0
        y_max = 0.0;
        // Find max volume in visible range
        for (const auto& bar : bars) {
            if (bar.time >= x_min && bar.time <= x_max) {
                y_max = std::max(y_max, bar.volume);
            }
        }
        // Check building bar too
        if (building_bar_.has_value()) {
            const auto& bar = building_bar_.value();
            if (bar.time >= x_min && bar.time <= x_max) {
                y_max = std::max(y_max, bar.volume);
            }
        }
        // Add 10% padding at top
        if (y_max > 0.0) {
            y_max *= 1.1;
        } else {
            y_max = 1.0;  // Minimum range if no data
        }
    }

    void VolumeIndicator::render_content(double x_min, double x_max) {
        const double timeframe_ms = static_cast<double>(timeframe_seconds_) * 1000.0;
        const double half_width = timeframe_ms * 0.35;  // Match candle body width
        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        size_t start_idx = 0;
        if (!times.empty()) {
            auto it = std::ranges::lower_bound(times, x_min);
            if (it != times.begin()) {
                start_idx = std::distance(times.begin(), it) - 1;
            }
        }
        // Pixel-decimation: when zoomed out far, aggregate bars that share a pixel column
        int last_px_col = -1;
        double col_volume = 0;
        bool col_bullish = true;
        float col_left_x = 0, col_right_x = 0;

        for (size_t i = start_idx; i < bars.size(); ++i) {
            if (times[i] > x_max) break;
            const ImVec2 center_px = ImPlot::PlotToPixels(times[i], 0);
            const int px_col = static_cast<int>(center_px.x);

            if (px_col == last_px_col) {
                // Same pixel column — accumulate max volume, keep last color
                col_volume = std::max(col_volume, volumes[i]);
                col_bullish = bars[i].bullish;
                continue;
            }

            // Flush previous column
            if (last_px_col >= 0 && col_volume > 0) {
                const ImVec2 bottom = ImVec2(col_left_x, ImPlot::PlotToPixels(0, 0).y);
                const ImVec2 top_px = ImPlot::PlotToPixels(0, col_volume);
                ImU32 col = col_bullish ? Theme::get_buy_color_u32(200) : Theme::get_sell_color_u32(200);
                draw_list->AddRectFilled(
                    ImVec2(col_left_x, top_px.y),
                    ImVec2(col_right_x, bottom.y),
                    col
                );
            }

            // Start new column
            last_px_col = px_col;
            col_volume = volumes[i];
            col_bullish = bars[i].bullish;
            const ImVec2 left = ImPlot::PlotToPixels(times[i] - half_width, 0);
            const ImVec2 right = ImPlot::PlotToPixels(times[i] + half_width, 0);
            col_left_x = left.x;
            col_right_x = std::max(right.x, left.x + 1.0f);  // At least 1px wide
        }
        // Flush final column
        if (last_px_col >= 0 && col_volume > 0) {
            const ImVec2 bottom = ImVec2(col_left_x, ImPlot::PlotToPixels(0, 0).y);
            const ImVec2 top_px = ImPlot::PlotToPixels(0, col_volume);
            ImU32 col = col_bullish ? Theme::get_buy_color_u32(200) : Theme::get_sell_color_u32(200);
            draw_list->AddRectFilled(
                ImVec2(col_left_x, top_px.y),
                ImVec2(col_right_x, bottom.y),
                col
            );
        }
        if (building_bar_.has_value()) {
            const auto& bar = building_bar_.value();
            if (bar.time >= x_min && bar.time <= x_max) {
                const auto bar_time = static_cast<double>(bar.time);
                ImU32 color = bar.bullish
                    ? Theme::get_buy_color_u32(200)
                    : Theme::get_sell_color_u32(200);
                const ImVec2 bottom = ImPlot::PlotToPixels(bar_time, 0);
                const ImVec2 top = ImPlot::PlotToPixels(bar_time, bar.volume);
                const ImVec2 left = ImPlot::PlotToPixels(bar_time - half_width, 0);
                const ImVec2 right = ImPlot::PlotToPixels(bar_time + half_width, 0);
                draw_list->AddRectFilled(
                    ImVec2(left.x, top.y),
                    ImVec2(right.x, bottom.y),
                    color
                );
            }
        }
        ImPlot::PopPlotClipRect();
    }
}
