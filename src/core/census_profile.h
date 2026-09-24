#pragma once
#include "core/census_history.h"
#include <array>

// Geometry and current-state selection shared by the chart and regression fixtures.
namespace census_profile {
struct Bounds { double low, high; };
template<class Frame, class Band> Bounds bounds(const Frame& f, const Band& b) {
    if (f.census_quality.version == 2) {
        const double half = std::sqrt(1.0025);
        return {b.price_mid / half, b.price_mid * half};
    }
    const double half = f.mark_price * f.band_width_pct / 200.0;
    return {b.price_mid - half, b.price_mid + half};
}
enum class State { missing, unavailable, stale, legacy, ready };
template<class Frame> State state(const Frame* f, int64_t asof) {
    if (!f || f->timestamp_ms > asof) return State::missing;
    if (asof - f->timestamp_ms > census_history::kFreshMs) return State::stale;
    if (f->census_status == "unavailable" || f->bands.empty()) return State::unavailable;
    if (f->census_quality.version == 0) return State::legacy;
    return State::ready;
}
struct Zone { double price=0, low=0, high=0, usd=0; bool is_long=false; };
struct Profile {
    std::array<Zone, census_history::kMaxBands*2> zones{};
    size_t count=0;
    double long_usd=0, short_usd=0, visible_usd=0, filtered_usd=0, outside_usd=0, max_usd=0;
};
template<class Frame> Profile build(const Frame& f, double low, double high, double floor) {
    Profile out;
    // Stored history can split sides at the same center; combine before filtering.
    for (const auto& b : f.bands) {
        const auto range = bounds(f,b);
        for (int side=0;side<2;++side) {
            const double usd=side==0?b.est_long_usd:b.est_short_usd;
            if (usd<=0) continue;
            if (side==0) out.long_usd+=usd; else out.short_usd+=usd;
            size_t i=0;
            for (;i<out.count;++i) if(out.zones[i].price==b.price_mid && out.zones[i].is_long==(side==0)) break;
            if (i==out.count) {
                if(out.count==out.zones.size()) continue; // history already enforces source budget
                out.zones[out.count++]={b.price_mid,range.low,range.high,0,side==0};
            }
            out.zones[i].usd+=usd;
        }
    }
    size_t kept=0;
    for(size_t i=0;i<out.count;++i) {
        auto z=out.zones[i];
        if(z.high<low||z.low>high) {out.outside_usd+=z.usd;continue;}
        if(z.usd<floor) {out.filtered_usd+=z.usd;continue;}
        out.visible_usd+=z.usd;out.max_usd=std::max(out.max_usd,z.usd);
        out.zones[kept++]=z;
    }
    out.count=kept;
    std::sort(out.zones.begin(),out.zones.begin()+out.count,[](const Zone& a,const Zone& b){
        if(a.usd!=b.usd)return a.usd>b.usd;
        if(a.price!=b.price)return a.price<b.price;
        return a.is_long>b.is_long;
    });
    return out;
}
// Fixed notional reference for historical brightness; no minimum current boost.
inline float opacity(double usd) {
    return static_cast<float>(0.65*std::clamp(std::log1p(usd)/std::log1p(1e7),0.0,1.0));
}
}
