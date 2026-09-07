#include "footprint_manager.h"
#include <algorithm>
#include <limits>

void FootprintManager::store_footprint(const std::string& symbol, CandleFootprint fp) {
    // The source owns minute snapshots. Other resolutions must never overwrite
    // one-minute data, and unknown time bounds cannot prove replay availability.
    if (fp.start_time <= 0 || fp.start_time % 60000 != 0 ||
        fp.end_time - fp.start_time != 60000) return;
    fp.valid = !fp.levels.empty();
    fp.version = ++data_version_;
    data_[symbol][fp.start_time] = std::move(fp);
}

void FootprintManager::on_trade(const std::string& market, int64_t ts,
                                double price, double qty, bool is_buy) {
    if (ts <= 0 || !std::isfinite(price) || price <= 0 || !std::isfinite(qty) || qty <= 0) return;
    const int64_t minute = ts / 60000 * 60000;
    if (get_footprint(market, minute)) return; // Authoritative snapshot wins.
    auto& minutes = live_[market];
    int64_t newest = minute;
    for (const auto& [start, fp] : minutes) newest = std::max(newest, start);
    if (minute < newest - 9 * 60000) return;
    // Bound provisional retention; history remains owned by complete snapshots.
    for (auto it = minutes.begin(); it != minutes.end();) {
        if (it->first < newest - 9 * 60000) it = minutes.erase(it);
        else ++it;
    }
    auto& fp = minutes[minute];
    fp.start_time = minute;
    fp.end_time = std::max(fp.end_time, ts); // Observed-through clock, not a minute-close claim.
    auto it = std::lower_bound(fp.levels.begin(), fp.levels.end(), price,
        [](const Level& lv, double p) { return lv.price < p; });
    if (it == fp.levels.end() || it->price != price) it = fp.levels.insert(it, Level{price});
    if (is_buy) { it->buy_volume += qty; fp.total_buy += qty; }
    else { it->sell_volume += qty; fp.total_sell += qty; }
    it->total_volume += qty;
    ++it->trade_count;
    it->delta = it->buy_volume - it->sell_volume;
    fp.total_volume += qty;
    fp.delta = fp.total_buy - fp.total_sell;
    fp.high_price = std::max(fp.high_price, price);
    if (fp.low_price == 0 || price < fp.low_price) fp.low_price = price;
    fp.valid = true;
    fp.version = ++data_version_;
}

const FootprintManager::CandleFootprint* FootprintManager::available(
    const std::string& market, int64_t start, int64_t as_of) const {
    const auto* closed = get_footprint(market, start);
    if (closed && closed->end_time <= as_of) return closed;
    auto market_it = live_.find(market);
    if (market_it == live_.end()) return nullptr;
    auto it = market_it->second.find(start);
    if (it == market_it->second.end() || it->second.end_time > as_of) return nullptr;
    return &it->second;
}

const FootprintManager::CandleFootprint* FootprintManager::get_footprint(
    const std::string& symbol, int64_t start_time) const
{
    auto sit = data_.find(symbol);
    if (sit == data_.end()) return nullptr;
    auto cit = sit->second.find(start_time);
    if (cit == sit->second.end()) return nullptr;
    return cit->second.valid ? &cit->second : nullptr;
}

int FootprintManager::candle_count(const std::string& symbol) const {
    auto sit = data_.find(symbol);
    if (sit == data_.end()) return 0;
    return static_cast<int>(sit->second.size());
}

void FootprintManager::clear(const std::string& symbol) {
    ++data_version_;
    data_.erase(symbol);
    live_.erase(symbol);
    merged_cache_.erase(symbol);
    if (last_symbol_ == symbol) {
        last_start_ = 0;
        last_end_ = 0;
        loading_ = false;
    }
}

