#pragma once

#include <cmath>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
// GBM Analytical Reach Probability — shared between
// LiquidationHeatmapManager and ShaderHeatmapRenderer (Phase 2a)
// ═══════════════════════════════════════════════════════════════════

namespace reach_math {

/// Standard normal CDF via erfc
inline double normal_cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

/// GBM reflection principle: P(price reaches target within T hours)
/// Mirrors analytics.GBMReachProbability in Go backend.
/// @param dist_pct   Distance from mark to target as percentage (always positive)
/// @param sigma_annual Annualized volatility (e.g. 1.20 for ~BTC)
/// @param time_hours  Horizon in hours (e.g. 4.0)
/// @return Probability [0, 1]
inline double gbm_reach_probability(double dist_pct, double sigma_annual, double time_hours) {
    if (dist_pct <= 0.0 || sigma_annual <= 0.0 || time_hours <= 0.0) return 0.0;
    const double T = time_hours / 8760.0;
    const double sqrt_T = std::sqrt(T);
    const double x_liq = std::abs(std::log(1.0 + dist_pct / 100.0));
    if (x_liq < 1e-12) return 1.0;
    const double sig_sqrt_T = sigma_annual * sqrt_T;
    if (sig_sqrt_T < 1e-12) return 0.0;
    // Zero-drift case: P = 2 * Phi(-x / (sigma*sqrt(T)))
    return std::clamp(2.0 * normal_cdf(-x_liq / sig_sqrt_T), 0.0, 1.0);
}

/// Compute reach_prob for a band given distance from mark.
/// Uses proto value if > 0.001, otherwise computes GBM analytically.
inline float compute_reach_for_band(double price_mid, double mark_price,
                                     float proto_reach, bool has_proto_reach) {
    if (has_proto_reach && proto_reach > 0.001f) {
        return proto_reach;
    }
    if (mark_price <= 0.0) return 1.0f;
    const double dist_pct = std::abs((price_mid - mark_price) / mark_price * 100.0);
    return static_cast<float>(gbm_reach_probability(dist_pct, 1.20, 4.0));
}

} // namespace reach_math
