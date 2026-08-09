#include "macd_indicator.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Indicators {

void MACDIndicator::add_candle(int64_t time, double close) {
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

void MACDIndicator::set_building_candle(int64_t time, double close) {
    has_building_ = true;
    building_time_ = static_cast<double>(time);
    // Rebuild with building candle appended
    // We'll compute building values during rebuild_cache
    std::vector<double> tmp = closes_;
    tmp.push_back(close);

    // Quick EMA computation for building candle
    auto ema = [](const std::vector<double>& data, int period) -> std::vector<double> {
        std::vector<double> result(data.size(), 0);
        if (data.empty() || period <= 0) return result;
        double mult = 2.0 / (period + 1);
        result[0] = data[0];
        for (size_t i = 1; i < data.size(); ++i)
            result[i] = (data[i] - result[i-1]) * mult + result[i-1];
        return result;
    };

    auto fast_ema = ema(tmp, fast_);
    auto slow_ema = ema(tmp, slow_);
    size_t last = tmp.size() - 1;
    building_macd_ = fast_ema[last] - slow_ema[last];
    // Approximate signal from existing + building
    if (!signal_line_.empty()) {
        double sig_mult = 2.0 / (signal_period_ + 1);
        building_signal_ = (building_macd_ - signal_line_.back()) * sig_mult + signal_line_.back();
    } else {
        building_signal_ = building_macd_;
    }
    building_hist_ = building_macd_ - building_signal_;
    cache_dirty_ = true;
}

void MACDIndicator::clear() {
    times_.clear();
    closes_.clear();
    macd_line_.clear();
    signal_line_.clear();
    histogram_.clear();
    has_building_ = false;
    cache_dirty_ = true;
}

void MACDIndicator::update() {
    if (cache_dirty_) rebuild_cache();
}

void MACDIndicator::rebuild_cache() {
    cache_dirty_ = false;
    const size_t n = closes_.size();
    macd_line_.resize(n);
    signal_line_.resize(n);
    histogram_.resize(n);
    if (n == 0) return;

    // EMA helper
    auto ema_inplace = [](const std::vector<double>& src, std::vector<double>& dst, int period) {
        double mult = 2.0 / (period + 1);
        dst.resize(src.size());
        dst[0] = src[0];
        for (size_t i = 1; i < src.size(); ++i)
            dst[i] = (src[i] - dst[i-1]) * mult + dst[i-1];
    };

    std::vector<double> fast_ema, slow_ema;
    ema_inplace(closes_, fast_ema, fast_);
    ema_inplace(closes_, slow_ema, slow_);

    for (size_t i = 0; i < n; ++i)
        macd_line_[i] = fast_ema[i] - slow_ema[i];

    // Signal line = EMA of MACD line
    double sig_mult = 2.0 / (signal_period_ + 1);
    signal_line_[0] = macd_line_[0];
    for (size_t i = 1; i < n; ++i)
        signal_line_[i] = (macd_line_[i] - signal_line_[i-1]) * sig_mult + signal_line_[i-1];

    for (size_t i = 0; i < n; ++i)
        histogram_[i] = macd_line_[i] - signal_line_[i];
}

void MACDIndicator::get_y_limits(double x_min, double x_max,
                                 double& y_min, double& y_max) const {
    y_min = std::numeric_limits<double>::infinity();
    y_max = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < times_.size(); i++) {
        if (times_[i] >= x_min && times_[i] <= x_max) {
            double lo = std::min({macd_line_[i], signal_line_[i], histogram_[i]});
            double hi = std::max({macd_line_[i], signal_line_[i], histogram_[i]});
            y_min = std::min(y_min, lo);
            y_max = std::max(y_max, hi);
        }
    }
    if (y_min == std::numeric_limits<double>::infinity()) {
        y_min = -1.0; y_max = 1.0;
    }
    const double padding = (y_max - y_min) * 0.15;
    y_min -= padding;
    y_max += padding;
}

