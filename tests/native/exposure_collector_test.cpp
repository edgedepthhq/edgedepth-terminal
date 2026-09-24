#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/exposure_summary.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace exposure;
std::string read(const std::string& path){std::ifstream f(path);assert(f.good());return {(std::istreambuf_iterator<char>(f)),{}};}
int main(int argc,char** argv){
 assert(argc==2);const std::string dir=argv[1];
 for(const std::string venue:{"bybit","binancef"}){
  const auto page=Json::parse(read(dir+"/"+venue+"-page.json"));const Identity id{page["venue"],page["market"],page["scenario_id"]};assert(id.venue==venue);
  Window window(id,page["from_ms"],page["to_ms"]);assert(window.accept(page));assert(window.view());size_t bands=0;
  for(const auto& raw:page["publications"]){Publication p;assert(decode(raw.get<std::string>(),id,p));bands+=p.bands.size();assert(p.raw==raw.get<std::string>());Identity wrong=id;wrong.venue=venue=="bybit"?"binancef":"bybit";Publication rejected;assert(!decode(raw.get<std::string>(),wrong,rejected));}
  assert(bands>0);
  const auto raw=read(dir+"/"+venue+"-summary.json");const auto j=Json::parse(raw);SummaryScope scope{id,j["from_ms"],j["to_ms"],j["width_ms"],j["candle_period_ms"]};std::string error;auto summary=decode_summary(raw,scope,scope.to,error);assert(summary&&error.empty());assert(!decode_summary(raw,scope,scope.to-1,error));
  std::cout<<venue<<" original publications="<<page["publications"].size()<<" bands="<<bands<<" summary columns="<<summary->columns.size()<<" PASS\n";
 }
}
