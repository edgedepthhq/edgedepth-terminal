#include "ui/chart_widget.h"
#include "core/app_context.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "core/research_url.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include "stream_handler.h"
#include <emscripten.h>
#include "ui/indicators/oi_indicator.h"

EM_JS(int, flow_export_assessment, (const char* text), {
    try {
        const url=URL.createObjectURL(new Blob([UTF8ToString(text)],{type:'application/json'}));
        const link=document.createElement('a'); link.href=url; link.download='edgedepth-flow-assessment.json';
        link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);return 1;
    } catch (_) { return 0; }
});

void ChartWidget::update_flow_positioning() {
    if(chart_type_!=ChartType::FlowPositioning)return;
    if(replay_selection_.active){flow_history_.reset();flow_history_.status="Release to inspect complete minutes.";return;}
    if(ctx_.replay_mgr().is_pack_mode()){flow_history_.reset();flow_history_.status="Raw contract evidence is not included in this replay pack.";return;}
    const bool replay=ctx_.replay_mgr().is_active();
    if(ctx_.replay_mgr().is_loading()){flow_history_.reset();return;}
    const int64_t clock=replay?ctx_.replay_mgr().interpolated_time_ms():static_cast<int64_t>(emscripten_date_now());
    const int64_t end=clock/60000*60000;
    int64_t from=end-5*60000,to=end;
    if(replay_selection_.start_ms!=replay_selection_.end_ms){
        // Whole minutes inside the dragged region. Never count partly selected bars.
        from=(std::min(replay_selection_.start_ms,replay_selection_.end_ms)+59999)/60000*60000;
        to=std::min(end,std::max(replay_selection_.start_ms,replay_selection_.end_ms)/60000*60000);
    } else if(replay) {
        from=std::max(from,(ctx_.replay_mgr().info().start_time_ms+59999)/60000*60000);
    }
    flow_from_=from;flow_to_=to;
    if(!Entitlements::hosted() || (!replay&&!Entitlements::is_pro())){
        flow_history_.reset();flow_history_.status="Recorded flow evidence requires a hosted Pro connection or replay.";return;
    }
    auto req=flow_history_.request(pair_,from,to,replay,emscripten_get_now());
    if(!req.empty()){
        const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
        req["data"]["entitlement_token"]=token?token:"";
        ctx_.stream_mgr().send_message(req.dump());
    }
}

