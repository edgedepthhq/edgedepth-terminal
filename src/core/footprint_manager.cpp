#include "footprint_manager.h"
#include "../stream_handler.h"
#include "../types/types.h"
#include <pb/messages.pb.h>

#include <zstd.h>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

// ═══════════════════════════════════════════════════════════════════════════════
// FootprintManager implementation
// ═══════════════════════════════════════════════════════════════════════════════

void FootprintManager::request_history(
    const Terminal::Pair& pair, int64_t start_ms, int64_t end_ms,
    StreamManager* sm)
{
    if (!sm) return;

    // Don't fire new requests while one is already in flight
    if (loading_) return;

    // Skip if requested range is already covered by what we have
    if (pair.symbol == last_symbol_ &&
        start_ms >= last_start_ && end_ms <= last_end_) {
        return;
    }

    // Extend the covered range (union of old + new)
    if (pair.symbol == last_symbol_ && last_start_ > 0) {
        start_ms = std::min(start_ms, last_start_);
        end_ms   = std::max(end_ms, last_end_);
    }

    last_symbol_ = pair.symbol;
    last_start_ = start_ms;
    last_end_ = end_ms;
    loading_ = true;

    nlohmann::json req;
    req["method"] = "get_footprint_history";
    req["data"]["pair"]["exchange"] = pair.exchange;
    req["data"]["pair"]["symbol"] = pair.symbol;
    req["data"]["start_time"] = start_ms;
    req["data"]["end_time"] = end_ms;

    sm->send_message(req.dump());
}

void FootprintManager::on_tick_volume_update(
    const std::string& symbol, const pb::TickVolumeUpdate& update)
{
    // Sentinel: timestamp_ms=0 signals end of historical batch
    if (update.timestamp_ms() == 0) {
        loading_ = false;
        return;
    }

    int64_t start_time = update.start_time();
    if (start_time == 0) start_time = update.timestamp_ms();

    CandleFootprint fp;
    fp.start_time   = start_time;
    fp.end_time     = update.end_time();
    fp.total_volume = update.total_volume();
    fp.total_buy    = update.buy_volume();
    fp.total_sell   = update.sell_volume();
    fp.delta        = fp.total_buy - fp.total_sell;
    fp.poc          = update.poc();
    fp.high_price   = update.high_price();
    fp.low_price    = update.low_price();

    // Decompress levels_data (zstd → protobuf TickVolumeLevels)
    const std::string& blob = update.levels_data();
    if (!blob.empty()) {
        // Try zstd decompression first
        unsigned long long frame_size = ZSTD_getFrameContentSize(blob.data(), blob.size());
        bool is_zstd = (frame_size != ZSTD_CONTENTSIZE_ERROR &&
                        frame_size != ZSTD_CONTENTSIZE_UNKNOWN);

        const uint8_t* proto_data = nullptr;
        size_t proto_size = 0;
        std::vector<uint8_t> decompressed;

        if (is_zstd && frame_size > 0 && frame_size < 10 * 1024 * 1024) {
            decompressed.resize(static_cast<size_t>(frame_size));
            size_t result = ZSTD_decompress(decompressed.data(), static_cast<size_t>(frame_size),
                                            blob.data(), blob.size());
            if (!ZSTD_isError(result)) {
                proto_data = decompressed.data();
                proto_size = result;
            }
        }

        // Fallback: try as raw protobuf
        if (!proto_data) {
            proto_data = reinterpret_cast<const uint8_t*>(blob.data());
            proto_size = blob.size();
        }

        pb::TickVolumeLevels levels_pb;
        if (levels_pb.ParseFromArray(proto_data, static_cast<int>(proto_size))) {
            fp.levels.reserve(levels_pb.levels_size());
            for (const auto& lv : levels_pb.levels()) {
                Level level;
                level.price        = lv.price();
                level.buy_volume   = lv.buy_volume();
                level.sell_volume  = lv.sell_volume();
                level.total_volume = lv.total_volume();
                level.trade_count  = lv.trade_count();
                level.delta        = lv.buy_volume() - lv.sell_volume();
                fp.levels.push_back(level);
            }
            // Sort by price ascending
            std::sort(fp.levels.begin(), fp.levels.end(),
                      [](const Level& a, const Level& b) { return a.price < b.price; });
        }
    }

    fp.valid = !fp.levels.empty();
    ++data_version_;
    fp.version = data_version_;
    data_[symbol][start_time] = std::move(fp);
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
    data_.erase(symbol);
    merged_cache_.erase(symbol);
    if (last_symbol_ == symbol) {
        last_start_ = 0;
        last_end_ = 0;
        loading_ = false;
    }
}

const FootprintManager::MergedCache* FootprintManager::get_merged_grouped(
    const std::string& symbol, int64_t candle_ts,
    int64_t tf_sec, double tick_per_row)
{
    auto& sym_cache = merged_cache_[symbol];
    auto it = sym_cache.find(candle_ts);

    // Fast path: cache entry exists with matching tick_per_row.
    // Check composite version to see if underlying data changed.
    if (it != sym_cache.end() && it->second.tick_per_row == tick_per_row) {
        // Compute composite version from constituent 1m buckets
        const int buckets = std::max(1, static_cast<int>(tf_sec / 60));
        int64_t base_ms = (candle_ts / 60000) * 60000;
        uint64_t composite_ver = 0;
        for (int b = 0; b < buckets; ++b) {
            const auto* sub = get_footprint(symbol, base_ms + b * 60000);
            if (sub) composite_ver += sub->version;
        }
        if (it->second.composite_ver == composite_ver) {
            return it->second.levels.empty() ? nullptr : &it->second;
        }
    }

    // Cache miss - need to merge and group
    const int buckets = std::max(1, static_cast<int>(tf_sec / 60));
    int64_t base_ms = (candle_ts / 60000) * 60000;

    CandleFootprint merged;
    uint64_t composite_ver = 0;
    bool any_data = false;
    for (int b = 0; b < buckets; ++b) {
        const auto* sub = get_footprint(symbol, base_ms + b * 60000);
        if (!sub) continue;
        any_data = true;
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
    if (fp.levels.empty()) return {};

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
    double max_total = 0.0;

    for (const auto& lv : fp.levels) {
        int64_t bucket_idx = static_cast<int64_t>(std::floor(lv.price / tick_per_row));
        auto& gl = buckets[bucket_idx];
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
        if (gl.total_volume > max_total) {
            max_total = gl.total_volume;
        }
    }

    // Build sorted output + mark POC and imbalances
    std::vector<GroupedLevel> result;
    result.reserve(buckets.size());
    for (auto& [idx, gl] : buckets) {
        gl.is_poc = (idx == poc_bucket);

        // Imbalance detection: one side > ratio × other side
        double buy = gl.buy_volume;
        double sell = gl.sell_volume;
        if (sell > 0.0 && buy / sell >= imbalance_ratio) {
            gl.is_imbalance = true;
            gl.buy_dominant = true;
        } else if (buy > 0.0 && sell / buy >= imbalance_ratio) {
            gl.is_imbalance = true;
            gl.buy_dominant = false;
        }

        result.push_back(std::move(gl));
    }

    // Sort by price ascending
    std::sort(result.begin(), result.end(),
              [](const GroupedLevel& a, const GroupedLevel& b) {
                  return a.price_mid < b.price_mid;
              });

    return result;
}
