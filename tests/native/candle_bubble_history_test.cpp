#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/candle_bubble_history.h"
#include <cassert>
#include <iostream>
using Json = nlohmann::json;
constexpr int64_t t0 = CandleBubbleHistory::tile_ms * 100;
constexpr int64_t now = t0 + CandleBubbleHistory::tile_ms + CandleBubbleHistory::settle_ms;
const Terminal::Pair pair{"binancef","btcusdt"};
Json request(CandleBubbleHistory& h,double clock=1000) {
    auto first = h.request(pair,t0,t0+60000,now,clock);
    if (!first.is_null()) return first;
    return h.request(pair,t0,t0+60000,now,clock+400);
}
Json response(const Json& req) {
    Json mins = Json::array();
    for (int64_t ms=t0;ms<t0+CandleBubbleHistory::tile_ms;ms+=60000)
        mins.push_back({{"start_ms",ms},{"observed",0},{"prints",Json::array()}});
    mins[0]["observed"]=1;
    mins[0]["prints"].push_back({{"timestamp_ms",t0+1},{"price",100},{"qty",2},{"is_buy",true},{"id","9007199254740993"}});
    return {{"request_id",req["data"]["request_id"]},{"exchange","binancef"},{"symbol","btcusdt"},
        {"result",{{"from_ms",t0},{"to_ms",t0+CandleBubbleHistory::tile_ms},{"auto_floor",100},
                   {"per_minute",16},{"minutes",mins}}}};
}
int main() {
    CandleBubbleHistory h;
    auto req=request(h);assert(h.loading());
    assert(h.request(pair,t0,t0+60000,now,1800).is_null());
    auto result=response(req);CandleBubbleHistory::receive(result);
    assert(!h.loading() && h.error.empty() && h.auto_floor==100);
    assert(h.contains(9007199254740993LL));
    int loaded,observed;h.coverage(t0,t0+900000,loaded,observed);assert(loaded==15 && observed==1);
    assert(h.request(pair,t0,t0+60000,now,5000).is_null()); // cached pan
    std::vector<const Terminal::Trade*> visible;
    h.append_visible(visible,t0,t0+60000);assert(visible.size()==1);
    h.reset();CandleBubbleHistory::receive(result);assert(h.tiles.empty()); // stale seek/reset
    req=request(h,6000);result=response(req);result["symbol"]="ethusdt";
    CandleBubbleHistory::receive(result);assert(h.tiles.empty() && !h.error.empty());
    h.reset();req=request(h,10000);result=response(req);result["result"]["minutes"][0]["prints"][0]["id"]="9.5";
    CandleBubbleHistory::receive(result);assert(h.tiles.empty() && !h.error.empty());
    h.reset();req=request(h,14000);h.request(pair,t0,t0+60000,now,27000);
    CandleBubbleHistory::receive(response(req));assert(h.tiles.empty() && !h.error.empty()); // late timeout
    h.reset();req=request(h,60000);result=response(req);CandleBubbleHistory::receive(result);
    // Expiring a tile also expires its duplicate-suppression IDs.
    h.request(pair,t0,t0+60000,now+CandleBubbleHistory::lookback_ms,65000);
    assert(h.tiles.empty() && !h.contains(9007199254740993LL));
    // One request loads four sections, cached independently for later pans.
    h.reset();
    const auto end=t0+4*CandleBubbleHistory::tile_ms;
    h.request(pair,t0,end,end+CandleBubbleHistory::settle_ms,70000);
    req=h.request(pair,t0,end,end+CandleBubbleHistory::settle_ms,70400);
    assert(req["data"]["from_ms"]==t0 && req["data"]["to_ms"]==end);
    result=response(req);result["result"]["to_ms"]=end;
    for(int64_t ms=t0+CandleBubbleHistory::tile_ms;ms<end;ms+=60000)
        result["result"]["minutes"].push_back({{"start_ms",ms},{"observed",0},{"prints",Json::array()}});
    CandleBubbleHistory::receive(result);
    assert(h.error.empty() && h.tiles.size()==4 && h.size_reference==200);
    assert(h.request(pair,t0,end,end+CandleBubbleHistory::settle_ms,71000).is_null());
    h.coverage(t0,end,loaded,observed);assert(loaded==60 && observed==1);
    std::cout << "PASS: cache, exact IDs, bounds, empty minutes, stale responses and expiry\n";
}
