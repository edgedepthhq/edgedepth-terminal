#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace Indicators {

    // ImPlot Y-axis formatter for abbreviated CVD values (e.g., "  1.2M", " -350K")
    inline int format_cvd_axis(double value, char* buf, int size, void*) {
        double abs_val = value < 0 ? -value : value;
        if (abs_val >= 1e9)       return snprintf(buf, size, "%7.1fB", value / 1e9);
        else if (abs_val >= 1e6)  return snprintf(buf, size, "%7.1fM", value / 1e6);
        else if (abs_val >= 1e3)  return snprintf(buf, size, "%7.1fK", value / 1e3);
        else                      return snprintf(buf, size, "%7.0f",  value);
    }

    // Cumulative Volume Delta indicator - rendered as candlesticks.
    // Each CVD candle: open = prev cumulative CVD, close = current cumulative CVD,
    // high/low from intra-candle CVD extremes. Colored green when rising, red falling.
    class CVDIndicator : public IndicatorBase {
    public:
        CVDIndicator() { height_ratio_ = 0.25f; }

        // Add a finalized candle's delta (from candle vbuy/vsell - no intra-candle H/L)
        void add_candle(int64_t time, double vbuy, double vsell);

        // Add with full intra-candle CVD OHLC (from Volume stream)
        void add_candle_with_cvd(int64_t time, double delta, double cvd_high, double cvd_low);

        // Update building (live) candle
        void set_building_candle(int64_t time, double vbuy, double vsell);

        void set_timeframe(int64_t seconds) { timeframe_seconds_ = seconds; }
        size_t bar_count() const { return bars_.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max,
                          double& y_min, double& y_max) const override;
        void update() override;
        void clear() override;
        const char* get_name() const override { return "CVD"; }

        // Custom Y-axis formatter
        ImPlotFormatter get_y_formatter() const override { return format_cvd_axis; }

        // Latest-value axis tag: current cumulative CVD, colored by rising/falling.
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
            format_cvd_axis(value, buf, size, nullptr);
        }

    private:
        struct DeltaBar {
            int64_t time;
            double delta;      // net delta for this candle (buy - sell)
            double cvd_high;   // intra-candle CVD max (relative to candle start)
            double cvd_low;    // intra-candle CVD min (relative to candle start)
            bool has_hl;       // true if cvd_high/low are real data (from Volume stream)
        };
        std::vector<DeltaBar> bars_;

        // Cached OHLC arrays for rendering (cumulative)
        struct CVDCandle {
            double time;
            double open;    // cumulative CVD at candle start
            double close;   // cumulative CVD at candle end
            double high;    // cumulative CVD high during candle
            double low;     // cumulative CVD low during candle
            bool bullish;   // close >= open
        };
        std::vector<CVDCandle> cached_candles_;
        int64_t timeframe_seconds_ = 300;

        // Building candle state
        bool has_building_ = false;
        int64_t building_time_ = 0;
        double building_delta_ = 0;

        bool cache_dirty_ = true;
        void rebuild_cache();
    };

} // namespace Indicators