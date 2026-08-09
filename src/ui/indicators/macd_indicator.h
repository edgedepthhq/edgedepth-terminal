#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>

namespace Indicators {

    class MACDIndicator : public IndicatorBase {
    public:
        MACDIndicator(int fast = 12, int slow = 26, int signal_period = 9)
            : fast_(fast), slow_(slow), signal_period_(signal_period)
        { height_ratio_ = 0.25f; }

        void add_candle(int64_t time, double close);
        void set_building_candle(int64_t time, double close);
        void set_timeframe(int64_t seconds) { timeframe_seconds_ = seconds; }
        size_t bar_count() const { return times_.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max,
                          double& y_min, double& y_max) const override;
        void update() override;
        void clear() override;
        const char* get_name() const override { return "MACD"; }
        const char* get_y_format() const override { return "%8.2f"; }

        // Latest-value axis tag: current MACD line, colored by histogram sign
        // (green when MACD is above its signal line, red when below).
        bool get_latest_value(double& out_value) const override {
            if (has_building_) { out_value = building_macd_; return true; }
            if (!macd_line_.empty()) { out_value = macd_line_.back(); return true; }
            return false;
        }
        int get_latest_direction() const override {
            double hist;
            if (has_building_) hist = building_hist_;
            else if (!histogram_.empty()) hist = histogram_.back();
            else return 0;
            if (hist > 0) return 1;
            if (hist < 0) return -1;
            return 0;
        }
        void format_latest_value(double value, char* buf, int size) const override {
            snprintf(buf, size, "%.2f", value);
        }

    private:
        int fast_, slow_, signal_period_;
        int64_t timeframe_seconds_ = 300;

        std::vector<double> closes_;
        std::vector<double> times_;
        std::vector<double> macd_line_;
        std::vector<double> signal_line_;
        std::vector<double> histogram_;

        // Building candle
        bool has_building_ = false;
        double building_time_ = 0;
        double building_macd_ = 0;
        double building_signal_ = 0;
        double building_hist_ = 0;

        bool cache_dirty_ = true;
        void rebuild_cache();
    };

} // namespace Indicators
