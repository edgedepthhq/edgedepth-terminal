#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>

namespace Indicators {

    class RSIIndicator : public IndicatorBase {
    public:
        explicit RSIIndicator(int period = 14) : period_(period) { height_ratio_ = 0.25f; }

        void add_candle(int64_t time, double close);
        void set_building_candle(int64_t time, double close);
        size_t bar_count() const { return times_.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max,
                          double& y_min, double& y_max) const override;
        void update() override;
        void clear() override;
        const char* get_name() const override { return "RSI"; }
        const char* get_y_format() const override { return "%7.0f"; }

        // Latest-value axis tag: current RSI, colored by rising/falling.
        bool get_latest_value(double& out_value) const override {
            if (has_building_) { out_value = building_rsi_; return true; }
            if (!rsi_values_.empty()) { out_value = rsi_values_.back(); return true; }
            return false;
        }
        int get_latest_direction() const override {
            double cur, prev;
            if (has_building_) {
                cur = building_rsi_;
                if (rsi_values_.empty()) return 0;
                prev = rsi_values_.back();
            } else {
                if (rsi_values_.size() < 2) return 0;
                cur = rsi_values_.back();
                prev = rsi_values_[rsi_values_.size() - 2];
            }
            if (cur > prev) return 1;
            if (cur < prev) return -1;
            return 0;
        }
        void format_latest_value(double value, char* buf, int size) const override {
            snprintf(buf, size, "%.1f", value);
        }

    private:
        int period_;
        std::vector<double> times_;
        std::vector<double> rsi_values_;

        // Building candle
        bool has_building_ = false;
        double building_time_ = 0;
        double building_rsi_ = 0;
        // Raw close data for recalculation
        std::vector<double> closes_;
        bool cache_dirty_ = true;
        void rebuild_cache();
        double compute_rsi(const std::vector<double>& closes, size_t up_to) const;
    };

} // namespace Indicators
