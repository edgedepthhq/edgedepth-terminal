#include "rendering/observed_liquidations.h"
#include <cstdlib>
#include <cstdio>
#define check(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)
#include <limits>
#include <vector>
struct Event { double price, avg_price, qty; bool is_buy; int64_t timestamp_ms; };
int main() {
    std::vector<Event> events{{100,101,2,true,1000},{100,0,3,false,1500},
        {100,100,4,true,2500},{100,100,999,false,3000}};
    auto paused = observed_liquidations::aggregate(events,1000,10000,2500);
    check(paused.count==3 && paused.shorts==602 && paused.longs==300);
    auto rewind = observed_liquidations::aggregate(events,1000,10000,1500);
    check(rewind.count==2 && rewind.shorts==202 && rewind.longs==300);
    auto narrow = observed_liquidations::aggregate(events,1500,2500,2500);
    check(narrow.count==2 && narrow.shorts==400 && narrow.longs==300);
    auto wide = observed_liquidations::aggregate(events,0,1800000,1800000);
    check(wide.size<=256 && wide.count==4 && wide.shorts==602 && wide.longs==100200);
    auto invalid=events.front(); invalid.qty=std::numeric_limits<double>::infinity();
    check(observed_liquidations::notional(invalid)==0);
    invalid.qty=-1; check(observed_liquidations::notional(invalid)==0);
    invalid.qty=1; invalid.price=invalid.avg_price=std::numeric_limits<double>::quiet_NaN();
    check(observed_liquidations::notional(invalid)==0);
    check(observed_liquidations::aggregate(events,3000,1000,1000).count==0);
}
