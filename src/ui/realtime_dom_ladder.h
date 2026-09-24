#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// realtime_dom_ladder.h - the real-time chart's attached depth ladder.
//
// Owned by the chart and drawn beside its plot, from the RealtimeDOMFrame the
// plot publishes that same frame: one immutable sampled book, one as-of clock
// and the absolute price-to-screen transform, so every row sits on the chart's
// own price grid. It never reads the live book. Because it is part of the
// chart window, it is shown and hidden with the chart; a separate DOM widget
// used to follow a docked, hidden real-time chart and show nothing.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/realtime_dom_frame.h"
#include "core/symbol_metadata.h"
#include <vector>

class RealtimeDomLadder {
public:
    // Call after the chart's plot has rendered this frame, with the mono font
    // pushed. A close button in the header clears `open`.
    void render(const RealtimeDOMFrame& frame, const PriceFormatter& fmt, bool* open);
    // Width that keeps the price and five compact quantity columns readable.
    static float preferred_width();

private:
    struct Row { double bid = 0, ask = 0, buy = 0, sell = 0; };
    std::vector<Row> rows_;
    bool display_usd_ = false;
};