const FootprintManager::MergedCache* FootprintManager::get_merged_grouped(
    const std::string& symbol, int64_t candle_ts,
    int64_t tf_sec, double tick_per_row, int64_t as_of_ms)
{
    if (tf_sec < 60 || tf_sec % 60 != 0 || candle_ts > as_of_ms) return nullptr;
    const bool provisional = as_of_ms - candle_ts < tf_sec * 1000;
    auto& sym_cache = merged_cache_[symbol];
    auto it = sym_cache.find(candle_ts);

    // Fast path: cache entry exists with matching tick_per_row.
    // Check composite version to see if underlying data changed.
    if (it != sym_cache.end() && it->second.tick_per_row == tick_per_row &&
        it->second.timeframe_seconds == tf_sec && it->second.comparison == comparison &&
        it->second.ratio == imbalance_ratio &&
        it->second.minimum_volume == imbalance_min_volume &&
        it->second.stack_levels == stacked_levels) {
        // Compute composite version from constituent 1m buckets
        const int buckets = std::max(1, static_cast<int>(tf_sec / 60));
        int64_t base_ms = (candle_ts / 60000) * 60000;
        uint64_t composite_ver = 0;
        for (int b = 0; b < buckets; ++b) {
            const auto* sub = available(symbol, base_ms + b * 60000, as_of_ms);
            if (sub && sub->end_time <= as_of_ms) composite_ver += sub->version;
        }
        if (it->second.composite_ver == composite_ver) {
            it->second.provisional = provisional;
            return it->second.levels.empty() ? nullptr : &it->second;
        }
    }

    // Cache miss - need to merge and group
    const int buckets = std::max(1, static_cast<int>(tf_sec / 60));
    int64_t base_ms = (candle_ts / 60000) * 60000;

    CandleFootprint merged;
    uint64_t composite_ver = 0;
    bool any_data = false;
    bool observed_trades = false;
    for (int b = 0; b < buckets; ++b) {
        const auto* sub = available(symbol, base_ms + b * 60000, as_of_ms);
        if (!sub || sub->end_time > as_of_ms) continue;
        any_data = true;
        observed_trades |= sub != get_footprint(symbol, base_ms + b * 60000);
        composite_ver += sub->version;
        merged.total_volume += sub->total_volume;
        merged.total_buy    += sub->total_buy;
        merged.total_sell   += sub->total_sell;
        merged.levels.insert(merged.levels.end(),
                             sub->levels.begin(), sub->levels.end());
        if (merged.high_price == 0.0 || sub->high_price > merged.high_price)
            merged.high_price = sub->high_price;
        if (merged.low_price == 0.0 || sub->low_price < merged.low_price)
            merged.low_price = sub->low_price;
    }

    MergedCache& cache = sym_cache[candle_ts];
    cache.timeframe_seconds = tf_sec;
    cache.comparison = comparison;
    cache.ratio = imbalance_ratio;
    cache.minimum_volume = imbalance_min_volume;
    cache.stack_levels = stacked_levels;
    cache.provisional = provisional;
    cache.observed_trades = observed_trades;
    cache.tick_per_row = tick_per_row;
    cache.composite_ver = composite_ver;

    if (!any_data) {
        cache.levels.clear();
        return nullptr;
    }

    merged.delta = merged.total_buy - merged.total_sell;
    merged.valid = true;

    cache.levels = group_levels(merged, tick_per_row);
    cache.total_volume = merged.total_volume;
    cache.total_buy    = merged.total_buy;
    cache.total_sell   = merged.total_sell;
    cache.delta        = merged.delta;
    cache.high_price   = merged.high_price;
    cache.low_price    = merged.low_price;

    return cache.levels.empty() ? nullptr : &cache;
}

