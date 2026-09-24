#include "core/realtime_archive.h"
#include "core/candle_manager.h"
#include "core/orderbook_manager.h"
#include <emscripten.h>
#include <map>
#include <chrono>
#include <charconv>
#include <nlohmann/json.hpp>
EM_JS_DEPS(realtime_archive_deps, "$stringToUTF8,$UTF8ToString");

EM_JS(void, archive_unique_id, (char* out), { stringToUTF8(crypto.randomUUID(),out,64); });
EM_JS(void, archive_create, (const char* id), { Module['rtArchive'].create(UTF8ToString(id)); });
EM_JS(void, archive_cancel, (const char* id), { Module['rtArchive'].cancel(UTF8ToString(id)); });
EM_JS(void, archive_clear, (const char* id), { Module['rtArchive'].clear(UTF8ToString(id)); });
EM_JS(int, archive_send, (const char* id, const double* ptr, int count, double clock), {
    const key=UTF8ToString(id),ready=Module['rtArchive'].capacity(key,count*8);
    return ready===1 ? Module['rtArchive'].append(key,HEAPU8.slice(ptr,ptr+count*8).buffer,clock) : ready;
});
EM_JS(int, archive_prepend, (const char* id, const double* ptr, int count, double clock), {
    const key=UTF8ToString(id);
    if(Module['rtArchive'].capacity(key,count*8)!==1)return 0;
    return Module['rtArchive'].append(key,HEAPU8.slice(ptr,ptr+count*8).buffer,clock,true);
});
EM_JS(int, archive_recover, (const char* id, const double* ptr, int count, double clock), {
    const key=UTF8ToString(id);
    if(Module['rtArchive'].capacity(key,count*8)!==1)return 0;
    return Module['rtArchive'].append(key,HEAPU8.slice(ptr,ptr+count*8).buffer,clock,2);
});
EM_JS(void, archive_status, (const char* id, double* result, char* error), {
    const s=Module['rtArchive'].state(UTF8ToString(id)); if(!s)return;
    HEAPF64.set([s.first,s.last,s.bytes,s.total,s.pending?1:0,s.view?s.view.buffer.byteLength/8+1:0],result/8);
    stringToUTF8(s.error||'',error,256);
});
EM_JS(int, archive_query, (const char* id, double from, double to, double cutoff, double step, double tick, double native_tick), {
    return Module['rtArchive'].query(UTF8ToString(id),from,to,cutoff,step,tick,native_tick);
});
EM_JS(void, archive_view, (const char* id, double* ptr, double* info), {
    const s=Module['rtArchive'].state(UTF8ToString(id));if(!s||!s.view)return;
    HEAPF64.set(new Float64Array(s.view.buffer),ptr/8);
    HEAPF64.set([s.view.step,s.view.tradeCount,s.view.grouped?1:0,s.view.source_bucket_ticks||1],info/8);s.view=null;
});

namespace { std::map<CandleManager*,std::weak_ptr<RealtimeArchive>> sessions; }

