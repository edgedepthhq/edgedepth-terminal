#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/touch_odds.h"
#include "touch_odds_lr_fixture.h"
#include <cassert>
using namespace touch_odds;
using Json = nlohmann::json;
Json frame(){
 Json bands=Json::array();
 for(int side:{-1,1})for(int off=1;off<=3;off++){double c=100*std::exp(side*off*std::log(1.0025));bands.push_back({{"side",side},{"offset",off},{"low",c/1.00125},{"high",c*1.00125},{"p",.9/off}});}
 return {{"v",1},{"venue","binancef"},{"symbol","aaveusdt"},{"status","scored"},{"asof",1000},{"expires_at",1201000},{"hide_at",601000},{"mark",100.0},{"bands",bands},
  {"cascade",{{"tier","top1"},{"hour_start",0},{"hour_end",3600000}}},{"server_now",31000}};
}
int main(){
 { auto j=frame(); j["history"]=true; j["available_at"]=31000; Snapshot past; assert(parse(j,past)); assert(past.history); assert(!past.bands_at(30999)); assert(past.bands_at(31000)); }
 Snapshot s;assert(parse(frame(),s));assert(s.band_count==6&&s.tier==Tier::Top1&&s.symbol=="aaveusdt");
 assert(s.bands_at(30000)&&s.bands_at(600999)&&!s.bands_at(601000));assert(s.tier_at(30000)&&!s.tier_at(3600000));
 // Every malformed variant leaves the previous snapshot untouched.
 for(int which=0;which<12;which++){
  auto bad=frame();
  switch(which){
   case 0:bad["v"]=2;break; case 1:bad["venue"]="bybit";break; case 2:bad["symbol"]="AAVEUSDT";break;
   case 3:bad["bands"][0]["p"]=1.2;break; case 4:bad["bands"][1]=bad["bands"][0];break; case 5:bad["bands"][2]["high"]=1;break;
   case 6:bad["bands"].erase(0);break; case 7:bad["hide_at"]=2000000;break; case 8:bad["cascade"]["tier"]="top10";break;
   case 9:bad["mark"]="100";break; case 10:bad["bands"][0]["side"]=0;break; default:bad["asof"]=1.5;break;
  }
  Snapshot keep=s;assert(!parse(bad,keep));assert(keep.band_count==6&&keep.asof==1000);
 }
 // Uncovered markets carry a reason and no bands; the tier can still arrive alone.
 auto held=frame();held["status"]="outside_frozen_universe";held["bands"]=Json::array();held.erase("asof");
 assert(parse(held,s)&&s.band_count==0&&!s.bands_at(30000)&&s.tier_at(30000));
 assert(std::string(missing_reason(s.status))=="not in the tested market set");
 held["bands"]=frame()["bands"];assert(!parse(held,s));
 held["status"]="mystery";held["bands"]=Json::array();assert(!parse(held,s));
 auto none=frame();none["status"]="no_current_frame";none["bands"]=Json::array();none["cascade"]=nullptr;
 assert(parse(none,s)&&s.tier==Tier::None&&!s.tier_at(30000));
 assert(!parse(Json::parse("[1,2]"),s));assert(!parse(Json(),s));
 // Long range: parity with independent synthetic calculations at every fixture case, and refusal of broken models.
 auto lr=frame();lr["longrange"]={{"asof",1000},{"hide_at",601000},{"mark",100.0},{"rv",{.002,.003,.004}},{"distance_range",{.005,.1}},{"horizons",Json::parse(kTouchLrHorizons)}};
 for(const auto& c:kTouchLrCases){
  lr["longrange"]["rv"]={c.rv[0],c.rv[1],c.rv[2]};Snapshot q;assert(parse(lr,q)&&q.longrange.at(600999)&&!q.longrange.at(601000));
  const Horizon* h=nullptr;for(const auto& x:q.longrange.horizons)if(x.name==c.horizon)h=&x;assert(h);
  const double got=probability(q.longrange,*h,100.0,100.0*(1+c.side*c.distance));
  assert(std::abs(got-c.p)<1e-9);
 }
 Snapshot q;parse(lr,q);assert(probability(q.longrange,q.longrange.horizons[0],100.0,100.3)<0&&probability(q.longrange,q.longrange.horizons[0],100.0,80.0)<0);
 assert(probability(q.longrange,q.longrange.horizons[2],100.0,101.0)>probability(q.longrange,q.longrange.horizons[2],100.0,103.0));
 assert(probability(q.longrange,q.longrange.horizons[2],100.0,102.0)>probability(q.longrange,q.longrange.horizons[0],100.0,102.0));
 for(int which=0;which<4;which++){
  auto bad=lr;
  switch(which){case 0:bad["longrange"]["rv"][1]=0;break;case 1:bad["longrange"]["horizons"].erase(0);break;
   case 2:bad["longrange"]["horizons"][0]["knots"]["p"][1]=0;break;default:bad["longrange"]["horizons"][1]["transform"]["sd"][0]=0;break;}
  Snapshot keep=s;assert(!parse(bad,keep));
 }
 auto nolr=frame();nolr["longrange"]=nullptr;assert(parse(nolr,q)&&!q.longrange.valid);
 return 0;
}
