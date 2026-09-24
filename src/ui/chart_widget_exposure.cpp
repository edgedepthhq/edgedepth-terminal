#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
#include "ui/chart_widget.h"
#include "replayer/replay_manager.h"
#include "core/display_time_zone.h"
#include <emscripten.h>
#include <cctype>
#include <cstdio>

// Stream generations differ per venue: Binance was re-seeded from recorded
// history onto seeded2 (its seeded-v1 streams took live decisions before the
// hand-over completed), Bybit stays on its forward-only seeded-v1 streams.
// Order: proportional, oldest first, newest first, matching the closing combo.
static const char* const* exposure_generation_ids(const std::string& exchange) {
    static const char* const binance[]={"native-buffer7-proportional-retain14d-seeded2-v1","native-buffer7-oldest-retain14d-seeded2-v1","native-buffer7-newest-retain14d-seeded2-v1"};
    static const char* const others[]={"native-buffer7-proportional-retain14d-seeded-v1","native-buffer7-oldest-retain14d-seeded-v1","native-buffer7-newest-retain14d-seeded-v1"};
    return exchange=="binancef"?binance:others;
}

void ChartWidget::update_exposure_v2() {
    if (!exposure_enabled_) { exposure_history_.reset("V2 disabled");exposure_summary_.reset("V2 disabled");return; }
    if (rt_mode_ || chart_type_!=ChartType::Candles) {
        exposure_history_.reset("Exposure scenarios require candlesticks");exposure_summary_.reset("Averages require candlesticks");return;
    }
    if (ctx_.replay_mgr().is_pack_mode() || ctx_.replay_mgr().is_loading()) {
        exposure_history_.reset("V2 unavailable in packs or while replay is loading");exposure_summary_.reset("Averages unavailable in packs or while replay loads");return;
    }
    const bool replay=ctx_.replay_mgr().is_active();
    // Interpolation can run ahead of the server, including when Pause freezes it.
    // Bounded reads use only the acknowledged playhead, never that extrapolation.
    exposure_clock_=replay?std::min(ctx_.replay_mgr().interpolated_time_ms(),ctx_.replay_mgr().info().confirmed_time_ms):int64_t(emscripten_date_now());
    if (replay && exposure_clock_<=0) {
        exposure_history_.reset("Waiting for confirmed replay clock");
        exposure_summary_.reset("Waiting for confirmed replay clock");return;
    }
    std::string market=pair_.symbol;
    for (char& c:market) c=char(std::toupper(static_cast<unsigned char>(c)));
    // A timeframe switch clears the viewport before its candles are fitted.
    // Do not issue a fallback-range request or keep the previous interval's data.
    if (last_visible_range_.X.Min<=0 || last_visible_range_.X.Max<=last_visible_range_.X.Min) {
        exposure_history_.reset("Waiting for chart range");
        exposure_summary_.reset("Waiting for chart range");return;
    }
    const int64_t visible=int64_t(last_visible_range_.X.Min);
    const int64_t to=std::min(exposure_clock_,int64_t(last_visible_range_.X.Max));
    exposure_view_to_=to;
    const int64_t replay_start=replay?ctx_.replay_mgr().info().start_time_ms:0;
    const int64_t period=candles().timeframe_seconds()*1000;
    exposure_averages_=exposure_detail_==2 || (exposure_detail_==0 && exposure::automatic_summary(visible,to,period));
    if (exposure_scenario_[0]=='\0') {
        std::snprintf(exposure_scenario_,sizeof(exposure_scenario_),"%s",exposure_generation_ids(pair_.exchange)[0]);
    } else {
        // A standard selection follows the chart across venues; a custom ID is left alone.
        const char* const* venue_ids=exposure_generation_ids(pair_.exchange);
        for (const char* const* family:{exposure_generation_ids("binancef"),exposure_generation_ids("bybit")})
            for (int i=0;i<3;++i)
                if (std::string_view(exposure_scenario_)==family[i] && std::string_view(exposure_scenario_)!=venue_ids[i]) {
                    std::snprintf(exposure_scenario_,sizeof(exposure_scenario_),"%s",venue_ids[i]);
                    exposure_history_.reset();exposure_summary_.reset();
                }
    }
    exposure::Json request;
    if (exposure_averages_) {
        exposure_history_.reset("Averaged history selected");
        if (period<=0 || period>exposure::summary_max_range) { exposure_summary_.reset("Unsupported summary candle interval");return; }
        const int64_t earliest=std::max<int64_t>(0,((to-exposure::summary_max_range+period-1)/period)*period);
        const int64_t from=std::max(replay_start,std::max(earliest,std::max<int64_t>(0,visible/period*period)));
        const int64_t width=exposure::summary_width(from,to,period);
        request=exposure_summary_.poll({{pair_.exchange,market,exposure_scenario_},from,to,width,period},exposure_clock_,replay,emscripten_get_now());
    } else {
        exposure_summary_.reset("Exact history selected");
        // Stable left edge prevents a fresh request every live frame.
        const int64_t from=exposure::exact_window_start(visible,to,std::max<int64_t>(1,replay_start));
        request=exposure_history_.poll({pair_.exchange,market,exposure_scenario_},from,exposure_clock_,replay,emscripten_get_now(),to);
    }
    if (!request.is_null()) {
        request["data"]["pair"]["symbol"]=pair_.symbol;
        const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
        request["data"]["entitlement_token"]=token?token:"";
        ctx_.stream_mgr().send_message(request.dump());
    }
}
// Compact settings under Layers > LIQUIDATIONS. Labels sit left of fixed-width
// widgets so nothing is clipped by the popup; the model explanation, legend
// and recorded-frame details are collapsed rather than always printed.
void ChartWidget::render_exposure_v2_settings() {
    // The control column is measured from where the label starts, not from the
    // window edge (SameLine(x) ignores Indent), so it lines up inside the
    // indented Layers menu group as well as at the window edge.
    auto labelled=[&](const char* label,float width=150.f) {
        const float label_x=ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(label_x+100.f);
        ImGui::SetNextItemWidth(width);
    };
    const char* const* scenario_ids=exposure_generation_ids(pair_.exchange);
    int selected=-1;
    for (int i=0;i<3;++i) if (std::string_view(exposure_scenario_)==scenario_ids[i]) selected=i;
    labelled("Closing rule");
    if (ImGui::Combo("##closing",&selected,"Proportional\0Oldest first\0Newest first\0")) {
        std::snprintf(exposure_scenario_,sizeof(exposure_scenario_),"%s",scenario_ids[selected]);
        exposure_history_.reset();exposure_summary_.reset();
    }
    labelled("History");
    if (ImGui::Combo("##history",&exposure_detail_,"Automatic\0Exact (up to 4h in view)\0Time averages\0")) { exposure_history_.reset();exposure_summary_.reset(); }
    labelled("Side");
    ImGui::Combo("##side",&exposure_display_.side,"Both\0Long\0Short\0");
    labelled("Hide below");
    ImGui::SliderFloat("##min",&exposure_display_.minimum_percent,0,10,"%.2f%% OI");
    labelled("Brightest at");
    ImGui::SliderFloat("##peak",&exposure_display_.peak_percent,1,100,"%.0f%% OI");
    labelled("Opacity");
    ImGui::SliderFloat("##opacity",&exposure_display_.opacity,0.1f,0.8f,"%.2f");
    ImGui::TextDisabled("Automatic: time averages beyond 30 minutes");
    ImGui::Checkbox("Fit price axis to bands",&exposure_fit_bands_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bands sit about 7%% from their entry price. Off: the chart keeps its\nnormal zoom and off-screen bands are counted in the chart note.");
    if (ImGui::SmallButton("Inspect small bands")) { exposure_display_.minimum_percent=0.01f;exposure_display_.peak_percent=1.f; }

    // One line that says what is on the chart right now.
    if (!exposure_enabled_) ImGui::TextDisabled("Layer off");
    else if (exposure_averages_) {
        if (const auto* summary=exposure_summary_.view(exposure_clock_)) {
            char interval[32];exposure::summary_interval_label(summary->scope.width,interval,sizeof(interval));
            ImGui::Text("%s averages, %zu time cells",interval,summary->columns.size());
        } else ImGui::TextWrapped("%s",exposure_summary_.status().c_str());
    } else {
        const auto state=exposure_history_.state(std::min(exposure_clock_,exposure_view_to_));
        const auto* timeline=exposure_history_.view();
        if (timeline && std::string_view(state.status)=="available" && state.publication) {
            const auto& f=state.publication->frame;
            const double ul=f["unknown_long_short"][0].get<double>(),us=f["unknown_long_short"][1].get<double>();
            ImGui::Text("Located: long %.2f%%, short %.2f%% of one side OI",100*(1-ul),100*(1-us));
            ImGui::TextDisabled("%zu bands; remaining inventory cannot be placed under this scenario",f["bands"].size());
            if (exposure::is_bootstrap(f)) ImGui::TextDisabled("Recorded history replayed to seed this stream, not a live minute");
            else if (f.contains("bootstrap_through_ms")) {
                char stamp[64]{};
                DisplayTimeZone::instance().format(f["bootstrap_through_ms"].get<int64_t>(),TimeZoneFormat::FullInspection,stamp,sizeof(stamp));
                ImGui::TextDisabled("Inventory seeded from recorded history through %s",stamp);
            }
        } else ImGui::TextWrapped("%s",state.reason.empty()?exposure_history_.status().c_str():state.reason.c_str());
    }

    if (ImGui::TreeNode("Custom scenario ID")) {
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputText("##scenario",exposure_scenario_,sizeof(exposure_scenario_))) { exposure_history_.reset();exposure_summary_.reset(); }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("About this model")) {
        ImGui::TextWrapped("Hypothetical positions inferred from trades and open interest, placed using a fixed 7%% margin buffer. Recorded history can initialize the model before live updates. Initial inventory, crossed levels, gaps and retention can leave inventory unlocated. Bands show scenario exposure as a share of one side's OI, not observed account liquidation levels or probability.");
        ImGui::TextWrapped("The three closing rules are alternative assumptions about which positions close first; never add them together.");
        ImGui::TextUnformatted("Colour: share of one side's OI, square-root scale");
        const auto pos=ImGui::GetCursorScreenPos();
        const float width=std::min(300.f,ImGui::GetContentRegionAvail().x);
        auto* draw=ImGui::GetWindowDrawList();
        for (int i=0;i<64;++i) {
            const double fraction=double(i)/63;
            draw->AddRectFilled(ImVec2(pos.x+width*i/64,pos.y),ImVec2(pos.x+width*(i+1)/64,pos.y+8),
                exposure::band_color(fraction*fraction*exposure_display_.peak_percent/100,exposure_display_));
        }
        ImGui::Dummy(ImVec2(width,10));
        ImGui::Text("0%%   %.1f%%   %.1f%%   %.0f%%+",exposure_display_.peak_percent/9,exposure_display_.peak_percent*4/9,exposure_display_.peak_percent);
        ImGui::TreePop();
    }
    if (exposure_enabled_ && ImGui::TreeNode("Recorded frame details")) {
        if (exposure_averages_) {
            if (const auto* summary=exposure_summary_.view(exposure_clock_)) exposure::render_summary_inspection(*summary);
            else ImGui::TextWrapped("%s",exposure_summary_.status().c_str());
        } else if (const auto* timeline=exposure_history_.view();timeline && std::string_view(exposure_history_.state(std::min(exposure_clock_,exposure_view_to_)).status)=="available") {
            exposure::render_inspection(*timeline,std::min(exposure_view_to_,exposure_history_.covered_to()));
        } else ImGui::Text("State: %s",exposure_history_.state(std::min(exposure_clock_,exposure_view_to_)).status);
        ImGui::TreePop();
    }
}
void ChartWidget::render_exposure_v2(int64_t clock) {
    if (!exposure_enabled_ || rt_mode_ || chart_type_!=ChartType::Candles) return;
    const float note_y=ctx_.replay_mgr().is_active()?56.f:32.f;
    exposure_clock_=clock;
    exposure::Visibility visibility;
    size_t drawn=0;
    if (exposure_averages_) {
        if (const auto* summary=exposure_summary_.view(clock)) drawn=exposure::render_summary_overlay(*summary,clock,exposure_display_,fmt_.price_fmt,&visibility);
        const auto pos=ImPlot::GetPlotPos();char note[256];
        if (const auto* summary=exposure_summary_.view(clock)) {
            char interval[32];exposure::summary_interval_label(summary->scope.width,interval,sizeof(interval));
            if (drawn==0 && visibility.outside>0) std::snprintf(note,sizeof(note),"V2 %s averages: %zu cells off-screen (bands sit about 7%% from price; Layers > Exposure V2 settings > Fit price axis to bands)",interval,visibility.outside);
            else std::snprintf(note,sizeof(note),"V2 %s averages: %zu cells, %zu hidden, %zu off-screen",interval,drawn,visibility.filtered,visibility.outside);
        }
        else std::snprintf(note,sizeof(note),"V2 averages: %s",exposure_summary_.status().c_str());
        ImPlot::PushPlotClipRect();ImPlot::GetPlotDrawList()->AddText(ImVec2(pos.x+12,pos.y+note_y),Theme::u32(Theme::Tokens::WARN),note);ImPlot::PopPlotClipRect();
        return;
    }
    if (const auto* timeline=exposure_history_.view()) drawn=exposure::render_overlay(*timeline,clock,exposure_display_,&visibility);
    const auto state=exposure_history_.state(std::min(clock,exposure_view_to_));
    char stamp[64]{};
    if (exposure_history_.view()) DisplayTimeZone::instance().format(exposure_history_.covered_to(),TimeZoneFormat::FullInspection,stamp,sizeof(stamp));
    char text[256];
    if (std::string_view(state.status)=="available" && drawn==0 && visibility.outside>0)
        std::snprintf(text,sizeof(text),"V2: %zu bands off-screen (bands sit about 7%% from price; Layers > Exposure V2 settings > Fit price axis to bands)",visibility.outside);
    else if (std::string_view(state.status)=="available")
        std::snprintf(text,sizeof(text),"V2: %zu bands, %zu hidden, %zu off-screen, through %s",drawn,visibility.filtered,visibility.outside,stamp);
    else
        std::snprintf(text,sizeof(text),"V2 %s: %s",state.status,exposure_history_.view()?stamp:exposure_history_.status().c_str());
    const auto pos=ImPlot::GetPlotPos();
    ImPlot::PushPlotClipRect();
    ImPlot::GetPlotDrawList()->AddText(ImVec2(pos.x+12,pos.y+note_y),Theme::u32(Theme::Tokens::WARN),text);
    ImPlot::PopPlotClipRect();
}
#endif
