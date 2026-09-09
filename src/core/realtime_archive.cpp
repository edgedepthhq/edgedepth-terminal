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
EM_JS(void, archive_status, (const char* id, double* result, char* error), {
    const s=Module['rtArchive'].state(UTF8ToString(id)); if(!s)return;
    HEAPF64.set([s.first,s.last,s.bytes,s.total,s.pending?1:0,s.view?s.view.buffer.byteLength/8+1:0],result/8);
    stringToUTF8(s.error||'',error,256);
});
EM_JS(int, archive_query, (const char* id, double from, double to, double cutoff, double step, double tick), {
    return Module['rtArchive'].query(UTF8ToString(id),from,to,cutoff,step,tick);
});
EM_JS(void, archive_view, (const char* id, double* ptr, double* info), {
    const s=Module['rtArchive'].state(UTF8ToString(id));if(!s||!s.view)return;
    HEAPF64.set(new Float64Array(s.view.buffer),ptr/8);
    HEAPF64.set([s.view.step,s.view.tradeCount,s.view.grouped?1:0],info/8);s.view=null;
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
    ++generation;batch_.clear();view_.clear();serial_=0;clock_=0;last_depth_ms_=0;first=last=0;bytes=total_bytes=0;
    valid_=lost_=loading=false;error.clear();dropped=0;sent_at_=0;
    book_generation_=books_.realtime_generation();
    seed_.clear();seen_ids_.clear();seeded_ids_.clear();first_depth_ms_=0;
    startup_requested_=startup_pending_=false;identities_complete_=true;
    startup_first=startup_end=0;startup_status.clear();startup_at_=emscripten_get_now();
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
    if(!error.empty())return;
    if(t.agg_trade_id>0) {
        if(seeded_ids_.contains(t.agg_trade_id))return;
        if(!startup_requested_ || startup_pending_) {
            if(seen_ids_.size()<32768)seen_ids_.insert(t.agg_trade_id);
            else identities_complete_=false;
        }
    } else identities_complete_=false;
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
    poll(); if (!error.empty()) { batch_.clear(); return; }
    if(startup_pending_ && emscripten_get_now()-startup_at_>5000) {
        startup_pending_=false;startup_status="Recent history unavailable; recording live";seen_ids_.clear();
    }
    if(!seed_.empty() && make_room(131072) &&
        archive_prepend(id_.c_str(),seed_.data(),int(seed_.size()),double(clock))==1) {
        seed_.clear();cancel_view();++generation;
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
        if(sample->segment_start && last_depth_ms_) gap(last_depth_ms_);
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
bool RealtimeArchive::query(int64_t from,int64_t to,int64_t cutoff,int64_t step,double tick) {
    if(loading||!error.empty())return false;
    // The worker must receive native capture through this query's cutoff before
    // taking its snapshot, or the chart would claim an uncaptured seam as loaded.
    if (!batch_.empty() && !make_room(131072)) return false;
    const bool accepted = archive_query(id_.c_str(),double(from),double(to),double(cutoff),double(step),tick) > 0;
    poll(); return accepted;
}
bool RealtimeArchive::take_view(std::deque<RealtimeDepthHistory::SamplePtr>& samples,std::deque<Terminal::Trade>& trades,int64_t& step) {
    double info[6]={};char message[256]={};archive_status(id_.c_str(),info,message);
    if(info[5]<=0 || info[5]>4500000)return false;
    view_.resize(size_t(info[5])-1);double detail[3]={};archive_view(id_.c_str(),view_.data(),detail);
    samples.clear();trades.clear();step=int64_t(detail[0]);view_trade_count=size_t(detail[1]);view_trades_grouped=detail[2]!=0;
    for(size_t p=0;p+3<=view_.size();) {
        const size_t n=size_t(view_[p+2]);if(n<3||p+n>view_.size())break;
        if(view_[p]==1 && n>=7) {
            auto sample=std::make_shared<RealtimeDepthHistory::Sample>();sample->timestamp_ms=int64_t(view_[p+1]);
            sample->segment_start=view_[p+3]!=0;sample->bid=view_[p+4];sample->ask=view_[p+5];
            sample->levels.reserve((n-7)/2);
            for(size_t i=p+7;i+1<p+n;i+=2)sample->levels.push_back({view_[i],view_[i+1]});
            samples.push_back(std::move(sample));
        } else if(view_[p]==2 && n==6) {
            Terminal::Trade t{};t.timestamp_ms=int64_t(view_[p+1]);t.price=view_[p+3];t.qty=view_[p+4];t.is_buy=view_[p+5]!=0;
            trades.push_back(t);
        }
        p+=n;
    }
    view_.clear();return true;
}

void RealtimeArchive::request_startup(StreamManager& stream) {
    if(stream.is_replay_mode() || startup_requested_ || !first_depth_ms_ || !error.empty())return;
    // Give trade delivery a moment to establish stable exchange identities.
    if(seen_ids_.empty() && emscripten_get_now()-startup_at_<2000)return;
    startup_requested_=true;
    const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
    if(!token || !*token) {startup_status="Recording live; recent history unavailable on this feed";seen_ids_.clear();return;}
    startup_at_=emscripten_get_now();startup_pending_=true;startup_end=first_depth_ms_;
    startup_status="Loading recent history; live feed running";
    stream.send_message(nlohmann::json({{"method","get_rt_history"},{"data",{
        {"request_id",id_},{"end_ms",startup_end},{"entitlement_token",token},
        {"pair",{{"exchange",pair_.exchange},{"symbol",pair_.symbol}}}
    }}}).dump());
}
void RealtimeArchive::receive_startup(const nlohmann::json& message) {
    if(!message.contains("request_id") || !message["request_id"].is_string())return;
    const auto id=message["request_id"].get<std::string>();
    for(auto& [_,weak]:sessions) if(auto archive=weak.lock())
        if(archive->id_==id && archive->startup_pending_) {archive->receive_seed(message);return;}
}
void RealtimeArchive::receive_seed(const nlohmann::json& m) {
    startup_pending_=false;
    startup_status="Recent history unavailable; recording live";
    const auto fail=[&](){seed_.clear();seen_ids_.clear();seeded_ids_.clear();startup_first=0;};
    if(m.contains("error") || !m.contains("end_ms") || !m["end_ms"].is_number_integer() ||
        m["end_ms"].get<int64_t>()!=startup_end || !m.contains("records") || !m["records"].is_array() ||
        m["records"].size()>62280 || !m.contains("trades") || !m["trades"].is_array() || m["trades"].size()>4096) {fail();return;}
    for(const auto& value:m["records"]) {
        if(!value.is_number()) {fail();return;}
        const double v=value.get<double>();if(!std::isfinite(v)) {fail();return;}seed_.push_back(v);
    }
    int64_t previous=0;
    for(size_t p=0;p<seed_.size();) {
        if(p+7>seed_.size()) {fail();return;}
        const double length=seed_[p+2];
        if(length<7 || length>519 || std::floor(length)!=length || p+size_t(length)>seed_.size() ||
            int(length)%2!=1 || seed_[p]!=1 || seed_[p+6]!=(length-7)/2 ||
            seed_[p+1]<startup_end-30000 || seed_[p+1]>=startup_end || seed_[p+1]<=previous ||
            seed_[p+4]<=0 || seed_[p+5]<=seed_[p+4]) {fail();return;}
        for(size_t i=p+7;i<p+size_t(length);i+=2)if(seed_[i]<=0 || seed_[i+1]<0) {fail();return;}
        previous=int64_t(seed_[p+1]);if(!startup_first)startup_first=previous;
        p+=size_t(length);
    }
    // End the recorded segment explicitly. Never extend the last historical
    // observation across an unobserved history/live interval.
    if(previous)seed_.insert(seed_.end(),{3,double(std::min(previous+500,startup_end-1)),3});
    int64_t trade_first=0;
    if(identities_complete_ && !seen_ids_.empty()) for(const auto& t:m["trades"]) {
        if(!t.is_object() || !t.contains("id") || !t["id"].is_string() || !t.contains("time") || !t["time"].is_number_integer() ||
            !t.contains("price") || !t["price"].is_number() || !t.contains("qty") || !t["qty"].is_number() ||
            !t.contains("buy") || !t["buy"].is_boolean()) {fail();return;}
        const auto id_text=t["id"].get<std::string>();int64_t id=0;
        const auto parsed=std::from_chars(id_text.data(),id_text.data()+id_text.size(),id);
        const int64_t ts=t["time"].get<int64_t>();const double price=t["price"].get<double>(),qty=t["qty"].get<double>();
        if(parsed.ec!=std::errc() || parsed.ptr!=id_text.data()+id_text.size() || id<=0 || ts<startup_end-30000 || ts>=startup_end ||
            !std::isfinite(price) || !std::isfinite(qty) || price<=0 || qty<=0) {fail();return;}
        if(seen_ids_.contains(id) || !seeded_ids_.insert(id).second)continue;
        seed_.insert(seed_.end(),{2,double(ts),6,price,qty,t["buy"].get<bool>()?1.0:0.0});
        if(!trade_first || ts<trade_first)trade_first=ts;
    }
    seen_ids_.clear();
    if(!startup_first && trade_first)startup_first=trade_first;
    if(seed_.empty()) {startup_status="No recent history yet; recording live";return;}
    char status[192];
    snprintf(status,sizeof(status),"%.0fs depth / %.0fs trades (partial)",
        previous && startup_first ? double(previous-startup_first)/1000:0.0,trade_first?double(startup_end-trade_first)/1000:0.0);
    startup_status=status;
}
