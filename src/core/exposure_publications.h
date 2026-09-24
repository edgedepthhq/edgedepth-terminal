#pragma once
// Development exposure-publication.v1 consumer. No inventory/threshold inference.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <iterator>
#include <utility>
#include <vector>

namespace exposure {
using Json = nlohmann::json;
struct Identity {
    std::string venue, market, scenario;
};
struct Band { int side; int64_t index; double low, high, mass; };
struct Publication {
    uint64_t sequence = 0;
    int64_t published = 0, expires = 0;
    std::string raw, missing;
    Json frame;
    std::vector<Band> bands;
};
inline bool shape(const Json& j, const char* fields) {
    if (!j.is_object()) return false;
    const std::string allowed = std::string("|") + fields + "|";
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key().find('|') != std::string::npos || allowed.find("|" + it.key() + "|") == std::string::npos) return false;
    return true;
}
inline bool text(const Json& j, const char* k) {
    return j.contains(k) && j[k].is_string() && !j[k].get_ref<const std::string&>().empty();
}
inline bool number(const Json& j, const char* k) {
    return j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>());
}
inline bool integer(const Json& j, const char* k, int64_t minimum = 0) {
    return j.contains(k) && j[k].is_number_integer() && j[k] >= minimum &&
        j[k] <= INT64_MAX;
}
inline bool near(double a, double b) { return std::abs(a-b) < 1e-8; }
inline bool fraction(const Json& v) {
    return v.is_number() && std::isfinite(v.get<double>()) && v >= 0 && v <= 1;
}
inline bool supported_source(const Json& f) {
    if (f.value("version",Json())=="exposure-scenario.v1" && f.value("source",Json())=="inferred_synthetic")
        return !f.contains("public_provenance") || f["public_provenance"].is_null();
    return f.value("version",Json())=="exposure-scenario.v2" && f.value("source",Json())=="inferred_public_scenario";
}
inline bool hash_text(const Json& j,const char* key) {
    if (!text(j,key)) return false;
    const auto& value=j[key].get_ref<const std::string&>();
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
// The authenticated backend verifies its Go-encoded assumptions digest. The
// client validates structure/dates and preserves bytes; it does NOT claim that
// C++ JSON reserialization reproduces Go float formatting or a portable digest.
// Two publication modes are accepted. "forward": a live minute, evaluated at
// or before its publication clock inside the dated assumption window.
// "bootstrap": a recorded-history minute replayed to seed a fresh stream's
// inventory; it is dated by its own historical decision, evaluated later, and
// the assumptions are the live roster's, so the window check does not apply.
// Neither mode is ever presented as the other; bootstrap frames and the
// forward frames that inherit their inventory say so through
// bootstrap_through_ms.
inline bool public_provenance(const Json& f,int64_t published,int64_t expires) {
    if (f["source"]=="inferred_synthetic") return true;
    if ((f["venue"]!="bybit" && f["venue"]!="binancef") || (f["market"]!="BTCUSDT" && f["market"]!="ETHUSDT" && f["market"]!="SOLUSDT") ||
        !f.contains("public_provenance")) return false;
    const auto& p=f["public_provenance"];
    const bool bootstrap=p.value("mode",Json())=="bootstrap";
    if (!shape(p,"version|mode|source_manifest_sha256|input_sha256|assumptions_sha256|evaluated_at_ms|assumptions") ||
        p.value("version",Json())!="native-public-scenario.v1" || (p.value("mode",Json())!="forward" && !bootstrap) ||
        !hash_text(p,"source_manifest_sha256") || !hash_text(p,"input_sha256") || !hash_text(p,"assumptions_sha256") ||
        !integer(p,"evaluated_at_ms",1) || p["evaluated_at_ms"]<f["as_of_ms"] || (!bootstrap && p["evaluated_at_ms"]>published) ||
        (bootstrap && (p["evaluated_at_ms"]<=f["as_of_ms"] || f["as_of_ms"]!=published)) ||
        !p.contains("assumptions")) return false;
    const auto& a=p["assumptions"];
    if (!shape(a,"version|available_at_ms|valid_from_ms|valid_until_ms|interpretation|scenario") ||
        a.value("version",Json())!="exposure-assumptions.v1" ||
        a.value("interpretation",Json())!="fixed_buffer_constant_maintenance_sensitivity_not_account_liquidation" ||
        a.value("scenario",Json())!=f["scenario"] || !integer(a,"available_at_ms",1) || !integer(a,"valid_from_ms",1) || !integer(a,"valid_until_ms",1) ||
        a["valid_from_ms"]<a["available_at_ms"] || a["valid_until_ms"]<=a["valid_from_ms"] ||
        a["valid_until_ms"].get<int64_t>()-a["valid_from_ms"].get<int64_t>()>86400000 ||
        (!bootstrap && (f["as_of_ms"]<a["valid_from_ms"] || f["as_of_ms"]>=a["valid_until_ms"])) ||
        p["evaluated_at_ms"]<a["available_at_ms"] || expires>a["valid_until_ms"]) return false;
    return true;
}
inline bool is_bootstrap(const Json& f) {
    return f.contains("public_provenance") && f["public_provenance"].is_object() && f["public_provenance"].value("mode",Json())=="bootstrap";
}
inline bool decode(const std::string& raw, const Identity& id, Publication& p) {
    if (raw.empty() || raw.size() > (1u << 20)) return false;
    // Duplicate JSON members cannot silently override an immutable identity.
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    auto j = Json::parse(raw, [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            duplicate = true;
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    }, false);
    if (duplicate || !shape(j, "version|sequence|venue|market|scenario_id|published_at_ms|expires_at_ms|missing_reason|frame") ||
        j.value("version", Json()) != "exposure-publication.v1" ||
        !text(j,"venue") || !text(j,"market") || !text(j,"scenario_id") ||
        j["venue"] != id.venue || j["market"] != id.market || j["scenario_id"] != id.scenario ||
        !j.contains("sequence") || !j["sequence"].is_number_unsigned() || j["sequence"] == 0 ||
        !integer(j,"published_at_ms",1) || !integer(j,"expires_at_ms",1) ||
        j["expires_at_ms"] <= j["published_at_ms"]) return false;
    p = Publication{};
    p.raw = raw;
    p.sequence = j["sequence"].get<uint64_t>();
    p.published = j["published_at_ms"].get<int64_t>();
    p.expires = j["expires_at_ms"].get<int64_t>();
    if (j.contains("missing_reason")) {
        if (!j["missing_reason"].is_string()) return false;
        p.missing = j["missing_reason"].get<std::string>();
    }
    if (!j.contains("frame") || j["frame"].is_null()) return !p.missing.empty();
    if (!p.missing.empty()) return false;
    const auto& f = j["frame"];
    if (!shape(f,"version|venue|market|as_of_ms|available_at_ms|oi_trade_mark_clocks|source|units|grid|status|scenario|mark|oi_one_side_base|unknown_long_short|unknown_reasons_long_short|oldest_input_age_ms|coverage|uncertainty|bands|public_provenance|bootstrap_through_ms") ||
        (f.contains("bootstrap_through_ms") && (!integer(f,"bootstrap_through_ms",1) || f["bootstrap_through_ms"]>f["as_of_ms"])) ||
        !supported_source(f) || f.value("venue",Json()) != id.venue ||
        f.value("market",Json()) != id.market ||
        f.value("units",Json()) != "fraction_of_one_side_oi" ||
        f.value("grid",Json()) != "log_1.0025_anchor_1.v1" ||
        !integer(f,"as_of_ms",1) || !integer(f,"available_at_ms",1) ||
        f["available_at_ms"] > f["as_of_ms"] || f["as_of_ms"] > p.published ||
        p.expires - f["as_of_ms"].get<int64_t>() > 120000 ||
        !number(f,"mark") || f["mark"] <= 0 || !number(f,"oi_one_side_base") || f["oi_one_side_base"] < 0 ||
        !integer(f,"oldest_input_age_ms") || !text(f,"coverage") ||
        f.value("uncertainty",Json()) != "scenario_sensitivity_not_confidence_interval") return false;
    if (!f.contains("scenario")) return false;
    const auto& s = f["scenario"];
    // retain_cohorts is optional (absent means disabled); when present it is the
    // publisher's bounded retention window, see backend LIQUIDATION_V2_PUBLISHER.md.
    if (!shape(s,"id|survival|turnover|buffer|maintenance|retain_cohorts") || s.value("id",Json()) != id.scenario ||
        (s.contains("retain_cohorts") && (!s["retain_cohorts"].is_number_integer() || s["retain_cohorts"]<1000 || s["retain_cohorts"]>40000)) ||
        (s.value("survival",Json()) != "oldest" && s.value("survival",Json()) != "newest" && s.value("survival",Json()) != "proportional") ||
        !number(s,"turnover") || s["turnover"] < 0 || s["turnover"] > 1 ||
        !number(s,"buffer") || !number(s,"maintenance") || s["maintenance"] < 0 || s["maintenance"] >= 1 ||
        s["buffer"] <= 0 || s["buffer"].get<double>() >= 1-s["maintenance"].get<double>()) return false;
    if (!public_provenance(f,p.published,p.expires)) return false;
    if (!f.contains("oi_trade_mark_clocks") || !f["oi_trade_mark_clocks"].is_array() || f["oi_trade_mark_clocks"].size()!=3) return false;
    for (const auto& c : f["oi_trade_mark_clocks"])
        if (!shape(c,"event_ms|available_ms") || !integer(c,"event_ms",1) || !integer(c,"available_ms",1) ||
            c["event_ms"] > c["available_ms"] || c["available_ms"] > f["available_at_ms"]) return false;
    if (!f.contains("unknown_long_short") || !f["unknown_long_short"].is_array() || f["unknown_long_short"].size()!=2 ||
        !f.contains("unknown_reasons_long_short") || !f["unknown_reasons_long_short"].is_array() || f["unknown_reasons_long_short"].size()!=2 ||
        !f.contains("bands") || !f["bands"].is_array() || f["bands"].size()>8192) return false;
    std::array<double,2> totals{};
    for (int side=0;side<2;++side) {
        const auto& u=f["unknown_long_short"][side];
        const auto& reasons=f["unknown_reasons_long_short"][side];
        if (!fraction(u) || !reasons.is_object()) return false;
        double sum=0;
        for (auto it=reasons.begin();it!=reasons.end();++it) {
            if (it.key().empty() || !fraction(it.value())) return false;
            sum+=it.value().get<double>();
        }
        if (!near(sum,u.get<double>())) return false;
        totals[side]=sum;
    }
    std::set<std::pair<int,int64_t>> seen;
    for (const auto& b:f["bands"]) {
        if (!shape(b,"side|index|low|high|mass") || !text(b,"side") ||
            (b["side"]!="long" && b["side"]!="short") || !integer(b,"index",INT64_MIN) ||
            !number(b,"low") || !number(b,"high") || b["low"]<=0 || b["high"]<=b["low"] ||
            !b.contains("mass") || !fraction(b["mass"]) || b["mass"]==0) return false;
        Band band{b["side"]=="long"?0:1,b["index"].get<int64_t>(),b["low"].get<double>(),b["high"].get<double>(),b["mass"].get<double>()};
        // Verify stable grid identity; draw the supplied bounds unchanged.
        const double center=std::pow(1.0025,double(band.index));
        if (!std::isfinite(center) || center<=0 ||
            std::abs(band.low/(center/std::sqrt(1.0025))-1)>1e-10 ||
            std::abs(band.high/(center*std::sqrt(1.0025))-1)>1e-10 ||
            !seen.insert({band.side,band.index}).second) return false;
        totals[band.side]+=band.mass;
        p.bands.push_back(band);
    }
    const bool flat=f["oi_one_side_base"]==0;
    if (flat ? f.value("status",Json())!="flat_observed_oi" :
        (f.value("status",Json())!="scenario" && f.value("status",Json())!="initial_unknown")) return false;
    for (double total:totals) if (!near(total,flat?0:1)) return false;
    p.frame=f;
    return true;
}
struct State {
    const Publication* publication=nullptr;
    const char* status="before_capture";
    std::string reason;
};
class Timeline {
public:
    explicit Timeline(Identity id,uint64_t first=1,int64_t from=0,int64_t to=INT64_MAX)
        : identity_(std::move(id)),first_(first),from_(from),to_(to) {
        if (first==0 || from<0 || to<from) error_="invalid history coverage";
    }
    // Main-thread owned, bounded development history. JSON control callbacks
    // ingest exact bytes here; replay backfill uses the same seam.
    bool ingest(const std::string& raw) {
        if (!error_.empty()) return false;
        Publication p;
        if (!decode(raw,identity_,p)) return refuse("invalid publication contract or identity");
        const auto existing=records_.find(p.sequence);
        if (existing!=records_.end()) {
            if (existing->second.raw==raw) return true;
            return refuse("conflicting immutable sequence");
        }
        if (records_.size()>=10000 || bytes_+raw.size()>(16u<<20)) return refuse("development history budget exceeded");
        const auto next=records_.lower_bound(p.sequence);
        if ((next!=records_.end() && p.published>next->second.published) ||
            (next!=records_.begin() && std::prev(next)->second.published>p.published))
            return refuse("publication clock regressed");
        bytes_+=raw.size();
        records_.emplace(p.sequence,std::move(p));
        return true;
    }
    State at(int64_t clock) const {
        if (!error_.empty()) return {nullptr,"refused",error_};
        if (clock<from_ || clock>to_) return {nullptr,"missing","outside_loaded_coverage"};
        const Publication* selected=nullptr;
        uint64_t expected=first_;
        for (const auto& [seq,p]:records_) {
            if (p.published>clock) break;
            // Out-of-order delivery is retained, but a hole cannot expose stale
            // frames or invent what was published during the missing interval.
            if (seq!=expected) return {nullptr,"missing","publication_sequence_gap"};
            expected=seq+1;
            selected=&p;
        }
        // A known hole whose next publication is still in the future is also
        // unresolved history; withhold until the contiguous prefix is restored.
        if (selected) {
            const auto next=records_.upper_bound(selected->sequence);
            if (next!=records_.end() && next->first!=expected)
                return {nullptr,"missing","publication_sequence_gap"};
        }
        if (!selected) return {};
        if (clock>=selected->expires) return {selected,"expired","recorded_frame_expired"};
        if (!selected->missing.empty()) return {selected,"missing",selected->missing};
        return {selected,"available",{}};
    }
    uint64_t first_sequence() const { return first_; }
    int64_t coverage_from() const { return from_; }
    int64_t coverage_to() const { return to_; }
    const std::map<uint64_t,Publication>& records() const { return records_; }
    const Identity& identity() const { return identity_; }
    const std::string& error() const { return error_; }
private:
    bool refuse(const char* why) { error_=why; return false; }
    Identity identity_;
    uint64_t first_;
    int64_t from_,to_;
    std::map<uint64_t,Publication> records_;
    size_t bytes_=0;
    std::string error_;
};
// Rectangles are exact immutable bounds/mass; time stops at replacement, expiry
// or the requested replay clock. Neither zoom nor scenario selection re-bins.
template<class Emit>
void rectangles(const Timeline& timeline,int64_t clock,Emit emit) {
    if (!timeline.error().empty()) return;
    clock=std::min(clock,timeline.coverage_to());
    uint64_t expected=timeline.first_sequence();
    for (auto it=timeline.records().begin();it!=timeline.records().end();++it) {
        const auto& p=it->second;
        if (it->first!=expected || p.published>=clock) break;
        ++expected;
        const auto next=std::next(it);
        if (next!=timeline.records().end() && next->first!=expected) break;
        const int64_t end=std::min({clock,p.expires,next==timeline.records().end()?clock:next->second.published});
        const int64_t begin=std::max(p.published,timeline.coverage_from());
        if (end<=begin || !p.missing.empty()) continue;
        for (const auto& band:p.bands) emit(p,band,begin,end);
    }
}
} // namespace exposure
