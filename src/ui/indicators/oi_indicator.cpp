#include "oi_indicator.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Indicators {

void OIIndicator::add_oi(int64_t time, double oi_usd) {
    if (oi_usd <= 0) return;

    // Update existing bar if same timestamp
    if (!bars_.empty() && bars_.back().time == time) {
        auto& bar = bars_.back();
        bar.close = oi_usd;
        bar.high = std::max(bar.high, oi_usd);
        bar.low = std::min(bar.low, oi_usd);
        cache_dirty_ = true;
        return;
    }
    // New candle: open = previous close (or this value if first)
    double open_val = bars_.empty() ? oi_usd : bars_.back().close;
    bars_.push_back({time, open_val, std::max(open_val, oi_usd),
                     std::min(open_val, oi_usd), oi_usd, false});
    cache_dirty_ = true;
}

void OIIndicator::add_candle_ohlc(int64_t time, double open, double high, double low, double close) {
    if (close <= 0) return;
    bars_.push_back({time, open, high, low, close, true});
    cache_dirty_ = true;
}

void OIIndicator::set_building_oi(int64_t time, double oi_usd) {
    if (oi_usd <= 0) return;
    has_building_ = true;
    building_time_ = time;
    building_oi_ = oi_usd;
    cache_dirty_ = true;
}

void OIIndicator::clear() {
    bars_.clear();
    cached_candles_.clear();
    has_building_ = false;
    cache_dirty_ = true;
}

void OIIndicator::update() {
    if (cache_dirty_) rebuild_cache();
}

void OIIndicator::rebuild_cache() {
    cache_dirty_ = false;
    const size_t n = bars_.size() + (has_building_ ? 1 : 0);
    cached_candles_.resize(n);

    for (size_t i = 0; i < bars_.size(); i++) {
        const auto& bar = bars_[i];
        OICandle& c = cached_candles_[i];
        c.time = static_cast<double>(bar.time);
        // Force open = previous close for continuity (no Y-axis gaps)
        if (i > 0) {
            c.open = cached_candles_[i - 1].close;
            c.high = std::max(bar.high, c.open);
            c.low = std::min(bar.low, c.open);
        } else {
            c.open = bar.open;
            c.high = bar.high;
            c.low = bar.low;
        }
        c.close = bar.close;
        c.high = std::max(c.high, c.close);
        c.low = std::min(c.low, c.close);
        c.bullish = c.close >= c.open;
    }

    if (has_building_ && !bars_.empty()) {
        const size_t bi = bars_.size();
        OICandle& c = cached_candles_[bi];
        c.time = static_cast<double>(building_time_);
        c.open = bars_.back().close;
        c.close = building_oi_;
        c.high = std::max(c.open, c.close);
        c.low = std::min(c.open, c.close);
        c.bullish = c.close >= c.open;
    }
}

void OIIndicator::get_y_limits(double x_min, double x_max,
                               double& y_min, double& y_max) const {
    y_min = std::numeric_limits<double>::infinity();
    y_max = -std::numeric_limits<double>::infinity();

    for (const auto& c : cached_candles_) {
        if (c.time >= x_min && c.time <= x_max) {
            y_min = std::min(y_min, c.low);
            y_max = std::max(y_max, c.high);
        }
    }
    if (y_min == std::numeric_limits<double>::infinity()) {
        y_min = 0;
        y_max = 1.0;
    }
    const double padding = (y_max - y_min) * 0.08;
    y_min -= padding;
    y_max += padding;
}

void OIIndicator::render_content(double x_min, double x_max) {
    if (cached_candles_.size() < 2) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const double timeframe_ms = static_cast<double>(timeframe_seconds_) * 1000.0;
    const double half_width = timeframe_ms * 0.35;

    // Binary search for first visible candle
    size_t start_idx = 0;
    for (size_t i = 0; i < cached_candles_.size(); i++) {
        if (cached_candles_[i].time >= x_min) {
            start_idx = (i > 0) ? i - 1 : 0;
            break;
        }
    }

    // Determine render mode based on zoom level
    const double visible_candles = (x_max - x_min) / timeframe_ms;
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const double pixels_per_candle = plot_size.x / visible_candles;
    const bool bar_mode = pixels_per_candle < 3.0;

    if (bar_mode) {
        int last_px_col = -1;
        double col_high = -1e18, col_low = 1e18;
        double col_open = 0, col_close = 0;
        bool col_started = false;

        for (size_t i = start_idx; i < cached_candles_.size(); i++) {
            const auto& c = cached_candles_[i];
            if (c.time > x_max) break;

            const ImVec2 px = ImPlot::PlotToPixels(c.time, c.close);
            const int px_col = static_cast<int>(px.x);

            if (px_col != last_px_col && col_started) {
                const ImU32 col = (col_close >= col_open)
                    ? Theme::get_buy_color_u32(255) : Theme::get_sell_color_u32(255);
                const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
                const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
                draw_list->AddLine(
                    ImVec2(static_cast<float>(last_px_col), top.y),
                    ImVec2(static_cast<float>(last_px_col), bot.y),
                    col, 1.0f);
                col_high = -1e18; col_low = 1e18;
                col_started = false;
            }
            if (!col_started) { col_open = c.open; col_started = true; }
            col_high = std::max(col_high, c.high);
            col_low = std::min(col_low, c.low);
            col_close = c.close;
            last_px_col = px_col;
        }
        if (col_started) {
            const ImU32 col = (col_close >= col_open)
                ? Theme::get_buy_color_u32(255) : Theme::get_sell_color_u32(255);
            const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
            const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
            draw_list->AddLine(
                ImVec2(static_cast<float>(last_px_col), top.y),
                ImVec2(static_cast<float>(last_px_col), bot.y),
                col, 1.0f);
        }
    } else {
        // Candlestick mode
        constexpr float MIN_BODY_HEIGHT = 3.0f;

        for (size_t i = start_idx; i < cached_candles_.size(); i++) {
            const auto& c = cached_candles_[i];
            if (c.time > x_max) break;

            const ImU32 color = c.bullish
                ? Theme::get_buy_color_u32(255)
                : Theme::get_sell_color_u32(255);

            const ImVec2 left = ImPlot::PlotToPixels(c.time - half_width, c.open);
            const ImVec2 right = ImPlot::PlotToPixels(c.time + half_width, c.close);
            float body_top = std::min(left.y, right.y);
            float body_bottom = std::max(left.y, right.y);

            if (body_bottom - body_top < MIN_BODY_HEIGHT) {
                float center_y = (body_top + body_bottom) * 0.5f;
                body_top = center_y - MIN_BODY_HEIGHT * 0.5f;
                body_bottom = center_y + MIN_BODY_HEIGHT * 0.5f;
            }

            float body_left = left.x;
            float body_right = std::max(right.x, left.x + 1.0f);

            draw_list->AddRectFilled(
                ImVec2(body_left, body_top),
                ImVec2(body_right, body_bottom),
                color);

            // Wicks
            const ImVec2 high_px = ImPlot::PlotToPixels(c.time, c.high);
            const ImVec2 low_px = ImPlot::PlotToPixels(c.time, c.low);

            if (high_px.y < body_top) {
                draw_list->AddLine(
                    ImVec2(high_px.x, high_px.y),
                    ImVec2(high_px.x, body_top),
                    color, 1.0f);
            }
            if (low_px.y > body_bottom) {
                draw_list->AddLine(
                    ImVec2(low_px.x, body_bottom),
                    ImVec2(low_px.x, low_px.y),
                    color, 1.0f);
            }
        }
    }

    ImPlot::PopPlotClipRect();
}

} // namespace Indicators
