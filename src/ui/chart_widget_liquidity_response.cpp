#include "ui/chart_widget.h"
#include "core/app_context.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include "stream_handler.h"
#include <emscripten.h>

EM_JS(void,response_export,(const char* value),{
    const url=URL.createObjectURL(new Blob([UTF8ToString(value)],{type:'application/json'}));
    const a=document.createElement('a');a.href=url;a.download='edgedepth-liquidity-response.json';a.click();
    setTimeout(()=>URL.revokeObjectURL(url),1000);
});

void ChartWidget::add_absorption_indicator(){
    if(indicator_mgr_.has_indicator_of_type<Indicators::AbsorptionIndicator>())return;
    indicator_mgr_.add_indicator(std::make_unique<Indicators::AbsorptionIndicator>(liquidity_response_history_,[this](){liquidity_response_evidence_open_=true;}));
}

void ChartWidget::update_liquidity_response(){
    if(!liquidity_response_enabled_&&!indicator_mgr_.has_indicator_of_type<Indicators::AbsorptionIndicator>()){liquidity_response_history_.reset("Layer off");return;}
    if(ctx_.replay_mgr().is_pack_mode()||ctx_.replay_mgr().is_loading()||replay_selection_.active){liquidity_response_history_.reset("Unavailable in packs or while seeking");return;}
    if(!rt_mode_&&chart_type_!=ChartType::Candles){liquidity_response_history_.reset("Use candlesticks or real-time view");return;}
    const bool replay=ctx_.replay_mgr().is_active();
    int64_t clock=replay?std::min(ctx_.replay_mgr().interpolated_time_ms(),ctx_.replay_mgr().info().confirmed_time_ms):int64_t(emscripten_date_now());
    if(clock<=0){liquidity_response_history_.reset("Waiting for replay clock");return;}
    // Recorded source batches trail live market data. Show the cutoff, never
    // extend inferred bands into the unobserved right edge. Five-second steps
    // keep this bounded evidence read out of the frame-rate path.
    if(!replay)clock-=15000;
    const int64_t right=last_visible_range_.X.Max>0?int64_t(last_visible_range_.X.Max):clock;
    const int64_t to=std::min(clock,right)/5000*5000;
    const int64_t replay_start=replay?ctx_.replay_mgr().info().start_time_ms:1;
    const int64_t timeframe=rt_mode_?60000:timeframe_seconds()*1000;
    if(timeframe<60000){liquidity_response_history_.reset("Absorption activity requires one-minute candles or above");return;}
    const int64_t left=last_visible_range_.X.Min>0?int64_t(last_visible_range_.X.Min):to-3600000;
    const int64_t from=std::max({replay_start,to-86400000,left/timeframe*timeframe});
    auto q=liquidity_response_history_.poll(pair_.exchange,pair_.symbol,tick_size_,from,to,replay,emscripten_get_now(),true,timeframe);
    if(liquidity_response_history_.revision!=liquidity_response_marker_revision_||liquidity_response_marker_tf_!=timeframe_seconds()){
        liquidity_response_markers_=liquidity_response::group_markers(liquidity_response_history_.episodes,timeframe_seconds()*1000);
        liquidity_response_marker_revision_=liquidity_response_history_.revision;
        liquidity_response_marker_tf_=timeframe_seconds();
    }
    if(!q.is_null()){
        const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
        q["data"]["entitlement_token"]=token?token:"";ctx_.stream_mgr().send_message(q.dump());
    }
}

void ChartWidget::render_liquidity_response_settings(){
    ImGui::TextWrapped("Observed trading against replenishing bids or asks. Hidden order identity and remaining size are unknown. Separate from liquidation heatmaps.");
    ImGui::TextWrapped("Pilot: Binance BTC/SOL, up to 24 hours of stored activity. Enable Absorption activity in Indicators for aligned bid/ask columns. Missing history stays unavailable.");
    ImGui::TextWrapped("%s",liquidity_response_history_.status.c_str());
    if(liquidity_response_history_.ready){
        char through[64]="";
        DisplayTimeZone::instance().format(liquidity_response_history_.through,TimeZoneFormat::FullInspection,through,sizeof(through));
        ImGui::TextWrapped("Observed through %s",through);
        ImGui::TextWrapped("Rule v1: 10-tick bands, at least $10,000 executed, 3 hit/refill cycles and 3 seconds. These are research thresholds, not a probability.");
        if(ImGui::Button("Export research observations"))response_export(liquidity_response_history_.source.dump().c_str());
        ImGui::TextWrapped("Executions use their source time, episode counts use first qualification. Whole-market scans and predictive validation are not yet available.");
    }
}