RealtimeArchive::RealtimeArchive(OrderbookManager& books,const Terminal::Pair& pair):books_(books),pair_(pair) {
    batch_.reserve(131072); pending_.reserve(RealtimeDepthHistory::max_samples); reset();
}
std::shared_ptr<RealtimeArchive> RealtimeArchive::acquire(CandleManager& candles, OrderbookManager& books,const Terminal::Pair& pair) {
    for(auto it=sessions.begin();it!=sessions.end();) {
        if(it->second.expired()) it=sessions.erase(it);else ++it;
    }
    if(auto current=sessions[&candles].lock())return current;
    auto current=std::shared_ptr<RealtimeArchive>(new RealtimeArchive(books,pair));
    sessions[&candles]=current;
    candles.set_realtime_observer([weak=std::weak_ptr<RealtimeArchive>(current)](const Terminal::Trade* trade){
        if(auto archive=weak.lock()) {if(trade)archive->append_trade(*trade);else archive->reset();}
    });
    return current;
}
bool RealtimeArchive::prepare_replay(CandleManager* candles, int64_t clock) {
    const auto it=sessions.find(candles);
    if(it==sessions.end())return true;
    auto archive=it->second.lock();
    if(!archive)return true;
    return archive->prepare_replay(clock);
}
bool RealtimeArchive::prepare_replay(int64_t clock) {
    update(clock);
    if(!error.empty())return true; // Recording failure must not stop playback.
    books_.copy_realtime_since(pair_,serial_,pending_);
    const bool pending=!pending_.empty() && pending_.front()->timestamp_ms<=clock;
    pending_.clear();
    // Leave room for the next small delivery group without growing any queue.
    return !pending && make_room(65536);
}
RealtimeArchive::~RealtimeArchive() {archive_clear(id_.c_str());}
void RealtimeArchive::reset(bool discard_existing) {
    if(!id_.empty())archive_clear(id_.c_str());
    char unique[64]={};archive_unique_id(unique);
    id_=pair_.exchange+":"+pair_.symbol+":"+unique;
    bubbles_.reset();
    ++generation;batch_.clear();view_.clear();serial_=0;clock_=0;last_depth_ms_=0;first=last=0;bytes=total_bytes=0;
    valid_=lost_=loading=false;error.clear();dropped=0;sent_at_=0;
    book_generation_=books_.realtime_generation();
    seed_.clear();deferred_seed_.reset();seen_ids_.clear();seeded_ids_.clear();seen_native_.clear();seeded_native_.clear();seeded_ranges_.clear();startup_live_first_ms_=0;first_depth_ms_=0;
    recovery_.clear();recovered_intervals_.clear();recovered_.clear();recovery_id_.clear();recovery_at_=0;
    startup_requested_=startup_pending_=false;identities_complete_=true;
    startup_first=startup_end=0;startup_max_levels_per_side=0;startup_status.clear();startup_at_=emscripten_get_now();
    // Start from activation, never silently claim pre-activation observations.
    if (discard_existing) {
        books_.copy_realtime_since(pair_,0,pending_);
        if(!pending_.empty())serial_=pending_.back()->serial;
    }
    pending_.clear();archive_create(id_.c_str());
}
bool RealtimeArchive::make_room(size_t count) {
    if(batch_.size()+count<=131072)return true;
    const int sent=archive_send(id_.c_str(),batch_.data(),int(batch_.size()),double(clock_));
    if(sent!=1)return false;
    batch_.clear();sent_at_=emscripten_get_now();
    return count<=131072;
}
void RealtimeArchive::gap(int64_t clock) {
    if(batch_.size()+3<=131072)batch_.insert(batch_.end(),{3,double(clock),3});
    else lost_=true;
}
void RealtimeArchive::append_trade(const Terminal::Trade& t) {
    if (!startup_requested_ || startup_pending_)
        startup_live_first_ms_ = startup_live_first_ms_ ? std::min(startup_live_first_ms_, t.timestamp_ms) : t.timestamp_ms;
    if(t.agg_trade_id>0) {
        const auto range=std::lower_bound(seeded_ranges_.begin(),seeded_ranges_.end(),t.agg_trade_id,
            [](const auto& r,int64_t id){return r.second<id;});
        if(range!=seeded_ranges_.end() && range->first<=t.agg_trade_id)return;
        if(seeded_ids_.contains(t.agg_trade_id))return;
        if(!startup_requested_ || startup_pending_) {
            if(seen_ids_.size()<32768)seen_ids_.insert(t.agg_trade_id);
            else identities_complete_=false;
        }
    } else if(!t.native_trade_id.empty()) {
        if(seeded_native_.contains(t.native_trade_id))return;
        if(!startup_requested_ || startup_pending_) {
            if(seen_native_.size()<32768)seen_native_.insert(t.native_trade_id);
            else identities_complete_=false;
        }
    } else identities_complete_=false;
    bubbles_.append(t, clock_);
    if(!error.empty())return;
    if(!make_room(6)){++dropped;return;}
    batch_.insert(batch_.end(),{2,double(t.timestamp_ms),6,t.price,t.qty,t.is_buy?1.0:0.0});
}
void RealtimeArchive::poll() {
    double info[6]={};char message[256]={};archive_status(id_.c_str(),info,message);
    first=int64_t(info[0]);last=int64_t(info[1]);bytes=info[2];total_bytes=info[3];loading=info[4]!=0;
    if (clock_>0) {last=std::min(last,clock_);if(first>last)first=last=0;}
    error=message;
}
void RealtimeArchive::update(int64_t clock) {
    if(book_generation_!=books_.realtime_generation()) reset(false);
    // Replay status corrections can move the interpolated clock backward.
    // Explicit source clear/seek owns resets; queries enforce the as-of cutoff.
    clock_=clock;
    bubbles_.advance(clock);
    poll(); if (!error.empty()) { batch_.clear(); return; }
    if(deferred_seed_ && (!seen_ids_.empty() || emscripten_get_now()-startup_at_>=2000)) {
        auto response=std::move(deferred_seed_);receive_seed(*response);
    }
    if(startup_pending_ && emscripten_get_now()-startup_at_>5000) {
        startup_pending_=false;startup_status="Recent history unavailable; recording live";seen_ids_.clear();
    }
    if(!seed_.empty() && make_room(131072) &&
        archive_prepend(id_.c_str(),seed_.data(),int(seed_.size()),double(clock))==1) {
        seed_.clear();cancel_view();++generation;
    }
    if(!recovered_.empty() && make_room(131072) &&
       archive_recover(id_.c_str(),recovered_.data(),int(recovered_.size()),double(clock))==1) {
        recovered_.clear();cancel_view();++generation;
    }
    const bool valid=books_.copy_realtime_since(pair_,serial_,pending_);
    for(const auto& sample:pending_) {
        if(sample->timestamp_ms>clock)break;
        const size_t n=7+sample->levels.size()*2;
        // Flush full batches before continuing catch-up. A busy worker leaves
        // the cursor here so the bounded owner history can be retried next frame.
        if(!make_room(n+6))break;
        if(serial_ && sample->serial!=serial_+1) {
            gap(last_depth_ms_ ? last_depth_ms_ : sample->timestamp_ms);lost_=true;++dropped;
        }
        if(last_depth_ms_ && (sample->segment_start || sample->timestamp_ms-last_depth_ms_>1000)) {
            gap(last_depth_ms_);
            // Only the missing interval is eligible. Never replace local observations
            // or infer depth from trade activity. Bound outstanding interruption work.
            if(recovery_.size()<8) recovery_.push_back({last_depth_ms_,sample->timestamp_ms});
        }
        if(!first_depth_ms_)first_depth_ms_=sample->timestamp_ms;
        serial_=sample->serial;
        last_depth_ms_=sample->timestamp_ms;
        batch_.insert(batch_.end(),{1,double(sample->timestamp_ms),double(n),
            sample->segment_start || lost_ || sample->timestamp_ms==first_depth_ms_ ? 1.0:0.0,sample->bid,sample->ask,double(sample->levels.size())});
        for(const auto& level:sample->levels)batch_.insert(batch_.end(),{level.price,level.size});
        lost_=false;
    }
    if(valid_&&!valid)gap(last_depth_ms_ ? last_depth_ms_ : clock);
    valid_=valid;pending_.clear();
    const double now=emscripten_get_now();
    if(!batch_.empty() && (batch_.size()>65536 || now-sent_at_>=1000)) {
        const int sent=archive_send(id_.c_str(),batch_.data(),int(batch_.size()),double(clock));
        if(sent!=0){batch_.clear();sent_at_=now;}
    }
    poll();
}
void RealtimeArchive::cancel_view() { archive_cancel(id_.c_str()); poll(); }
bool RealtimeArchive::query(int64_t from,int64_t to,int64_t cutoff,int64_t step,double tick,double native_tick) {
    if(loading||!error.empty())return false;
    // The worker must receive native capture through this query's cutoff before
    // taking its snapshot, or the chart would claim an uncaptured seam as loaded.
    if (!batch_.empty() && !make_room(131072)) return false;
    const bool accepted = archive_query(id_.c_str(),double(from),double(to),double(cutoff),double(step),tick,native_tick) > 0;
    poll(); return accepted;
}
bool RealtimeArchive::take_view(std::deque<RealtimeDepthHistory::SamplePtr>& samples,std::deque<Terminal::Trade>& trades,int64_t& step) {
    double info[6]={};char message[256]={};archive_status(id_.c_str(),info,message);
    if(info[5]<=0 || info[5]>4500000)return false;
    view_.resize(size_t(info[5])-1);double detail[4]={};archive_view(id_.c_str(),view_.data(),detail);
    samples.clear();trades.clear();step=int64_t(detail[0]);view_trade_count=size_t(detail[1]);view_trades_grouped=detail[2]!=0;
    for(size_t p=0;p+3<=view_.size();) {
        const size_t n=size_t(view_[p+2]);if(n<3||p+n>view_.size())break;
        if(view_[p]==1 && n>=7) {
            auto sample=std::make_shared<RealtimeDepthHistory::Sample>();sample->timestamp_ms=int64_t(view_[p+1]);
            sample->source_bucket_ticks=std::isfinite(detail[3]) && detail[3]>=1 && detail[3]<=1000000 ? int(detail[3]):1;
            sample->segment_start=view_[p+3]!=0;sample->bid=view_[p+4];sample->ask=view_[p+5];
            sample->levels.reserve((n-7)/2);
            for(size_t i=p+7;i+1<p+n;i+=2)sample->levels.push_back({view_[i],view_[i+1]});
            samples.push_back(std::move(sample));
        } else if((view_[p]==2 && n==6) || (view_[p]==4 && n==9)) {
            Terminal::Trade t{};t.timestamp_ms=int64_t(view_[p+1]);t.price=view_[p+3];t.qty=view_[p+4];t.is_buy=view_[p+5]!=0;
            if(n==9) {t.summary_low=view_[p+6];t.summary_high=view_[p+7];t.summary_count=uint64_t(view_[p+8]);}
            trades.push_back(t);
        }
        p+=n;
    }
    view_.clear();return true;
}

