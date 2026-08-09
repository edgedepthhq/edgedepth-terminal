// ═══════════════════════════════════════════════════════════════════════════════
// preview_candle_store.cpp — replay scrub-preview candle store
// ═══════════════════════════════════════════════════════════════════════════════

#include "preview_candle_store.h"
#include "../stream_handler.h"
#include <algorithm>

void PreviewCandleStore::apply_batch(int64_t timeframe_sec,
                                     std::vector<Terminal::Candle>&& batch) {
    timestamps_.clear();
    opens_.clear();
    highs_.clear();
    lows_.clear();
    closes_.clear();
    timestamps_.reserve(batch.size());
    opens_.reserve(batch.size());
    highs_.reserve(batch.size());
    lows_.reserve(batch.size());
    closes_.reserve(batch.size());
    for (const auto& c : batch) {
        timestamps_.push_back(static_cast<double>(c.timestamp_ms));
        opens_.push_back(c.open);
        highs_.push_back(c.high);
        lows_.push_back(c.low);
        closes_.push_back(c.close);
    }
    timeframe_sec_ = timeframe_sec;
}

void PreviewCandleStore::clear() {
    timestamps_.clear();
    opens_.clear();
    highs_.clear();
    lows_.clear();
    closes_.clear();
    timeframe_sec_ = 0;
    requested_tf_sec_ = 0;
}

void PreviewCandleStore::request(StreamManager& streams,
                                 const Terminal::Pair& pair,
                                 int64_t timeframe_sec) {
    // The backend floors sub-minute requests to 1m; mirror that here so a 15s
    // chart doesn't re-request on every frame the served TF (1m) mismatches.
    if (timeframe_sec < 60) timeframe_sec = 60;
    // Dedupe: same TF already in flight or already served.
    if (requested_tf_sec_ == timeframe_sec) return;
    if (timeframe_sec_ == timeframe_sec && ready()) return;
    requested_tf_sec_ = timeframe_sec;
    streams.request_replay_preview_candles(pair, timeframe_sec);
}

bool PreviewCandleStore::close_at(int64_t ts_ms, double& out) const {
    if (timestamps_.empty()) return false;
    const double t = static_cast<double>(ts_ms);
    // upper_bound - 1: the last candle whose open time is <= ts.
    auto it = std::upper_bound(timestamps_.begin(), timestamps_.end(), t);
    if (it == timestamps_.begin()) return false;
    const size_t idx = static_cast<size_t>(std::distance(timestamps_.begin(), it)) - 1;
    out = closes_[idx];
    return true;
}

bool PreviewCandleStore::minmax_in_range(double t0_ms, double t1_ms,
                                         double& lo, double& hi) const {
    if (timestamps_.empty() || t1_ms < t0_ms) return false;
    // Include the candle straddling t0 (its open time may be before t0).
    auto first = std::lower_bound(timestamps_.begin(), timestamps_.end(), t0_ms);
    if (first != timestamps_.begin()) --first;
    size_t i = static_cast<size_t>(std::distance(timestamps_.begin(), first));
    bool any = false;
    lo = 0.0;
    hi = 0.0;
    for (; i < timestamps_.size() && timestamps_[i] <= t1_ms; ++i) {
        if (!any) {
            lo = lows_[i];
            hi = highs_[i];
            any = true;
        } else {
            lo = std::min(lo, lows_[i]);
            hi = std::max(hi, highs_[i]);
        }
    }
    return any;
}
