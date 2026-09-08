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
                       5000.0, double(RealtimeDepthHistory::retention_ms)),
            following || (wheel < 0 && !paused)};
}
