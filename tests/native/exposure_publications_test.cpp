#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rendering/exposure_overlay.h"
#include "imgui_internal.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace exposure;

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream file(path); assert(file.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file,line)) lines.push_back(line);
    assert(lines.size()==56);
    return lines;
}
void rejects(const std::string& raw,const Identity& id,const std::function<void(Json&)>& change) {
    Timeline timeline(id); assert(timeline.ingest(raw));
    auto bad=Json::parse(raw); change(bad);
    assert(!timeline.ingest(bad.dump()));
    assert(std::string_view(timeline.at(INT64_MAX).status)=="refused");
    size_t count=0; rectangles(timeline,INT64_MAX,[&](auto&,auto&,auto,auto){++count;}); assert(count==0);
}
int main(int argc,char** argv) {
    assert(argc==2);
    assert(!shape(Json{{"version|sequence",1}},"version|sequence"));
    size_t series_count=0,record_count=0,rectangle_count=0;
    ImGui::CreateContext(); ImPlot::CreateContext();
    // Default hide-below is 0.01% of one side OI (5127adc); 0.009% hides, 0.01% shows.
    assert(!shown(Band{0,0,1,2,.00009},Display{}));
    assert(shown(Band{0,0,1,2,.0001},Display{}));
    assert(!shown(Band{1,0,1,2,.5},Display{0,25,1}));
    assert(band_color(.25,Display{})==ImGui::ColorConvertFloat4ToU32(ImPlot::SampleColormap(1,ImPlotColormap_Viridis)));
    assert(band_color(.0625,Display{})==ImGui::ColorConvertFloat4ToU32(ImPlot::SampleColormap(.5f,ImPlotColormap_Viridis)));
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,900); io.DeltaTime=1.f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    for (const std::string venue:{"binancef","bybit"}) for (const std::string policy:{"oldest","newest","proportional"}) {
        Identity id{venue,"BTCUSDT",policy};
        const auto lines=read_lines(std::string(argv[1])+"/"+venue+"-"+policy+".jsonl");
        Timeline timeline(id),reordered(id);
        for (const auto& line:lines) { assert(timeline.ingest(line)); assert(timeline.ingest(line)); ++record_count; }
        for (auto it=lines.rbegin();it!=lines.rend();++it) assert(reordered.ingest(*it));
        assert(timeline.records().size()==lines.size());
        const int64_t start=1800000000000;
        assert(std::string_view(timeline.at(start).status)=="before_capture");
        assert(std::string_view(timeline.at(start+25*60000+5000).status)=="missing");
        assert(timeline.at(start+25*60000+5000).reason=="trade_interval_incomplete");
        assert(std::string_view(timeline.at(start+42*60000).status)=="expired");
        assert(std::string_view(timeline.at(start+44*60000+5000).status)=="available");
        // Forward/backward arbitrary seeking; expiry is tested at its exact boundary.
        for (int direction:{1,-1}) for (int step=0;step<63;++step) {
            const int64_t clock=start+(direction==1?step:62-step)*60000+5000;
            const auto a=timeline.at(clock),b=reordered.at(clock);
            assert(std::string_view(a.status)==b.status && a.reason==b.reason);
            if (a.publication) { assert(b.publication); assert(a.publication->raw==b.publication->raw); }
        }
        for (const auto& [seq,p]:timeline.records()) {
            const auto state=timeline.at(p.published);
            assert(state.publication && state.publication->sequence==seq && state.publication->raw==lines[seq-1]);
            if (seq==40 || seq==56) assert(std::string_view(timeline.at(p.expires).status)=="expired");
            if (!p.frame.is_null()) {
                double mass[2]={p.frame["unknown_long_short"][0].get<double>(),p.frame["unknown_long_short"][1].get<double>()};
                for (const auto& band:p.bands) mass[band.side]+=band.mass;
                assert(near(mass[0],1)&&near(mass[1],1));
            }
        }
        rectangles(timeline,start+62*60000,[&](const Publication& p,const Band& band,int64_t begin,int64_t end) {
            assert(begin==p.published && end<=p.expires && end>begin);
            bool matched=false;
            for (const auto& raw_band:p.frame["bands"])
                if (raw_band["index"]==band.index && raw_band["side"]==(band.side==0?"long":"short")) {
                    assert(raw_band["low"].get<double>()==band.low && raw_band["high"].get<double>()==band.high && raw_band["mass"].get<double>()==band.mass);
                    matched=true;
                }
            assert(matched); ++rectangle_count;
        });
        // Exercise the real ImPlot renderer at multiple zooms and all absence states.
        for (int step:{0,19,25,42,44}) for (int zoom:{0,1}) {
            const int64_t clock=start+step*60000+(step==0?0:5000);
            for (int pass=0;pass<2;++pass) {
                ImGui::NewFrame(); ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("V2 QA");
                if (ImPlot::BeginPlot("price",ImVec2(1100,500))) {
                    ImPlot::SetupAxesLimits(double(start),double(start+63*60000),zoom?95:85,zoom?105:115,ImGuiCond_Always);
                    const auto limits=ImPlot::GetPlotLimits(); // Finish axis setup before measuring overlay vertices.
                    const auto before=ImPlot::GetPlotDrawList()->VtxBuffer.Size;
                    const size_t emitted=render_overlay(timeline,clock);
                    const auto after=ImPlot::GetPlotDrawList()->VtxBuffer.Size;
                    assert(after-before==int(emitted)*4);
                    // Concentration color is opaque; all shown cells keep exact bounds.
                    int vertex=before;
                    rectangles(timeline,clock,[&](auto&,const Band& band,int64_t begin,int64_t end) {
                        if (double(end)<limits.X.Min || double(begin)>limits.X.Max || band.high<limits.Y.Min || band.low>limits.Y.Max) return;
                        if (!shown(band,Display{})) return;
                        const auto color=band_color(band.mass,Display{});
                        const auto a=ImPlot::PlotToPixels(double(begin),band.high);
                        const auto b=ImPlot::PlotToPixels(double(end),band.low);
                        const ImVec2 expected[]={a,ImVec2(b.x,a.y),b,ImVec2(a.x,b.y)};
                        for (const auto& point:expected) {
                            const auto& actual=ImPlot::GetPlotDrawList()->VtxBuffer[vertex++];
                            assert(actual.pos.x==point.x && actual.pos.y==point.y && actual.col==color);
                        }
                    });
                    assert(vertex==after);
                    if (step==0) assert(emitted==0);
                    for (int i=before;i<after;++i) {
                        const auto& pos=ImPlot::GetPlotDrawList()->VtxBuffer[i].pos;
                        assert(std::isfinite(pos.x)&&std::isfinite(pos.y));
                    }
                    ImPlot::EndPlot();
                }
                render_inspection(timeline,clock);
                ImGui::End(); ImGui::Render();
            }
        }
        // Gaps can arrive in any order; do not use the previous frame until filled.
        Timeline page(id); assert(page.ingest(lines[19]));
        assert(std::string_view(page.at(start+19*60000+5000).status)=="missing");
        assert(page.at(start+19*60000+5000).reason=="publication_sequence_gap");
        size_t page_rects=0; rectangles(page,INT64_MAX,[&](auto&,auto&,auto,auto){++page_rects;}); assert(page_rects==0);
        Timeline regressed(id); assert(regressed.ingest(lines[2]));
        auto late=Json::parse(lines[1]); late["published_at_ms"]=start+130000;
        assert(!regressed.ingest(late.dump()));
        Timeline gap(id); assert(gap.ingest(lines[0])); assert(gap.ingest(lines[2]));
        assert(std::string_view(gap.at(start+125000).status)=="missing");
        assert(gap.ingest(lines[1])); assert(gap.at(start+125000).publication->sequence==3);
        Timeline tie(id); auto second=Json::parse(lines[1]); second["published_at_ms"]=Json::parse(lines[2])["published_at_ms"];
        assert(tie.ingest(lines[0])); assert(tie.ingest(second.dump())); assert(tie.ingest(lines[2]));
        assert(tie.at(start+125000).publication->sequence==3);
        rejects(lines[19],id,[](Json& j){j["version"]="exposure-publication.v2";});
        rejects(lines[19],id,[](Json& j){j["scenario_id"]="other";});
        rejects(lines[19],id,[](Json& j){j["frame"]["units"]="USD";});
        rejects(lines[19],id,[](Json& j){j["frame"]["version"]="unknown";});
        rejects(lines[19],id,[](Json& j){j["frame"]["bands"][0]["low"]=1;});
        rejects(lines[19],id,[](Json& j){j["frame"]["bands"][0]["mass"]=0.8;});
        rejects(lines[19],id,[](Json& j){j["frame"]["unknown_reasons_long_short"][0]=Json::object();});
        rejects(lines[19],id,[](Json& j){j["frame"]["scenario"]["buffer"]=nullptr;});
        rejects(lines[19],id,[](Json& j){j["frame"]["oi_trade_mark_clocks"]=Json::array();});
        rejects(lines[19],id,[](Json& j){j["missing_reason"]="missing";});
        rejects(lines[19],id,[](Json& j){j["expires_at_ms"]=0;});
        rejects(lines[19],id,[](Json& j){j["sequence"]=-1;});
        // Public-provenance modes on top of a synthetic line: "forward" (with the
        // inherited bootstrap_through_ms mark) and "bootstrap" (a recorded-history
        // minute dated by its own decision) decode; a bootstrap frame dated to any
        // other clock, or an unknown mode, is refused. Digests are structural here.
        {
            const std::string hash(64,'a');
            auto make=[&](const std::string& mode,bool historical){
                auto j=Json::parse(lines[19]);
                auto& f=j["frame"];
                const int64_t as_of=f["as_of_ms"].get<int64_t>();
                const int64_t now=as_of+(historical?7200000:1);
                f["version"]="exposure-scenario.v2"; f["source"]="inferred_public_scenario"; f["venue"]="bybit"; f["market"]="BTCUSDT";
                j["venue"]="bybit"; j["market"]="BTCUSDT";
                Json a{{"version","exposure-assumptions.v1"},{"available_at_ms",historical?now-1:as_of-1},{"valid_from_ms",historical?now-1:as_of-1},{"valid_until_ms",(historical?now-1:as_of-1)+86400000},{"interpretation","fixed_buffer_constant_maintenance_sensitivity_not_account_liquidation"},{"scenario",f["scenario"]}};
                f["public_provenance"]=Json{{"version","native-public-scenario.v1"},{"mode",mode},{"source_manifest_sha256",hash},{"input_sha256",hash},{"assumptions_sha256",hash},{"evaluated_at_ms",now},{"assumptions",a}};
                j["published_at_ms"]=historical?as_of:now; j["expires_at_ms"]=as_of+120000;
                if (!historical) f["bootstrap_through_ms"]=as_of-60000;
                return j;
            };
            Identity pid{"bybit","BTCUSDT",id.scenario};
            Publication decoded;
            assert(decode(make("forward",false).dump(),pid,decoded) && decoded.frame.contains("bootstrap_through_ms"));
            assert(decode(make("bootstrap",true).dump(),pid,decoded) && is_bootstrap(decoded.frame));
            auto wrong=make("bootstrap",true); wrong["published_at_ms"]=wrong["frame"]["as_of_ms"].get<int64_t>()+1; wrong["expires_at_ms"]=wrong["frame"]["as_of_ms"].get<int64_t>()+120000;
            assert(!decode(wrong.dump(),pid,decoded));
            auto unknown=make("forward",false); unknown["frame"]["public_provenance"]["mode"]="reconstructed";
            assert(!decode(unknown.dump(),pid,decoded));
            auto future=make("forward",false); future["frame"]["bootstrap_through_ms"]=future["frame"]["as_of_ms"].get<int64_t>()+1;
            assert(!decode(future.dump(),pid,decoded));
        }
        Publication decoded;
        assert(!decode("{",id,decoded)); assert(!decode("[]",id,decoded));
        assert(!decode("{\"version\":\"bad\","+lines[0].substr(1),id,decoded));
        Timeline conflict(id); assert(conflict.ingest(lines[0])); assert(!conflict.ingest(lines[0]+" "));
        ++series_count;
    }
    ImPlot::DestroyContext(); ImGui::DestroyContext();
    std::cout<<"PASS: "<<series_count<<" series, "<<record_count<<" exact publications, "<<rectangle_count<<" conserved rectangles; seeks, states, retries, order, refusal and ImPlot mesh\n";
}
