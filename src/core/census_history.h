#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <iterator>
#include <utility>

// Bounded, source-timestamped snapshots. No interpolation over capture gaps.
namespace census_history {
inline constexpr int64_t kColumnMs = 60000;
inline constexpr int64_t kFreshMs = 120000;
inline constexpr size_t kMaxFrames = 361;
inline constexpr size_t kMaxBands = 256;
template<class Frame> class History {
public:
    bool insert(Frame frame) {
        if (frame.timestamp_ms <= 0 || frame.bands.size() > kMaxBands ||
            !std::isfinite(frame.mark_price) || frame.mark_price < 0 ||
            !std::isfinite(frame.band_width_pct) || frame.band_width_pct <= 0 ||
            !std::isfinite(frame.flow_intensity) || frame.flow_intensity < 0 || frame.flow_intensity > 1)
            return false;
        if (frame.census_status != "sampled" && frame.census_status != "legacy" && frame.census_status != "unavailable") return false;
        if (frame.census_observed_at_ms < 0 || frame.census_observed_at_ms > frame.timestamp_ms) return false;
        if (frame.census_status != "unavailable" && frame.mark_price <= 0) return false;
        if (frame.census_status == "unavailable" && !frame.bands.empty()) return false;
        for (const auto& b : frame.bands)
            if (!std::isfinite(b.price_mid) || b.price_mid <= 0 ||
                !std::isfinite(b.est_long_usd) || b.est_long_usd < 0 ||
                !std::isfinite(b.est_short_usd) || b.est_short_usd < 0) return false;
        const auto& q = frame.census_quality;
        if (q.version > 1) return false;
        if (q.version == 1) {
            if (q.venue_received_at_ms < 0 || q.venue_received_at_ms > frame.timestamp_ms ||
                !std::isfinite(q.weighted_wallet_age_ms) || q.weighted_wallet_age_ms < 0 || q.weighted_wallet_age_ms > 300000 ||
                q.p95_wallet_age_ms < 0 || q.p95_wallet_age_ms > 300000) return false;
            for (double value : {q.sampled_notional_usd, q.usable_notional_usd, q.unlocated_notional_usd,
                                q.stale_notional_usd, q.coverage_denominator_usd, q.far_filtered_notional_usd})
                if (!std::isfinite(value) || value < 0) return false;
            if (q.sampled_positions != q.usable_positions + q.unlocated_positions ||
                q.far_filtered_positions > q.unlocated_positions || q.far_filtered_notional_usd > q.unlocated_notional_usd ||
                std::abs(q.sampled_notional_usd - q.usable_notional_usd - q.unlocated_notional_usd) > 1e-8 * std::max(1.0, q.sampled_notional_usd)) return false;
            const double coverage = q.coverage_denominator_usd > 0 ? std::min(1.0, q.usable_notional_usd / q.coverage_denominator_usd) : 0.0;
            if (std::abs(frame.flow_intensity - coverage) > 1e-8) return false;
            if (frame.census_status == "legacy") return false;
            if (frame.census_status == "sampled" && (q.usable_positions == 0 || q.usable_notional_usd <= 0 ||
                frame.census_observed_at_ms <= 0 || frame.timestamp_ms - frame.census_observed_at_ms > 300000)) return false;
            if (frame.census_status == "unavailable" && (q.usable_positions != 0 || q.usable_notional_usd != 0 || frame.census_observed_at_ms != 0)) return false;
        }
        const auto old = frames_.find(frame.timestamp_ms);
        // A legacy DB backfill must not erase live observation-age metadata.
        if (old != frames_.end() && ((old->second.census_status != "legacy" && frame.census_status == "legacy") ||
            old->second.census_quality.version > frame.census_quality.version)) return true;
        frames_.insert_or_assign(frame.timestamp_ms, std::move(frame));
        while (frames_.size() > kMaxFrames) frames_.erase(frames_.begin());
        return true;
    }
    const Frame* at(int64_t asof) const {
        auto it = frames_.upper_bound(asof);
        return it == frames_.begin() ? nullptr : &std::prev(it)->second;
    }
    // Only the latest observation known at this playhead may be held to now.
    // Older columns never paint through an unknown gap to a later snapshot.
    int64_t display_end(int64_t timestamp, int64_t asof) const {
        const auto frame = frames_.find(timestamp);
        if (frame == frames_.end() || timestamp > asof) return timestamp;
        const auto next = std::next(frame);
        if (next == frames_.end() || next->first > asof) return asof;
        return std::min({asof, timestamp + kColumnMs, next->first});
    }
    const auto& frames() const { return frames_; }
private:
    std::map<int64_t, Frame> frames_;
};
}
