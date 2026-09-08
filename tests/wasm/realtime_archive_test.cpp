#include "core/orderbook_manager.h"
#include <string>
#include <memory>
#define private public
#include "core/realtime_archive.h"
#undef private
#include <emscripten.h>
#include <cstdio>

int test_archive_capture() {
    int failures=0;
    auto expect=[&](bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);++failures;}};
    OrderbookManager books;
    Terminal::Pair pair{"binancef","fixture"};
    auto owner=std::unique_ptr<RealtimeArchive>(new RealtimeArchive(books,pair));
    auto& archive=*owner;
    pb::BookUpdate seed;
    seed.set_snapshot(true);seed.set_timestamp_ms(1001);seed.set_last_update_id(100);
    auto* bid=seed.add_bids();bid->set_price(100);bid->set_size(2);
    auto* ask=seed.add_asks();ask->set_price(101);ask->set_size(3);
    books.apply_orderbook_snapshot_from_pb(pair,seed);
    archive.append_trade({100,2,1001,true});archive.append_trade({100,2,1001,true});
    archive.sent_at_=-1000000;archive.update(1000);
    expect(archive.serial_==0,"archive defers future depth until the as-of clock");
    archive.sent_at_=-1000000;archive.update(1001);
    expect(archive.serial_==1,"archive collects eligible depth independently of display cursor");
    const int records=EM_ASM_INT({return Module['rtArchive'].state(UTF8ToString($0)).records.length;},archive.id_.c_str());
    expect(records==3,"capture sends both identical trades and original sampled depth");
    std::vector<RealtimeDepthHistory::SamplePtr> frozen;
    books.copy_realtime_since(pair,0,frozen);
    pb::BookUpdate delta;
    delta.set_timestamp_ms(1101);delta.set_first_update_id(101);delta.set_previous_update_id(100);delta.set_last_update_id(101);
    auto* level=delta.add_bids();level->set_price(100);level->set_size(9);
    books.apply_book_update_from_pb(pair,delta);
    archive.sent_at_=-1000000;archive.update(1101);
    expect(frozen.front()->levels.front().size==2 && archive.serial_==2,"frozen book stays immutable while archive collection advances");
    // Interrupt and reseed between collection polls: the prior final bin must
    // still be retired even though the next poll sees a valid book again.
    books.interrupt_realtime();seed.set_timestamp_ms(1301);seed.set_last_update_id(200);
    books.apply_orderbook_snapshot_from_pb(pair,seed);
    archive.sent_at_=-1000000;archive.update(1301);
    const bool gap=EM_ASM_INT({return Module['rtArchive'].state(UTF8ToString($0)).records.some(r=>r[0]===3&&r[1]===1101);},archive.id_.c_str());
    expect(gap,"fast interruption/reseed preserves a gap across collection polls");
    EM_ASM({Module['archiveBlocked']=true;});
    for(int i=0;i<25000;++i)archive.append_trade({100,1,1400+i,true});
    archive.sent_at_=-1000000;archive.update(1400);
    expect(archive.batch_.size()<=131072 && archive.dropped>0 && !archive.lost_,"trade backpressure bounds capture RAM without inventing depth gaps");
    EM_ASM({Module['archiveBlocked']=false;});
    archive.sent_at_=-1000000;archive.update(1400);
    expect(archive.batch_.empty(),"capture recovers when the bounded transport has capacity");
    archive.reset();
    seed.clear_bids();seed.clear_asks();
    for(int i=0;i<512;++i) {
        auto* b=seed.add_bids();b->set_price(100-i*0.01);b->set_size(2);
        auto* a=seed.add_asks();a->set_price(101+i*0.01);a->set_size(3);
    }
    seed.set_timestamp_ms(30001);seed.set_last_update_id(400);
    books.apply_orderbook_snapshot_from_pb(pair,seed);
    for(int i=1;i<200;++i) {
        delta.set_timestamp_ms(30001+i*100);delta.set_first_update_id(400+i);
        delta.set_previous_update_id(399+i);delta.set_last_update_id(400+i);
        books.apply_book_update_from_pb(pair,delta);
    }
    const auto start_serial=archive.serial_;
    EM_ASM({Module['archiveBlocked']=true;});
    archive.update(50000);
    expect(archive.serial_>start_serial && archive.serial_<start_serial+200 && archive.dropped==0,
        "full-depth burst defers uncollected samples while transport is blocked");
    EM_ASM({Module['archiveBlocked']=false;});
    archive.sent_at_=-1000000;archive.update(50000);
    archive.sent_at_=-1000000;archive.update(50000);
    const int depths=EM_ASM_INT({return Module['rtArchive'].state(UTF8ToString($0)).records.filter(r=>r[0]===1).length;},archive.id_.c_str());
    expect(depths==200 && archive.dropped==0,"catch-up drains multiple bounded batches without depth loss");
    const auto generation=archive.generation;
    books.clear_all();seed.set_timestamp_ms(3001);seed.set_last_update_id(300);
    books.apply_orderbook_snapshot_from_pb(pair,seed);
    archive.sent_at_=-1000000;archive.update(3001);
    expect(archive.generation>generation && archive.serial_==1,"new owner generation captures its first fresh seed instead of skipping it");
    const auto before_correction=archive.generation;
    archive.update(2000);
    expect(archive.serial_==1 && archive.generation==before_correction,"ordinary replay clock corrections do not erase recorded history");
    expect(!archive.query(1000,2000,2000,100,1),"rejected query reports failure so navigation can retry");
    return failures;
}
