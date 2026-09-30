#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/touch_zones.h"
#include <cassert>
using namespace touch_zones;
int main(){
 // Two clusters above, one below, noise outside the 0.5-10% range and inside 0.5%.
 std::vector<Fuel> f={{102.0,5},{102.1,4},{101.95,3},{104.0,2},{104.05,2},{97.0,6},{100.2,50},{130.0,40},{75.0,40},{103.0,1}};
 auto z=strongest(f,100.0,3);
 // 100.2 is inside 0.5% and 130/75 beyond 10%; 103 sits within 1% of the kept 102 and 104 zones.
 assert(z.size()==3);
 assert(z[0].side==1&&z[0].price>101.9&&z[0].price<102.2&&z[0].mass==12); // merged 101.95-102.1
 assert(z[1].side==-1&&std::abs(z[1].price-97.0)<1e-9);
 assert(z[2].side==1&&z[2].price>103.9&&z[2].price<104.1&&z[2].mass==4);
 for(const auto& x:z){const double d=std::abs(x.price/100.0-1.0);assert(d>=kMin&&d<=kMax);}
 for(size_t i=0;i<z.size();++i)for(size_t j=i+1;j<z.size();++j)assert(std::abs(std::log(z[i].price/z[j].price))>=kSeparation);
 assert(strongest(f,100.0,1).size()==2);
 assert(strongest({},100.0,3).empty()&&strongest(f,0.0,3).empty());
 return 0;
}
