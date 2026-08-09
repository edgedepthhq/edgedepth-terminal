#include "indicator_series.h"

#include <algorithm>
#include <cstring>

namespace Series {

    int8_t regime_index(const char* regime) {
        if (!regime || !*regime) return 0;
        // VPIN HMM labels (backend internal/hmm/hmm.go VPINLabels) and the
        // threshold fallback share one vocabulary: Normal/Elevated/High/Critical.
        switch (regime[0]) {
            case 'E': case 'e': return 1; // Elevated
            case 'H': case 'h': return 2; // High
            case 'C': case 'c': return 3; // Critical
            default:            return 0; // Normal / unknown
        }
    }

} // namespace Series

const std::vector<Series::VPINPoint> IndicatorSeriesManager::kEmptyVPIN{};

void IndicatorSeriesManager::add_vpin(const std::string& symbol,
                                      const Series::VPINPoint& p) {
    if (p.ts_ms <= 0) return;
    auto& s = vpin_[symbol];
    auto& v = s.points;
    // Fast path: append (live stream + ascending seeds).
    if (v.empty() || p.ts_ms > v.back().ts_ms) {
        v.push_back(p);
        ++s.revision;
        return;
    }
    // Sorted insert / replace-on-equal (idempotent re-delivery).
    auto it = std::lower_bound(v.begin(), v.end(), p.ts_ms,
        [](const Series::VPINPoint& a, int64_t ts) { return a.ts_ms < ts; });
    if (it != v.end() && it->ts_ms == p.ts_ms) {
        *it = p;
    } else {
        v.insert(it, p);
    }
    ++s.revision;
}

void IndicatorSeriesManager::add_vpin_batch(const std::string& symbol,
                                            const Series::VPINPoint* pts,
                                            size_t n) {
    if (!pts || n == 0) return;
    auto& s = vpin_[symbol];
    s.points.reserve(s.points.size() + n);
    for (size_t i = 0; i < n; ++i) {
        const auto& p = pts[i];
        if (p.ts_ms <= 0) continue;
        auto& v = s.points;
        if (v.empty() || p.ts_ms > v.back().ts_ms) {
            v.push_back(p);
            continue;
        }
        auto it = std::lower_bound(v.begin(), v.end(), p.ts_ms,
            [](const Series::VPINPoint& a, int64_t ts) { return a.ts_ms < ts; });
        if (it != v.end() && it->ts_ms == p.ts_ms) *it = p;
        else v.insert(it, p);
    }
    ++s.revision;
}

const std::vector<Series::VPINPoint>&
IndicatorSeriesManager::vpin(const std::string& symbol) const {
    auto it = vpin_.find(symbol);
    return it != vpin_.end() ? it->second.points : kEmptyVPIN;
}

uint64_t IndicatorSeriesManager::vpin_revision(const std::string& symbol) const {
    auto it = vpin_.find(symbol);
    return it != vpin_.end() ? it->second.revision : 0;
}

int64_t IndicatorSeriesManager::vpin_oldest_ts(const std::string& symbol) const {
    auto it = vpin_.find(symbol);
    if (it == vpin_.end() || it->second.points.empty()) return 0;
    return it->second.points.front().ts_ms;
}

void IndicatorSeriesManager::trim_after(int64_t cutoff_ms) {
    for (auto& [sym, s] : vpin_) {
        auto& v = s.points;
        auto it = std::upper_bound(v.begin(), v.end(), cutoff_ms,
            [](int64_t ts, const Series::VPINPoint& a) { return ts < a.ts_ms; });
        if (it != v.end()) {
            v.erase(it, v.end());
            ++s.revision;
        }
    }
}

void IndicatorSeriesManager::clear_all() {
    for (auto& [sym, s] : vpin_) {
        if (!s.points.empty()) {
            s.points.clear();
            ++s.revision;
        }
    }
}
