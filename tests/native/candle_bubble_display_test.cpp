#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/candle_bubble_display.h"
#include <cassert>
#include <iostream>
using namespace CandleBubbleDisplay;
int main() {
    // BTC and LSK-sized distributions must express the same relative size.
    for(double reference : {100000.0,2000.0}) {
        float previous=0;
        for(double multiple : {0.1,1.,4.2,10.,42.}) {
            float r=radius(reference*multiple,reference);
            assert(r>previous && r<24); previous=r;
        }
    }
    std::vector<Marker> selected;
    assert(retain(selected,{0,100,100,12},1400));
    assert(!retain(selected,{1,104,100,8},1400)); // overlap keeps strongest identity
    assert(retain(selected,{2,150,100,5},1400));
    assert(selected[0].index==0 && selected[1].index==2);
    auto dense=[](float width) {
        std::vector<Marker> view;
        for(size_t i=0;i<5760;++i) retain(view,{i,float(i)*width/5760,100,7},width);
        for(size_t i=1;i<view.size();++i) assert(view[i].x-view[i-1].x>=18);
        return view.size();
    };
    assert(dense(1400)>dense(350));
    assert(dense(1400)<100); // a wall of thousands becomes readable, original prints
    std::cout<<"PASS: BTC/LSK scale separation; dense selection "<<dense(1400)<<"/5760; zoom reveals detail\n";
}
