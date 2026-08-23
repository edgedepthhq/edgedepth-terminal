// ═══════════════════════════════════════════════════════════════════════════════
// reach_math_test.cpp - native pin on the GBM reach probability.
//
// reach_math is the ONE piece of pricing maths the client computes itself: the
// liquidation heatmap asks "will price touch this band inside the horizon?" and
// colours the band by the answer. The Go backend computes the same number
// (analytics.GBMReachProbability) and ships it in the proto; the client formula
// is the fallback for bands that arrive without one. The two MUST agree, so the
// closed form is pinned here against hand-computed values of the reflection
// principle rather than against whatever the code happens to return today.
//
//   P(touch within T) = 2 * Phi(-x / (sigma * sqrt(T))),  x = |ln(1 + d/100)|
//
// The shape properties (monotone in distance / vol / horizon, symmetric about
// the mark, always a probability) are the ones a mis-signed term breaks while
// still returning something plausible-looking on the chart.
// ═══════════════════════════════════════════════════════════════════════════════

#include "../../src/core/reach_math.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_near(double got, double want, double tolerance, const char* what) {
    if (!std::isfinite(got) || std::fabs(got - want) > tolerance) {
        std::fprintf(stderr, "FAIL %s: got %.12f, want %.12f\n", what, got, want);
        ++failures;
    }
}

// The reference the backend and the client both claim to implement, written out
// independently of reach_math's own factoring so a change to either side shows.
double reference_reach(double dist_pct, double sigma_annual, double time_hours) {
    const double x = std::fabs(std::log1p(dist_pct / 100.0));
    const double sig_sqrt_t = sigma_annual * std::sqrt(time_hours / 8760.0);
    return std::erfc(x / (sig_sqrt_t * std::sqrt(2.0)));  // 2 * Phi(-x/s)
}

void test_normal_cdf() {
    using reach_math::normal_cdf;
    expect_near(normal_cdf(0.0), 0.5, 1e-12, "Phi(0)");
    expect_near(normal_cdf(1.0), 0.841344746068543, 1e-12, "Phi(1)");
    expect_near(normal_cdf(1.959963984540054), 0.975, 1e-12, "Phi(1.96) is the 97.5% point");
    // Reflection symmetry. A colormap fed a CDF that lost this maps the band
    // above the mark differently from its mirror below it.
    for (double x = 0.25; x <= 4.0; x += 0.25) {
        expect_near(normal_cdf(-x) + normal_cdf(x), 1.0, 1e-12, "Phi(-x) + Phi(x) == 1");
    }
    expect_near(normal_cdf(-40.0), 0.0, 1e-12, "Phi far left tail underflows to 0");
    expect_near(normal_cdf(40.0), 1.0, 1e-12, "Phi far right tail saturates at 1");
}

void test_closed_form_matches_reference() {
    using reach_math::gbm_reach_probability;
    // A near band at BTC-ish vol over the heatmap's own 4h horizon: the case
    // that actually decides most of the rendered field.
    expect_near(gbm_reach_probability(0.5, 1.20, 4.0),
                reference_reach(0.5, 1.20, 4.0), 1e-12, "0.5% at 120% vol over 4h");
    expect_near(gbm_reach_probability(2.0, 1.20, 4.0),
                reference_reach(2.0, 1.20, 4.0), 1e-12, "2% at 120% vol over 4h");
    // Far band / low vol / long horizon, to exercise the tails of erfc.
    expect_near(gbm_reach_probability(15.0, 0.35, 1.0),
                reference_reach(15.0, 0.35, 1.0), 1e-12, "15% at 35% vol over 1h");
    expect_near(gbm_reach_probability(1.0, 0.80, 168.0),
                reference_reach(1.0, 0.80, 168.0), 1e-12, "1% at 80% vol over a week");

    // The log matters: distance is a RETURN, not a linear fraction. A 100% move
    // is ln(2) away, not 1.0 away, and the two differ by a lot at this size.
    const double sig_sqrt_t = 1.20 * std::sqrt(4.0 / 8760.0);
    expect_near(gbm_reach_probability(100.0, 1.20, 4.0),
                std::erfc(std::log(2.0) / (sig_sqrt_t * std::sqrt(2.0))), 1e-12,
                "100% away is ln(2) in log space");
}

