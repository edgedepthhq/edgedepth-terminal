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
inline bool realtime_view_has_live_edge(bool following, double right, int64_t clock) {
    return following || right >= double(clock);
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