void ChartWidget::render_flow_positioning() {
    using namespace Theme;
    const auto& e=flow_history_.evidence;
    const auto& a=flow_history_.assessment;
    const bool ready=flow_history_.ready && e.from==flow_from_ && e.to==flow_to_;
    const double x0=stored_x_min_,x1=stored_x_max_;
    double maximum=1,oi_min=0,oi_max=1;
    if(ready){
        for(const auto& b:e.bars)maximum=std::max({maximum,b.buy,b.sell});
        bool first=true;for(const auto& o:e.oi)if(o.contracts>0){if(first){oi_min=oi_max=o.contracts;first=false;}else{oi_min=std::min(oi_min,o.contracts);oi_max=std::max(oi_max,o.contracts);}}
        const double pad=std::max(1.0,(oi_max-oi_min)*.15);oi_min-=pad;oi_max+=pad;
    }
    const bool show_oi=ready && a.raw_count>0;
    for(int lane=0;lane<(show_oi?2:1);++lane){
        if(lane==0){
            ImGui::TextUnformatted("Traded quantity / minute");
            ImGui::SameLine(0,12);ImGui::TextColored(get_buy_color(), "Buy");
            ImGui::SameLine(0,12);ImGui::TextColored(get_sell_color(), "Sell");
        } else ImGui::TextDisabled("Open interest / contracts at original sample times");
        if(ImPlot::BeginPlot(lane==0?"##FlowAggression":"##FlowOI",ImVec2(-1,lane==0?100: 70),ImPlotFlags_NoTitle|ImPlotFlags_NoLegend|ImPlotFlags_NoMouseText|ImPlotFlags_NoMenus)){
            ImPlot::SetupAxis(ImAxis_X1,nullptr,ImPlotAxisFlags_Lock|(lane==0&&show_oi?ImPlotAxisFlags_NoTickLabels:0));
            ImPlot::SetupAxis(ImAxis_Y1,nullptr,ImPlotAxisFlags_Opposite|ImPlotAxisFlags_Lock|((lane==1&&(!ready||a.raw_count==0))?ImPlotAxisFlags_NoTickLabels|ImPlotAxisFlags_NoGridLines:0));
            ImPlot::SetupAxisLimits(ImAxis_X1,x0,x1,ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1,lane==0?0:oi_min,lane==0?maximum*1.3:oi_max,ImGuiCond_Always);
            if(lane==1||!show_oi) setup_time_axis_ticks(x0,x1);
            ImPlot::SetupAxisFormat(ImAxis_Y1,Indicators::format_oi_axis);
            ImPlot::SetupFinish();
            ImPlot::PushPlotClipRect();
            auto* dl=ImPlot::GetPlotDrawList();
            if(flow_to_>flow_from_){
                const auto left=ImPlot::PlotToPixels(double(flow_from_),0),right=ImPlot::PlotToPixels(double(flow_to_),0);
                const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();
                dl->AddRectFilled(ImVec2(left.x,pos.y),ImVec2(right.x,pos.y+size.y),u32(Tokens::TX1,0.06f));
            }
            if(ready&&lane==0){
                const auto origin=ImPlot::PlotToPixels(x0,0);
                const auto edge=ImPlot::PlotToPixels(x1,0);
                dl->AddLine(origin,edge,u32(Tokens::TX2),1);
                for(const auto& b:e.bars){
                    // Adjacent columns share a zero baseline, centered on the price candle.
                    const auto left=ImPlot::PlotToPixels(double(b.time)-22000,b.buy);
                    const auto middle=ImPlot::PlotToPixels(double(b.time),0);
                    const auto right=ImPlot::PlotToPixels(double(b.time)+22000,b.sell);
                    dl->AddRectFilled(left,ImVec2(middle.x-std::min(1.0f,(middle.x-left.x)*.15f),middle.y),get_buy_color_u32(210));
                    dl->AddRectFilled(ImVec2(middle.x+std::min(1.0f,(right.x-middle.x)*.15f),right.y),ImVec2(right.x,middle.y),get_sell_color_u32(210));
                }
            }
            if(ready&&lane==1){
                const flow_positioning::OI* previous=nullptr;
                for(const auto& o:e.oi){
                    if(o.contracts<=0){previous=nullptr;continue;}
                    const auto p=ImPlot::PlotToPixels(double(o.time),o.contracts);
                    if(previous&&o.time-previous->time<=30000)dl->AddLine(ImPlot::PlotToPixels(double(previous->time),previous->contracts),p,u32(Tokens::TX2),1);
                    dl->AddCircleFilled(p,2,u32(Tokens::TX1));previous=&o;
                }
            }
            if(ImPlot::IsPlotHovered()){
                const auto mouse=ImPlot::GetPlotMousePos();
                if(ready&&lane==0){
                    for(const auto& bar:e.bars)if(std::abs(mouse.x-double(bar.time))<30000){
                        Theme::begin_tooltip();
                        ImGui::Text("Minute starting %s",research_url::iso_utc(bar.time).c_str());
                        ImGui::Text("Buy %.6f / Sell %.6f quantity",bar.buy,bar.sell);
                        if(bar.buy+bar.sell>0)ImGui::Text("Buy share %.1f%% / Net buy quantity %+.6f",100*bar.buy/(bar.buy+bar.sell),bar.buy-bar.sell);
                        Theme::end_tooltip();break;
                    }
                }
                if(ready&&lane==1&&!e.oi.empty()){
                    auto nearest=std::min_element(e.oi.begin(),e.oi.end(),[&](const auto& l,const auto& r){return std::abs(double(l.time)-mouse.x)<std::abs(double(r.time)-mouse.x);});
                    {
                        Theme::begin_tooltip();
                        ImGui::Text("%s",research_url::iso_utc(nearest->time).c_str());
                        if(nearest->contracts>0)ImGui::Text("%.6f contracts",nearest->contracts);else ImGui::TextUnformatted("Raw contracts missing in this observation");
                        Theme::end_tooltip();
                    }
                }
            }
            ImPlot::PopPlotClipRect();ImPlot::EndPlot();
        }
    }
    ImGui::BeginChild("##FlowAssessment",ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,5));
    const auto start=research_url::iso_utc(flow_from_);
    const auto end=research_url::iso_utc(flow_to_);
    ImGui::TextWrapped("%s | %.10s  %.5s to %.5s UTC | %lld complete min",
        replay_selection_.start_ms!=replay_selection_.end_ms?"Selected move":"Latest evidence",
        start.c_str(),start.c_str()+11,end.c_str()+11,
        static_cast<long long>(std::max<int64_t>(0,flow_to_-flow_from_)/60000));
    if(!ready){ImGui::TextWrapped("%s",flow_history_.status.c_str());ImGui::TextWrapped("Shift-drag 1 to 30 complete minutes on the price chart to investigate a move.");}
    else {
        ImGui::Separator();
        const double total=a.buy+a.sell;
        if(a.complete_bars&&total>0)ImGui::TextWrapped("Price %s %.2f%%; %s.",
            a.price_pct>0?"rose":a.price_pct<0?"fell":"changed",std::abs(a.price_pct),
            a.buy/total>.55?"aggressive buying dominated":a.buy/total<.45?"aggressive selling dominated":"buying and selling were balanced");
        ImGui::TextWrapped("%s",a.hypothesis);
        if(ImGui::BeginTable("##FlowMetrics",3,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV)){
            ImGui::TableNextColumn();ImGui::TextDisabled("PRICE");
            if(a.complete_bars)ImGui::Text("%+.2f%%",a.price_pct);else ImGui::TextUnformatted("Incomplete");
            ImGui::TableNextColumn();ImGui::TextDisabled("AGGRESSOR BUY SHARE");
            if(a.complete_bars&&total>0)ImGui::Text("%.1f%%",100*a.buy/total);else ImGui::TextUnformatted("Unavailable");
            ImGui::TableNextColumn();ImGui::TextDisabled("OPEN INTEREST");
            if(a.usable_oi)ImGui::Text("%+.3f%%",a.oi_pct);else ImGui::TextUnformatted(a.raw_count?"Incomplete":"Unavailable");
            ImGui::EndTable();
        }
        char buy[32],sell[32],forced_buy[32],forced_sell[32];
        Indicators::format_oi_axis(a.buy,buy,sizeof(buy),nullptr);
        Indicators::format_oi_axis(a.sell,sell,sizeof(sell),nullptr);
        Indicators::format_oi_axis(a.forced_buy,forced_buy,sizeof(forced_buy),nullptr);
        Indicators::format_oi_axis(a.forced_sell,forced_sell,sizeof(forced_sell),nullptr);
        ImGui::TextWrapped("Traded quantity: buy %s / sell %s. Columns show only this evidence window.",buy,sell);
        ImGui::TextWrapped("Reported liquidations: buys $%s / sells $%s (%lld reports; partial coverage).",forced_buy,forced_sell,static_cast<long long>(a.report_count));
        if(!a.usable_oi)ImGui::TextWrapped("Next: inspect a period with complete raw OI to distinguish net contract growth from contraction.");
        else ImGui::TextWrapped("Next: compare the price move with OI and flow minute by minute. Net OI cannot identify individual openings or closings.");
        if(ImGui::SmallButton("Inspect evidence"))ImGui::OpenPopup("Flow evidence");
        ImGui::SameLine();
        if(ImGui::SmallButton("Save assessment")){
            auto document=flow_history_.source;
            document["version"]="terminal_flow_assessment.v1";
            document["interpretation"]=a.hypothesis;
            document["price_change_pct"]=a.price_pct;
            document["limitations"]="Retrospective stored evidence; original availability unverified. Observed liquidations are incomplete. Contract changes do not identify individual openings/closings. OI intervals are not price cells.";
            document["sources"]={"candles","open_interest.open_interest_contracts","liquidation_events"};
            flow_export_failed_=!flow_export_assessment(document.dump(2).c_str());
        }
        if(flow_export_failed_)ImGui::TextWrapped("Assessment download failed. Try again.");
        ImGui::SetNextWindowSize(ImVec2(std::min(580.0f,ImGui::GetIO().DisplaySize.x-40.0f),0),ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,12));
        ImGui::PushStyleColor(ImGuiCol_PopupBg,Tokens::PANEL);
        if(ImGui::BeginPopup("Flow evidence")){
            ImGui::Text("%s / %s",pair_.exchange.c_str(),pair_.symbol.c_str());
            ImGui::TextWrapped("%s",a.explanation);
            ImGui::TextWrapped("Exact interval: %s to %s (end exclusive)",start.c_str(),end.c_str());
            ImGui::Text("Buy %.6f / Sell %.6f quantity",a.buy,a.sell);
            ImGui::Text("Reported forced buys $%.2f / sells $%.2f",a.forced_buy,a.forced_sell);
            ImGui::Text("Raw OI %zu/%zu samples; largest gap %.1fs",a.raw_count,e.oi.size(),a.max_gap/1000.0);
            ImGui::TextWrapped("Retrospective stored evidence. Original availability is unverified. Liquidation reports cover only part of forced activity.");
            ImGui::Text("Minute bars %zu / %lld",e.bars.size(),static_cast<long long>((e.to-e.from)/60000));
            if(a.raw_count){ImGui::Text("First raw OI: %s",research_url::iso_utc(a.first_oi).c_str());ImGui::Text("Last raw OI: %s",research_url::iso_utc(a.last_oi).c_str());ImGui::Text("Last sample to selected end: %.3fs",(e.to-a.last_oi)/1000.0);}
            ImGui::TextWrapped("OI points are sampled observations, not trade-level opening/closing labels. Missing raw values are never replaced with USD notional or state refresh times.");
            ImGui::TextWrapped("Covering/unwinding wording requires a price move, falling raw OI, complete minute coverage and more than 55%% matching aggression. It is a hypothesis, not a causal finding.");
            ImGui::TextWrapped("Saved assessment includes these source rows and exact boundaries. It is a local JSON file, not a published Studio assessment.");
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
}
