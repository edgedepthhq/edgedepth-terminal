#pragma once
#include "indicator_base.h"
#include <vector>
#include <cstdint>
#include <cstdio>

namespace Indicators {
    struct VolumeBar {
        int64_t time;
        double volume;
        bool bullish;
    };

    // ImPlot Y-axis formatter for abbreviated USDT volume (e.g., "  1.2M", "  350K")
    // Uses minimum-width format specifiers so labels are consistently wide,
    // preventing Y-axis label area width mismatch with the main chart that
    // causes x-axis drift between candles and volume bars when zoomed out.
    inline int format_volume_usdt(double value, char* buf, int size, void*) {
        if (value >= 1e9)       return snprintf(buf, size, "%7.1fB", value / 1e9);
        else if (value >= 1e6)  return snprintf(buf, size, "%7.1fM", value / 1e6);
        else if (value >= 1e3)  return snprintf(buf, size, "%7.0fK", value / 1e3);
        else                    return snprintf(buf, size, "%7.0f",  value);
    }

    class VolumeIndicator : public IndicatorBase {
    public:
        VolumeIndicator() = default;
        void add_bar(int64_t time, double volume, bool bullish);
        void set_current_bar(int64_t time, double volume, bool bullish);
        void set_timeframe(int64_t seconds) { timeframe_seconds_ = seconds; }
        size_t get_bar_count() const { return bars.size(); }

        void render_content(double x_min, double x_max) override;
        void get_y_limits(double x_min, double x_max, double& y_min, double& y_max) const override;

        void update() override;
        void clear() override;

        const char* get_name() const override { return "Vol (USDT)"; }

        // Custom Y-axis formatter (abbreviated USDT volumes)
        ImPlotFormatter get_y_formatter() const override { return format_volume_usdt; }

        // Latest-value axis tag: current (building) bar volume, colored by bar direction.
        bool get_latest_value(double& out_value) const override {
            if (building_bar_) { out_value = building_bar_->volume; return true; }
            if (!bars.empty()) { out_value = bars.back().volume; return true; }
            return false;
        }
        int get_latest_direction() const override {
            if (building_bar_) return building_bar_->bullish ? 1 : -1;
            if (!bars.empty()) return bars.back().bullish ? 1 : -1;
            return 0;
        }
        void format_latest_value(double value, char* buf, int size) const override {
            format_volume_usdt(value, buf, size, nullptr);
        }

        int64_t get_timeframe() const {
            return timeframe_seconds_;
        }

    private:
        std::vector<VolumeBar> bars;
        std::vector<double> times;
        std::vector<double> volumes;
        std::vector<ImU32> colors;
        int64_t timeframe_seconds_ = 300;
        std::optional<VolumeBar> building_bar_;
        void rebuild_cache();
    };
}