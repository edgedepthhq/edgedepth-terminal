#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace liquidity_response {
using Json=nlohmann::json;
struct Episode {
    std::string id,side,status;
    int64_t start=0,known=0,updated=0;
    double low=0,high=0,executed=0,added=0,removed=0,detection_executed=0;
    int cycles=0,detection_cycles=0;
};
inline bool visible(const Episode& p,int64_t clock){return p.known>0&&p.known<=clock&&p.updated<=clock;}
struct ActivitySide { double executed=0; int episodes=0,cycles=0; int64_t persistence=0; };
struct ActivityBar { int64_t time=0; std::optional<ActivitySide> bid,ask; };
struct MarkerGroup {
    int64_t candle=0;
    bool bid=false;
    std::vector<size_t> members;
};
// Display grouping only. Every underlying observation remains inspectable.
inline std::vector<MarkerGroup> group_markers(const std::vector<Episode>& episodes,int64_t timeframe_ms){
    std::vector<MarkerGroup> groups;
    if(timeframe_ms<=0)return groups;
    std::map<std::pair<int64_t,bool>,size_t> lookup;
    for(size_t i=0;i<episodes.size();i++){
        const auto& p=episodes[i];if(p.known<=0)continue;
        const auto key=std::make_pair(p.known/timeframe_ms*timeframe_ms,p.side=="bid");
        auto [it,added]=lookup.emplace(key,groups.size());
        if(added)groups.push_back({key.first,key.second,{}});
        groups[it->second].members.push_back(i);
    }
    for(auto& group:groups)std::sort(group.members.begin(),group.members.end(),[&](size_t x,size_t y){return episodes[x].known<episodes[y].known;});
    return groups;
}
// Main-thread-only request ownership, separate from all liquidation state.
class History {
public:
    std::vector<Episode> episodes;
    std::vector<ActivityBar> bars;
    int64_t timeframe=60000;
    Json source,selected_source;
    uint64_t revision=0;
    std::vector<Episode> selected;
    size_t selected_index=0;
    int64_t selected_through=0;
    void select(const std::vector<size_t>& members){
        selected.clear();selected_index=0;selected_through=through;selected_source=source;
        for(auto i:members)if(i<episodes.size())selected.push_back(episodes[i]);
    }
    std::string status="Layer off";
    int64_t through=0;
    bool ready=false;
    History(){instances.insert(this);}
    History(const History&)=delete;
    History& operator=(const History&)=delete;
    ~History(){reset();instances.erase(this);}
    static void invalidate_all(const char* reason){for(auto* h:instances)h->reset(reason);}
    void reset(const char* reason="Waiting for observations"){
        pending.erase(id);id.clear();episodes.clear();bars.clear();source={};ready=false;through=0;status=reason;next=0;selected.clear();selected_source={};selected_through=0;revision++;
    }
    Json poll(const std::string& venue,const std::string& symbol,double tick,int64_t from,int64_t to,bool replay,double steady,bool history=false,int64_t tf=60000){
        if(history!=continuous||tf!=timeframe||venue!=exchange||symbol!=market||replay!=in_replay||to<end||(to==end&&from!=begin)||from>end||tick!=grid){
            reset();continuous=history;timeframe=tf;exchange=venue;market=symbol;in_replay=replay;begin=from;end=to;grid=tick;
        }
        if(venue!="binancef"||(symbol!="btcusdt"&&symbol!="solusdt")){status="Pilot: Binance BTC and SOL only";return {};}
        if(!std::isfinite(tick)||tick<=0||from<=0||to<=from||to-from>(history?86400000:120000)){status=history?"Select up to 24 hours with a known tick size":"Select up to two minutes with a known tick size";return {};}
        if(history&&(tf<60000||tf>86400000||tf%60000!=0)){status="Use one-minute candles or above";return {};}
        if(!id.empty()&&steady-sent>10000){reset("No response; server may not support this layer");next=steady+15000;}
        if(!id.empty()||steady<next)return {};
        exchange=venue;market=symbol;in_replay=replay;begin=from;end=to;grid=tick;
        id="response-"+std::to_string(++serial);pending[id]=this;sent=steady;
        return {{"method","get_liquidity_response"},{"data",{{"request_id",id},{"pair",{{"exchange",venue},{"symbol",symbol}}},{"tick_size",tick},{"from_ms",from},{"to_ms",to},{"history",history},{"timeframe_ms",tf}}}};
    }
    static void receive(const Json& j){
        if(!j.is_object()||!j.contains("request_id")||!j["request_id"].is_string())return;
        auto it=pending.find(j["request_id"].get<std::string>());if(it!=pending.end()){auto* history=it->second;history->accept(j);if(!history->ready){history->selected.clear();history->selected_source={};}}
    }
private:
    inline static std::map<std::string,History*> pending;
    inline static std::set<History*> instances;
    inline static uint64_t serial=0;
    std::string id,exchange,market;
    int64_t begin=0,end=0;
    double grid=0,sent=0,next=0;
    bool in_replay=false,continuous=false;
    static bool number(const Json& j,const char* k){return j.contains(k)&&j[k].is_number()&&std::isfinite(j[k].get<double>());}
    static bool integer(const Json& j,const char* k){return j.contains(k)&&j[k].is_number_integer();}
    static bool text(const Json& j,const char* k){return j.contains(k)&&j[k].is_string();}
    void accept(const Json& j){
        pending.erase(id);id.clear();ready=false;episodes.clear();bars.clear();source={};through=0;next=sent+5000;revision++;
        if(text(j,"error")){selected.clear();status=j["error"].get<std::string>();if(continuous)status=status.find("history_unavailable")!=std::string::npos?"No stored absorption history for this window":"Absorption history unavailable";return;}
        status="Invalid or mismatched evidence response";
        if(!text(j,"exchange")||j["exchange"]!=exchange||!text(j,"symbol")||j["symbol"]!=market||!text(j,"mode")||j["mode"]!=(in_replay?"replay":"live")||!j.contains("result")||!j["result"].is_object())return;
        const auto& r=j["result"];
        if(!text(r,"version")||r["version"]!=(continuous?"absorption.chart.v1":"liquidity_response.v1")||!integer(r,"from_ms")||r["from_ms"]!=begin||!integer(r,"to_ms")||r["to_ms"]!=end||!integer(r,"through_ms")||r["through_ms"].get<int64_t>()>end||r["through_ms"].get<int64_t>()<begin||(!continuous&&(!text(r,"status")||(r["status"]!="observed"&&r["status"]!="incomplete_tail")))||!r.contains("config")||!r["config"].is_object()||!number(r["config"],"tick_size")||r["config"]["tick_size"]!=grid||!r.contains("episodes")||!r["episodes"].is_array()||r["episodes"].size()>4096)return;
        std::vector<ActivityBar> activity;
        if(continuous){
            if(!integer(r,"timeframe_ms")||r["timeframe_ms"]!=timeframe||!text(r,"namespace")||!text(r,"detector_version")||r["detector_version"]!="liquidity_response.v1"||!r.contains("bars")||!r["bars"].is_array()||r["bars"].size()>1441)return;
            int64_t previous=-1;
            for(const auto& row:r["bars"]){
                if(!row.is_object()||!integer(row,"time_ms"))return;
                ActivityBar bar;bar.time=row["time_ms"];
                if(bar.time<=previous||bar.time%timeframe!=0||bar.time<begin/timeframe*timeframe||bar.time>=end)return;
                previous=bar.time;
                for(const char* key:{"bid","ask"}){
                    if(!row.contains(key))return;
                    const auto& side=row[key];
                    if(side.is_null())continue;
                    if(!side.is_object()||!number(side,"executed_usd")||!integer(side,"new_episodes")||!integer(side,"max_detection_cycles")||!integer(side,"max_detection_persistence_ms"))return;
                    ActivitySide value;value.executed=side["executed_usd"];value.episodes=side["new_episodes"];value.cycles=side["max_detection_cycles"];value.persistence=side["max_detection_persistence_ms"];
                    if(value.executed<0||value.episodes<0||value.cycles<0||value.persistence<0)return;
                    if(std::string(key)=="bid")bar.bid=value;else bar.ask=value;
                }
                activity.push_back(bar);
            }
        }
        std::set<std::string> identities;
        std::vector<Episode> result;
        for(const auto& p:r["episodes"]){
            if(!p.is_object()||!text(p,"id")||!text(p,"resting_side")||!text(p,"status")||!integer(p,"started_ms")||!integer(p,"known_ms")||!integer(p,"updated_ms")||!integer(p,"refill_cycles")||!integer(p,"detection_cycles"))return;
            for(const char* field:{"price_low","price_high","executed_usd","added_base","removed_base","detection_executed_usd"})if(!number(p,field))return;
            Episode e;e.id=p["id"];e.side=p["resting_side"];e.status=p["status"];e.start=p["started_ms"];e.known=p["known_ms"];e.updated=p["updated_ms"];e.low=p["price_low"];e.high=p["price_high"];e.executed=p["executed_usd"];e.added=p["added_base"];e.removed=p["removed_base"];e.cycles=p["refill_cycles"];e.detection_cycles=p["detection_cycles"];e.detection_executed=p["detection_executed_usd"];
            if((e.side!="bid"&&e.side!="ask")||e.start<=0||(!continuous&&e.start<begin)||e.updated<e.start||e.updated>end||(e.known!=0&&(e.known<e.start||e.known>e.updated))||e.low<=0||e.high<=e.low||e.executed<0||e.added<0||e.removed<0||e.cycles<0||e.detection_cycles<0||e.detection_cycles>e.cycles||e.detection_executed<0||e.detection_executed>e.executed)return;
            if(!identities.insert(e.id).second||e.updated>r["through_ms"].get<int64_t>())return;
            result.push_back(std::move(e));
        }
        bars=std::move(activity);episodes=std::move(result);through=r["through_ms"];source=j;ready=true;
        if(continuous){status="Recorded activity; missing intervals are unavailable, not zero";return;}
        status=r["status"]=="observed"?"Recorded observations; completeness not certified":"Recording tail incomplete";
    }
};
}
