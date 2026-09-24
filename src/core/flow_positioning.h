#pragma once
#include "core/workspace_settings.h"
#include "types/types.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace flow_positioning {
inline constexpr int64_t max_span_ms=4*60*60000;
inline const char* error_message(const std::string& code) {
    if(code=="pro_required")return "Account access required. Reconnect with a current Pro account token.";
    if(code=="outside_recent_window")return "This server still restricts flow evidence to six hours. Historical evidence needs the updated server.";
    if(code=="outside_history_window")return "This interval is outside your account history access window.";
    if(code=="outside_replay_window")return "This interval is outside the active replay window or ahead of its playhead.";
    if(code=="invalid_window")return "Select 1 minute to 4 hours of complete minutes. Older servers support only 30 minutes.";
    if(code=="busy")return "Evidence service is busy. Retrying shortly.";
    if(code=="timeout")return "Evidence read timed out. Available components remain visible; retry shortly.";
    if(code=="unsupported_source")return "This source is not supported by the connected server.";
    if(code=="observation_limit")return "Source exceeds the observation budget. Select a shorter interval.";
    if(code=="missing_records")return "No stored records in this interval.";
    if(code=="context_changed")return "Replay context changed during the read. Loading the current selection.";
    return "Source temporarily unavailable. Retrying shortly.";
}

struct Bar { int64_t time=0; double open=0, close=0, buy=0, sell=0; };
struct OI { int64_t time=0; double contracts=0; bool recovered=false; }; // zero means unavailable, never a measured zero
struct Liquidation { int64_t time=0; bool buy=false; double usd=0; int64_t count=0; };
struct Evidence {
    int64_t from=0,to=0,retrieved=0;
    std::vector<Bar> bars;
    std::vector<OI> oi;
    std::vector<Liquidation> liquidations;
    bool liquidations_available=true;
    std::string bars_status="available",oi_status="available",liquidations_status="available";
};
struct Assessment {
    double buy=0,sell=0,price_pct=0,oi_pct=0,forced_buy=0,forced_sell=0;
    int64_t first_oi=0,last_oi=0,max_gap=0,report_count=0;
    size_t raw_count=0,recovered_count=0;
    double oi_start=0,oi_end=0;
    int64_t forced_buy_count=0,forced_sell_count=0;
    bool complete_bars=false,usable_oi=false;
    const char* explanation="Waiting for recorded evidence.";
    const char* hypothesis="Insufficient evidence for a positioning interpretation.";
};
inline Assessment assess(const Evidence& e) {
    Assessment a;
    int64_t next=e.from;
    bool contiguous=true;
    for(const auto& b:e.bars) {
        if(b.time!=next)contiguous=false;
        next=b.time+60000; a.buy+=b.buy; a.sell+=b.sell;
    }
    a.complete_bars=contiguous && !e.bars.empty() && next==e.to;
    if(a.complete_bars) a.price_pct=(e.bars.back().close/e.bars.front().open-1)*100;
    double first=0,last=0;
    bool missing=false;
    for(const auto& o:e.oi) {
        if(o.contracts<=0){missing=true;continue;}
        if(a.raw_count==0){first=o.contracts;a.first_oi=o.time;}
        else a.max_gap=std::max(a.max_gap,o.time-a.last_oi);
        ++a.raw_count;if(o.recovered)++a.recovered_count;last=o.contracts;a.last_oi=o.time;
    }
    a.usable_oi=a.raw_count>=2 && !missing && a.first_oi-e.from<=30000 &&
        e.to-a.last_oi<=30000 && a.max_gap<=30000;
    a.oi_start=first;a.oi_end=last;
    if(a.raw_count>=2) a.oi_pct=(last/first-1)*100;
    for(const auto& l:e.liquidations){(l.buy?a.forced_buy:a.forced_sell)+=l.usd;a.report_count+=l.count;(l.buy?a.forced_buy_count:a.forced_sell_count)+=l.count;}
    if(!a.complete_bars){a.explanation="Incomplete minute coverage. Select a shorter recorded move.";return a;}
    const double total=a.buy+a.sell;
    if(total<=0){a.explanation="No recorded aggression in these minutes.";return a;}
    a.explanation=a.price_pct>0 ? "Price rose; inspect aggression and contract changes below."
        : a.price_pct<0 ? "Price fell; inspect aggression and contract changes below."
        : "Price ended unchanged; inspect the two-sided flow.";
    if(!a.usable_oi){a.hypothesis=a.raw_count<2 ? "OI unavailable. No complete positioning assessment."
        : "OI has missing or stale observations. Positioning interpretation withheld.";return a;}
    // No universal materiality cutoff: display measured size without mapping a
    // tiny signed change to participant behavior or a covering/unwinding claim.
    a.hypothesis="OI is net open contracts. Its measured change does not identify openings, closings or the cause of this move.";
    return a;
}

