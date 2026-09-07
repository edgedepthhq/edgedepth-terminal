#include "footprint_manager.h"
#include "../stream_handler.h"
#include "../types/types.h"
#include <pb/messages.pb.h>
#include <zstd.h>
#include <algorithm>
#include <nlohmann/json.hpp>

void FootprintManager::request_history(
    const Terminal::Pair& pair, int64_t start_ms, int64_t end_ms,
    StreamManager* sm)
{
    if (!sm) return;

    // Don't fire new requests while one is already in flight
    if (loading_) return;

    // Skip if requested range is already covered by what we have
    if (market_key(pair.exchange, pair.symbol) == last_symbol_ &&
        start_ms >= last_start_ && end_ms <= last_end_) {
        return;
    }

    // Extend the covered range (union of old + new)
    if (market_key(pair.exchange, pair.symbol) == last_symbol_ && last_start_ > 0) {
        start_ms = std::min(start_ms, last_start_);
        end_ms   = std::max(end_ms, last_end_);
    }

    last_symbol_ = market_key(pair.exchange, pair.symbol);
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

    store_footprint(symbol, std::move(fp));
}
