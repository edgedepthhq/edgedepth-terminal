#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Display selection only: records keep their execution time, price and value.
namespace CandleBubbleDisplay {
inline float radius(double value, double reference) {
    if (!(reference > 0) || !(value > 0) || !std::isfinite(value)) return 0;
    // Log-compressed value, not area-proportional. Reference is independent of
    // the user's visibility floor. P90/10x P90/100x P90 are ~8/14/18px: large
    // prints must not blot out the candle they sit on.
    return float(std::min(18.0, 2.5 + 1.75 * std::log2(1.0 + 8.0*value/reference)));
}
struct Marker { size_t index; float x, y, radius; };
// Input is strongest first with stable identity tie-breaks. A screen budget and
// disc separation reveal more detail on zoom without moving or summing prints.
inline bool retain(std::vector<Marker>& selected, Marker next, float width) {
    const size_t budget = size_t(std::clamp(int(width/14), 12, 180));
    if (selected.size() >= budget) return false;
    for (const auto& old : selected) {
        const float dx=next.x-old.x, dy=next.y-old.y;
        const float separation=next.radius+old.radius+4;
        if (dx*dx+dy*dy < separation*separation) return false;
    }
    selected.push_back(next); return true;
}
}
