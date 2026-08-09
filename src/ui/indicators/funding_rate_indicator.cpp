#include "funding_rate_indicator.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cmath>

namespace Indicators {

void FundingRateIndicator::add_bar(int64_t time, double funding_rate) {
    if (!bars_.empty() && bars_.back().time == time) {
        bars_.back().rate = funding_rate;
        cache_dirty_ = true;
        return;
    }
    bars_.push_back({time, funding_rate});
    cache_dirty_ = true;
}

void FundingRateIndicator::set_building_bar(int64_t time, double funding_rate) {
    has_building_ = true;
    building_time_ = time;
    building_rate_ = funding_rate;
    cache_dirty_ = true;
}

void FundingRateIndicator::clear() {
    bars_.clear();
    cached_times_.clear();
    cached_rates_.clear();
    has_building_ = false;
    cache_dirty_ = true;
}

void FundingRateIndicator::update() {
    if (cache_dirty_) rebuild_cache();
}

void FundingRateIndicator::rebuild_cache() {
    cache_dirty_ = false;
    const size_t n = bars_.size() + (has_building_ ? 1 : 0);
    cached_times_.resize(n);
    cached_rates_.resize(n);
    for (size_t i = 0; i < bars_.size(); i++) {
        cached_times_[i] = static_cast<double>(bars_[i].time);
        cached_rates_[i] = bars_[i].rate;
    }
    if (has_building_) {
        cached_times_[bars_.size()] = static_cast<double>(building_time_);
        cached_rates_[bars_.size()] = building_rate_;
    }
}

void FundingRateIndicator::get_y_limits(double x_min, double x_max,
                                         double& y_min, double& y_max) const {
    y_min = 0.0;
    y_max = 0.0;
    for (size_t i = 0; i < cached_times_.size(); i++) {
        if (cached_times_[i] >= x_min && cached_times_[i] <= x_max) {
            y_min = std::min(y_min, cached_rates_[i]);
            y_max = std::max(y_max, cached_rates_[i]);
        }
    }
    // Symmetric around zero
    double abs_max = std::max(std::abs(y_min), std::abs(y_max));
    if (abs_max < 0.0001) abs_max = 0.0001;
    y_min = -abs_max * 1.1;
    y_max = abs_max * 1.1;
}

void FundingRateIndicator::render_content(double x_min, double x_max) {
    if (cached_times_.size() < 2) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const double timeframe_ms = static_cast<double>(timeframe_seconds_) * 1000.0;
    const double half_width = timeframe_ms * 0.45;  // Nearly touching bars, minimal gap

    // MMT-style: blue bars above zero, pink/red bars below zero
    const ImU32 pos_color = IM_COL32(80, 140, 230, 220);   // blue
    const ImU32 neg_color = IM_COL32(220, 70, 100, 220);    // pink/red

    size_t start_idx = 0;
    for (size_t i = 0; i < cached_times_.size(); i++) {
        if (cached_times_[i] >= x_min) {
            start_idx = (i > 0) ? i - 1 : 0;
            break;
        }
    }

    const float zero_y = ImPlot::PlotToPixels(0, 0).y;

    for (size_t i = start_idx; i < cached_times_.size(); i++) {
        if (cached_times_[i] > x_max) break;
        if (cached_rates_[i] == 0) continue;

        const ImU32 col = (cached_rates_[i] >= 0) ? pos_color : neg_color;
        const ImVec2 left = ImPlot::PlotToPixels(cached_times_[i] - half_width, 0);
        const ImVec2 right = ImPlot::PlotToPixels(cached_times_[i] + half_width, 0);
        const float val_y = ImPlot::PlotToPixels(0, cached_rates_[i]).y;

        float bar_left = left.x;
        float bar_right = std::max(right.x, left.x + 1.0f);

        draw_list->AddRectFilled(
            ImVec2(bar_left, std::min(zero_y, val_y)),
            ImVec2(bar_right, std::max(zero_y, val_y)),
            col);
    }

    // Zero line
    const ImVec2 zero_left = ImPlot::PlotToPixels(x_min, 0);
    const ImVec2 zero_right = ImPlot::PlotToPixels(x_max, 0);
    draw_list->AddLine(zero_left, zero_right, IM_COL32(128, 128, 128, 80), 1.0f);

    ImPlot::PopPlotClipRect();
}

} // namespace Indicators
