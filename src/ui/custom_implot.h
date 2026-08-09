#pragma once

#include "implot.h"
#include "imgui.h"

namespace CustomImPlot {
    void PlotCandlestick (
        const char* label_id,
        const double* xs,
        const double* opens,
        const double* closes,
        const double* lows,
        const double* highs,
        int count,
        bool tooltip = true,
        float width_percent = 0.25f,
        ImVec4 bullCol = ImVec4(0,1,0,1),
        ImVec4 bearCol = ImVec4(1,0,0,1)
    );

    // Draws a TradingView-style value tag pinned to the (Opposite/right) Y-axis
    // gutter at the pixel position of `value`, plus a thin dashed reference line
    // across the plot at that level. `bg_color` is the tag fill (typically green
    // when the current candle/bar is bullish, red when bearish). `text` is the
    // pre-formatted label drawn in white inside the tag.
    //
    // MUST be called between BeginPlot/EndPlot for the target plot. The current
    // Y-axis (ImAxis_Y1) is used for the plot<->pixel mapping.
    void DrawAxisValueTag(
        double value,
        const char* text,
        ImU32 bg_color,
        bool draw_line = true
    );
}