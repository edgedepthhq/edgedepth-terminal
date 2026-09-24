#pragma once
#include "core/exposure_page.h"

namespace exposure {
// Bound request width around the visible end, including historical zooms.
inline int64_t exact_window_start(int64_t visible,int64_t end,int64_t minimum=1) {
    const int64_t earliest=((end-4*3600000+299999)/300000)*300000;
    return std::max<int64_t>(minimum,std::max(earliest,visible/300000*300000));
}
// JSON control callbacks already run on the main thread. The registry is only
// for correlation, never a worker-thread publication cache.
class History {
public:
    History() { instances_.insert(this); }
    History(const History&)=delete;
    History& operator=(const History&)=delete;
    ~History() { reset(); instances_.erase(this); }
    void reset(const char* reason="V2 awaiting a bounded publication window") {
        pending_.erase(request_id_); request_id_.clear(); window_.reset();display_.reset();
        next_read_=0; status_=reason;
    }
    static void invalidate_all(const char* reason) {
        for (auto* history:instances_) history->reset(reason);
    }
    Json poll(const Identity& identity,int64_t from,int64_t clock,bool replay,double steady,int64_t end=0) {
        const int64_t to=end>0?std::min(end,clock):clock;
        if (identity.venue!=identity_.venue || identity.market!=identity_.market || identity.scenario!=identity_.scenario ||
            replay!=replay_ || (last_clock_>0 && clock<last_clock_)) {
            reset(); identity_=identity;from_=from;replay_=replay;
        }
        if (from!=from_ || to<last_to_) {
            pending_.erase(request_id_);request_id_.clear();window_.reset();next_read_=0;from_=from;
        }
        last_clock_=clock;last_to_=to;
        if (from<=0 || to<from || to-from>4*60*60*1000) { reset("V2 window must be within four hours"); return Json(); }
        if (!request_id_.empty() && steady-sent_>=10000) {
            reset("V2 page response timed out; history withheld"); next_read_=steady+30000;
        }
        if (!request_id_.empty() || steady<next_read_) return Json();
        if (display_ && display_->from()==from_ && to<=display_->to()) return Json();
        if (!window_ || window_->complete() || !window_->error().empty())
            window_=std::make_unique<Window>(identity_,from_,to);
        request_id_="exposure-"+std::to_string(++serial_);
        pending_[request_id_]=this;sent_=steady;
        status_="Loading immutable V2 window; partial pages withheld";
        Json data={{"request_id",request_id_},{"pair",{{"exchange",identity_.venue},{"symbol",identity_.market}}},
            {"scenario_id",identity_.scenario},{"from_ms",window_->from()},{"to_ms",window_->to()}};
        auto cursor=window_->cursor(); if (!cursor.is_null()) data["cursor"]=std::move(cursor);
        return {{"method","get_exposure_v2"},{"data",std::move(data)}};
    }
    static void receive(const Json& response,bool replay) {
        if (!text(response,"request_id")) return;
        const auto found=pending_.find(response["request_id"].get_ref<const std::string&>());
        if (found==pending_.end() || found->second->replay_!=replay) return;
        found->second->accept(response);
    }
    const Timeline* view() const { return display_?display_->view():nullptr; }
    int64_t covered_to() const { return display_?display_->to():0; }
    const std::string& status() const { return status_; }
    State state(int64_t clock) const {
        const auto* timeline=view();
        if (!timeline) return {nullptr,"missing",status_};
        auto result=timeline->at(std::min(clock,covered_to()));
        if (result.publication && clock>=result.publication->expires)
            return {result.publication,"expired","recorded_frame_expired"};
        return result;
    }
private:
    void accept(const Json& response) {
        pending_.erase(request_id_);request_id_.clear();
        if (response.contains("error")) {
            status_=text(response,"error")?"V2 unavailable: "+response["error"].get<std::string>():"Invalid V2 error response";
            if (response["error"]=="busy") { next_read_=sent_+1000; return; }
            window_.reset();display_.reset();next_read_=sent_+30000;return;
        }
        if (response.value("mode",Json())!=(replay_?"replay":"live") || !response.contains("page") || !window_ || !window_->accept(response["page"])) {
            status_=window_ && !window_->error().empty()?window_->error():"Invalid V2 page response";
            window_.reset();display_.reset();next_read_=sent_+30000;return;
        }
        if (window_->complete()) { display_=std::move(window_);status_="Complete recorded V2 snapshot";next_read_=sent_+5000; }
        else { status_="Loading immutable V2 window; partial pages withheld";next_read_=0; }
    }
    inline static uint64_t serial_=0;
    inline static std::map<std::string,History*> pending_;
    inline static std::set<History*> instances_;
    Identity identity_;
    int64_t from_=0,last_clock_=0,last_to_=0;
    bool replay_=false;
    double sent_=0,next_read_=0;
    std::string request_id_,status_="V2 disabled";
    std::unique_ptr<Window> window_,display_;
};
} // namespace exposure
