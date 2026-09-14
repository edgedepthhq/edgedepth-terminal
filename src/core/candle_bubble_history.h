#pragma once
#include "types/types.h"
#include "core/workspace_settings.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <string>
#include <unordered_set>

// Main-thread display cache only. Historical records never enter live candle,
// volume, alerts or RT handlers. Request identity owns callback lifetime.
class CandleBubbleHistory {
public:
    static constexpr int64_t tile_ms = 15 * 60 * 1000;
    static constexpr int64_t lookback_ms = 6 * 60 * 60 * 1000;
    static constexpr int64_t settle_ms = 2 * 60 * 1000;
    struct Tile {
        int64_t end = 0;
        std::vector<Terminal::Trade> prints;
        std::vector<int64_t> observed_minutes;
    };
    std::map<int64_t, Tile> tiles;
    double auto_floor = 0;
    double size_reference = 0;
    std::string error;
    bool loading() const { return !pending_id_.empty(); }
    ~CandleBubbleHistory() { reset(); }
    CandleBubbleHistory() = default;
    CandleBubbleHistory(const CandleBubbleHistory&) = delete;
    CandleBubbleHistory& operator=(const CandleBubbleHistory&) = delete;
    void reset() {
        pending_.erase(pending_id_); pending_id_.clear(); tiles.clear(); ids_.clear();
        error.clear(); auto_floor = 0; size_reference = 0; retry_at_ = 0;
    }
    // Empty JSON means no work. Newest sections load first; a settled viewport
    // and one outstanding request prevent pan/zoom from flooding the socket.
    nlohmann::json request(const Terminal::Pair& pair, int64_t from, int64_t to,
                           int64_t now_ms, double steady_ms) {
        if (loading() && steady_ms - sent_at_ > 12000) {
            pending_.erase(pending_id_); pending_id_.clear();
            error = "History unavailable on this connection"; retry_at_ = steady_ms + 30000;
        }
        const int64_t earliest = ((now_ms - lookback_ms + tile_ms - 1) / tile_ms) * tile_ms;
        const int64_t latest = (now_ms - settle_ms) / 60000 * 60000;
        while (!tiles.empty() && tiles.begin()->first < earliest) {
            for (const auto& t : tiles.begin()->second.prints) ids_.erase(t.agg_trade_id);
            tiles.erase(tiles.begin());
        }
        from = std::max(from, earliest); to = std::min(to, latest);
        const int64_t first = from / tile_ms * tile_ms;
        const int64_t last = to > from ? (to - 1) / tile_ms * tile_ms : first - tile_ms;
        if (first != view_first_ || last != view_last_) {
            view_first_ = first; view_last_ = last; changed_at_ = steady_ms;
        }
        if (loading() || steady_ms < retry_at_ || steady_ms - changed_at_ < 350 || to <= from) return {};
        for (int64_t start = last; start >= first; start -= tile_ms) {
            const int64_t end = std::min(start + tile_ms, latest);
            const auto it = tiles.find(start);
            if (it != tiles.end() && it->second.end >= end) continue;
            // Bundle up to four adjacent missing sections, newest first.
            int64_t batch_start=start;
            for(int n=1;n<4 && batch_start-tile_ms>=first;++n) {
                if(tiles.contains(batch_start-tile_ms)) break;
                batch_start-=tile_ms;
            }
            pending_from_ = batch_start; pending_to_ = end; pair_ = pair;
            pending_id_ = "candle-bubbles-" + std::to_string(++serial_);
            pending_[pending_id_] = this; sent_at_ = steady_ms; error.clear();
            return {{"method","get_candle_bubbles"},{"data",{{"request_id",pending_id_},
                {"from_ms",batch_start},{"to_ms",end},{"pair",{{"exchange",pair.exchange},{"symbol",pair.symbol}}}}}};
        }
        return {};
    }
    static void receive(const nlohmann::json& j) {
        if (!j.is_object() || !j.contains("request_id") || !j["request_id"].is_string()) return;
        const auto it = pending_.find(j["request_id"].get<std::string>());
        if (it != pending_.end()) it->second->accept(j);
    }
    void append_visible(std::vector<const Terminal::Trade*>& out, int64_t from, int64_t to) const {
        for (const auto& [start, tile] : tiles) {
            if (tile.end <= from || start > to) continue;
            for (const auto& t : tile.prints) if (t.timestamp_ms >= from && t.timestamp_ms <= to) out.push_back(&t);
        }
    }
    bool contains(int64_t id) const {
        if (id <= 0) return false;
        return ids_.contains(id);
    }
    void coverage(int64_t from, int64_t to, int& loaded, int& observed) const {
        loaded = observed = 0;
        for (const auto& [start, tile] : tiles) {
            const int64_t a = std::max(start,from), b = std::min(tile.end,to);
            if (b <= a) continue;
            loaded += int((b - a + 59999) / 60000);
            for (auto minute : tile.observed_minutes) if (minute + 60000 > a && minute < b) ++observed;
        }
    }
private:
    inline static uint64_t serial_ = 0;
    inline static std::map<std::string,CandleBubbleHistory*> pending_;
    std::unordered_set<int64_t> ids_;
    std::string pending_id_;
    Terminal::Pair pair_;
    int64_t pending_from_ = 0, pending_to_ = 0, view_first_ = 0, view_last_ = 0;
    double sent_at_ = 0, retry_at_ = 0, changed_at_ = 0;
    void accept(const nlohmann::json& j) {
        pending_.erase(pending_id_); pending_id_.clear(); retry_at_ = sent_at_ + 250;
        if (j.contains("error")) {
            error = "History unavailable; live prints remain visible";
            retry_at_ = sent_at_ + (j["error"] == "busy" ? 1500 : 30000); return;
        }
        auto fail = [&] { error = "Invalid historical trade response"; retry_at_ = sent_at_ + 30000; };
        if (!j.contains("exchange") || !j["exchange"].is_string() || j["exchange"] != pair_.exchange ||
            !j.contains("symbol") || !j["symbol"].is_string() || j["symbol"] != pair_.symbol) { fail(); return; }
        const auto& result = workspace::object(j,"result");
        int64_t from = 0, to = 0; int cap = 0; double floor = 0;
        workspace::read(result,"from_ms",from,1,9e15); workspace::read(result,"to_ms",to,1,9e15);
        workspace::read(result,"per_minute",cap,16,16); workspace::read(result,"auto_floor",floor,0,1e20);
        const auto mins = result.find("minutes");
        if (from != pending_from_ || to != pending_to_ || cap != 16 || mins == result.end() ||
            !mins->is_array() || mins->size() != size_t((to-from)/60000)) { fail(); return; }
        Tile tile; tile.end = to;
        std::unordered_set<int64_t> ids;
        int64_t minute = from;
        for (const auto& m : *mins) {
            if (!m.is_object()) {fail();return;}
            int64_t start = 0; int observed = -1;
            workspace::read(m,"start_ms",start,1,9e15); workspace::read(m,"observed",observed,0,2000000);
            const auto prints = m.find("prints");
            if (start != minute || observed < 0 || prints == m.end() || !prints->is_array() ||
                prints->size() > 16 || prints->size() > size_t(observed)) {fail();return;}
            if (observed > 0) tile.observed_minutes.push_back(start);
            for (const auto& p : *prints) {
                if (!p.is_object()) {fail();return;}
                Terminal::Trade t{};
                workspace::read(p,"timestamp_ms",t.timestamp_ms,double(start),double(start+59999));
                workspace::read(p,"price",t.price,0,1e18); workspace::read(p,"qty",t.qty,0,1e18);
                if (!p.contains("is_buy") || !p["is_buy"].is_boolean() || !p.contains("id") || !p["id"].is_string()) {fail();return;}
                t.is_buy = p["is_buy"].get<bool>();
                const auto& id = p["id"].get_ref<const std::string&>();
                const auto parsed = std::from_chars(id.data(),id.data()+id.size(),t.agg_trade_id);
                if (parsed.ec != std::errc{} || parsed.ptr != id.data()+id.size() || t.agg_trade_id <= 0 ||
                    t.timestamp_ms < start || t.price <= 0 || t.qty <= 0 || !std::isfinite(t.price*t.qty) || !ids.insert(t.agg_trade_id).second) {fail();return;}
                tile.prints.push_back(t);
            }
            minute += 60000;
        }
        std::sort(tile.prints.begin(),tile.prints.end(),[](const auto& a,const auto& b){return a.timestamp_ms<b.timestamp_ms;});
        // Cache each original 15-minute section independently so overlapping
        // viewport requests reuse it and expiration preserves identity joins.
        for(int64_t start=from;start<to;start+=tile_ms) {
            Tile part; part.end=std::min(start+tile_ms,to);
            for(const auto& t:tile.prints) if(t.timestamp_ms>=start && t.timestamp_ms<part.end) part.prints.push_back(t);
            for(auto m:tile.observed_minutes) if(m>=start && m<part.end) part.observed_minutes.push_back(m);
            tiles[start]=std::move(part);
        }
        ids_.clear();
        for (const auto& [_, cached] : tiles)
            for (const auto& t : cached.prints) ids_.insert(t.agg_trade_id);
        // Calibrate from the selected large-print distribution, not the tiny-print
        // visibility floor. P90 lands at ~9px; orders of magnitude remain distinct.
        if (!(size_reference > 0) && !tile.prints.empty()) {
            std::vector<double> values;
            for (const auto& t:tile.prints) values.push_back(t.price*t.qty);
            std::sort(values.begin(),values.end());
            size_reference=values[(values.size()-1)*9/10];
        }
        // No arrival or pan can replace the scale of already displayed prints.
        if (!(auto_floor > 0) && floor > 0) auto_floor = floor;
        error.clear();
    }
};
