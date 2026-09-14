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
#include "ui/research_moment_panel.h"

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
        to=std::max(replay_selection_.start_ms,replay_selection_.end_ms)/60000*60000;
    } else if(replay) {
        from=std::max(from,(ctx_.replay_mgr().info().start_time_ms+59999)/60000*60000);
    }
    flow_from_=from;flow_to_=to;
    if(to>end){flow_history_.reset();flow_history_.status="Selected interval extends beyond the latest complete minute or replay playhead. Seek forward or select an earlier interval.";return;}
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

namespace {
void flow_time(int64_t epoch,char* out,size_t size) {
    DisplayTimeZone::instance().format(epoch,TimeZoneFormat::FullInspection,out,size);
}
void flow_split(double left,double right,const char* label) {
    const auto pos=ImGui::GetCursorScreenPos();
    const float width=ImGui::GetContentRegionAvail().x;
    const float radius=43.0f;
    const ImVec2 center(pos.x+width*.5f,pos.y+54.0f);
    ImGui::Dummy(ImVec2(width,108));
    auto* draw=ImGui::GetWindowDrawList();
    const double total=left+right;
    draw->AddCircle(center,radius,Theme::u32(Theme::Tokens::BD1),64,12.0f);
    if(total>0){
        constexpr float pi=3.14159265358979323846f;
        const float start=-pi*.5f,split=start+2*pi*float(left/total);
        if(left>0){draw->PathArcTo(center,radius,start,split,64);draw->PathStroke(Theme::get_buy_color_u32(210),0,12.0f);}
        if(right>0){draw->PathArcTo(center,radius,split,start+2*pi,64);draw->PathStroke(Theme::get_sell_color_u32(210),0,12.0f);}
    }
    char value[32]{};
    if(total>0)snprintf(value,sizeof(value),"%.1f%%",100*left/total);
    else snprintf(value,sizeof(value),"No volume");
    const auto size=ImGui::CalcTextSize(value);
    draw->AddText(ImVec2(center.x-size.x*.5f,center.y-size.y*.5f),Theme::u32(Theme::Tokens::TX1),value);
    ImGui::TextWrapped("%s",label);
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
    // At most 240 paired columns. Each column remains one original complete
    // minute; no display downsampling rewrites quantities or OI observations.
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
            const auto plot_pos=ImPlot::GetPlotPos(),plot_size=ImPlot::GetPlotSize();
            crosshair_state_.last_plot_max=ImVec2(plot_pos.x+plot_size.x,plot_pos.y+plot_size.y);
            if(ImPlot::IsPlotHovered()){
                crosshair_state_.is_active=true;crosshair_state_.indicator_hovered=true;crosshair_state_.chart_hovered=false;
                crosshair_state_.plot_pos=ImPlot::GetPlotMousePos();
                crosshair_state_.hovered_plot_min=plot_pos;crosshair_state_.hovered_plot_max=crosshair_state_.last_plot_max;
                crosshair_state_.hovered_y_min=lane==0?0:oi_min;
                crosshair_state_.hovered_y_max=lane==0?maximum*1.3:oi_max;
                crosshair_state_.indicator_y_formatter=Indicators::format_oi_axis;
            }
            ImPlot::PopPlotClipRect();ImPlot::EndPlot();
        }
    }
    ImGui::BeginChild("##FlowAssessment",ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,5));
    char start[128]{},end[128]{};flow_time(flow_from_,start,sizeof(start));flow_time(flow_to_,end,sizeof(end));
    const bool selected=replay_selection_.start_ms!=replay_selection_.end_ms;
    ImGui::TextWrapped("%s | %s to %s | %lld complete min",selected?"Selected move":"Latest evidence",start,end,
        static_cast<long long>(std::max<int64_t>(0,flow_to_-flow_from_)/60000));
    if(selected){if(ImGui::SmallButton("Latest 5 minutes")){replay_selection_.start_ms=replay_selection_.end_ms=0;}}

    if(!ready){ImGui::TextWrapped("%s",flow_history_.status.c_str());ImGui::TextWrapped("Shift-drag 1 minute to 4 hours on the price chart. Only complete minutes are assessed.");}
    else {
        ImGui::Separator();
        const double total=a.buy+a.sell;
        if(a.complete_bars&&total>0)ImGui::TextWrapped("Price %s %.2f%%; aggressive buys were %.1f%% of traded quantity.",
            a.price_pct>0?"rose":a.price_pct<0?"fell":"changed",std::abs(a.price_pct),100*a.buy/total);
        if(!a.complete_bars)ImGui::TextWrapped("%s",a.explanation);
        if(!a.usable_oi)ImGui::TextWrapped("%s",a.hypothesis);
        if(ImGui::BeginTable("##FlowMetrics",3,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV)){
            ImGui::TableNextColumn();ImGui::TextDisabled("PRICE");
            if(a.complete_bars)ImGui::Text("%+.2f%%",a.price_pct);else ImGui::TextUnformatted("Incomplete");
            ImGui::TableNextColumn();ImGui::TextDisabled("AGGRESSOR BUY SHARE");
            if(a.complete_bars&&total>0)ImGui::Text("%.1f%%",100*a.buy/total);else ImGui::TextUnformatted("Unavailable");
            ImGui::TableNextColumn();ImGui::TextDisabled("OPEN INTEREST");
            if(a.usable_oi)ImGui::Text("%+.4f%%",a.oi_pct);else ImGui::TextUnformatted(a.raw_count?"Incomplete":"Unavailable");
            if(a.raw_count){char first[32]{},last[32]{};Indicators::format_oi_axis(a.oi_start,first,sizeof(first),nullptr);Indicators::format_oi_axis(a.oi_end,last,sizeof(last),nullptr);ImGui::TextWrapped("%s to %s contracts",first,last);}

            ImGui::EndTable();
        }
        char buy[32],sell[32],forced_buy[32],forced_sell[32];
        Indicators::format_oi_axis(a.buy,buy,sizeof(buy),nullptr);
        Indicators::format_oi_axis(a.sell,sell,sizeof(sell),nullptr);
        Indicators::format_oi_axis(a.forced_buy,forced_buy,sizeof(forced_buy),nullptr);
        Indicators::format_oi_axis(a.forced_sell,forced_sell,sizeof(forced_sell),nullptr);
        const bool split_table=ImGui::BeginTable("##FlowSplits",ImGui::GetContentRegionAvail().x>=620?2:1,ImGuiTableFlags_SizingStretchSame);
        if(split_table)ImGui::TableNextColumn();
        ImGui::TextUnformatted("Who crossed the spread?");
        flow_split(a.buy,a.sell,"Aggressive buy share");
        if(total>0){
            ImGui::TextWrapped("Aggressive buys: %.1f%% (%s base-asset units)",100*a.buy/total,buy);
            ImGui::TextWrapped("Aggressive sells: %.1f%% (%s base-asset units)",100*a.sell/total,sell);
        }
        ImGui::TextWrapped("Quantity, not trader count. Every trade has a buyer and seller; the aggressor takes available liquidity.%s",a.complete_bars?"":" Partial recorded minutes.");
        if(split_table)ImGui::TableNextColumn();
        ImGui::TextUnformatted("Reported liquidations");
        if(e.liquidations_available){
            flow_split(a.forced_buy,a.forced_sell,"Short-liquidation share of reported USD");
            ImGui::TextWrapped("Forced buys / short liquidations $%s (%lld reports)",forced_buy,static_cast<long long>(a.forced_buy_count));
            ImGui::TextWrapped("Forced sells / long liquidations $%s (%lld reports)",forced_sell,static_cast<long long>(a.forced_sell_count));
            ImGui::TextWrapped("Reported USD only; partial feed coverage. No reports does not mean no liquidations.");
        } else ImGui::TextWrapped("Reported liquidations: %s",flow_positioning::error_message(e.liquidations_status));
        if(split_table)ImGui::EndTable();
        if(a.raw_count){
            if(a.raw_count>=2)ImGui::TextWrapped("Change between observed endpoints: %+.6f contracts (%+.4f%%).",a.oi_end-a.oi_start,a.oi_pct);
        }
        ImGui::TextWrapped("Coverage: %zu/%lld minutes; %zu/%zu raw OI samples; largest raw-sample gap %.1fs.",e.bars.size(),static_cast<long long>((e.to-e.from)/60000),a.raw_count,e.oi.size(),a.max_gap/1000.0);
        if(e.bars_status!="available")ImGui::TextWrapped("Price/aggression: %s",flow_positioning::error_message(e.bars_status));
        if(e.oi_status!="available")ImGui::TextWrapped("OI: %s",flow_positioning::error_message(e.oi_status));
        ImGui::TextWrapped("Hover to compare the same minute. Latest stays latest while you pan; Shift-drag selects a move.");
        if(ImGui::SmallButton("Inspect final minute conditions"))ui::ResearchMomentPanel::instance().open(pair_.symbol,flow_to_-60000);
        if(ImGui::IsItemHovered())Theme::tooltip("Opens research conditions at the final complete minute of this exact interval. Finding similar moments proposes a study; it does not prove an edge.");
        if(ImGui::SmallButton("Inspect evidence"))ImGui::OpenPopup("Flow evidence");
        ImGui::SameLine();
        if(ImGui::SmallButton("Save assessment")){
            auto document=flow_history_.source;
            document["version"]="terminal_flow_assessment.v1";
            document["interpretation"]=a.hypothesis;
            document["price_change_pct"]=a.complete_bars?nlohmann::json(a.price_pct):nlohmann::json(nullptr);
            document["scope_mode"]=selected?"selected":"latest";
            document["display_time_zone"]=DisplayTimeZone::instance().zone_name();
            document["quantity_unit"]="base_asset";document["oi_unit"]="contracts";document["liquidation_unit"]="USD";
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
            ImGui::TextWrapped("Exact interval: %s to %s (end exclusive)",start,end);
            ImGui::Text("Buy %.6f / Sell %.6f quantity",a.buy,a.sell);
            ImGui::Text("Reported forced buys $%.2f / sells $%.2f",a.forced_buy,a.forced_sell);
            ImGui::Text("Raw OI %zu/%zu samples; largest gap %.1fs",a.raw_count,e.oi.size(),a.max_gap/1000.0);
            if(a.raw_count)ImGui::Text("First %.6f / last %.6f contracts",a.oi_start,a.oi_end);
            ImGui::TextWrapped("%s",a.hypothesis);
            ImGui::TextWrapped("Retrospective stored evidence. Original availability is unverified. Liquidation reports cover only part of forced activity.");
            ImGui::Text("Minute bars %zu / %lld",e.bars.size(),static_cast<long long>((e.to-e.from)/60000));
            if(a.raw_count){char first[128]{},last[128]{};flow_time(a.first_oi,first,sizeof(first));flow_time(a.last_oi,last,sizeof(last));ImGui::TextWrapped("First raw OI: %s",first);ImGui::TextWrapped("Last raw OI: %s",last);ImGui::Text("Last sample to selected end: %.3fs",(e.to-a.last_oi)/1000.0);}

            ImGui::TextWrapped("OI points are sampled observations, not trade-level opening/closing labels. Missing raw values are never replaced with USD notional or state refresh times.");
            ImGui::TextWrapped("No universal OI materiality threshold is applied. Read the exact size and sample coverage; a small signed change alone is not evidence of covering or unwinding.");
            ImGui::TextWrapped("Saved assessment includes these source rows and exact boundaries. It is a local JSON file, not a published Studio assessment.");
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
    if(ready&&crosshair_state_.is_active){
        const int64_t minute=static_cast<int64_t>(std::round(crosshair_state_.plot_pos.x/60000.0))*60000;
        if(minute>=e.from&&minute<e.to){
            Theme::begin_tooltip();
            char label[128]{};flow_time(minute,label,sizeof(label));ImGui::TextWrapped("Minute starting %s",label);
            const auto bar=std::find_if(e.bars.begin(),e.bars.end(),[&](const auto& b){return b.time==minute;});
            if(bar!=e.bars.end()){
                char open[32]{},close[32]{};fmt_.format_price(open,sizeof(open),bar->open);fmt_.format_price(close,sizeof(close),bar->close);ImGui::Text("Price open %s / close %s",open,close);
                ImGui::Text("Aggressor buy %.6f / sell %.6f base units",bar->buy,bar->sell);
            } else ImGui::TextUnformatted("Price and aggression: no stored minute");
            const flow_positioning::OI* first=nullptr;const flow_positioning::OI* last=nullptr;size_t raw=0,polls=0;int64_t gap=0;
            for(const auto& o:e.oi)if(o.time>=minute&&o.time<minute+60000){++polls;if(o.contracts>0){if(!first)first=&o;if(last)gap=std::max(gap,o.time-last->time);last=&o;++raw;}}
            if(first&&last){
                char first_time[128]{},last_time[128]{};flow_time(first->time,first_time,sizeof(first_time));flow_time(last->time,last_time,sizeof(last_time));
                ImGui::TextWrapped("OI %.6f at %s",first->contracts,first_time);ImGui::TextWrapped("OI %.6f at %s",last->contracts,last_time);
                ImGui::Text("%zu/%zu raw samples; largest internal gap %.1fs",raw,polls,gap/1000.0);
            } else ImGui::Text("Raw OI unavailable (%zu recorded poll timestamps)",polls);
            double buys=0,sells=0;int64_t buy_reports=0,sell_reports=0;
            for(const auto& l:e.liquidations)if(l.time==minute){(l.buy?buys:sells)+=l.usd;(l.buy?buy_reports:sell_reports)+=l.count;}
            if(e.liquidations_available){ImGui::Text("Forced buys $%.2f (%lld) / sells $%.2f (%lld)",buys,static_cast<long long>(buy_reports),sells,static_cast<long long>(sell_reports));ImGui::TextDisabled("Reported liquidations only; partial coverage");}
            else ImGui::TextWrapped("Liquidations: %s",flow_positioning::error_message(e.liquidations_status));
            Theme::end_tooltip();
        }
    }
}