void RealtimeArchive::request_startup(StreamManager& stream) {
    if(stream.is_replay_mode() || !first_depth_ms_ || !error.empty())return;
    // The archive remains active while the chart shows candles. Keep its owned
    // DOM feed recoverable there too, using the existing throttled refresh owner.
    if(!valid_ || (clock_>last_depth_ms_ && clock_-last_depth_ms_>15000))
        stream.refresh_orderbook({pair_,Terminal::Stream::Orderbook,0},clock_);
    const double now=emscripten_get_now();
    if(!recovery_id_.empty() && now-recovery_at_>5000) {
        recovery_id_.clear();
        if(!recovery_.empty() && recovery_.front().attempts>=3) recovery_.pop_front();
    }
    if(startup_requested_) {
        if(startup_pending_ || !seed_.empty() || !recovered_.empty() || !recovery_id_.empty() || recovery_.empty())return;
        const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
        if(!token || !*token)return;
        auto& interval=recovery_.front();
        if(interval.attempts && now-recovery_at_<5000)return;
        if(interval.attempts>=3 || clock_-interval.to>startup_window_ms) {recovery_.pop_front();return;}
        recovery_id_=id_+":recovery:"+std::to_string(++recovery_serial_);
        recovery_at_=now;++interval.attempts;
        // The server accepts 4096 or 16384. Recovery ignores returned trades.
        stream.send_message(nlohmann::json({{"method","get_rt_history"},{"data",{
            {"request_id",recovery_id_},{"end_ms",interval.to},{"window_ms",60000},
            {"encoding","protobuf-zstd"},{"binary_window_ms",startup_window_ms},
            {"trade_limit",4096},{"levels_per_side",512},{"entitlement_token",token},
            {"pair",{{"exchange",pair_.exchange},{"symbol",pair_.symbol}}}
        }}}).dump());
        return;
    }
    // Fetch in parallel with establishing live trade identities.
    startup_requested_=true;
    const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
    if(!token || !*token) {startup_status="Recording live; recent history unavailable on this feed";seen_ids_.clear();return;}
    startup_at_=emscripten_get_now();startup_pending_=true;startup_end=first_depth_ms_;
    startup_status="Loading recent history; live feed running";
    stream.send_message(nlohmann::json({{"method","get_rt_history"},{"data",{
        {"request_id",id_},{"end_ms",startup_end},{"window_ms",60000},{"encoding","protobuf-zstd"},{"binary_window_ms",startup_window_ms},{"trade_limit",startup_trade_limit},{"levels_per_side",512},{"grouped_trades",true},{"entitlement_token",token},
        {"pair",{{"exchange",pair_.exchange},{"symbol",pair_.symbol}}}
    }}}).dump());
}
void RealtimeArchive::receive_startup(const nlohmann::json& message) {
    if(!message.contains("request_id") || !message["request_id"].is_string())return;
    const auto id=message["request_id"].get<std::string>();
    for(auto& [_,weak]:sessions) if(auto archive=weak.lock()) {
        if(archive->recovery_id_==id) {archive->recovery_id_.clear();return;}
        if(archive->id_==id && archive->startup_pending_) {archive->receive_seed(message);return;}
    }
}
void RealtimeArchive::receive_startup(const pb::RealtimeHistory& message, const Terminal::Pair& pair) {
    for(auto& [_,weak]:sessions) if(auto archive=weak.lock()) {
        if(archive->recovery_id_==message.request_id() && archive->pair_.exchange==pair.exchange && archive->pair_.symbol==pair.symbol) {
            archive->receive_recovery(message);return;
        }
        if(archive->id_==message.request_id() && archive->startup_pending_ &&
           archive->pair_.exchange==pair.exchange && archive->pair_.symbol==pair.symbol) {
            archive->receive_seed(message);return;
        }
    }
}
void RealtimeArchive::receive_seed(const nlohmann::json& m) {
    pb::RealtimeHistory response;
    const auto invalid=[&](){response.set_end_ms(-1);receive_seed(response);};
    if(m.contains("error") || !m.contains("end_ms") || !m["end_ms"].is_number_integer() ||
       !m.contains("records") || !m["records"].is_array() || m["records"].size()>size_t((startup_window_ms/500+1)*519) ||
       !m.contains("trades") || !m["trades"].is_array() || m["trades"].size()>startup_trade_limit) {invalid();return;}
    response.set_end_ms(m["end_ms"].get<int64_t>());
    for(const auto& value:m["records"]) {
        if(!value.is_number()) {invalid();return;}
        response.add_records(value.get<double>());
    }
    for(const auto& t:m["trades"]) {
        if(!t.is_object() || !t.contains("id") || !t["id"].is_string() || !t.contains("time") || !t["time"].is_number_integer() ||
           !t.contains("price") || !t["price"].is_number() || !t.contains("qty") || !t["qty"].is_number() ||
           !t.contains("buy") || !t["buy"].is_boolean()) {invalid();return;}
        const auto text=t["id"].get<std::string>();int64_t id=0;
        if(text.empty()) {invalid();return;}
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);
        auto* trade=response.add_trades();
        if(parsed.ec==std::errc() && parsed.ptr==text.data()+text.size())trade->set_agg_trade_id(id);
        else trade->set_native_trade_id(text);
        trade->set_timestamp_ms(t["time"].get<int64_t>());
        trade->set_price(t["price"].get<double>());trade->set_qty(t["qty"].get<double>());trade->set_is_buy(t["buy"].get<bool>());
    }
    receive_seed(response);
}
void RealtimeArchive::receive_seed(const pb::RealtimeHistory& m) {
    // A quick response can beat the first live trade. Retain the existing
    // identity grace period without delaying the network request itself.
    if(startup_requested_ && startup_pending_ && identities_complete_ && (m.trades_size()>0 || m.trade_bins_size()>0) && m.end_ms()==startup_end &&
       seen_ids_.empty() && emscripten_get_now()-startup_at_<2000) {
        deferred_seed_=std::make_shared<pb::RealtimeHistory>(m);return;
    }
    deferred_seed_.reset();
    startup_pending_=false;
    startup_status="Recent history unavailable; recording live";
    const auto fail=[&](){seed_.clear();seen_ids_.clear();seeded_ids_.clear();seen_native_.clear();seeded_native_.clear();seeded_ranges_.clear();startup_first=0;startup_max_levels_per_side=0;};
    if(m.end_ms()!=startup_end || m.records_size()>(startup_window_ms/500+1)*2055 ||
       size_t(m.trades_size())>startup_trade_limit || m.trade_bins_size()>1800) {fail();return;}
    seed_.clear();
    for(double value:m.records()) {
        if(!std::isfinite(value)) {fail();return;}seed_.push_back(value);
    }
    int64_t previous=0;
    for(size_t p=0;p<seed_.size();) {
        if(p+7>seed_.size()) {fail();return;}
        const double length=seed_[p+2];
        if(length<7 || length>2055 || std::floor(length)!=length || p+size_t(length)>seed_.size() ||
            int(length)%2!=1 || seed_[p]!=1 || seed_[p+6]!=(length-7)/2 ||
            seed_[p+1]<startup_end-startup_window_ms || seed_[p+1]>=startup_end || seed_[p+1]<=previous ||
            seed_[p+4]<=0 || seed_[p+5]<=seed_[p+4]) {fail();return;}
        for(size_t i=p+7;i<p+size_t(length);i+=2)if(seed_[i]<=0 || seed_[i+1]<0) {fail();return;}
        int bids=0, asks=0;
        for(size_t i=p+7;i<p+size_t(length);i+=2) {
            if(seed_[i]<=seed_[p+4])++bids;else ++asks;
        }
        startup_max_levels_per_side=std::max({startup_max_levels_per_side,bids,asks});
        previous=int64_t(seed_[p+1]);if(!startup_first)startup_first=previous;
        p+=size_t(length);
    }
    // End the recorded segment explicitly. Never extend the last historical
    // observation across an unobserved history/live interval.
    if(previous)seed_.insert(seed_.end(),{3,double(std::min(previous+500,startup_end)),3});
    int64_t trade_first=0;
    if(identities_complete_ && (!seen_ids_.empty() || !seen_native_.empty())) for(const auto& t:m.trades()) {
        const int64_t id=t.agg_trade_id(),ts=t.timestamp_ms();
        const double price=t.price(),qty=t.qty();
        const bool native=id<=0 && !t.native_trade_id().empty();
        if((id<=0 && !native) || ts<startup_end-startup_window_ms || ts>=startup_end ||
           !std::isfinite(price) || !std::isfinite(qty) || price<=0 || qty<=0) {fail();return;}
        if(native) {
            if(seen_native_.contains(t.native_trade_id()) || !seeded_native_.insert(t.native_trade_id()).second)continue;
        } else if(seen_ids_.contains(id) || !seeded_ids_.insert(id).second)continue;
        seed_.insert(seed_.end(),{2,double(ts),6,price,qty,t.is_buy()?1.0:0.0});
        if(!trade_first || ts<trade_first)trade_first=ts;
    }
    bool grouped=false;
    int64_t raw_first=startup_end;
    for(const auto& t:m.trades())raw_first=std::min(raw_first,t.timestamp_ms());
    int64_t previous_bin=-1; bool previous_side=true;
    if(identities_complete_ && !seen_ids_.empty()) for(const auto& b:m.trade_bins()) {
        if(b.start_ms()<previous_bin || (b.start_ms()==previous_bin && (previous_side || !b.is_buy()))) {fail();return;}
        previous_bin=b.start_ms();previous_side=b.is_buy();
        if(b.start_ms()%100 || b.end_ms()!=b.start_ms()+100 || b.start_ms()<startup_end-startup_window_ms ||
           b.end_ms()>startup_end || b.end_ms()>raw_first || b.first_id()<=0 || b.last_id()<b.first_id() ||
           !b.count() || uint64_t(b.count())>uint64_t(b.last_id()-b.first_id())+1 ||
           !std::isfinite(b.price()) || !std::isfinite(b.qty()) || !std::isfinite(b.low()) || !std::isfinite(b.high()) ||
           b.qty()<=0 || b.low()<=0 || b.low()>b.price() || b.high()<b.price()) {fail();return;}
        // Never mix a summary with live records already collected from its bin.
        if(startup_live_first_ms_ && b.end_ms()>startup_live_first_ms_)continue;
        seed_.insert(seed_.end(),{4,double(b.start_ms()),9,b.price(),b.qty(),b.is_buy()?1.0:0.0,b.low(),b.high(),double(b.count())});
        seeded_ranges_.push_back({b.first_id(),b.last_id()});grouped=true;
        if(!trade_first || b.start_ms()<trade_first)trade_first=b.start_ms();
    }
    std::sort(seeded_ranges_.begin(),seeded_ranges_.end());
    size_t ranges=0;
    for(const auto& r:seeded_ranges_) {
        if(ranges && r.first-1<=seeded_ranges_[ranges-1].second)
            seeded_ranges_[ranges-1].second=std::max(seeded_ranges_[ranges-1].second,r.second);
        else seeded_ranges_[ranges++]=r;
    }
    seeded_ranges_.resize(ranges);
    seen_ids_.clear();seen_native_.clear();
    if(!startup_first && trade_first)startup_first=trade_first;
    if(seed_.empty()) {startup_status="No recent history yet; recording live";return;}
    // Feed only the fully validated, identity-joined seed. Later archive query
    // summaries never rebuild or replace these exact-price publications.
    for (size_t p = 0; p < seed_.size(); p += size_t(seed_[p + 2])) {
        if (seed_[p] != 2 && seed_[p] != 4) continue;
        Terminal::Trade t{};
        t.timestamp_ms = int64_t(seed_[p + 1]); t.price = seed_[p + 3];
        t.qty = seed_[p + 4]; t.is_buy = seed_[p + 5] != 0;
        if (seed_[p] == 4) {
            t.summary_low = seed_[p + 6]; t.summary_high = seed_[p + 7];
            t.summary_count = uint64_t(seed_[p + 8]);
        }
        bubbles_.append(t, clock_);
    }
    char status[192];
    snprintf(status,sizeof(status),"%.0fs depth / %.0fs trades (partial)%s",
        previous && startup_first ? double(previous-startup_first)/1000:0.0,trade_first?double(startup_end-trade_first)/1000:0.0, grouped?"; older trades grouped at 100ms":"");
    startup_status=status;
}

