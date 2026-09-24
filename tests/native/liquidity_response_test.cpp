#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/liquidity_response.h"
#include <cassert>
using namespace liquidity_response;
Json reply(const Json& q){return {{"request_id",q["data"]["request_id"]},{"exchange","binancef"},{"symbol","btcusdt"},{"mode","replay"},{"result",{{"version","liquidity_response.v1"},{"from_ms",100000},{"to_ms",110000},{"through_ms",109000},{"status","observed"},{"config",{{"tick_size",.1}}},{"episodes",Json::array({{{"id","a"},{"resting_side","bid"},{"status","observed"},{"started_ms",100100},{"known_ms",104000},{"updated_ms",109000},{"price_low",100},{"price_high",101},{"executed_usd",20000},{"added_base",100},{"removed_base",100},{"refill_cycles",4},{"detection_cycles",3},{"detection_executed_usd",15000}}})}}}};}
int main(){
 History h;auto q=h.poll("binancef","btcusdt",.1,100000,110000,true,0);auto good=reply(q);History::receive(good);assert(h.ready&&h.episodes.size()==1);assert(!visible(h.episodes[0],105000));assert(visible(h.episodes[0],109000));
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,6000);auto old=reply(q);h.reset();History::receive(old);assert(!h.ready);
 for(int which=0;which<5;which++){q=h.poll("binancef","btcusdt",.1,100000,110000,true,12000+which*6000);auto bad=reply(q);if(which==0)bad["symbol"]="solusdt";if(which==1)bad["mode"]="live";if(which==2)bad["result"]["episodes"][0]["updated_ms"]=120000;if(which==3)bad["result"]["episodes"][0]["executed_usd"]="bad";if(which==4)bad["result"]["config"]["tick_size"]=1;History::receive(bad);assert(!h.ready);h.reset();}
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,50000);old=reply(q);h.poll("binancef","solusdt",.01,100000,110000,true,50001);History::receive(old);assert(!h.ready);
 h.reset();assert(h.poll("bybit","BTCUSDT",.1,100000,110000,false,60000).is_null());
 // A moving clock preserves in-flight ownership; a rewind destroys it.
 h.reset();q=h.poll("binancef","btcusdt",.1,100000,110000,true,70000);
 assert(h.poll("binancef","btcusdt",.1,100000,115000,true,70001).is_null());
 History::receive(reply(q));assert(h.ready);
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,76000);
 auto rewind=h.poll("binancef","btcusdt",.1,100000,105000,true,76001);
 History::receive(reply(q));assert(!h.ready&&!rewind.is_null());
 h.reset();q=h.poll("binancef","btcusdt",.1,100000,110000,true,80000);
 History::receive(reply(q));assert(h.ready&&h.through>0);
 History::invalidate_all("test reconnect");assert(!h.ready&&h.through==0&&h.episodes.empty());
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,81000);
 History::invalidate_all("test seek");History::receive(reply(q));assert(!h.ready);
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,82000);
 auto refused=reply(q);refused["result"]["status"]="unknown";History::receive(refused);assert(!h.ready&&h.through==0);

 h.reset();q=h.poll("binancef","btcusdt",.1,100000,110000,true,90000);
 History::receive(reply(q));h.select({0});assert(h.selected.size()==1&&h.selected[0].detection_cycles==3);
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,96000);
 auto invalid=reply(q);invalid["symbol"]="solusdt";History::receive(invalid);assert(!h.ready&&h.selected.empty());
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,102000);
 History::receive(reply(q));h.select({0});
 History::invalidate_all("rewind");assert(h.selected.empty());
 // An unsupported market is unavailable, never a retained BTC observation.
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,108000);
 History::receive(reply(q));h.select({0});assert(h.ready);
 assert(h.poll("binancef","mubarakusdt",.0001,100000,110000,true,108001).is_null());
 assert(!h.ready&&h.episodes.empty()&&h.selected.empty()&&h.through==0);
 assert(h.status=="Pilot: Binance BTC and SOL only");
 // A pinned export survives a newer successful poll unchanged.
 h.reset();q=h.poll("binancef","btcusdt",.1,100000,110000,true,114000);
 History::receive(reply(q));h.select({0});const auto pinned=h.selected_source;
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,120000);
 auto newer=reply(q);newer["result"]["episodes"][0]["executed_usd"]=30000;
 History::receive(newer);
 assert(h.ready&&h.source!=pinned&&h.selected_source==pinned);
 assert(h.selected[0].executed==20000&&h.episodes[0].executed==30000);
 History::invalidate_all("market changed");assert(h.selected_source.empty()&&h.selected.empty());
 Episode first;first.id="first";first.side="bid";first.known=61000;
 Episode second=first;second.id="second";second.known=62000;
 Episode ask=first;ask.side="ask";
 Episode forming=first;forming.known=0;
 auto groups=group_markers({first,second,ask,forming},60000);
 assert(groups.size()==2&&groups[0].members.size()==2&&groups[1].members.size()==1);
 assert(group_markers({first},0).empty());


 // Continuous history uses a separate version and validates candle scope.
 h.reset();q=h.poll("binancef","btcusdt",.1,100000,110000,true,126000,true,60000);
 auto chart=reply(q);chart["result"]["version"]="absorption.chart.v1";
 chart["result"].erase("status");chart["result"]["namespace"]="epoch";
 chart["result"]["detector_version"]="liquidity_response.v1";
 chart["result"]["timeframe_ms"]=60000;
 chart["result"]["episodes"][0]["started_ms"]=90000;
 chart["result"]["bars"]=Json::array({{{"time_ms",60000},{"bid",{{"executed_usd",20000},{"new_episodes",1},{"max_detection_cycles",3},{"max_detection_persistence_ms",4000}}},{"ask",nullptr}}});
 History::receive(chart);assert(h.ready&&h.bars.size()==1&&h.bars[0].bid&& !h.bars[0].ask);
 assert(h.bars[0].bid->executed==20000&&h.episodes[0].start==90000);
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,132000,true,60000);
 chart["request_id"]=q["data"]["request_id"];chart["result"]["timeframe_ms"]=300000;
 History::receive(chart);assert(!h.ready&&h.bars.empty());
 q=h.poll("binancef","btcusdt",.1,100000,110000,true,138000,true,60000);
 auto old_chart=chart;old_chart["request_id"]=q["data"]["request_id"];
 auto changed=h.poll("binancef","btcusdt",.1,100000,110000,true,138001,true,300000);
 History::receive(old_chart);assert(!h.ready&&!changed.is_null());
}
