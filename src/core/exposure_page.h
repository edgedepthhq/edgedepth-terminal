#pragma once
#include "core/exposure_publications.h"
#include <charconv>
#include <memory>

namespace exposure {
inline bool sequence_string(const Json& j,const char* field,uint64_t& value) {
    if (!text(j,field)) return false;
    const auto& s=j[field].get_ref<const std::string&>();
    if (s.size()>20 || (s.size()>1 && s[0]=='0')) return false;
    for (const char c:s) if (c<'0' || c>'9') return false;
    const auto result=std::from_chars(s.data(),s.data()+s.size(),value);
    return result.ec==std::errc{} && result.ptr==s.data()+s.size() && value<=uint64_t(INT64_MAX);
}
// One bounded, immutable-high-water read. Partial pages never reach a renderer.
// A subsequent live poll/replay range uses a new Window, replacing this history.
class Window {
public:
    Window(Identity identity,int64_t from,int64_t to)
        : identity_(std::move(identity)),from_(from),to_(to) {}
    bool accept(const Json& page) {
        if (!error_.empty()) return false;
        if (!last_page_.is_null() && page==last_page_) return true;
        if (complete_) return fail("page arrived after completed window");
        uint64_t anchor=0,through=0,next=0;
        if (!shape(page,"version|venue|market|scenario_id|from_ms|to_ms|anchor_sequence|through_sequence|next_sequence|start_coverage|publications|complete") ||
            page.value("version",Json())!="exposure-page.v1" || page.value("venue",Json())!=identity_.venue ||
            page.value("market",Json())!=identity_.market || page.value("scenario_id",Json())!=identity_.scenario ||
            !integer(page,"from_ms") || !integer(page,"to_ms") || page["from_ms"]!=from_ || page["to_ms"]!=to_ ||
            to_<from_ || to_-from_>4*60*60*1000 ||
            !sequence_string(page,"anchor_sequence",anchor) || !sequence_string(page,"through_sequence",through) ||
            !sequence_string(page,"next_sequence",next) || !page.contains("complete") || !page["complete"].is_boolean() ||
            !page.contains("publications") || !page["publications"].is_array() || page["publications"].size()>16 ||
            page.dump().size()>(2u<<20)) return fail("invalid page contract, identity or range");
        const auto coverage=page.value("start_coverage",Json());
        const bool anchored=coverage=="anchored";
        if ((!anchored && coverage!="before_capture") || (anchored ? anchor==0 || through<anchor : anchor!=0) ||
            next>through) return fail("invalid page coverage");
        if (!timeline_) {
            anchor_=anchor; through_=through; anchored_=anchored;
            timeline_=std::make_unique<Timeline>(identity_,anchored?anchor:1,from_,to_);
        } else if (anchor!=anchor_ || through!=through_ || anchored!=anchored_) return fail("page high-water changed");
        const bool first=timeline_->records().empty();
        uint64_t expected=first?(anchored?anchor:1):cursor_+1;
        if (!first && cursor_==UINT64_MAX) return fail("sequence overflow");
        for (const auto& raw:page["publications"]) {
            if (!raw.is_string()) return fail("publication must be original wire string");
            Publication p;
            if (!decode(raw.get_ref<const std::string&>(),identity_,p) || p.sequence!=expected ||
                p.sequence>through || p.published>to_ ||
                (timeline_->records().empty() && anchored ? p.published>from_ : p.published<=from_))
                return fail("publication gap or invalid anchor/window");
            if (!timeline_->ingest(p.raw)) return fail("invalid immutable publication sequence");
            cursor_=p.sequence;
            if (expected==UINT64_MAX && &raw!=&page["publications"].back()) return fail("sequence overflow");
            ++expected;
        }
        if (next!=cursor_ || (page["publications"].empty() && through!=0) ||
            page["complete"].get<bool>()!=(cursor_==through)) return fail("incomplete page cursor proof");
        complete_=page["complete"].get<bool>();
        last_page_=page;
        return true;
    }
    const Timeline* view() const { return complete_ && error_.empty()?timeline_.get():nullptr; }
    bool complete() const { return complete_ && error_.empty(); }
    const std::string& error() const { return error_; }
    Json cursor() const {
        if (!timeline_ || complete_) return Json();
        return {{"anchor_sequence",std::to_string(anchor_)},{"through_sequence",std::to_string(through_)},{"after_sequence",std::to_string(cursor_)}};
    }
    int64_t from() const { return from_; }
    int64_t to() const { return to_; }
private:
    bool fail(const char* reason) { error_=reason; complete_=false; return false; }
    Identity identity_;
    int64_t from_,to_;
    uint64_t anchor_=0,through_=0,cursor_=0;
    bool anchored_=false,complete_=false;
    std::unique_ptr<Timeline> timeline_;
    Json last_page_;
    std::string error_;
};
} // namespace exposure
