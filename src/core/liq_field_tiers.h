#pragma once
#include "core/liq_field_brackets.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace liq_field {
inline constexpr std::string_view kModelVersion = "lf.v2";
inline constexpr std::array<double, 6> kLeverages{5, 10, 25, 50, 75, 100};

// Match the pinned Go model. Other venues and unknown symbols retain the
// legacy uncapped assumptions, explicitly identified in the layer details.
inline double max_leverage(std::string_view exchange, std::string_view symbol) {
    if (exchange != "binancef") return 0;
    size_t lo = 0, hi = kLeverageCaps.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (kLeverageCaps[mid].symbol < symbol) lo = mid + 1;
        else hi = mid;
    }
    return lo < kLeverageCaps.size() && kLeverageCaps[lo].symbol == symbol
        ? kLeverageCaps[lo].leverage : 0;
}

struct TierSelection { uint8_t enabled = 0; uint8_t floor = 0; };
inline TierSelection select_tiers(uint8_t mask, double cap) {
    TierSelection out;
    for (size_t i = 0; i < kLeverages.size(); ++i)
        if ((mask & (1u << i)) && (cap <= 0 || kLeverages[i] <= cap))
            out.enabled |= static_cast<uint8_t>(1u << i);
    int remaining = 3;
    for (int i = 5; i >= 0 && remaining; --i) {
        if (out.enabled & (1u << i)) {
            out.floor |= static_cast<uint8_t>(1u << i);
            --remaining;
        }
    }
    return out;
}
}
