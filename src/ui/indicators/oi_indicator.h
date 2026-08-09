#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace Indicators {

    // Y-axis formatter for OI in contract quantity (e.g., "109.2K", "16.8M")
    inline int format_oi_axis(double value, char* buf, int size, void*) {
        double abs_val = std::abs(value);
        if (abs_val >= 1e9)       return snprintf(buf, size, "%.2fB", value / 1e9);
        else if (abs_val >= 1e6)  return snprintf(buf, size, "%.2fM", value / 1e6);
        else if (abs_val >= 1e3)  return snprintf(buf, size, "%.2fK", value / 1e3);
        else                      return snprintf(buf, size, "%.1f",  value);
    }

    // Open Interest indicator — rendered as OHLC candlesticks.
    // Green candle = OI increased (open < close), Red = OI decreased.
    // Body = open/close of OI per candle period.
    // Wicks = intra-candle OI high/low (tracked via stat stream updates).
    class OIIndicator : public IndicatorBase {
    public:
        OIIndicator() { height_ratio_ = 0.20f; }

        // Set OI snapshot for a candle period (from Stat stream)
        void add_oi(int64_t time, double oi_usd);

        // Add pre-aggregated OHLC candle (from historical OI query)
        void add_candle_ohlc(int64_t time, double open, double high, double low, double close);

        // Update building (live) candle
        void set_building_oi(int64_t time, double oi_usd);

        void set_timeframe(int64_t seconds) { timeframe_seconds_ = seconds; }
        size_t bar_count() const { return bars_.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max,
                          double& y_min, double& y_max) const override;
        void update() override;
        void clear() override;
        const char* get_name() const override { return "Open Interest"; }

        ImPlotFormatter get_y_formatter() const override { return format_oi_axis; }

        // Latest-value axis tag: current OI, colored by candle direction.
        bool get_latest_value(double& out_value) const override {
            if (cached_candles_.empty()) return false;
            out_value = cached_candles_.back().close;
            return true;
        }
        int get_latest_direction() const override {
            if (cached_candles_.empty()) return 0;
            return cached_candles_.back().bullish ? 1 : -1;
        }
        void format_latest_value(double value, char* buf, int size) const override {
            format_oi_axis(value, buf, size, nullptr);
        }

    private:
        struct OIBar {
            int64_t time;
            double open;   // OI at candle start
            double high;   // max OI during candle
            double low;    // min OI during candle
            double close;  // OI at candle end (latest update)
            bool finalized;
        };
        std::vector<OIBar> bars_;

        // Cached for rendering
        struct OICandle {
            double time;
            double open, high, low, close;
            bool bullish;
        };
        std::vector<OICandle> cached_candles_;
        int64_t timeframe_seconds_ = 300;

        bool has_building_ = false;
        int64_t building_time_ = 0;
        double building_oi_ = 0;

        bool cache_dirty_ = true;
        void rebuild_cache();
    };

} // namespace Indicators
