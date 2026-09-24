#pragma once
#include "core/exposure_summary.h"

namespace exposure {
// Main-thread control-message owner, independent of original publication history.
class SummaryHistory {
public:
    SummaryHistory() { instances_.insert(this); }
    SummaryHistory(const SummaryHistory&)=delete;
    SummaryHistory& operator=(const SummaryHistory&)=delete;
    ~SummaryHistory() { reset();instances_.erase(this); }
    void reset(const char* reason="Averaged history not loaded") {
        pending_.erase(request_id_);request_id_.clear();summary_.reset();next_read_=0;status_=reason;
    }
    static void invalidate_all(const char* reason) { for (auto* instance:instances_) instance->reset(reason); }
    Json poll(const SummaryScope& desired,int64_t clock,bool replay,double steady) {
        auto stable=desired;stable.to=scope_.to;
        const auto& a=desired.identity;const auto& b=scope_.identity;
        if (a.venue!=b.venue || a.market!=b.market || a.scenario!=b.scenario ||
            replay!=replay_ || clock<last_clock_ || desired.candle_period!=scope_.candle_period ||
            desired.width!=scope_.width) reset();
        else if (!(stable==scope_) || desired.to<scope_.to) {
            pending_.erase(request_id_);request_id_.clear();next_read_=0;
        }
        last_clock_=clock;replay_=replay;
        if (desired.from<0 || desired.to<=desired.from || desired.to>clock || desired.to-desired.from>summary_max_range ||
            desired.candle_period<=0 || desired.width<desired.candle_period || desired.width>summary_max_range || desired.width%desired.candle_period!=0) {
            reset("Averages require a valid range of up to 90 days, ending at the playback clock");return Json();
        }
        // Preserve a pending immutable request end while live time advances.
        if (!request_id_.empty() && steady-sent_>=10000) { reset("Averaged history timed out");next_read_=steady+30000; }
        if (!request_id_.empty() || steady<next_read_) return Json();
        if (summary_ && desired==summary_->scope) return Json();
        scope_=desired;request_id_="exposure-summary-"+std::to_string(++serial_);
        pending_[request_id_]=this;sent_=steady;status_="Loading averaged history";
        return {{"method","get_exposure_v2_summary"},{"data",{{"request_id",request_id_},
            {"pair",{{"exchange",scope_.identity.venue},{"symbol",scope_.identity.market}}},{"scenario_id",scope_.identity.scenario},
            {"from_ms",scope_.from},{"to_ms",scope_.to},{"width_ms",scope_.width},{"candle_period_ms",scope_.candle_period}}}};
    }
    static void receive(const Json& response,bool replay) {
        if (!text(response,"request_id")) return;
        const auto found=pending_.find(response["request_id"].get_ref<const std::string&>());
        if (found==pending_.end() || found->second->replay_!=replay) return;
        found->second->accept(response);
    }
    const Summary* view(int64_t clock) const { return summary_ && summary_at(*summary_,clock)?summary_.get():nullptr; }
    const std::string& status() const { return status_; }
private:
    void accept(const Json& response) {
        pending_.erase(request_id_);request_id_.clear();
        if (response.contains("error")) {
            status_=text(response,"error")?"Averaged history unavailable: "+response["error"].get<std::string>():"Invalid summary error";
            if (response["error"]!="busy") summary_.reset();
            next_read_=sent_+(response["error"]=="busy"?1000:30000);return;
        }
        if (response.value("mode",Json())!=(replay_?"replay":"live") || !response.contains("summary") || !response["summary"].is_object()) {
            reset("Invalid averaged history response");next_read_=sent_+30000;return;
        }
        summary_=decode_summary(response["summary"].dump(),scope_,last_clock_,status_);
        if (summary_) { status_="Averaged history loaded; coverage may contain gaps";next_read_=sent_+5000; }
        else next_read_=sent_+30000;
    }
    inline static uint64_t serial_=0;
    inline static std::map<std::string,SummaryHistory*> pending_;
    inline static std::set<SummaryHistory*> instances_;
    SummaryScope scope_;
    std::unique_ptr<Summary> summary_;
    std::string request_id_,status_="Averages disabled";
    bool replay_=false;
    int64_t last_clock_=0;
    double sent_=0,next_read_=0;
};
// Minute publications need multiple serial pages in a normal chart viewport.
// Use one explicitly labelled average response except when inspecting close up.
inline bool automatic_summary(int64_t from,int64_t to,int64_t period) {
    return period>=1800000 || to-from>1800000;
}
// Pick a UTC grid at least as wide as a candle, with <=1024 columns including
// clipped edge columns. This is request sizing, not client-side aggregation.
inline int64_t summary_width(int64_t from,int64_t to,int64_t period) {
    if (from<0 || to<=from || to-from>summary_max_range || period<=0 || period>summary_max_range) return 0;
    const int64_t target=(to-from+1022)/1023;
    const int64_t multiple=std::max<int64_t>(1,(target+period-1)/period);
    const int64_t width=multiple*period;
    return width<=summary_max_range?width:0;
}
} // namespace exposure