// Main-thread-only, one outstanding read per chart, bounded to four hours.
// Request IDs prevent stale responses after seeks, market changes and closure.
class History {
public:
    Evidence evidence;
    Assessment assessment;
    nlohmann::json source;
    std::string status="Select 1 minute to 4 hours.";
    bool ready=false;
    History()=default;
    History(const History&)=delete;
    History& operator=(const History&)=delete;
    ~History(){reset();}
    void reset(){pending.erase(id);id.clear();ready=false;evidence={};assessment={};source={};next_read=0;}
    nlohmann::json request(const Terminal::Pair& pair,int64_t from,int64_t to,bool replay,double steady) {
        if(from!=wanted_from||to!=wanted_to||replay!=wanted_replay||pair.exchange!=market.exchange||pair.symbol!=market.symbol){
            reset();wanted_from=from;wanted_to=to;wanted_replay=replay;market=pair;status="Loading recorded evidence...";
        }
        if(from<=0||to<=from||to-from>max_span_ms||from%60000||to%60000){status="Select 1 minute to 4 hours of complete minutes.";ready=false;return {};}
        if(!id.empty()&&steady-sent>10000){pending.erase(id);id.clear();status="No evidence response within 10 seconds. The server may be unsupported or the connection timed out.";next_read=steady+30000;}
        if(!id.empty()||steady<next_read)return {};
        id="flow-"+std::to_string(++serial);pending[id]=this;sent=steady;
        return {{"method","get_flow_positioning"},{"data",{{"request_id",id},{"pair",{{"exchange",pair.exchange},{"symbol",pair.symbol}}},{"from_ms",from},{"to_ms",to}}}};
    }
    static void receive(const nlohmann::json& j){
        auto key=j.find("request_id");if(key==j.end()||!key->is_string())return;
        auto it=pending.find(key->get<std::string>());if(it!=pending.end())it->second->accept(j);
    }
private:
    std::string id;
    Terminal::Pair market;
    int64_t wanted_from=0,wanted_to=0;
    bool wanted_replay=false;
    double sent=0,next_read=0;
    inline static uint64_t serial=0;
    inline static std::map<std::string,History*> pending;
    static bool number(const nlohmann::json& o,const char* key,double& out,double min=0) {
        auto it=o.find(key);if(it==o.end()||!it->is_number())return false;
        out=it->get<double>();return std::isfinite(out)&&out>=min&&out<=9e15;
    }
    static bool timestamp(const nlohmann::json& o,const char* key,int64_t& out){
        double n=0;if(!number(o,key,n,1)||std::floor(n)!=n)return false;out=static_cast<int64_t>(n);return true;
    }
    void accept(const nlohmann::json& j){
        pending.erase(id);id.clear();next_read=sent+30000;
        auto fail=[&](){ready=false;status="Evidence response is malformed or belongs to a different scope.";next_read=sent+30000;};
        if(j.contains("error")){
            if(!j["error"].is_string()){fail();return;}
            const auto code=j["error"].get<std::string>();
            ready=false;status=error_message(code);next_read=sent+(code=="busy"?3000:30000);return;
        }
        for(const char* key:{"exchange","symbol","mode"})if(!j.contains(key)||!j[key].is_string()){fail();return;}
        if(j["exchange"]!=market.exchange||j["symbol"]!=market.symbol||j["mode"]!=(wanted_replay?"replay":"live")){fail();return;}
        Evidence e;
        if(!timestamp(j,"from_ms",e.from)||!timestamp(j,"to_ms",e.to)||!timestamp(j,"retrieved_at_ms",e.retrieved)||e.from!=wanted_from||e.to!=wanted_to){fail();return;}
        for(const char* key:{"bars","oi","liquidations"})if(!j.contains(key)||!j[key].is_array()){fail();return;}
        if(j["bars"].size()>240||j["oi"].size()>8192||j["liquidations"].size()>480){fail();return;}
        for(const auto& row:j["bars"]){Bar b;if(!row.is_object()||!timestamp(row,"time",b.time)||b.time<e.from||b.time>=e.to||b.time%60000||(!e.bars.empty()&&b.time<=e.bars.back().time)||!number(row,"open",b.open,1e-12)||!number(row,"close",b.close,1e-12)||!number(row,"buy",b.buy)||!number(row,"sell",b.sell)){fail();return;}e.bars.push_back(b);}
        for(const auto& row:j["oi"]){OI o;if(!row.is_object()||!timestamp(row,"time",o.time)||o.time<e.from||o.time>e.to||(!e.oi.empty()&&o.time<=e.oi.back().time)||!row.contains("contracts")){fail();return;}if(!row["contracts"].is_null()&&!number(row,"contracts",o.contracts,1e-12)){fail();return;}
            if(row["contracts"].is_null()&&row.contains("recovered_contracts")){
                double usd=0,mark=0;
                if((!row.contains("source")||!row["source"].is_string()||row["source"]!="stored_usd_mark")||!number(row,"recovered_contracts",o.contracts,1e-12)||!number(row,"stored_oi_usd",usd,1e-12)||!number(row,"stored_mark_price",mark,1e-12)||std::abs(usd/mark-o.contracts)>std::max(1e-9,o.contracts*1e-10)){fail();return;}
                o.recovered=true;
            }
            e.oi.push_back(o);}
        for(const auto& row:j["liquidations"]){Liquidation l;if(!row.is_object()||!timestamp(row,"time",l.time)||l.time<e.from||l.time>=e.to||l.time%60000||!row.contains("buy")||!row["buy"].is_boolean()||!number(row,"usd",l.usd)||!timestamp(row,"count",l.count)){fail();return;}l.buy=row["buy"].get<bool>();e.liquidations.push_back(l);}
        if(j.contains("components")){
            const auto& c=j["components"];
            if(!c.is_object()){fail();return;}
            for(const char* key:{"bars","oi","liquidations"})if(!c.contains(key)||!c[key].is_string()){fail();return;}
            e.bars_status=c["bars"].get<std::string>();e.oi_status=c["oi"].get<std::string>();e.liquidations_status=c["liquidations"].get<std::string>();
            e.liquidations_available=e.liquidations_status=="available";
        }
        evidence=std::move(e);assessment=assess(evidence);source=j;source.erase("request_id");ready=true;status="Recorded minute evidence";
    }
};
}
