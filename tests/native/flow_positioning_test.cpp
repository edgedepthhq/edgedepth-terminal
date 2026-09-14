#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/flow_positioning.h"
#include <cassert>
#include <iostream>
using namespace flow_positioning;
constexpr int64_t t0=1789254420000;
Evidence complete(){
    Evidence e;e.from=t0;e.to=t0+120000;
    e.bars={{t0,.28830,.31554,12962147,8134177},{t0+60000,.31552,.32450,11041955,9007598}};
    for(int i=0;i<=12;++i)e.oi.push_back({t0+i*10000,1000.0-i});
    e.liquidations={{t0,true,281303.14,20},{t0,false,290.86,1}};
    return e;
}
nlohmann::json reply(const nlohmann::json& request){
    return {{"type","flow_positioning"},{"request_id",request["data"]["request_id"]},{"exchange","binancef"},{"symbol","lskusdt"},{"mode","replay"},{"from_ms",t0},{"to_ms",t0+120000},{"retrieved_at_ms",t0+120000},
    {"bars",nlohmann::json::array({{{"time",t0},{"open",.28830},{"close",.31554},{"buy",12962147},{"sell",8134177}},{{"time",t0+60000},{"open",.31552},{"close",.32450},{"buy",11041955},{"sell",9007598}}})},
    {"oi",nlohmann::json::array({{{"time",t0},{"contracts",nullptr}},{{"time",t0+120000},{"contracts",nullptr}}})},{"liquidations",nlohmann::json::array()}};
}
int main(){
    auto e=complete();auto a=assess(e);
    assert(a.complete_bars&&a.usable_oi&&a.buy==24004102&&a.sell==17141775);
    assert(std::abs(a.price_pct-12.556364897676042)<1e-9);
    assert(std::string(a.hypothesis).find("Consistent with short covering")!=std::string::npos);
    // OI retires contracts without converting price-driven USD changes.
    for(auto& o:e.oi)o.contracts=1000;
    assert(assess(e).oi_pct==0&&std::string(assess(e).hypothesis).find("Mixed")!=std::string::npos);
    e=complete();e.oi[5].contracts=0;assert(!assess(e).usable_oi);
    e=complete();e.oi.erase(e.oi.begin()+1,e.oi.begin()+6);assert(!assess(e).usable_oi);
    e=complete();e.oi.erase(e.oi.begin(),e.oi.begin()+4);assert(!assess(e).usable_oi);
    e=complete();e.oi.resize(8);assert(!assess(e).usable_oi);
    e=complete();e.bars.erase(e.bars.begin());assert(!assess(e).complete_bars);
    e=complete();for(auto& b:e.bars)b.buy=b.sell;assert(std::string(assess(e).hypothesis).find("Mixed")!=std::string::npos);
    e=complete();for(auto& b:e.bars){b.close=b.open*.9;std::swap(b.buy,b.sell);}assert(std::string(assess(e).hypothesis).find("long unwinding")!=std::string::npos);
    History h;Terminal::Pair pair{"binancef","lskusdt"};
    auto request=h.request(pair,t0,t0+120000,true,0);assert(!request.empty());
    auto good=reply(request);History::receive(good);assert(h.ready&&!h.assessment.usable_oi&&h.assessment.raw_count==0);
    assert(h.evidence.oi.size()==2); // NULL observations preserved, never zero-filled into measured data.
    request=h.request(pair,t0,t0+120000,true,6000);auto wrong=reply(request);wrong["mode"]="live";History::receive(wrong);assert(!h.ready);
    h.reset();request=h.request(pair,t0,t0+120000,true,40000);auto old=reply(request);
    h.request(pair,t0+60000,t0+120000,true,41000);History::receive(old);assert(!h.ready); // rewind/selection generation
    h.reset();request=h.request(pair,t0,t0+120000,true,80000);auto future=reply(request);future["oi"][1]["time"]=t0+120001;History::receive(future);assert(!h.ready);
    h.reset();request=h.request(pair,t0,t0+120000,true,120000);auto malformed=reply(request);malformed["bars"][0]["buy"]="large";History::receive(malformed);assert(!h.ready);
    h.reset();assert(h.request(pair,t0,t0+31*60000,true,160000).empty());
    std::cout<<"Flow evidence: alignment, raw OI, stale/missing, hypotheses, scope and stale callbacks pass\n";
}
