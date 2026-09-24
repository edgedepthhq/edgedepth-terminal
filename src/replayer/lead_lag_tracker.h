#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// lead_lag_tracker.h - who moved first, and by how much, between the markets
// of one replay session.
//
// A compare replay plays two (or three) markets on ONE clock: the box merges
// every symbol's tape by exchange timestamp, so the order trades arrive in IS
// the order they happened. This tracker turns that into two facts a viewer
// can read off a strip instead of eyeballing two charts:
//
//   1. FIRST MOVE. From an anchor instant (the session anchor, or the last
//      seek target), the first trade in each market whose price sits at least
//      `threshold_bps` away from that market's anchor price. The difference
//      between the two markets' first-move instants is the lead, in ms, with
//      the direction each moved.
//
//   2. LAG. Over a trailing window, log returns bucketed at a fixed grain
//      (200 ms), forward-filled, and the lag at which the cross-correlation
//      of the two return series peaks. A positive lag means the primary
//      leads: the follower's returns line up with the primary's returns from
//      `lag` earlier. Reported with the correlation at the peak and the
//      number of overlapping buckets, so a thin tape reads as thin.
//
// Pure: no ImGui, no Emscripten, no clock of its own. Every timestamp is the
// exchange timestamp the trade carried, so wall-clock speed (0.1x, 4x) never
// enters. tests/native/lead_lag_tracker_test.cpp pins it with a host g++.
// ═══════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace leadlag {

inline constexpr int64_t kBucketMs = 200;          // return grain
inline constexpr int     kWindowBuckets = 300;     // 60 s trailing window
inline constexpr int     kMaxLagBuckets = 25;      // +-5 s of lag searched
inline constexpr int     kMinOverlap = 30;         // fewer buckets: no lag reported
inline constexpr double  kDefaultThresholdBps = 10.0;

struct Leg {
    std::string symbol;
    double  anchor_px = 0.0;    // first trade at or after the anchor instant
    int64_t anchor_ts = 0;
    double  last_px = 0.0;
    int64_t last_ts = 0;
    int64_t first_move_ts = 0;  // 0 = not yet moved past the threshold
    int     first_move_dir = 0; // +1 up, -1 down
    // Ring of last prices per bucket since the anchor. Index = bucket ordinal
    // modulo kWindowBuckets; `filled[i]` says the slot holds a price for the
    // ordinal in `ordinal[i]` (a stale slot from a lap ago is not a price).
    std::vector<double>  bucket_px;
    std::vector<int64_t> bucket_ord;
    std::vector<char>    bucket_filled;

    double change_bps() const {
        if (anchor_px <= 0.0 || last_px <= 0.0) return 0.0;
        return (last_px / anchor_px - 1.0) * 10000.0;
    }
};

struct LagEstimate {
    bool    ok = false;
    int     lag_buckets = 0;    // >0: primary leads by lag_buckets * kBucketMs
    double  rho = 0.0;          // correlation at the peak
    int     overlap = 0;        // buckets both legs contributed
    int64_t lag_ms() const { return static_cast<int64_t>(lag_buckets) * kBucketMs; }
};

struct Result {
    int64_t anchor_ms = 0;
    double  threshold_bps = kDefaultThresholdBps;
    std::vector<Leg> legs;      // legs[0] is the primary
    // First-move lead of the primary over the first compare leg: positive
    // when the primary moved first. Only meaningful when both moved.
    bool    both_moved = false;
    int64_t first_move_lead_ms = 0;
    LagEstimate lag;            // primary vs the first compare leg
};

class Tracker {
public:
    // Start (or restart) from an anchor instant. Everything before it is
    // forgotten: a seek is a new question.
    void reset(int64_t anchor_ms, const std::vector<std::string>& symbols,
               double threshold_bps = kDefaultThresholdBps) {
        anchor_ms_ = anchor_ms;
        threshold_bps_ = threshold_bps > 0.0 ? threshold_bps : kDefaultThresholdBps;
        legs_.clear();
        legs_.reserve(symbols.size());
        for (const auto& s : symbols) {
            Leg leg;
            leg.symbol = s;
            leg.bucket_px.assign(kWindowBuckets, 0.0);
            leg.bucket_ord.assign(kWindowBuckets, -1);
            leg.bucket_filled.assign(kWindowBuckets, 0);
            legs_.push_back(std::move(leg));
        }
    }

    bool armed() const { return anchor_ms_ > 0 && legs_.size() >= 2; }
    int64_t anchor_ms() const { return anchor_ms_; }

    // One trade of `symbol` at exchange time ts_ms. Trades before the anchor
    // are ignored; the first at or after it fixes the leg's anchor price.
    void on_trade(const std::string& symbol, int64_t ts_ms, double px) {
        if (anchor_ms_ <= 0 || ts_ms < anchor_ms_ || !(px > 0.0)) return;
        Leg* leg = find(symbol);
        if (!leg) return;
        if (leg->anchor_px <= 0.0) {
            leg->anchor_px = px;
            leg->anchor_ts = ts_ms;
        }
        leg->last_px = px;
        leg->last_ts = ts_ms;
        if (leg->first_move_ts == 0) {
            const double bps = (px / leg->anchor_px - 1.0) * 10000.0;
            if (std::fabs(bps) >= threshold_bps_) {
                leg->first_move_ts = ts_ms;
                leg->first_move_dir = bps > 0.0 ? 1 : -1;
            }
        }
        const int64_t ord = (ts_ms - anchor_ms_) / kBucketMs;
        const int slot = static_cast<int>(ord % kWindowBuckets);
        leg->bucket_px[slot] = px;
        leg->bucket_ord[slot] = ord;
        leg->bucket_filled[slot] = 1;
    }