void RealtimeArchive::receive_recovery(const pb::RealtimeHistory& m) {
    recovery_id_.clear();
    if(recovery_.empty())return;
    const auto interval=recovery_.front();recovery_.pop_front();
    if(m.end_ms()!=interval.to || m.records_size()>(startup_window_ms/500+1)*2055)return;
    std::vector<double> records;
    int64_t previous=0;
    for(int p=0;p<m.records_size();) {
        if(p+7>m.records_size())return;
        const double length=m.records(p+2),time=m.records(p+1);
        if(!std::isfinite(length) || length<7 || length>2055 || std::floor(length)!=length ||
           p+int(length)>m.records_size() || int(length)%2!=1 || m.records(p)!=1 ||
           m.records(p+6)!=(length-7)/2 || !std::isfinite(time) || time<=previous ||
           time<interval.to-startup_window_ms || time>=interval.to ||
           m.records(p+4)<=0 || m.records(p+5)<=m.records(p+4))return;
        for(int i=p;i<p+int(length);++i)if(!std::isfinite(m.records(i)))return;
        for(int i=p+7;i<p+int(length);i+=2)if(m.records(i)<=0 || m.records(i+1)<0)return;
        previous=int64_t(time);
        if(time>interval.from) {
            const auto start=records.size();
            for(int i=p;i<p+int(length);++i)records.push_back(m.records(i));
            if(!start)records[3]=1; // Coverage starts at its first actual observation.
        }
        p+=int(length);
    }
    if(records.empty())return;
    records.insert(records.end(),{3,double(std::min(previous+500,interval.to)),3});
    recovered_intervals_.push_back({int64_t(records[1]),interval.to});
    while(!recovered_intervals_.empty() && recovered_intervals_.front().second<clock_-target_ms)
        recovered_intervals_.pop_front();
    // At most one recovery per observed 100ms boundary in the retained session.
    while(recovered_intervals_.size()>18000)recovered_intervals_.pop_front();
    recovered_=std::move(records);
    // Deliberately depth only: trades may have continued during the interruption.
    // Startup identity budgets cannot deduplicate an arbitrary later trade gap.
}