void MACDIndicator::render_content(double x_min, double x_max) {
    if (times_.size() < 2) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const double timeframe_ms = static_cast<double>(timeframe_seconds_) * 1000.0;
    const double half_width = timeframe_ms * 0.35;

    // Binary search for visible start
    size_t start_idx = 0;
    {
        auto it = std::lower_bound(times_.begin(), times_.end(), x_min);
        if (it != times_.begin()) start_idx = static_cast<size_t>(it - times_.begin()) - 1;
    }

    // Zero line
    const ImVec2 z_a = ImPlot::PlotToPixels(x_min, 0);
    const ImVec2 z_b = ImPlot::PlotToPixels(x_max, 0);
    draw_list->AddLine(z_a, z_b, IM_COL32(128, 128, 128, 40), 1.0f);

    // Layer 1: Histogram bars - pixel-decimated
    {
        int last_px_col = -1;
        double col_hist = 0;
        float col_left_x = 0, col_right_x = 0;

        for (size_t i = start_idx; i < times_.size(); i++) {
            if (times_[i] > x_max) break;
            const ImVec2 center_px = ImPlot::PlotToPixels(times_[i], 0);
            const int px_col = static_cast<int>(center_px.x);

            if (px_col == last_px_col) {
                if (std::abs(histogram_[i]) > std::abs(col_hist))
                    col_hist = histogram_[i];
                continue;
            }
            if (last_px_col >= 0) {
                const ImU32 bar_col = (col_hist >= 0)
                    ? Theme::get_buy_color_u32(100) : Theme::get_sell_color_u32(100);
                const float zero_y = ImPlot::PlotToPixels(0, 0).y;
                const float val_y = ImPlot::PlotToPixels(0, col_hist).y;
                draw_list->AddRectFilled(
                    ImVec2(col_left_x, std::min(zero_y, val_y)),
                    ImVec2(col_right_x, std::max(zero_y, val_y)),
                    bar_col);
            }
            last_px_col = px_col;
            col_hist = histogram_[i];
            const ImVec2 left = ImPlot::PlotToPixels(times_[i] - half_width, 0);
            const ImVec2 right = ImPlot::PlotToPixels(times_[i] + half_width, 0);
            col_left_x = left.x;
            col_right_x = std::max(right.x, left.x + 1.0f);
        }
        if (last_px_col >= 0) {
            const ImU32 bar_col = (col_hist >= 0)
                ? Theme::get_buy_color_u32(100) : Theme::get_sell_color_u32(100);
            const float zero_y = ImPlot::PlotToPixels(0, 0).y;
            const float val_y = ImPlot::PlotToPixels(0, col_hist).y;
            draw_list->AddRectFilled(
                ImVec2(col_left_x, std::min(zero_y, val_y)),
                ImVec2(col_right_x, std::max(zero_y, val_y)),
                bar_col);
        }
    }

    // Helper lambda: draw pixel-decimated line
    auto draw_line = [&](const std::vector<double>& values, ImU32 color, float thickness) {
        int last_px = -1;
        size_t last_drawn = start_idx;
        for (size_t i = start_idx + 1; i < values.size(); i++) {
            if (times_[i] > x_max) break;
            const ImVec2 px = ImPlot::PlotToPixels(times_[i], values[i]);
            const int px_col = static_cast<int>(px.x);
            if (px_col == last_px) continue;
            if (last_px >= 0) {
                const ImVec2 p1 = ImPlot::PlotToPixels(times_[last_drawn], values[last_drawn]);
                draw_list->AddLine(p1, px, color, thickness);
            }
            last_drawn = i;
            last_px = px_col;
        }
    };

    // Layer 2: MACD line (blue-ish)
    draw_line(macd_line_, IM_COL32(100, 150, 255, 255), 1.5f);
    // Layer 3: Signal line (orange)
    draw_line(signal_line_, IM_COL32(255, 160, 80, 255), 1.0f);

    // Building candle extension
    if (has_building_ && !macd_line_.empty()) {
        const ImVec2 p1m = ImPlot::PlotToPixels(times_.back(), macd_line_.back());
        const ImVec2 p2m = ImPlot::PlotToPixels(building_time_, building_macd_);
        draw_list->AddLine(p1m, p2m, IM_COL32(100, 150, 255, 180), 1.5f);

        const ImVec2 p1s = ImPlot::PlotToPixels(times_.back(), signal_line_.back());
        const ImVec2 p2s = ImPlot::PlotToPixels(building_time_, building_signal_);
        draw_list->AddLine(p1s, p2s, IM_COL32(255, 160, 80, 180), 1.0f);
    }

    ImPlot::PopPlotClipRect();
}

} // namespace Indicators