    // The readout at `now_ms` (the replay clock). Cheap enough for a few Hz;
    // not for every frame.
    Result compute(int64_t now_ms) const {
        Result r;
        r.anchor_ms = anchor_ms_;
        r.threshold_bps = threshold_bps_;
        r.legs = legs_;
        if (legs_.size() < 2) return r;
        const Leg& a = legs_[0];
        const Leg& b = legs_[1];
        if (a.first_move_ts > 0 && b.first_move_ts > 0) {
            r.both_moved = true;
            r.first_move_lead_ms = b.first_move_ts - a.first_move_ts;
        }
        r.lag = estimate_lag(a, b, anchor_ms_, now_ms);
        return r;
    }

private:
    Leg* find(const std::string& symbol) {
        for (auto& l : legs_) if (l.symbol == symbol) return &l;
        return nullptr;
    }

    // Forward-filled per-bucket log returns over the trailing window ending at
    // now_ms. A bucket with no trade takes the previous price (return 0), and
    // the series starts at the first bucket both legs have a price for.
    static void returns_for(const Leg& leg, int64_t anchor_ms, int64_t now_ms,
                            std::vector<double>& out, std::vector<char>& have) {
        out.assign(kWindowBuckets, 0.0);
        have.assign(kWindowBuckets, 0);
        if (leg.anchor_px <= 0.0) return;
        const int64_t last_ord = (now_ms - anchor_ms) / kBucketMs;
        const int64_t first_ord = std::max<int64_t>(0, last_ord - kWindowBuckets + 1);
        double prev = 0.0;
        for (int64_t ord = first_ord; ord <= last_ord; ++ord) {
            const int slot = static_cast<int>(ord % kWindowBuckets);
            const int i = static_cast<int>(ord - first_ord);
            double px = prev;
            if (leg.bucket_filled[slot] && leg.bucket_ord[slot] == ord) px = leg.bucket_px[slot];
            if (px > 0.0 && prev > 0.0) {
                out[i] = std::log(px / prev);
                have[i] = 1;
            }
            if (px > 0.0) prev = px;
        }
    }

    // Both legs bucket from the SAME origin, the tracker anchor (on_trade
    // computes ordinals from it); a leg's own anchor_ts is only where its
    // first post-anchor trade landed.
    static LagEstimate estimate_lag(const Leg& a, const Leg& b, int64_t anchor_ms, int64_t now_ms_in) {
        LagEstimate est;
        const int64_t now_ms = std::max(now_ms_in, std::max(a.last_ts, b.last_ts));
        std::vector<double> ra, rb;
        std::vector<char> ha, hb;
        returns_for(a, anchor_ms, now_ms, ra, ha);
        returns_for(b, anchor_ms, now_ms, rb, hb);
        // Standardise each series over the buckets it has, so a market with
        // ten times the volatility does not dominate the covariance.
        auto stats = [](const std::vector<double>& x, const std::vector<char>& h, double& mean, double& sd, int& n) {
            mean = 0.0; sd = 0.0; n = 0;
            for (int i = 0; i < kWindowBuckets; ++i) if (h[i]) { mean += x[i]; ++n; }
            if (n < 2) return;
            mean /= n;
            for (int i = 0; i < kWindowBuckets; ++i) if (h[i]) sd += (x[i] - mean) * (x[i] - mean);
            sd = std::sqrt(sd / (n - 1));
        };
        double ma, sa, mb, sb; int na, nb;
        stats(ra, ha, ma, sa, na);
        stats(rb, hb, mb, sb, nb);
        if (na < kMinOverlap || nb < kMinOverlap || !(sa > 0.0) || !(sb > 0.0)) return est;
        double best = 0.0; int best_lag = 0; int best_n = 0; bool any = false;
        for (int lag = -kMaxLagBuckets; lag <= kMaxLagBuckets; ++lag) {
            // rho(lag) = corr(a[t], b[t + lag]): lag > 0 means b follows a.
            double acc = 0.0; int n = 0;
            for (int t = 0; t < kWindowBuckets; ++t) {
                const int u = t + lag;
                if (u < 0 || u >= kWindowBuckets || !ha[t] || !hb[u]) continue;
                acc += ((ra[t] - ma) / sa) * ((rb[u] - mb) / sb);
                ++n;
            }
            if (n < kMinOverlap) continue;
            const double rho = acc / (n - 1);
            if (!any || rho > best) { best = rho; best_lag = lag; best_n = n; any = true; }
        }
        if (!any) return est;
        est.ok = true;
        est.lag_buckets = best_lag;
        est.rho = best;
        est.overlap = best_n;
        return est;
    }

    int64_t anchor_ms_ = 0;
    double threshold_bps_ = kDefaultThresholdBps;
    std::vector<Leg> legs_;
};

}  // namespace leadlag
