#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rendering/census_overlay.h"
#include "types/types.h"
#include <cassert>
#include <emscripten.h>
namespace Theme {
void begin_tooltip(){ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(8,8));ImGui::BeginTooltip();}
void end_tooltip(){ImGui::EndTooltip();ImGui::PopStyleVar();}
}
using Frame=Terminal::LiquidationHeatmapSnapshot;
static Frame fixture(bool saga,int64_t ts) {
    Frame f;f.timestamp_ms=ts;f.census_observed_at_ms=ts-27000;
    f.mark_price=saga?.023:75600;f.band_width_pct=.25;f.census_status="sampled";
    auto& q=f.census_quality;q.version=2;q.venue_received_at_ms=ts-9000;
    q.sampled_positions=46;q.usable_positions=6;q.unlocated_positions=40;
    q.unlocated_notional_usd=737258;q.weighted_wallet_age_ms=27000;q.p95_wallet_age_ms=27000;
    const double prices[]={.97,.985,.99,1.01,1.02,1.035};
    for(int i=0;i<6;++i) {
        Terminal::LiquidationBand b{};
        b.price_mid=std::exp(std::round(std::log(f.mark_price*prices[i])/std::log1p(.0025))*std::log1p(.0025));
        const double usd=(i==1?25000000:double((i+1)*2000000));
        if(i<3){b.est_long_usd=usd;f.total_long_risk_usd+=usd;}
        else{b.est_short_usd=usd;f.total_short_risk_usd+=usd;}
        f.bands.push_back(b);q.usable_notional_usd+=usd;
    }
    q.sampled_notional_usd=q.usable_notional_usd+q.unlocated_notional_usd;
    q.coverage_denominator_usd=5508043936;f.flow_intensity=q.usable_notional_usd/q.coverage_denominator_usd;
    return f;
}
int main() {
    ImGui::CreateContext();ImPlot::CreateContext();
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.0f/60;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsAlpha8(&pixels,&w,&h);
    EM_ASM({globalThis.qa=({atlas:{w:$1,h:$2,alpha:Array.from(HEAPU8.subarray($0,$0+$1*$2))},frames:[]});},pixels,w,h);
    const int64_t now=1789508340000LL;
    for(int scenario=0;scenario<5;++scenario) {
        const bool saga=scenario==1||scenario==2;
        auto f=fixture(saga,now-10000);
        census_history::History<Frame> history;
        auto old=f;old.timestamp_ms=now-300000;old.census_observed_at_ms=old.timestamp_ms-27000;
        old.census_quality.venue_received_at_ms=old.timestamp_ms-9000;
        old.census_quality.version=1;assert(history.insert(old));
        if(scenario==1) { // SAGA zero located must not retain the historical current profile.
            f.bands.clear();f.census_status="unavailable";f.census_observed_at_ms=0;
            f.census_quality.usable_positions=0;f.census_quality.unlocated_positions=46;
            f.census_quality.usable_notional_usd=0;f.census_quality.sampled_notional_usd=737258;
            f.census_quality.weighted_wallet_age_ms=0;f.census_quality.p95_wallet_age_ms=0;
            f.total_long_risk_usd=0;f.total_short_risk_usd=0;f.flow_intensity=0;
        } else if(scenario==2) { // $13 observation is filtered explicitly, never brightened.
            f.bands.resize(1);f.bands[0].est_long_usd=0;f.bands[0].est_short_usd=13;
            f.census_quality.usable_positions=1;f.census_quality.unlocated_positions=45;
            f.census_quality.usable_notional_usd=13;f.census_quality.sampled_notional_usd=737271;
            f.total_long_risk_usd=0;f.total_short_risk_usd=13;f.flow_intensity=13/f.census_quality.coverage_denominator_usd;
        }
        assert(history.insert(f));
        auto totals=census_profile::build(f,0,1e12,0);
        assert(totals.long_usd==f.total_long_risk_usd&&totals.short_usd==f.total_short_risk_usd);
        const int64_t asof=scenario==3?now+180000:now;
        assert(census_profile::state(history.at(asof),asof)==(scenario==1?census_profile::State::unavailable:scenario==3?census_profile::State::stale:census_profile::State::ready));
        io.DisplaySize=ImVec2(scenario==4?700:1280,720);
        for(int pass=0;pass<2;++pass) {
            ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Synthetic HL exposure QA",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("SYNTHETIC FIXTURE | production HL renderer");
            if(ImPlot::BeginPlot("##price",ImVec2(-1,-1),ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes(nullptr,nullptr,ImPlotAxisFlags_NoTickLabels,ImPlotAxisFlags_Opposite);
                ImPlot::SetupAxesLimits(double(now-600000),double(now+120000),f.mark_price*.95,f.mark_price*1.05,ImGuiCond_Always);
                double xs[]={double(now-600000),double(now-400000),double(now-200000),double(now)},ys[]={f.mark_price*.98,f.mark_price*1.015,f.mark_price*.99,f.mark_price};
                ImPlot::PlotLine("Synthetic price",xs,ys,4);
                census_overlay::render(&history,asof,scenario==1||scenario==3,1000,saga?"SAGA":"BTC",true,saga?"%.5f":"%.1f",
                    [](int64_t ts,char* out,size_t size){std::snprintf(out,size,"as of %lld UTC",static_cast<long long>(ts));});
                ImPlot::EndPlot();
            }
            ImGui::End();ImGui::Render();
        }
        auto* data=ImGui::GetDrawData();assert(data->TotalVtxCount>0);
        EM_ASM({qa.frames.push({scenario:$0,width:$1,height:$2,lists:[]});},scenario,int(io.DisplaySize.x),int(io.DisplaySize.y));
        for(int i=0;i<data->CmdListsCount;++i) {
            const auto* list=data->CmdLists[i];
            EM_ASM({const verts=[];for(let i=0;i<$1;i++){const p=$0+i*20;verts.push([HEAPF32[p/4],HEAPF32[p/4+1],HEAPF32[p/4+2],HEAPF32[p/4+3],HEAPU32[p/4+4]]);}qa.frames.at(-1).lists.push({verts,indices:Array.from(HEAPU16.subarray($2/2,$2/2+$3)),commands:[]});},list->VtxBuffer.Data,list->VtxBuffer.Size,list->IdxBuffer.Data,list->IdxBuffer.Size);
            for(const auto& cmd:list->CmdBuffer) {
                assert(!cmd.UserCallback);
                EM_ASM({qa.frames.at(-1).lists.at(-1).commands.push({clip:[$0,$1,$2,$3],count:$4,offset:$5,vertex:$6});},cmd.ClipRect.x,cmd.ClipRect.y,cmd.ClipRect.z,cmd.ClipRect.w,cmd.ElemCount,cmd.IdxOffset,cmd.VtxOffset);
            }
        }
    }
    EM_ASM({require('fs').writeFileSync('census-overlay-mesh.json',JSON.stringify(qa));});
    ImPlot::DestroyContext();ImGui::DestroyContext();
    std::puts("PASS: actual renderer handles BTC, zero/$13 SAGA, stale history and narrow charts");
}
