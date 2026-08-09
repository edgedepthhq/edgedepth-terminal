#include "rsi_indicator.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Indicators {

void RSIIndicator::add_candle(int64_t time, double close) {
    // Update existing if same timestamp
    if (!closes_.empty() && !times_.empty() &&
        static_cast<int64_t>(times_.back()) == time) {
        closes_.back() = close;
        cache_dirty_ = true;
        return;
    }
    times_.push_back(static_cast<double>(time));
    closes_.push_back(close);
    cache_dirty_ = true;
}

void RSIIndicator::set_building_candle(int64_t time, double close) {
    has_building_ = true;
    building_time_ = static_cast<double>(time);
    // Temporarily append building close to compute RSI
    std::vector<double> tmp = closes_;
    tmp.push_back(close);
    building_rsi_ = compute_rsi(tmp, tmp.size());
    cache_dirty_ = true;
}

void RSIIndicator::clear() {
    times_.clear();
    closes_.clear();
    rsi_values_.clear();
    has_building_ = false;
    cache_dirty_ = true;
}

void RSIIndicator::update() {
    if (cache_dirty_) rebuild_cache();
}

double RSIIndicator::compute_rsi(const std::vector<double>& closes, size_t up_to) const {
    if (up_to < static_cast<size_t>(period_) + 1) return 50.0;
    double avg_gain = 0, avg_loss = 0;
    // Initial SMA of gains/losses
    for (size_t i = up_to - period_; i < up_to; ++i) {
        double change = closes[i] - closes[i - 1];
        if (change > 0) avg_gain += change;
        else avg_loss -= change;
    }
    avg_gain /= period_;
    avg_loss /= period_;
    if (avg_loss < 1e-12) return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

void RSIIndicator::rebuild_cache() {
    cache_dirty_ = false;
    rsi_values_.resize(times_.size());
    // Wilder's smoothed RSI
    if (closes_.size() < static_cast<size_t>(period_) + 1) {
        std::fill(rsi_values_.begin(), rsi_values_.end(), 50.0);
        return;
    }
    // Initial averages
    double avg_gain = 0, avg_loss = 0;
    for (int i = 1; i <= period_; ++i) {
        double change = closes_[i] - closes_[i - 1];
        if (change > 0) avg_gain += change;
        else avg_loss -= change;
    }
    avg_gain /= period_;
    avg_loss /= period_;
    // Fill first period_ values as 50
    for (int i = 0; i < period_; ++i) rsi_values_[i] = 50.0;
    // RSI at period_ index
    if (avg_loss < 1e-12) rsi_values_[period_] = 100.0;
    else {
        double rs = avg_gain / avg_loss;
        rsi_values_[period_] = 100.0 - (100.0 / (1.0 + rs));
    }
    // Smoothed RSI for remaining
    for (size_t i = period_ + 1; i < closes_.size(); ++i) {
        double change = closes_[i] - closes_[i - 1];
        double gain = change > 0 ? change : 0;
        double loss = change < 0 ? -change : 0;
        avg_gain = (avg_gain * (period_ - 1) + gain) / period_;
        avg_loss = (avg_loss * (period_ - 1) + loss) / period_;
        if (avg_loss < 1e-12) rsi_values_[i] = 100.0;
        else {
            double rs = avg_gain / avg_loss;
            rsi_values_[i] = 100.0 - (100.0 / (1.0 + rs));
        }
    }
}

void RSIIndicator::get_y_limits(double /*x_min*/, double /*x_max*/,
                                double& y_min, double& y_max) const {
    y_min = 0.0;
    y_max = 100.0;
}

void RSIIndicator::render_content(double x_min, double x_max) {
    if (rsi_values_.size() < 2) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    // Reference lines: 70 (overbought) and 30 (oversold)
    const ImVec2 l70_a = ImPlot::PlotToPixels(x_min, 70);
    const ImVec2 l70_b = ImPlot::PlotToPixels(x_max, 70);
    const ImVec2 l30_a = ImPlot::PlotToPixels(x_min, 30);
    const ImVec2 l30_b = ImPlot::PlotToPixels(x_max, 30);
    const ImVec2 l50_a = ImPlot::PlotToPixels(x_min, 50);
    const ImVec2 l50_b = ImPlot::PlotToPixels(x_max, 50);
    draw_list->AddLine(l70_a, l70_b, IM_COL32(200, 80, 80, 80), 1.0f);
    draw_list->AddLine(l30_a, l30_b, IM_COL32(80, 200, 80, 80), 1.0f);
    draw_list->AddLine(l50_a, l50_b, IM_COL32(128, 128, 128, 40), 1.0f);

    // Shade overbought/oversold zones
    draw_list->AddRectFilled(
        ImPlot::PlotToPixels(x_min, 100), ImPlot::PlotToPixels(x_max, 70),
        IM_COL32(200, 80, 80, 15));
    draw_list->AddRectFilled(
        ImPlot::PlotToPixels(x_min, 30), ImPlot::PlotToPixels(x_max, 0),
        IM_COL32(80, 200, 80, 15));

    // Binary search for visible start
    size_t start_idx = 0;
    {
        auto it = std::lower_bound(times_.begin(), times_.end(), x_min);
        if (it != times_.begin()) start_idx = static_cast<size_t>(it - times_.begin()) - 1;
    }

    // RSI line — pixel-decimated, color by zone
    int last_px_col = -1;
    size_t last_drawn = start_idx;
    for (size_t i = start_idx + 1; i < rsi_values_.size(); i++) {
        if (times_[i] > x_max) break;
        const ImVec2 px = ImPlot::PlotToPixels(times_[i], rsi_values_[i]);
        const int px_col = static_cast<int>(px.x);
        if (px_col == last_px_col) continue;

        if (last_px_col >= 0) {
            const ImVec2 p1 = ImPlot::PlotToPixels(times_[last_drawn], rsi_values_[last_drawn]);
            ImU32 col;
            double val = rsi_values_[i];
            if (val >= 70) col = IM_COL32(220, 90, 90, 255);
            else if (val <= 30) col = IM_COL32(90, 220, 90, 255);
            else col = IM_COL32(180, 160, 220, 255);
            draw_list->AddLine(p1, px, col, 1.5f);
        }
        last_drawn = i;
        last_px_col = px_col;
    }

    // Building candle extension
    if (has_building_ && !rsi_values_.empty()) {
        const ImVec2 p1 = ImPlot::PlotToPixels(times_.back(), rsi_values_.back());
        const ImVec2 p2 = ImPlot::PlotToPixels(building_time_, building_rsi_);
        ImU32 col = building_rsi_ >= 70 ? IM_COL32(220, 90, 90, 180)
                  : building_rsi_ <= 30 ? IM_COL32(90, 220, 90, 180)
                  : IM_COL32(180, 160, 220, 180);
        draw_list->AddLine(p1, p2, col, 1.5f);
    }

    ImPlot::PopPlotClipRect();
}

} // namespace Indicators
