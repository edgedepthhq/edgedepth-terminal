#include "core/realtime_archive.h"
#include "core/candle_manager.h"
#include "core/orderbook_manager.h"
#include <emscripten.h>
#include <map>
#include <chrono>
EM_JS_DEPS(realtime_archive_deps, "$stringToUTF8,$UTF8ToString");

EM_JS(void, archive_unique_id, (char* out), { stringToUTF8(crypto.randomUUID(),out,64); });
EM_JS(void, archive_create, (const char* id), { Module['rtArchive'].create(UTF8ToString(id)); });
EM_JS(void, archive_cancel, (const char* id), { Module['rtArchive'].cancel(UTF8ToString(id)); });
EM_JS(void, archive_clear, (const char* id), { Module['rtArchive'].clear(UTF8ToString(id)); });
EM_JS(int, archive_send, (const char* id, const double* ptr, int count, double clock), {
    const key=UTF8ToString(id),ready=Module['rtArchive'].capacity(key,count*8);
    return ready===1 ? Module['rtArchive'].append(key,HEAPU8.slice(ptr,ptr+count*8).buffer,clock) : ready;
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

RealtimeArchive::RealtimeArchive(OrderbookManager& books,const Terminal::Pair& pair):books_(books),pair_(pair) {
    batch_.reserve(131072); pending_.reserve(RealtimeDepthHistory::max_samples); reset();
}
std::shared_ptr<RealtimeArchive> RealtimeArchive::acquire(CandleManager& candles, OrderbookManager& books,const Terminal::Pair& pair) {
    static std::map<CandleManager*,std::weak_ptr<RealtimeArchive>> sessions;
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
RealtimeArchive::~RealtimeArchive() {archive_clear(id_.c_str());}
void RealtimeArchive::reset(bool discard_existing) {
    if(!id_.empty())archive_clear(id_.c_str());
    char unique[64]={};archive_unique_id(unique);
    id_=pair_.exchange+":"+pair_.symbol+":"+unique;
    ++generation;batch_.clear();view_.clear();serial_=0;clock_=0;last_depth_ms_=0;first=last=0;bytes=total_bytes=0;
    valid_=lost_=loading=false;error.clear();dropped=0;sent_at_=0;
    book_generation_=books_.realtime_generation();
    // Start from activation, never silently claim pre-activation observations.
    if (discard_existing) {
        books_.copy_realtime_since(pair_,0,pending_);
        if(!pending_.empty())serial_=pending_.back()->serial;
    }
    pending_.clear();archive_create(id_.c_str());
}
void RealtimeArchive::gap(int64_t clock) {
    if(batch_.size()+3<=131072)batch_.insert(batch_.end(),{3,double(clock),3});
    else lost_=true;
}
void RealtimeArchive::append_trade(const Terminal::Trade& t) {
    if(!error.empty())return;
    if(batch_.size()+6>131072){++dropped;lost_=true;return;}
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
    const bool valid=books_.copy_realtime_since(pair_,serial_,pending_);
    for(const auto& sample:pending_) {
        if(sample->timestamp_ms>clock)break;
        if(serial_ && sample->serial!=serial_+1) {
            gap(last_depth_ms_ ? last_depth_ms_ : sample->timestamp_ms);lost_=true;++dropped;
        }
        if(sample->segment_start && last_depth_ms_) gap(last_depth_ms_);
        serial_=sample->serial;
        last_depth_ms_=sample->timestamp_ms;
        const size_t n=7+sample->levels.size()*2;
        if(batch_.size()+n>131072){++dropped;lost_=true;continue;}
        batch_.insert(batch_.end(),{1,double(sample->timestamp_ms),double(n),
            sample->segment_start || lost_ ? 1.0:0.0,sample->bid,sample->ask,double(sample->levels.size())});
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