void ChartWidget::render_liquidity_response(){
    if((!liquidity_response_enabled_&&!liquidity_response_evidence_open_)||(!rt_mode_&&chart_type_!=ChartType::Candles))return;
    auto& history=liquidity_response_history_;
    const int64_t clock=ctx_.replay_mgr().is_active()?std::min(ctx_.replay_mgr().interpolated_time_ms(),ctx_.replay_mgr().info().confirmed_time_ms):int64_t(emscripten_date_now());
    const auto limits=ImPlot::GetPlotLimits();auto* draw=ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    auto band=[&](const liquidity_response::Episode& p){
        const double shift=rt_mode_?0.:timeframe_seconds()*500.;
        const double left=std::max(double(p.known)-shift,limits.X.Min),right=std::min(double(p.updated)-shift,limits.X.Max);
        if(right<left)return;
        const auto a=ImPlot::PlotToPixels(left,p.high),b=ImPlot::PlotToPixels(right,p.low);
        const auto tone=p.side=="bid"?Theme::Tokens::UP:Theme::Tokens::DOWN;
        const float opacity=p.status=="observed"?1.f:.45f;
        draw->AddRectFilled(a,b,Theme::u32(tone,.12f*opacity));
        draw->AddLine(ImVec2(a.x,(a.y+b.y)*.5f),ImVec2(b.x,(a.y+b.y)*.5f),Theme::u32(tone,opacity),2);
    };
    auto mark=[&](const liquidity_response::Episode& p,size_t count){
        const auto point=ImPlot::PlotToPixels(double(p.known),(p.high+p.low)*.5);
        const auto tone=p.side=="bid"?Theme::Tokens::UP:Theme::Tokens::DOWN;
        const auto color=Theme::u32(tone,p.status=="observed"?1.f:.55f);
        if(p.side=="bid")draw->AddCircleFilled(point,4,color);
        else draw->AddRect(ImVec2(point.x-4,point.y-4),ImVec2(point.x+4,point.y+4),color,0,0,2);
        if(count>1){char label[24];std::snprintf(label,sizeof(label),"%zu",count);const float width=ImGui::CalcTextSize(label).x;const float right=ImPlot::GetPlotPos().x+ImPlot::GetPlotSize().x;const float x=point.x+7+width<right?point.x+7:point.x-7-width;draw->AddText(ImVec2(x,point.y-8),Theme::u32(Theme::Tokens::TX2),label);}
    };
    auto visible=[&](const liquidity_response::Episode& p){return liquidity_response::visible(p,clock)&&p.updated>=limits.X.Min&&p.known<=limits.X.Max&&p.high>=limits.Y.Min&&p.low<=limits.Y.Max;};
    size_t hovered=history.episodes.size();const liquidity_response::MarkerGroup* hovered_group=nullptr;
    const auto mouse=ImGui::GetMousePos();
    if(history.ready){
        if(rt_mode_){
            std::vector<size_t> recent;
            for(size_t i=0;i<history.episodes.size();i++)if(visible(history.episodes[i]))recent.push_back(i);
            std::sort(recent.begin(),recent.end(),[&](size_t a,size_t b){return history.episodes[a].updated>history.episodes[b].updated;});
            if(recent.size()>32)recent.resize(32);
            for(size_t i:recent){
                const auto& p=history.episodes[i];band(p);mark(p,1);
                const auto a=ImPlot::PlotToPixels(double(p.known),p.high),b=ImPlot::PlotToPixels(double(p.updated),p.low);
                if(ImPlot::IsPlotHovered()&&mouse.x>=a.x-6&&mouse.x<=b.x+6&&mouse.y>=std::min(a.y,b.y)-6&&mouse.y<=std::max(a.y,b.y)+6)hovered=i;
            }
        }else{
            // Candle activity belongs in the aligned indicator pane. Only an
            // explicitly selected episode draws its supporting price segment.
            if(!history.selected.empty()&&history.selected_index<history.selected.size()){
                const auto& p=history.selected[history.selected_index];if(visible(p))band(p);
            }
        }
    }
    if(hovered<history.episodes.size()){
        const auto& p=history.episodes[hovered];Theme::begin_tooltip();
        ImGui::TextWrapped("%s",p.side=="bid"?"Bid absorption: sellers meet replenishing bids":"Ask absorption: buyers meet replenishing asks");
        ImGui::Text("At detection: $%.0f executed | %d refill cycles | %.1fs",p.detection_executed,p.detection_cycles,double(p.known-p.start)/1000);
        if(hovered_group&&hovered_group->members.size()>1)ImGui::Text("%zu observations, not orders or hidden size. Click to inspect each.",hovered_group->members.size());
        else ImGui::TextUnformatted("Click to pin the evidence. Hidden size is unknown.");
        Theme::end_tooltip();
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&!drawing_layer_.captures_mouse()&&!ImGui::GetIO().KeyShift&&!ImGui::GetIO().KeyCtrl){
            if(hovered_group)history.select(hovered_group->members);else history.select({hovered});
            liquidity_response_evidence_open_=true;
        }
    }
    const auto pos=ImPlot::GetPlotPos();char stamp[64]="";
    if(history.through>0)DisplayTimeZone::instance().format(history.through,TimeZoneFormat::TimeSeconds,stamp,sizeof(stamp));
    const char* state=history.ready?"stored observations":history.status.c_str();
    char label[192];
    if(history.ready)std::snprintf(label,sizeof(label),"Absorption: %s%s%s",state,stamp[0]?" through ":"",stamp);
    else std::snprintf(label,sizeof(label),"Absorption: %s",state);
    if(rt_mode_)draw->AddText(ImVec2(pos.x+12,pos.y+110),Theme::u32(Theme::Tokens::TX2),label);
    ImPlot::PopPlotClipRect();
    if(!history.ready||history.selected.empty()||history.selected_index>=history.selected.size()||!liquidity_response::visible(history.selected[history.selected_index],clock))liquidity_response_evidence_open_=false;
    if(!liquidity_response_evidence_open_)return;
    char evidence_title[96];std::snprintf(evidence_title,sizeof(evidence_title),"Absorption evidence###absorption-%p",static_cast<void*>(this));
    ImGui::SetNextWindowSize(ImVec2(400,420),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300,200),ImVec2(600,800));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,12));
    if(ImGui::Begin(evidence_title,&liquidity_response_evidence_open_)){
        {
            if(history.selected.size()>1){
                char selection[48];std::snprintf(selection,sizeof(selection),"%zu of %zu",history.selected_index+1,history.selected.size());
                if(ImGui::BeginCombo("Observation",selection)){
                    for(size_t i=0;i<history.selected.size();i++){
                        const auto& item=history.selected[i];char time[64]="",entry[160];
                        DisplayTimeZone::instance().format(item.known,TimeZoneFormat::TimeSeconds,time,sizeof(time));
                        std::snprintf(entry,sizeof(entry),"%s | $%.0f | %d cycles",time,item.detection_executed,item.detection_cycles);
                        ImGui::PushID(int(i));
                        if(ImGui::Selectable(entry,i==history.selected_index))history.selected_index=i;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
            const auto& p=history.selected[history.selected_index];
            ImGui::Text("%s / %s",pair_.exchange.c_str(),pair_.symbol.c_str());
            ImGui::TextWrapped("%s",p.side=="bid"?"Bid absorption: sellers meet replenishing bids":"Ask absorption: buyers meet replenishing asks");
            char known[64]="",through[64]="";
            DisplayTimeZone::instance().format(p.known,TimeZoneFormat::FullInspection,known,sizeof(known));
            DisplayTimeZone::instance().format(history.selected_through,TimeZoneFormat::FullInspection,through,sizeof(through));
            ImGui::TextWrapped("Detected %s",known);ImGui::TextWrapped("Pinned window through %s",through);
            ImGui::TextUnformatted("Band from");ImGui::SameLine(0,6);ImGui::Text(fmt_.price_fmt,p.low);
            ImGui::TextUnformatted("to (exclusive)");ImGui::SameLine(0,6);ImGui::Text(fmt_.price_fmt,p.high);
            ImGui::Separator();
            ImGui::Text("At detection: $%.0f | %d cycles",p.detection_executed,p.detection_cycles);
            ImGui::Text("Persistence at detection: %.1f seconds",double(p.known-p.start)/1000);
            ImGui::Text("At last update: $%.0f | %d cycles",p.executed,p.cycles);
            ImGui::Text("Added %.4g / removed %.4g base units",p.added,p.removed);
            ImGui::Text("Observed %.1fs | %s",double(p.updated-p.start)/1000,p.status.c_str());
            ImGui::TextWrapped("Hidden size and order identity are unknown. Removal can be execution or cancellation. Source completeness is not certified.");
            ImGui::TextWrapped("Research query handoff is not available yet.");
            if(ImGui::Button("Export pinned evidence")){
                auto evidence=history.selected_source;
                evidence["selected_episode_id"]=p.id;
                response_export(evidence.dump().c_str());
            }
            if(ImGui::Button("Clear pinned evidence")){history.selected.clear();history.selected_source={};liquidity_response_evidence_open_=false;}
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
