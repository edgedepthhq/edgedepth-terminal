#pragma once
#include <algorithm>
#include "core/realtime_history.h"

// ImPlot has already zoomed a detached axis. Following axes are input-locked
// by SetupAxisLimits(Always), so apply the same wheel factor to their span.
// Navigation never changes the live display pause or replay transport clock.
struct RealtimeZoom {
    double span_ms;
    bool follow;
};
inline RealtimeZoom realtime_zoom(double displayed_span, float wheel,
                                  double zoom_rate, bool following, bool paused) {
    const double rate = wheel > 0 ? -zoom_rate / (1.0 + 2.0 * zoom_rate) :
                        wheel < 0 ? zoom_rate : 0.0;
    return {std::clamp(displayed_span * (following ? 1.0 + rate : 1.0),
                       5000.0, (1800000.0 / 0.88)),
            following || (wheel < 0 && !paused)};
}

// Keep at least 48 displayed depth rows on quiet markets. Fidelity groups
// several native ticks into a row; a native-tick-only floor creates huge bands.
inline double realtime_price_half_span(double low, double high, double tick, int grouping) {
    const double center = (low + high) * 0.5;
    return std::max({(high - low) * 0.5, tick * std::max(1, grouping) * 24.0,
                     center * 0.0005});
}

inline bool realtime_pan_detaches(float dx, float dy) {
    return std::abs(dx) > 12.0f && std::abs(dx) > std::abs(dy);
}
// Classify the whole gesture, not one frame's jitter during a horizontal pan.
inline bool realtime_price_pan_detaches(float dx, float dy) {
    return std::abs(dy) > 12.0f && std::abs(dy) > std::abs(dx);
}
inline bool realtime_view_has_live_edge(bool following, double right, int64_t clock) {
    return following || right >= double(clock);
}

// A following clock may move forward during a query. Widening the view may
// not replace good history with a snapshot that starts later than requested.
inline bool realtime_query_start_matches(bool following, int64_t desired_from,
                                         int64_t queried_from, int64_t step) {
    return following ? queried_from <= desired_from + step :
        std::abs(desired_from - queried_from) <= step;
}

// Linked depth is a numeric ladder. Its fixed fidelity sets the price span;
// neither history nor a volatile market may squeeze rows below the font height.
struct RealtimePriceWindow {
    double bucket = 0;
    double height = 0;
    struct Range { double low, high; };
    Range update(double low, double high, double step, double pixels,
                 double row_height, double focus, bool follow) {
        const double rows = std::max(1.0, std::floor(pixels / row_height));
        const double maximum = rows * step;
        double center = (low + high) * 0.5;
        double span = high - low;
        if (bucket <= 0) {
            span = maximum;
            if (std::isfinite(focus)) center = focus;
        } else {
            span *= step / bucket * pixels / height;
        }
        span = std::clamp(span, std::min(step, maximum), maximum);
        if (follow && std::isfinite(focus) && std::abs(focus - center) > span * 0.25)
            center = std::round(focus / step) * step;
        bucket = std::isfinite(focus) || bucket > 0 ? step : 0;
        height = pixels;
        return {center - span * 0.5, center + span * 0.5};
    }
};

// Fit observed prices, not remote resting orders. Expand immediately; require
// five seconds of spare room before reducing the display increment.
struct RealtimeAutoFit {
    RealtimePriceWindow::Range range{};
    int grouping = 0;
    int64_t spare_since = 0, last_clock = 0;
    void update(double low, double high, double tick, int minimum,
                double pixels, double row_height, int64_t clock) {
        if (!std::isfinite(low) || !std::isfinite(high) || high < low ||
            !std::isfinite(tick) || tick <= 0 || pixels <= 0 || row_height <= 0) return;
        if (clock < last_clock) { grouping = 0; spare_since = 0; }
        last_clock = clock;
        minimum = std::max(1, minimum);
        const double rows = std::max(4.0, std::floor(pixels / row_height));
        const double padding = std::max((high - low) * 0.10, tick * minimum * 2);
        low -= padding; high += padding;
        const double required = (high - low) / ((rows - 2) * tick);
        int wanted = minimum;
        // Integer native-tick multiples, also divisible by the archive's
        // selected minimum grouping so a retained bin never straddles rows.
        for (int64_t decade = 1; wanted < required && decade <= 100000000; decade *= 10)
            for (int factor : {1, 2, 5, 10}) {
                const int64_t candidate = decade * factor;
                if (candidate >= minimum && candidate % minimum == 0 && candidate >= required) {
                    wanted = int(candidate); break;
                }
            }
        if (wanted < required) return; // Outside the supported integer grid.
        bool resize = false;
        if (grouping == 0 || wanted > grouping) {
            grouping = wanted; spare_since = 0; resize = true;
        } else if (wanted < grouping && (high-low) < (range.high-range.low) * 0.6) {
            if (!spare_since) spare_since = clock;
            if (clock - spare_since >= 5000) { grouping = wanted; spare_since = 0; resize = true; }
        } else spare_since = 0;
        const double step = tick * grouping;
        const double span = rows * step;
        if (resize || std::abs((range.high-range.low)-span) > step * 0.01 ||
            low < range.low || high > range.high) {
            const double center = std::round((low+high) * 0.5 / step) * step;
            range = {center-span*0.5, center+span*0.5};
        }
    }
};