std::vector<FootprintManager::GroupedLevel> FootprintManager::group_levels(
    const CandleFootprint& fp, double tick_per_row) const
{
    if (fp.levels.empty() || !std::isfinite(tick_per_row)) return {};

    // Auto tick_per_row: target ~15-20 rows per candle
    if (tick_per_row <= 0.0) {
        double range = fp.high_price - fp.low_price;
        if (range <= 0.0) range = 1.0;
        tick_per_row = range / 18.0; // ~18 rows target
        // Snap to something reasonable (at least 1 tick)
        if (tick_per_row < 1e-8) tick_per_row = 1e-8;
    }

    // Group raw levels into tick_per_row buckets
    std::unordered_map<int64_t, GroupedLevel> buckets;


    for (const auto& lv : fp.levels) {
        if (!std::isfinite(lv.price) || !std::isfinite(lv.buy_volume) ||
            !std::isfinite(lv.sell_volume) || lv.buy_volume < 0 || lv.sell_volume < 0 ||
            !std::isfinite(lv.total_volume) || lv.total_volume <= 0) continue;
        double index = lv.price / tick_per_row;
        // Correct only floating-point error at an exact grid edge, not prices
        // genuinely inside a bucket (e.g. 0.3 / 0.1 is just below 3).
        const double nearest = std::round(index);
        if (std::abs(index - nearest) <= 4 * std::numeric_limits<double>::epsilon() *
                std::max(1.0, std::abs(index))) index = nearest;
        index = std::floor(index);
        if (!std::isfinite(index) || index <= static_cast<double>(INT64_MIN) ||
            index >= static_cast<double>(INT64_MAX)) continue;
        int64_t bucket_idx = static_cast<int64_t>(index);
        auto& gl = buckets[bucket_idx];
        gl.bucket_index = bucket_idx;
        if (gl.total_volume == 0.0) {
            gl.price_lo  = bucket_idx * tick_per_row;
            gl.price_hi  = gl.price_lo + tick_per_row;
            gl.price_mid = gl.price_lo + tick_per_row * 0.5;
        }
        gl.buy_volume   += lv.buy_volume;
        gl.sell_volume  += lv.sell_volume;
        gl.total_volume += lv.total_volume;
        gl.delta        += lv.delta;
    }

    // Find POC bucket and max volume for imbalance detection
    int64_t poc_bucket = 0;
    double poc_vol = 0.0;
    for (auto& [idx, gl] : buckets) {
        if (gl.total_volume > poc_vol) {
            poc_vol = gl.total_volume;
            poc_bucket = idx;
        }
    }

    // Build sorted output + mark POC and imbalances
    std::vector<GroupedLevel> result;
    result.reserve(buckets.size());
    for (auto& [idx, gl] : buckets) {
        gl.is_poc = (idx == poc_bucket);

        result.push_back(std::move(gl));
    }

    // Sort by price ascending
    std::sort(result.begin(), result.end(),
              [](const GroupedLevel& a, const GroupedLevel& b) {
                  return a.price_mid < b.price_mid;
              });

    // Buy at p compares with sells one grouped row below; sell at p with
    // buys one row above. Missing rows and zero denominators never qualify.
    auto adjacent = [](const GroupedLevel& lo, const GroupedLevel& hi) {
        return lo.bucket_index != INT64_MAX && hi.bucket_index == lo.bucket_index + 1;
    };
    auto qualifies = [this](double numerator, double denominator) {
        return std::isfinite(imbalance_ratio) && imbalance_ratio > 1.0f &&
            std::isfinite(imbalance_min_volume) &&
            numerator > 0.0 && numerator >= std::max(0.0, imbalance_min_volume) &&
            denominator > 0.0 && numerator / denominator >= imbalance_ratio;
    };
    for (size_t i = 0; i < result.size(); ++i) {
        auto& row = result[i];
        if (comparison == Comparison::SamePrice) {
            row.buy_imbalance = qualifies(row.buy_volume, row.sell_volume);
            row.sell_imbalance = qualifies(row.sell_volume, row.buy_volume);
        } else {
            row.buy_imbalance = i > 0 && adjacent(result[i-1], row) &&
                qualifies(row.buy_volume, result[i-1].sell_volume);
            row.sell_imbalance = i+1 < result.size() && adjacent(row, result[i+1]) &&
                qualifies(row.sell_volume, result[i+1].buy_volume);
        }
    }
    if (stacked_levels >= 2) {
        // Linear passes mark the whole maximal run, independently per side.
        for (bool buy : {false, true}) {
            size_t begin = 0;
            while (begin < result.size()) {
                auto flagged = [buy](const GroupedLevel& r) {
                    return buy ? r.buy_imbalance : r.sell_imbalance;
                };
                if (!flagged(result[begin])) { ++begin; continue; }
                size_t end = begin + 1;
                while (end < result.size() && flagged(result[end]) &&
                       adjacent(result[end-1], result[end])) ++end;
                if (end - begin >= static_cast<size_t>(stacked_levels)) {
                    for (size_t j = begin; j < end; ++j) {
                        if (buy) result[j].buy_stack = true;
                        else result[j].sell_stack = true;
                    }
                }
                begin = end;
            }
        }
    }
    return result;
}
