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
    expect(!archive.prepare_replay(50000),"replay waits when recorder transport is blocked");
    expect(archive.serial_>start_serial && archive.serial_<start_serial+200 && archive.dropped==0,
        "full-depth burst defers uncollected samples while transport is blocked");
    EM_ASM({Module['archiveBlocked']=false;});
    archive.sent_at_=-1000000;archive.update(50000);
    archive.sent_at_=-1000000;archive.update(50000);
    const int depths=EM_ASM_INT({return Module['rtArchive'].state(UTF8ToString($0)).records.filter(r=>r[0]===1).length;},archive.id_.c_str());
    expect(depths==200 && archive.dropped==0,"catch-up drains multiple bounded batches without depth loss");
    expect(archive.prepare_replay(50000),"replay resumes once pending observations have drained");
    std::deque<Terminal::Trade> displayed{{100,1,1000,true},{100,2,1001,true}};
    std::deque<Terminal::Trade> recent;
    for(size_t i=0;i<RealtimeTradeHistory::max_trades;++i)
        recent.push_back({100,1,int64_t(2000+i),true});
    expect(!refresh_realtime_trade_tail(displayed,recent,1000,30000) &&
        displayed.size()==20001 && displayed[1].timestamp_ms==1001 && displayed.back().timestamp_ms==21999,
        "rolling ring exhaustion retains displayed history and advances current trades");
    displayed={{100,1,22000,true}};
    recent={{100,2,22001,true},{100,2,22001,true},{100,3,22002,false}};
    expect(refresh_realtime_trade_tail(displayed,recent,22000,22001) && displayed.size()==3,
        "fresh snapshot joins identical records without leaking future trades");
    expect(refresh_realtime_trade_tail(displayed,recent,22000,22001) && displayed.size()==3,
        "repeated tail refresh preserves multiplicity without duplication");
    const auto generation=archive.generation;
    books.clear_all();seed.set_timestamp_ms(3001);seed.set_last_update_id(300);
    books.apply_orderbook_snapshot_from_pb(pair,seed);
    archive.sent_at_=-1000000;archive.update(3001);
    expect(archive.generation>generation && archive.serial_==1,"new owner generation captures its first fresh seed instead of skipping it");
    const auto before_correction=archive.generation;
    archive.update(2000);
    expect(archive.serial_==1 && archive.generation==before_correction,"ordinary replay clock corrections do not erase recorded history");
    archive.append_trade({100,1,2000,true});
    EM_ASM({Module['archiveBlocked']=true;});
    expect(!archive.query(1000,2000,2000,100,1) && !archive.batch_.empty(),
        "query waits for pending capture instead of claiming an incomplete cutoff");
    EM_ASM({Module['archiveBlocked']=false;});
    expect(!archive.query(1000,2000,2000,100,1) && archive.batch_.empty(),
        "query flushes native records before handing its cutoff to the worker");
    expect(!archive.query(1000,2000,2000,100,1),"rejected query reports failure so navigation can retry");
    archive.reset();archive.startup_pending_=true;archive.startup_requested_=true;archive.startup_end=100000;
    archive.append_trade({100,1,99999,true,10});
    nlohmann::json response={{"end_ms",100000},{"records",{1,99000,11,1,100,101,2,100,2,101,3}},
        {"trades",nlohmann::json::array({
            {{"id","9"},{"time",99000},{"price",100},{"qty",1},{"buy",true}},
            {{"id","10"},{"time",99999},{"price",100},{"qty",1},{"buy",true}},
            {{"id","11"},{"time",99000},{"price",100},{"qty",1},{"buy",true}},
            {{"id","9"},{"time",99000},{"price",100},{"qty",1},{"buy",true}}
        })}};
    const auto book_serial=archive.serial_;
    archive.receive_seed(response);
    expect(archive.seed_.size()==26 && archive.seeded_ids_.size()==2,
        "startup removes only matching exchange IDs and keeps distinct identical trades");
    expect(archive.serial_==book_serial && archive.startup_first==99000,
        "history seed never advances live orderbook cursor");
    const auto buffered=archive.batch_.size();
    archive.append_trade({100,1,99000,true,9});
    expect(archive.batch_.size()==buffered,"late live copy of a seeded ID is not recorded twice");
    archive.append_trade({100,1,99000,true,12});
    expect(archive.batch_.size()==buffered+6,"distinct identical late trade remains visible");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    response["records"][2]=100000000;
    archive.receive_seed(response);
    expect(archive.seed_.empty() && archive.seeded_ids_.empty() && archive.startup_first==0,
        "malformed startup response is rejected atomically");
    archive.startup_pending_=true;archive.startup_at_=-100000;
    archive.update(100000);
    expect(!archive.startup_pending_ && archive.error.empty(),"history timeout leaves live recorder usable");
    archive.reset();
    expect(archive.startup_first==0 && archive.seeded_ids_.empty(),"reconnect clears startup identity and generation state");
    return failures;
}
