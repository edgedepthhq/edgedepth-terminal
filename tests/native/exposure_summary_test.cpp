#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/exposure_summary_history.h"
#include "rendering/exposure_summary_overlay.h"
#include "imgui_internal.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace exposure;
std::string read(const std::string& path) { std::ifstream f(path);assert(f.good());return {(std::istreambuf_iterator<char>(f)),{}}; }
SummaryScope scope_for(const Json& j) { return {{j["venue"],j["market"],j["scenario_id"]},j["from_ms"],j["to_ms"],j["width_ms"],j["candle_period_ms"]}; }
void close_number(double a,double b) { assert(std::abs(a-b)<=1e-8*std::max({1.,std::abs(a),std::abs(b)})); }
// Independent source-interval oracle: walk every publication/expiry boundary,
// including those inside a display column. No client-side production aggregator.
void verify_integrals(const Summary& s,const Json& originals) {
    std::vector<Publication> publications;
    for (const auto& raw:originals) {
        Publication p;assert(decode(raw.get<std::string>(),s.scope.identity,p));
        if (p.published<=s.scope.to) publications.push_back(p);
    }
    for (const auto& column:s.columns) {
        std::set<int64_t> edges{column.start,column.end};
        for (const auto& p:publications) for (auto t:{p.published,p.expires}) if (t>column.start && t<column.end) edges.insert(t);
        int64_t exposure=0,flat=0;std::array<int64_t,4> unavailable{};std::array<double,2> unknown{};
        std::map<std::pair<int,int64_t>,std::pair<double,int64_t>> cells;
        for (auto edge=edges.begin();std::next(edge)!=edges.end();++edge) {
            const int64_t dt=*std::next(edge)-*edge;const Publication* selected=nullptr;
            for (const auto& p:publications) if (p.published<=*edge && (!selected || p.sequence>selected->sequence)) selected=&p;
            if (!selected) { unavailable[s.start_coverage=="not_retained"?3:2]+=dt;continue; }
            if (*edge>=selected->expires) { unavailable[1]+=dt;continue; }
            if (!selected->missing.empty()) { unavailable[0]+=dt;continue; }
            if (selected->frame["oi_one_side_base"]==0) { flat+=dt;continue; }
            exposure+=dt;
            for (int side=0;side<2;++side) unknown[side]+=selected->frame["unknown_long_short"][side].get<double>()*dt;
            for (const auto& band:selected->bands) { auto& c=cells[{band.side,band.index}];c.first+=band.mass*dt;c.second+=dt; }
        }
        assert(exposure==column.exposure && flat==column.flat && unavailable==column.unavailable && cells.size()==column.cells.size());
        for (int side=0;side<2;++side) close_number(unknown[side],column.unknown[side]);
        for (const auto& cell:column.cells) {
            const auto expected=cells.at({cell.band.side,cell.band.index});close_number(expected.first,cell.mass_ms);assert(expected.second==cell.active_ms);
            close_number(cell.band.mass,cell.mass_ms/column.duration());
            const double center=std::pow(1.0025,double(cell.band.index));
            assert(cell.band.low==center/std::sqrt(1.0025) && cell.band.high==center*std::sqrt(1.0025));
        }
    }
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string dir=argv[1];
    ImGui::CreateContext();ImPlot::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1200,900);io.DeltaTime=1.f/60;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    std::string error;size_t column_count=0,cell_count=0;
    for (const std::string name:{"four-hour","daily","daily-partial-replay","utc-boundary-missing","sparse-90d-four-hour","not-retained","empty-not-retained","positive-unknown"}) {
        const auto raw=read(dir+"/"+name+".json");const auto j=Json::parse(raw);const auto scope=scope_for(j);
        auto summary=decode_summary(raw,scope,scope.to,error);assert(summary && error.empty());assert(summary->raw==raw);
        verify_integrals(*summary,Json::parse(read(dir+"/"+(name=="four-hour" || name=="daily" || name=="daily-partial-replay"?"publications":name+"-publications")+".json")));
        column_count+=summary->columns.size();for (const auto& c:summary->columns) cell_count+=c.cells.size();
        assert(!decode_summary(raw,scope,scope.to-1,error));assert(!summary_at(*summary,scope.to-1));
        if (name=="positive-unknown") {
            double unknown=0;int64_t missing=0;size_t located=0;
            for (const auto& c:summary->columns) { unknown+=c.unknown[0]+c.unknown[1];missing+=c.unavailable[0];located+=c.cells.size(); }
            assert(unknown>0 && missing>0 && located>0);
        }
        if (name=="sparse-90d-four-hour") assert(summary->columns.size()==540);
        for (int zoom:{0,1}) for (int pass=0;pass<2;++pass) {
            ImGui::NewFrame();ImGui::SetNextWindowSize(io.DisplaySize);ImGui::Begin("Summary QA");
            if (ImPlot::BeginPlot("Summary",ImVec2(1100,550))) {
                ImPlot::SetupAxesLimits(double(scope.from),double(scope.to),zoom?75:60,zoom?120:160,ImGuiCond_Always);
                const auto limits=ImPlot::GetPlotLimits();auto* draw=ImPlot::GetPlotDrawList();const int before=draw->VtxBuffer.Size;
                const Display display{0,25,0};const auto emitted=render_summary_overlay(*summary,scope.to,display);size_t expected=0;
                for (const auto& c:summary->columns) for (const auto& cell:c.cells) if (cell.band.high>=limits.Y.Min && cell.band.low<=limits.Y.Max) {
                    ++expected;const auto a=ImPlot::PlotToPixels(double(c.start),cell.band.high),b=ImPlot::PlotToPixels(double(c.end),cell.band.low);
                    const auto color=band_color(cell.band.mass,display);bool exact=false;
                    for (int v=before;v+3<draw->VtxBuffer.Size;++v) {
                        const auto& x=draw->VtxBuffer[v];const auto& y=draw->VtxBuffer[v+2];
                        if (x.pos.x==a.x && x.pos.y==a.y && y.pos.x==b.x && y.pos.y==b.y && x.col==color && y.col==color) { exact=true;break; }
                    }
                    assert(exact);
                }
                assert(emitted==expected);
                if (name=="empty-not-retained") {
                    const float strip=ImPlot::GetPlotPos().y+ImPlot::GetPlotSize().y-7;
                    for (int v=before;v<draw->VtxBuffer.Size;++v) assert(draw->VtxBuffer[v].pos.y>=strip);
                }
                const auto after=draw->VtxBuffer.Size;assert(render_summary_overlay(*summary,scope.to-1,display)==0);assert(draw->VtxBuffer.Size==after);
                ImPlot::EndPlot();
            }
            render_summary_inspection(*summary);ImGui::End();ImGui::Render();
        }
    }
    auto golden=Json::parse(read(dir+"/four-hour.json"));const auto scope=scope_for(golden);
    for (int m=0;m<16;++m) {
        auto bad=golden;
        if (m==0) bad["complete"]=false;
        if (m==1) bad["columns"][0]["end_ms"]=scope.to-1;
        if (m==2) bad["columns"][0]["exposure_ms"]=0;
        if (m==3) bad["columns"][0]["unknown_mass_ms"][0]=1;
        if (m==4) bad["columns"][0]["cells"][0]["mass_ms"]=-1;
        if (m==5) bad["columns"][0]["cells"][0]["active_ms"]=0;
        if (m==6) bad["columns"][0]["cells"].push_back(bad["columns"][0]["cells"][0]);
        if (m==7) bad["columns"][0]["cells"][0]["index"]=INT64_MAX;
        if (m==8) bad["first_sequence"]="01";
        if (m==9) bad["first_sequence"]="9223372036854775808";
        if (m==10) bad["definition"]["grid"]="moving";
        if (m==11) bad["definition"]=nullptr;
        if (m==12) bad["market"]="ETHUSDT";
        if (m==13) bad["columns"][0]["unavailable_ms"]["invented"]=0;
        if (m==14) bad["columns"][0]["unavailable_ms"]["expired"]=-1;
        if (m==15) bad["anchor_sequence"]="1";
        assert(!decode_summary(bad.dump(),scope,scope.to,error));
    }
    assert(!decode_summary("{\"complete\":false,"+golden.dump().substr(1),scope,scope.to,error));
    assert(!decode_summary(std::string((16u<<20)+1,' '),scope,scope.to,error));
    auto too_many=scope;too_many.to=too_many.from+summary_max_range+1;assert(!decode_summary(golden.dump(),too_many,too_many.to,error));
    too_many=scope;too_many.from=0;too_many.to=1025;too_many.width=too_many.candle_period=1;assert(!decode_summary(golden.dump(),too_many,too_many.to,error));
    auto too_dense=golden;too_dense["columns"][0]["cells"]=Json::array();
    const auto cell=golden["columns"][0]["cells"][0];for (int i=0;i<65537;++i) too_dense["columns"][0]["cells"].push_back(cell);
    assert(!decode_summary(too_dense.dump(),scope,scope.to,error));
    assert(summary_width(0,summary_max_range,14400000)==14400000);
    assert(summary_width(0,summary_max_range,86400000)==86400000);
    assert(summary_width(0,summary_max_range+1,60000)==0);
    assert(!automatic_summary(0,1800000,60000));
    assert(automatic_summary(0,1800001,60000));
    assert(automatic_summary(0,3600000,3600000));
    const auto alpha=(band_color(.25,Display{})>>IM_COL32_A_SHIFT)&255;
    assert(alpha>=114 && alpha<=115);
    SummaryHistory history;
    auto response=[&](const Json& q){return Json{{"type","exposure_v2_summary"},{"request_id",q["data"]["request_id"]},{"mode","replay"},{"summary",golden}};};
    const auto a=history.poll(scope,scope.to,true,1000);assert(a["method"]=="get_exposure_v2_summary");
    SummaryHistory::receive(response(a),false);assert(!history.view(scope.to));
    SummaryHistory::receive(response(a),true);assert(history.view(scope.to));assert(!history.view(scope.to-1));
    assert(history.poll(scope,scope.to,true,2000).is_null());
    const auto* retained=history.view(scope.to);
    auto later=scope;later.to+=1000;
    const auto refresh=history.poll(later,later.to,true,7000);
    assert(!refresh.is_null() && history.view(later.to)==retained);
    assert(history.view(later.to)->scope.to==scope.to); // Never stretch coverage.
    auto refreshed=response(refresh);
    refreshed["summary"]["to_ms"]=later.to;
    auto& last=refreshed["summary"]["columns"].back();last["end_ms"]=later.to;
    last["unavailable_ms"]["expired"]=last["unavailable_ms"]["expired"].get<int64_t>()+1000;
    SummaryHistory::receive(refreshed,true);
    assert(history.view(later.to) && history.view(later.to)->scope.to==later.to);
    retained=history.view(later.to);
    // A busy refresh keeps the validated display without extending coverage.
    auto busy_scope=later;busy_scope.to+=1000;
    const auto busy_refresh=history.poll(busy_scope,busy_scope.to,true,14000);
    SummaryHistory::receive({{"request_id",busy_refresh["data"]["request_id"]},{"error","busy"}},true);
    assert(history.view(busy_scope.to)==retained && retained->scope.to==later.to);
    // A different candle interval must never render the previous interval's grid.
    auto switched=later;switched.candle_period*=2;switched.width*=2;
    const auto switch_request=history.poll(switched,busy_scope.to,true,14100);
    assert(!switch_request.is_null() && !history.view(later.to));
    SummaryHistory::receive(response(busy_refresh),true);assert(!history.view(later.to));
    const auto restored=history.poll(scope,busy_scope.to,true,14200);
    SummaryHistory::receive(response(restored),true);assert(history.view(later.to));
    retained=history.view(later.to);
    auto pan_scope=later;pan_scope.from+=1000;
    const auto pan=history.poll(pan_scope,busy_scope.to,true,14300);
    assert(!pan.is_null() && history.view(later.to)==retained);
    SummaryHistory::receive(response(refresh),true);assert(history.view(later.to)==retained); // Cancelled refresh.
    SummaryHistory::receive({{"request_id",pan["data"]["request_id"]},{"error","pro_required"}},true);
    assert(!history.view(later.to));
    history.reset();
    SummaryHistory::invalidate_all("disconnect");assert(!history.view(scope.to));
    SummaryHistory::receive(response(a),true);assert(!history.view(scope.to));
    const auto b=history.poll(scope,scope.to,true,3000);
    auto backwards=scope;backwards.to-=1000;history.poll(backwards,backwards.to,true,3100);
    SummaryHistory::receive(response(b),true);assert(!history.view(scope.to));
    history.reset();const auto busy=history.poll(scope,scope.to,true,4000);
    SummaryHistory::receive({{"request_id",busy["data"]["request_id"]},{"error","busy"}},true);
    assert(history.poll(scope,scope.to,true,4500).is_null());assert(!history.poll(scope,scope.to,true,5000).is_null());
    history.poll(scope,scope.to,true,15000);assert(history.status().find("timed out")!=std::string::npos);
    ImPlot::DestroyContext();ImGui::DestroyContext();
    std::cout<<"PASS: 8 Go summary fixtures, "<<column_count<<" columns, "<<cell_count<<" exact mean cells; source-integral oracle, ImPlot mesh, replay cutoff, coverage, caps and stale-request guards\n";
}
