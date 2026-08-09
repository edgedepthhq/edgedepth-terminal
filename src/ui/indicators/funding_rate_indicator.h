#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace Indicators {

    // Y-axis formatter for funding rate (percentage with 4 decimals)
    inline int format_funding_axis(double value, char* buf, int size, void*) {
        return snprintf(buf, size, "%7.4f", value);
    }

    // Funding Rate indicator — rendered as histogram bars (MMT-style).
    // Blue bars above zero = longs pay shorts (positive funding).
    // Red bars below zero = shorts pay longs (negative funding).
    // Data from Stat stream (funding field), plotted per-candle.
    class FundingRateIndicator : public IndicatorBase {
    public:
        FundingRateIndicator() {
            height_ratio_ = 0.15f;
            height_pixels_ = 120.0f;
        }

        void add_bar(int64_t time, double funding_rate);
        void set_building_bar(int64_t time, double funding_rate);
        void set_timeframe(int64_t seconds) { timeframe_seconds_ = seconds; }
        size_t bar_count() const { return bars_.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max,
                          double& y_min, double& y_max) const override;
        void update() override;
        void clear() override;
        const char* get_name() const override { return "Funding Rate"; }

        ImPlotFormatter get_y_formatter() const override { return format_funding_axis; }

        // Latest-value axis tag: current funding rate, colored by sign
        // (positive = green, negative = red).
        bool get_latest_value(double& out_value) const override {
            if (has_building_) { out_value = building_rate_; return true; }
            if (!bars_.empty()) { out_value = bars_.back().rate; return true; }
            return false;
        }
        int get_latest_direction() const override {
            double rate;
            if (has_building_) rate = building_rate_;
            else if (!bars_.empty()) rate = bars_.back().rate;
            else return 0;
            if (rate > 0) return 1;
            if (rate < 0) return -1;
            return 0;
        }
        void format_latest_value(double value, char* buf, int size) const override {
            format_funding_axis(value, buf, size, nullptr);
        }

    private:
        struct FundingBar {
            int64_t time;
            double rate;
        };
        std::vector<FundingBar> bars_;
        std::vector<double> cached_times_;
        std::vector<double> cached_rates_;
        int64_t timeframe_seconds_ = 300;

        bool has_building_ = false;
        int64_t building_time_ = 0;
        double building_rate_ = 0;

        bool cache_dirty_ = true;
        void rebuild_cache();
    };

} // namespace Indicators