void test_guard_clauses() {
    using reach_math::gbm_reach_probability;
    // Every degenerate input is "unreachable", never NaN. The heatmap multiplies
    // this into an alpha, so a NaN here would blank a whole band.
    expect_near(gbm_reach_probability(0.0, 1.20, 4.0), 0.0, 0.0, "zero distance");
    expect_near(gbm_reach_probability(-1.0, 1.20, 4.0), 0.0, 0.0, "negative distance");
    expect_near(gbm_reach_probability(1.0, 0.0, 4.0), 0.0, 0.0, "zero vol");
    expect_near(gbm_reach_probability(1.0, -1.0, 4.0), 0.0, 0.0, "negative vol");
    expect_near(gbm_reach_probability(1.0, 1.20, 0.0), 0.0, 0.0, "zero horizon");
    expect_near(gbm_reach_probability(1.0, 1.20, -4.0), 0.0, 0.0, "negative horizon");

    // A distance so small it vanishes in log space is certain, not zero: the
    // band is already at the mark.
    expect_near(gbm_reach_probability(1e-15, 1.20, 4.0), 1.0, 0.0, "sub-epsilon distance is certain");
    // A vol*sqrt(T) that vanishes is unreachable rather than a divide by zero.
    expect_near(gbm_reach_probability(1.0, 1e-300, 4.0), 0.0, 0.0, "vanishing vol*sqrt(T)");
}

void test_monotonicity_and_range() {
    using reach_math::gbm_reach_probability;
    // Farther is never more likely.
    double prev = 2.0;
    for (double d = 0.1; d <= 25.0; d += 0.1) {
        const double p = gbm_reach_probability(d, 1.20, 4.0);
        expect_true(p <= prev + 1e-15, "reach is non-increasing in distance");
        expect_true(p >= 0.0 && p <= 1.0, "reach stays a probability over distance");
        prev = p;
    }
    // More vol is never less likely.
    prev = -1.0;
    for (double s = 0.05; s <= 5.0; s += 0.05) {
        const double p = gbm_reach_probability(2.0, s, 4.0);
        expect_true(p >= prev - 1e-15, "reach is non-decreasing in vol");
        expect_true(p >= 0.0 && p <= 1.0, "reach stays a probability over vol");
        prev = p;
    }
    // More time is never less likely.
    prev = -1.0;
    for (double h = 0.25; h <= 72.0; h += 0.25) {
        const double p = gbm_reach_probability(2.0, 1.20, h);
        expect_true(p >= prev - 1e-15, "reach is non-decreasing in horizon");
        expect_true(p >= 0.0 && p <= 1.0, "reach stays a probability over horizon");
        prev = p;
    }
    // Both limits are attained, not merely approached to something odd.
    expect_near(gbm_reach_probability(0.01, 400.0, 8760.0), 1.0, 1e-6, "huge vol reaches everything");
    expect_near(gbm_reach_probability(500.0, 0.10, 0.25), 0.0, 1e-12, "far band at low vol is unreachable");
}

void test_band_reach_prefers_the_backend_value() {
    using reach_math::compute_reach_for_band;
    // A real proto value wins outright: the backend saw the whole order flow,
    // the client only sees a mark price.
    expect_near(compute_reach_for_band(100.0, 101.0, 0.42f, true), 0.42, 1e-6,
                "proto reach is used when present");
    // 0.001 is the "unset" sentinel, and the comparison is strict, so exactly
    // 0.001 still falls through to the analytic path.
    expect_true(compute_reach_for_band(100.0, 101.0, 0.001f, true) !=
                    static_cast<float>(0.001f),
                "0.001 is treated as unset, not as a reach of 0.1%");
    expect_true(compute_reach_for_band(100.0, 101.0, 0.0f, true) > 0.0f,
                "a zero proto reach falls through to the analytic path");
    expect_true(compute_reach_for_band(100.0, 101.0, 0.42f, false) !=
                    static_cast<float>(0.42f),
                "a proto reach that was never set is ignored");

    // No mark price yet: assume reachable rather than hiding the band.
    expect_near(compute_reach_for_band(100.0, 0.0, 0.0f, false), 1.0, 0.0, "unknown mark is fully reachable");
    expect_near(compute_reach_for_band(100.0, -5.0, 0.0f, false), 1.0, 0.0, "negative mark is fully reachable");

    // Bands equidistant above and below the mark colour identically. The liq
    // field is drawn on both sides of price and must not favour one.
    const float above = compute_reach_for_band(105.0, 100.0, 0.0f, false);
    const float below = compute_reach_for_band(95.0, 100.0, 0.0f, false);
    expect_near(above, below, 1e-6, "reach is symmetric about the mark");
    expect_true(above > 0.0f && above < 1.0f, "a 5% band is neither certain nor impossible");

    // And it agrees with the raw formula at the heatmap's fixed (1.20, 4h).
    expect_near(above, reference_reach(5.0, 1.20, 4.0), 1e-6,
                "band reach uses the 120% / 4h heatmap defaults");
}

}  // namespace

int main() {
    test_normal_cdf();
    test_closed_form_matches_reference();
    test_guard_clauses();
    test_monotonicity_and_range();
    test_band_reach_prefers_the_backend_value();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("reach_math_test: all assertions passed\n");
    return 0;
}
