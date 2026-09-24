#pragma once
#include "core/exposure_page.h"

namespace exposure {
inline constexpr int64_t summary_max_range=90LL*24*60*60*1000;
struct SummaryScope {
    Identity identity;
    int64_t from=0,to=0,width=0,candle_period=0;
    bool operator==(const SummaryScope& other) const {
        return identity.venue==other.identity.venue && identity.market==other.identity.market &&
            identity.scenario==other.identity.scenario && from==other.from && to==other.to &&
            width==other.width && candle_period==other.candle_period;
    }
};
struct SummaryCell {
    Band band; // Same fixed grid. mass is the mean over the ENTIRE column.
    double mass_ms=0;
    int64_t active_ms=0; // Time this bin was reported, not one cohort's survival.
};
struct SummaryColumn {
    int64_t start=0,end=0,exposure=0,flat=0;
    std::array<int64_t,4> unavailable{}; // missing, expired, before_capture, not_retained
    std::array<double,2> unknown{}; // Fraction * milliseconds, not fraction alone.
    std::vector<SummaryCell> cells;
    int64_t duration() const { return end-start; }
    int64_t missing() const { return duration()-exposure-flat; }
};
// Display-only averages. Never ingest these into the exact publication Timeline.
struct Summary {
    SummaryScope scope;
    std::string raw,start_coverage;
    uint64_t first=0,anchor=0,through=0;
    Json definition;
    std::vector<SummaryColumn> columns;
};
inline bool summary_definition(const Json& d,const Identity& id) {
    if (d.is_null()) return true; // No frame in the pinned stream is explicit.
    if (!shape(d,"version|source|units|grid|scenario|provenance_version|assumption_version|assumption_interpretation") ||
        d.value("units",Json())!="fraction_of_one_side_oi" || d.value("grid",Json())!="log_1.0025_anchor_1.v1" || !d.contains("scenario")) return false;
    const bool synthetic=d.value("version",Json())=="exposure-scenario.v1" && d.value("source",Json())=="inferred_synthetic";
    if (synthetic) {
        if (d.contains("provenance_version") || d.contains("assumption_version") || d.contains("assumption_interpretation")) return false;
    } else if (d.value("version",Json())!="exposure-scenario.v2" || d.value("source",Json())!="inferred_public_scenario" ||
        (id.venue!="bybit" && id.venue!="binancef") || (id.market!="BTCUSDT" && id.market!="ETHUSDT" && id.market!="SOLUSDT") ||
        d.value("provenance_version",Json())!="native-public-scenario.v1" ||
        d.value("assumption_version",Json())!="exposure-assumptions.v1" ||
        d.value("assumption_interpretation",Json())!="fixed_buffer_constant_maintenance_sensitivity_not_account_liquidation") return false;
    const auto& s=d["scenario"];
    return shape(s,"id|survival|turnover|buffer|maintenance|retain_cohorts") && s.value("id",Json())==id.scenario &&
        (!s.contains("retain_cohorts") || (s["retain_cohorts"].is_number_integer() && s["retain_cohorts"]>=1000 && s["retain_cohorts"]<=40000)) &&
        (s.value("survival",Json())=="oldest" || s.value("survival",Json())=="newest" || s.value("survival",Json())=="proportional") &&
        number(s,"turnover") && s["turnover"]>=0 && s["turnover"]<=1 && number(s,"buffer") && number(s,"maintenance") &&
        s["maintenance"]>=0 && s["maintenance"]<1 && s["buffer"]>0 && s["buffer"].get<double>()<1-s["maintenance"].get<double>();
}
inline std::unique_ptr<Summary> decode_summary(const std::string& raw,const SummaryScope& scope,int64_t cutoff,std::string& error) {
    auto refuse=[&](const char* reason)->std::unique_ptr<Summary> { error=reason;return nullptr; };
    error.clear();
    if (raw.empty() || raw.size()>(16u<<20) || scope.identity.venue.empty() || scope.identity.market.empty() || scope.identity.scenario.empty() ||
        scope.from<0 || scope.to<=scope.from || scope.to-scope.from>summary_max_range || scope.to>cutoff ||
        scope.candle_period<=0 || scope.width<scope.candle_period || scope.width>summary_max_range || scope.width%scope.candle_period!=0 ||
        scope.to>INT64_MAX-scope.width) return refuse("Invalid summary scope, cutoff or byte budget");
    const auto count=(scope.to-1)/scope.width-scope.from/scope.width+1;
    if (count>1024) return refuse("Summary exceeds 1024 time columns");
    bool duplicate=false;
    std::vector<std::set<std::string>> keys;
    const auto j=Json::parse(raw,[&](int,Json::parse_event_t event,Json& value) {
        if (event==Json::parse_event_t::object_start) keys.emplace_back();
        if (event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) duplicate=true;
        if (event==Json::parse_event_t::object_end) keys.pop_back();
        return true;
    },false);
    if (duplicate || !shape(j,"version|venue|market|scenario_id|from_ms|to_ms|width_ms|candle_period_ms|start_coverage|first_sequence|anchor_sequence|through_sequence|definition|columns|complete") ||
        j.value("version",Json())!="exposure-summary.v1" || j.value("venue",Json())!=scope.identity.venue ||
        j.value("market",Json())!=scope.identity.market || j.value("scenario_id",Json())!=scope.identity.scenario ||
        !integer(j,"from_ms") || j["from_ms"]!=scope.from || !integer(j,"to_ms",1) || j["to_ms"]!=scope.to ||
        !integer(j,"width_ms",1) || j["width_ms"]!=scope.width || !integer(j,"candle_period_ms",1) || j["candle_period_ms"]!=scope.candle_period ||
        j.value("complete",Json())!=true || !text(j,"start_coverage") || !j.contains("definition") || !summary_definition(j["definition"],scope.identity) ||
        !j.contains("columns") || !j["columns"].is_array() || j["columns"].size()!=size_t(count)) return refuse("Invalid or incomplete summary contract");
    auto result=std::make_unique<Summary>();result->scope=scope;result->raw=raw;result->definition=j["definition"];
    result->start_coverage=j["start_coverage"].get<std::string>();
    if (!sequence_string(j,"first_sequence",result->first) || !sequence_string(j,"anchor_sequence",result->anchor) || !sequence_string(j,"through_sequence",result->through) ||
        (result->through==0 ? result->first!=0 || result->anchor!=0 : result->first==0 || result->first>result->through)) return refuse("Invalid summary sequence range");
    if (result->start_coverage=="anchored") {
        if (result->anchor==0 || result->first!=result->anchor) return refuse("Summary anchor mismatch");
    } else if (result->start_coverage=="before_capture") {
        if (result->anchor!=0 || (result->through>0 && result->first!=1)) return refuse("Invalid capture prefix");
    } else if (result->start_coverage!="not_retained" || result->anchor!=0) return refuse("Invalid retention prefix");
    if (result->through==0 && !result->definition.is_null()) return refuse("Empty summary cannot assert a model definition");
    result->columns.reserve(size_t(count));int64_t next=scope.from;size_t cells=0;
    const char* reasons[]={"missing","expired","before_capture","not_retained"};
    for (const auto& c:j["columns"]) {
        SummaryColumn column;
        const int64_t expected_end=std::min(scope.to,(next/scope.width+1)*scope.width);
        if (!shape(c,"start_ms|end_ms|exposure_ms|flat_ms|unavailable_ms|unknown_mass_ms|cells") ||
            !integer(c,"start_ms") || c["start_ms"]!=next || !integer(c,"end_ms",1) || c["end_ms"]!=expected_end ||
            !integer(c,"exposure_ms") || c["exposure_ms"]>expected_end-next || !integer(c,"flat_ms") || c["flat_ms"]>expected_end-next ||
            !c.contains("unavailable_ms") || !shape(c["unavailable_ms"],"missing|expired|before_capture|not_retained") ||
            !c.contains("unknown_mass_ms") || !c["unknown_mass_ms"].is_array() || c["unknown_mass_ms"].size()!=2 ||
            !c.contains("cells") || !c["cells"].is_array() || cells+c["cells"].size()>65536) return refuse("Invalid summary column or cell budget");
        column.start=next;column.end=expected_end;column.exposure=c["exposure_ms"].get<int64_t>();column.flat=c["flat_ms"].get<int64_t>();
        int64_t duration=column.exposure+column.flat;
        for (int i=0;i<4;++i) {
            if (!integer(c["unavailable_ms"],reasons[i]) || c["unavailable_ms"][reasons[i]]>column.duration()) return refuse("Invalid unavailable duration");
            column.unavailable[i]=c["unavailable_ms"][reasons[i]].get<int64_t>();duration+=column.unavailable[i];
        }
        if (duration!=column.duration() || (result->definition.is_null() && column.exposure+column.flat!=0)) return refuse("Summary duration does not conserve");
        if (result->through==0 && column.unavailable[result->start_coverage=="before_capture"?2:3]!=column.duration()) return refuse("Empty stream has inconsistent coverage");
        std::array<double,2> totals{};
        for (int side=0;side<2;++side) {
            const auto& u=c["unknown_mass_ms"][side];
            if (!u.is_number() || !std::isfinite(u.get<double>()) || u<0 || u>column.exposure) return refuse("Invalid unlocated inventory integral");
            totals[side]=column.unknown[side]=u.get<double>();
        }
        std::set<std::pair<int,int64_t>> seen;
        column.cells.reserve(c["cells"].size());
        for (const auto& cell:c["cells"]) {
            if (!shape(cell,"side|index|mass_ms|active_ms") || (cell.value("side",Json())!="long" && cell.value("side",Json())!="short") ||
                !integer(cell,"index",INT64_MIN) || !number(cell,"mass_ms") || cell["mass_ms"]<=0 ||
                !integer(cell,"active_ms",1) || cell["active_ms"]>column.exposure || cell["mass_ms"].get<double>()>cell["active_ms"].get<double>()+1e-8*std::max(1.,cell["active_ms"].get<double>())) return refuse("Invalid summary bin integral");
            const int side=cell["side"]=="long"?0:1;
            const int64_t index=cell["index"].get<int64_t>();
            const double center=std::pow(1.0025,double(index)),low=center/std::sqrt(1.0025),high=center*std::sqrt(1.0025);
            if (!std::isfinite(low) || !std::isfinite(high) || low<=0 || high<=low || !seen.emplace(side,index).second) return refuse("Invalid or duplicate fixed price bin");
            const double mass=cell["mass_ms"].get<double>();totals[side]+=mass;
            column.cells.push_back({{side,index,low,high,mass/double(column.duration())},mass,cell["active_ms"].get<int64_t>()});++cells;
        }
        for (const auto sum:totals) if (!std::isfinite(sum) || std::abs(sum-double(column.exposure))>1e-8*std::max(1.,double(column.exposure))) return refuse("Summary mass does not conserve");
        next=column.end;result->columns.push_back(std::move(column));
    }
    return result;
}
// Refuse a future-containing aggregate in full, not just its visible tail. The
// server must recompute partial replay columns BEFORE aggregating source frames.
inline bool summary_at(const Summary& summary,int64_t cutoff) { return summary.scope.to<=cutoff; }
} // namespace exposure
