#include "core/orderbook_manager.h"
#include "core/message_parser.h"
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
    const char compressed[]={0x28,char(0xb5),0x2f,char(0xfd),0x20,5,0x29,0,0,'h','e','l','l','o'};
    const auto plain=MessageParser::decompress_zstd(std::string(compressed,sizeof compressed));
    expect(plain.success && plain.data=="hello","shared decoder accepts a known-size Zstd frame");
    const char huge[]={0x28,char(0xb5),0x2f,char(0xfd),char(0xa0),1,0,0,0x20,1,0,0};
    expect(!MessageParser::decompress_zstd(std::string(huge,sizeof huge)).success,
        "oversized known-size frame is rejected before allocation");
    expect(!MessageParser::decompress_zstd(std::string(compressed,10)).success,"truncated compressed frame is rejected");
    expect(MessageParser::decompress_zstd("raw").data=="raw","raw protobuf fallback remains available");
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
    archive.bubbles_.advance(100100);
    double published_value=0;
    archive.bubbles().for_each(0,100100,100100,[&](const auto& bubble){published_value+=bubble.value();});
    expect(archive.bubbles().size()==2 && published_value==300,
        "frozen bubbles join validated startup trades and live IDs exactly once");
    const auto buffered=archive.batch_.size();
    archive.append_trade({100,1,99000,true,9});
    expect(archive.batch_.size()==buffered,"late live copy of a seeded ID is not recorded twice");
    archive.append_trade({100,1,99000,true,12});
    expect(archive.batch_.size()==buffered+6,"distinct identical late trade remains visible");
    archive.bubbles_.advance(100200);
    double after_late=0;
    archive.bubbles().for_each(0,100200,100200,[&](const auto& bubble){after_late+=bubble.value();});
    expect(after_late==published_value && archive.bubbles().late_records==1,
        "late unique trade stays in archive without mutating a published bubble");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    expect(archive.bubbles().size()==0,"archive reset clears frozen bubble publications");
    auto adjacent=response;
    adjacent["records"][1]=99500;
    adjacent["trades"]=nlohmann::json::array();
    archive.receive_seed(adjacent);
    expect(archive.seed_.size()==14 && archive.seed_[12]==100000,
        "adjacent startup history ends at live start, not inside the preceding bin");

    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    auto minute=response;
    minute["records"]=nlohmann::json::array();
    minute["trades"]=nlohmann::json::array({
        {{"id","99"},{"time",10000},{"price",100},{"qty",1},{"buy",true}}
    });
    archive.append_trade({100,1,99999,true,10});
    for(int64_t ts=10000;ts<100000;ts+=500)
        for(double value : {1.0,double(ts),11.0,ts==10000?1.0:0.0,100.0,101.0,2.0,100.0,2.0,101.0,3.0})
            minute["records"].push_back(value);
    archive.receive_seed(minute);
    expect(archive.startup_first==10000 && archive.seed_.size()==180*11+3+6 && archive.seeded_ids_.contains(99),
        "full 90 seconds of depth and identified trades is accepted");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    minute["records"][1]=9999;
    archive.receive_seed(minute);
    expect(archive.seed_.empty() && archive.startup_first==0,
        "observations before the requested 90 seconds are rejected");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    auto burst=response;
    burst["end_ms"]=100001;
    burst["records"]=nlohmann::json::array();
    burst["trades"]=nlohmann::json::array();
    archive.startup_end=100001;
    archive.append_trade({100,1,100000,true,99999});
    for(int i=0;i<181;++i) {
        const int64_t ts=i==0?10001:10000+i*500;
        for(double v : {1.0,double(ts),519.0,i==0?1.0:0.0,100.0,101.0,256.0}) burst["records"].push_back(v);
        for(int j=0;j<256;++j) {burst["records"].push_back(50.0+j);burst["records"].push_back(2.0);}
    }
    for(int i=0;i<16384;++i) burst["trades"].push_back({
        {"id",std::to_string(i+1)},{"time",10001+i},{"price",100},{"qty",2},{"buy",true}});
    pb::RealtimeHistory binary;
    binary.set_end_ms(100001);
    for(const auto& value:burst["records"])binary.add_records(value.get<double>());
    for(const auto& value:burst["trades"]) {
        auto* t=binary.add_trades();t->set_agg_trade_id(std::stoll(value["id"].get<std::string>()));
        t->set_timestamp_ms(value["time"].get<int64_t>());t->set_price(100);t->set_qty(2);t->set_is_buy(true);
    }
    binary.mutable_trades(16383)->set_agg_trade_id(9007199254740993LL);
    pb::RealtimeHistory decoded;
    expect(decoded.ParseFromString(binary.SerializeAsString()),"binary history parses through generated protobuf");
    archive.receive_seed(decoded);
    expect(archive.seeded_ids_.contains(9007199254740993LL),"protobuf preserves IDs above double precision");
    expect(archive.seeded_ids_.size()==16384 && archive.seed_.size()==181*519+3+16384*6,
        "maximum depth plus 16384 distinct trade records are retained exactly");
    expect(archive.seed_.size()*sizeof(double)<2*1024*1024,
        "maximum startup payload fits the existing worker transport budget");
    pb::RealtimeHistory wide=decoded;
    wide.clear_records();
    for(int i=0;i<181;++i) {
        const int64_t ts=i==0?10001:10000+i*500;
        for(double v:{1.0,double(ts),2055.0,i==0?1.0:0.0,1000.0,1001.0,1024.0})wide.add_records(v);
        for(int j=0;j<1024;++j) {wide.add_records(489+j);wide.add_records(2);}
    }
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;
    archive.append_trade({100,2,100000,true,99999});archive.receive_seed(wide);
    expect(archive.seed_.size()==181*2055+3+16384*6 && archive.seed_.size()*8<4*1024*1024,
        "512-level startup plus exact trade tail fits the four MiB transport budget");
    expect(archive.startup_max_levels_per_side==512,
        "startup reports actual received price coverage from both book sides");
    const auto burst_size=archive.batch_.size();
    archive.append_trade({100,2,10001,true,1});
    expect(archive.batch_.size()==burst_size,"large startup still suppresses late duplicate IDs");
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;
    burst["trades"].push_back(burst["trades"].back());
    archive.receive_seed(burst);
    expect(archive.seed_.empty(),"oversized startup trade response is rejected");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    response["records"][2]=100000000;
    archive.receive_seed(response);
    expect(archive.seed_.empty() && archive.seeded_ids_.empty() && archive.startup_first==0,
        "malformed startup response is rejected atomically");
    archive.startup_pending_=true;archive.startup_at_=-100000;
    archive.update(100000);
    expect(!archive.startup_pending_ && archive.error.empty(),"history timeout leaves live recorder usable");
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;archive.startup_requested_=true;
    archive.receive_seed(decoded);
    expect(archive.deferred_seed_ && archive.seed_.empty(),"early binary response waits for overlap identities");
    archive.append_trade({100,2,100000,true,1});
    archive.update(100001);
    expect(!archive.deferred_seed_ && !archive.startup_pending_ && archive.seeded_ids_.contains(9007199254740993LL),
        "live identity releases fetched history without a second network request");
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;
    archive.append_trade({100,2,100000,true,99999});
    pb::RealtimeHistory grouped;grouped.set_end_ms(100001);
    auto* bin=grouped.add_trade_bins();bin->set_start_ms(10100);bin->set_end_ms(10200);
    bin->set_price(100);bin->set_qty(20);bin->set_low(98);bin->set_high(103);bin->set_count(10);
    bin->set_first_id(9007199254740993LL);bin->set_last_id(9007199254741002LL);
    archive.receive_seed(grouped);
    expect(archive.seed_.size()==9 && archive.seed_[0]==4 && archive.seed_[6]==98 && archive.seed_[8]==10,
        "summary preserves quantity, extremes and count");
    const size_t before_summary_duplicate=archive.batch_.size();
    archive.append_trade({100,2,10105,true,9007199254740997LL});
    expect(archive.batch_.size()==before_summary_duplicate,"late ID inside exact summary range is suppressed without double rounding");
    archive.append_trade({100,2,10106,true,9007199254741003LL});
    expect(archive.batch_.size()==before_summary_duplicate+6,"unseen ID outside summary range is retained");
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;
    archive.append_trade({100,2,10150,true,9007199254740997LL});
    archive.receive_seed(grouped);
    expect(archive.seed_.empty(),"summary overlapping previously received live records is not counted twice");
    archive.reset();archive.startup_end=100001;archive.startup_pending_=true;
    archive.append_trade({100,2,100000,true,99999});
    *grouped.add_trade_bins()=grouped.trade_bins(0);archive.receive_seed(grouped);
    expect(archive.seed_.empty(),"duplicate summary side is rejected atomically");
    archive.reset();
    expect(archive.startup_first==0 && archive.startup_max_levels_per_side==0 && archive.seeded_ids_.empty(),"reconnect clears startup identity, coverage and generation state");
    archive.reset();archive.recovery_.push_back({1000,3000});
    pb::RealtimeHistory repaired;repaired.set_end_ms(3000);
    for(double value:std::vector<double>{1,500,11,1,100,101,2,100,2,101,3,
        1,1500,11,0,100,101,2,100,4,101,3,1,2500,11,1,100,101,2,100,6,101,3}) repaired.add_records(value);
    auto* duplicate=repaired.add_trades();duplicate->set_timestamp_ms(1500);duplicate->set_qty(9);
    archive.receive_recovery(repaired);
    expect(archive.recovered_.size()==25 && archive.recovered_[1]==1500 && archive.recovered_[3]==1 &&
        archive.recovered_[14]==1 && archive.recovered_[23]==3000,
        "recovery clips to missing interval, preserves upstream boundary and records explicit end");
    expect(archive.recovered_in(1000,2000) && !archive.recovered_in(3000,4000),
        "only intersecting repaired history requests archive projection");
    expect(archive.recovered_[0]==1 && archive.recovered_[11]==1 && archive.recovered_[22]==3,
        "depth-only recovery never duplicates separately continuing trades");
    archive.recovered_.clear();archive.recovery_.push_back({1000,3000});repaired.set_records(13,20000);
    archive.receive_recovery(repaired);
    expect(archive.recovered_.empty(),"malformed recovery is rejected atomically");
    archive.reset();
    expect(archive.recovery_.empty() && !archive.recovered_in(0,10000),"source reset clears repair coverage and pending work");
    EM_ASM({const s=Module['rtArchive'].state(UTF8ToString($0));
        s.pending=true;
        s.view=({step:500,source_bucket_ticks:20,tradeCount:0,grouped:false,
            buffer:new Float64Array([1,1000,11,1,100,101,2,100,40,101,60]).buffer});
    },archive.id_.c_str());
    std::deque<RealtimeDepthHistory::SamplePtr> width_samples;std::deque<Terminal::Trade> width_trades;int64_t width_step=0;
    expect(archive.take_view(width_samples,width_trades,width_step) && width_samples.front()->source_bucket_ticks==20 &&
        width_samples.front()->levels.front().size==40,
        "archive uses returned source width while another query is pending without altering quantities");
    archive.error="storage fixture failure";
    archive.append_trade({100,2,1101,true});
    archive.bubbles_.advance(1200);
    expect(archive.bubbles().size()==1,"storage failure does not interrupt independent live bubble capture");
    EM_ASM({const s=Module['rtArchive'].state(UTF8ToString($0));
        s.view=({step:1000,tradeCount:9999,grouped:true,
            buffer:new Float64Array([4,1100,9,999,1000,1,1,2000,9999]).buffer});
    },archive.id_.c_str());
    archive.take_view(width_samples,width_trades,width_step);
    double unchanged_value=0;
    archive.bubbles().for_each(0,2000,2000,[&](const auto& bubble){unchanged_value+=bubble.value();});
    expect(unchanged_value==200,"coarse archive refresh cannot rebuild or replace published bubbles");
    archive.reset();archive.startup_end=100000;archive.startup_pending_=true;
    auto malformed=response;malformed["trades"][3]["qty"]=-1;
    archive.receive_seed(malformed);archive.bubbles_.advance(100100);
    expect(archive.seed_.empty() && archive.bubbles().size()==0,
        "invalid startup response cannot partially publish bubbles");
    RealtimeDepthHistory::Sample native_sample;
    expect(native_sample.source_bucket_ticks==1,"native observations retain one native tick source width");
    return failures;
}
