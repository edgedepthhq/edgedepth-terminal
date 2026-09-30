#pragma once
// The strongest standing zones of the scenario map near the live price, for touch-odds labels.
// Mass is the standing fuel within 0.5% (log) of a level, the same window the registered
// long-range study used. The map itself showed no
// predictive edge there; these labels only attach calibrated touch odds to the zones people look at.
#include <algorithm>
#include <cmath>
#include <vector>

namespace touch_zones {

struct Fuel { double price; double mass; };
struct Zone { double price; double mass; int side; };

inline constexpr double kWindow = 0.005;    // log half-width of a zone
inline constexpr double kSeparation = 0.01; // log distance between labelled zones
inline constexpr double kMin = 0.005, kMax = 0.10;

// At most per_side zones above and below live, 0.5% to 10% away, strongest first on each side.
inline std::vector<Zone> strongest(std::vector<Fuel> fuel, double live, int per_side) {
    std::vector<Zone> out;
    if (!(live > 0) || per_side <= 0) return out;
    const double lo = std::log(live) - std::log1p(kMax) - kWindow, hi = std::log(live) + std::log1p(kMax) + kWindow;
    std::vector<std::pair<double, double>> pts;  // (log price, mass)
    for (const auto& f : fuel)
        if (f.price > 0 && f.mass > 0 && std::isfinite(f.mass)) {
            const double lp = std::log(f.price);
            if (lp >= lo && lp <= hi) pts.emplace_back(lp, f.mass);
        }
    std::sort(pts.begin(), pts.end());
    const size_t n = pts.size();
    std::vector<double> prefix(n + 1, 0.0), weighted(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + pts[i].second;
        weighted[i + 1] = weighted[i] + pts[i].second*pts[i].first;
    }
    struct Cand { double lp, mass; };
    std::vector<Cand> cands;
    size_t a = 0, b = 0;
    for (size_t i = 0; i < n; ++i) {
        while (a < n && pts[a].first < pts[i].first - kWindow) ++a;
        while (b < n && pts[b].first <= pts[i].first + kWindow) ++b;
        const double m = prefix[b] - prefix[a];
        if (m > 0) cands.push_back({(weighted[b] - weighted[a])/m, m});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.mass > y.mass; });
    int up = 0, down = 0;
    for (const auto& c : cands) {
        const double d = std::exp(c.lp)/live - 1.0;
        const int side = d > 0 ? 1 : -1;
        if (std::abs(d) < kMin || (side > 0 ? d > kMax : -d > kMax)) continue;
        if ((side > 0 ? up : down) >= per_side) continue;
        bool near = false;
        for (const auto& z : out) near = near || std::abs(std::log(z.price) - c.lp) < kSeparation;
        if (near) continue;
        out.push_back({std::exp(c.lp), c.mass, side});
        (side > 0 ? up : down)++;
        if (up >= per_side && down >= per_side) break;
    }
    return out;
}

}  // namespace touch_zones
