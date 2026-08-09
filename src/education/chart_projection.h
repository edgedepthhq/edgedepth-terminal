#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// chart_projection.h — per-frame snapshot of the main chart's coordinate system
//
// The lesson spotlight must project (candle index, price) → screen pixels to draw
// its dim/cut-out/ring over the chart. ImPlot's PlotToPixels only works between
// BeginPlot/EndPlot, so the ChartWidget publishes its plot rect + axis ranges
// here once per frame (inside its plot scope), and the lesson overlay — which runs
// later in the frame on the foreground draw list — reads it and does the linear
// projection itself (same math as the HTML reference's xOfProgress / yOfPrice).
//
// Single global: there is one main price chart per page (single-symbol client).
// `valid` is false until the chart has rendered at least one frame.
// ═══════════════════════════════════════════════════════════════════════════════

#include "imgui.h"

namespace edu {

struct ChartProjection {
    bool   valid   = false;   // false until the chart publishes a frame
    ImVec2 plot_min{};        // top-left of the plot area, screen px
    ImVec2 plot_max{};        // bottom-right of the plot area, screen px
    double x_min_ms = 0.0;    // visible X range (candle time, ms)
    double x_max_ms = 0.0;
    double y_min = 0.0;       // visible Y range (price), INCLUDING the chart's padding
    double y_max = 0.0;

    // time(ms) → screen x. Clamps nothing; caller clamps to plot rect if needed.
    float x_of_time(double t_ms) const {
        if (x_max_ms <= x_min_ms) return plot_min.x;
        double f = (t_ms - x_min_ms) / (x_max_ms - x_min_ms);
        return plot_min.x + static_cast<float>(f) * (plot_max.x - plot_min.x);
    }
    // price → screen y (y axis is inverted: higher price = smaller screen y)
    float y_of_price(double price) const {
        if (y_max <= y_min) return plot_min.y;
        double f = (price - y_min) / (y_max - y_min);
        return plot_max.y - static_cast<float>(f) * (plot_max.y - plot_min.y);
    }

    // ─── Inverse projection (screen px → chart space) ────────────────────────
    // The studio CAPTURE path needs the reverse of the two functions above: an
    // author drags a pixel box on the chart and we recover the chart-space
    // {time, price} bounds to store on the annotation. These are the exact
    // algebraic inverses of x_of_time / y_of_price — no new state, just the same
    // ranges read the other way. Pixels OUTSIDE the plot rect extrapolate
    // linearly (no clamp); the caller clamps to [x_min_ms,x_max_ms] /
    // [y_min,y_max] if it wants on-chart-only capture.

    // screen x → time(ms)
    double time_of_x(float px) const {
        const double span = static_cast<double>(plot_max.x - plot_min.x);
        if (span <= 0.0) return x_min_ms;
        const double f = (static_cast<double>(px) - plot_min.x) / span;
        return x_min_ms + f * (x_max_ms - x_min_ms);
    }
    // screen y → price (inverted axis: smaller screen y = higher price)
    double price_of_y(float py) const {
        const double span = static_cast<double>(plot_max.y - plot_min.y);
        if (span <= 0.0) return y_min;
        const double f = (static_cast<double>(plot_max.y) - py) / span;
        return y_min + f * (y_max - y_min);
    }
};

// Defined in chart_widget.cpp.
ChartProjection& chart_projection();

} // namespace edu
