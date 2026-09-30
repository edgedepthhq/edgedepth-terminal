#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace compression {
struct Geometry {
    int64_t asof = 0, peak = 0, touch = 0, origin = 0, support_from = 0, support_to = 0;
    double price = 0, slope = 0, support_price = 0, support_slope = 0;
    bool valid() const {
        return asof > 0 && peak > 0 && peak < touch && touch < asof && origin == peak &&
            support_from > 0 && support_from < support_to && support_to < asof &&
            std::isfinite(price) && price > 0 && std::isfinite(slope) && slope < 0 &&
            std::isfinite(support_price) && support_price > 0 && std::isfinite(support_slope);
    }
    double resistance(double t) const { return price + slope * (t - static_cast<double>(origin)); }
    double support(double t) const { return support_price + support_slope * (t - static_cast<double>(support_from)); }
    bool visible(int64_t clock) const { return valid() && clock >= asof; }
};
}
