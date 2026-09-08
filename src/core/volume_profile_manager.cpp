#include "volume_profile_manager.h"
#include "../stream_handler.h"
#include "../types/types.h"
#include <pb/messages.pb.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <chrono>
#include <cstdio>

using json = nlohmann::json;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void VolumeProfileManager::request_profile(
    const std::string& symbol,
    int64_t start_ms, int64_t end_ms,
    double tick_per_row,
    StreamManager* stream_mgr)
{
    if (!stream_mgr || !enabled_) return;

    // Debounce: skip if range hasn't changed much
    auto now = now_ms();
    if (now - last_request_time_ms_ < kDebounceMs) return;

    // Check if range changed significantly
    if (last_start_ms_ != 0 && last_end_ms_ != 0) {
        double range = static_cast<double>(last_end_ms_ - last_start_ms_);
        double start_delta = std::abs(static_cast<double>(start_ms - last_start_ms_));
        double end_delta = std::abs(static_cast<double>(end_ms - last_end_ms_));
        if (range > 0 && (start_delta / range) < kRangeChangePct
                      && (end_delta / range) < kRangeChangePct) {
            return;  // Range didn't change enough
        }
    }

    last_request_time_ms_ = now;
    last_start_ms_ = start_ms;
    last_end_ms_ = end_ms;

    // Send JSON request to backend (method + data.pair pattern)
    json req;
    req["method"] = "get_volume_profile";
    req["data"] = {
        {"pair", {
            {"exchange", "binancef"},
            {"symbol", symbol}
        }},
        {"start_time", start_ms},
        {"end_time", end_ms},
        {"tick_per_row", tick_per_row}
    };

    stream_mgr->send_message(req.dump());
}

void VolumeProfileManager::on_profile_response(
    const std::string& symbol,
    const pb::VolumeProfileResponse& resp)
{
    ProfileData& pd = profiles_[symbol];
    pd.poc = resp.poc();
    pd.vah = resp.vah();
    pd.val = resp.val();
    pd.total_volume = resp.total_volume();
    pd.value_area_vol = resp.value_area_volume();
    pd.start_time = resp.start_time();
    pd.end_time = resp.end_time();

    pd.levels.clear();
    pd.levels.reserve(resp.levels_size());

    for (const auto& lvl : resp.levels()) {
        Level l;
        l.price        = lvl.price();
        l.buy_volume   = lvl.buy_volume();
        l.sell_volume  = lvl.sell_volume();
        l.total_volume = lvl.total_volume();
        l.delta        = l.buy_volume - l.sell_volume;
        l.volume_pct   = lvl.volume_percentage();
        l.is_poc       = lvl.is_poc();
        l.in_value_area = lvl.in_value_area();
        pd.levels.push_back(l);
    }

    pd.valid = !pd.levels.empty();
}

const VolumeProfileManager::ProfileData*
VolumeProfileManager::get_profile(const std::string& symbol) const
{
    auto it = profiles_.find(symbol);
    if (it == profiles_.end() || !it->second.valid) return nullptr;
    return &it->second;
}

void VolumeProfileManager::invalidate(const std::string& symbol) {
    auto it = profiles_.find(symbol);
    if (it != profiles_.end()) {
        it->second.valid = false;
    }
    // Reset debounce so next render triggers a fresh request
    last_request_time_ms_ = 0;
    last_start_ms_ = 0;
    last_end_ms_ = 0;
}

void VolumeProfileManager::invalidate_all() {
    for (auto& [sym, pd] : profiles_) {
        pd.valid = false;
    }
    last_request_time_ms_ = 0;
    last_start_ms_ = 0;
    last_end_ms_ = 0;
}
