#pragma once
#include "core/census_profile.h"
#include "rendering/theme.h"
#include "implot.h"
#include <cstdio>

namespace census_overlay {
inline void money(double value, char* out, size_t size) {
    if(value>=1e9) std::snprintf(out,size,"$%.2fB",value/1e9);
    else if(value>=1e6) std::snprintf(out,size,"$%.2fM",value/1e6);
    else if(value>=1e3) std::snprintf(out,size,"$%.1fK",value/1e3);
    else std::snprintf(out,size,"$%.0f",value);
}
// Called inside the price plot. No source mutation or inferred inventory survival.
template<class Frame, class FormatTime>
void render(const census_history::History<Frame>* history, int64_t asof,
            bool show_history, double min_usd, const char* coin, bool cross_venue,
            const char* price_fmt, FormatTime format_time) {
    using namespace census_profile;
    const Frame* latest=history?history->at(asof):nullptr;
    const auto status=state(latest,asof);
    const auto pos=ImPlot::GetPlotPos(), size=ImPlot::GetPlotSize();
    const auto limits=ImPlot::GetPlotLimits();
    auto* draw=ImPlot::GetPlotDrawList();
    const auto mouse=ImGui::GetIO().MousePos;
    const bool hovered=ImPlot::IsPlotHovered();
    const Frame* hover_frame=nullptr;
    double hover_longs=0,hover_shorts=0,hover_low=0,hover_high=0;
    ImPlot::PushPlotClipRect();
    if(show_history && history) {
        const auto at=ImPlot::GetPlotMousePos();
        for(const auto& [ts,f]:history->frames()) {
            // Never extend an old observation beyond its original one-minute column.
            const int64_t end=std::min(history->display_end(ts,asof),ts+census_history::kColumnMs);
            if(ts>asof||ts>limits.X.Max||end<limits.X.Min||end<=ts||f.census_status=="unavailable")continue;
            for(const auto& b:f.bands) {
                const auto r=bounds(f,b);
                if(r.high<limits.Y.Min||r.low>limits.Y.Max)continue;
                for(int side=0;side<2;++side) {
                    const double usd=side==0?b.est_long_usd:b.est_short_usd;
                    if(usd<min_usd||usd<=0)continue;
                    const double low=side==0?r.low:b.price_mid, high=side==0?b.price_mid:r.high;
                    const auto tint=side==0?Theme::Tokens::ICEBERG_VIOLET:Theme::Tokens::UP;
                    draw->AddRectFilled(ImPlot::PlotToPixels(double(ts),high),ImPlot::PlotToPixels(double(end),low),Theme::u32(tint,opacity(usd)));
                    if(hovered&&at.x>=ts&&at.x<end&&at.y>=r.low&&at.y<=r.high) {
                        hover_frame=&f;hover_low=r.low;hover_high=r.high;
                        if(side==0)hover_longs+=usd;else hover_shorts+=usd;
                    }
                }
            }
        }
    }
    Profile profile;
    const bool current=status==State::ready;
    if(current)profile=build(*latest,limits.Y.Min,limits.Y.Max,min_usd);
    const float right=pos.x+size.x-10;
    const float width=std::min(110.0f,size.x*.18f);
    const float line=ImGui::GetTextLineHeight()+4;
    const float panel_height=line*4+16;
    float label_ys[6]={};int labels=0,long_labels=0,short_labels=0;
    if(current)for(size_t i=0;i<profile.count;++i) {
        const auto& z=profile.zones[i];
        const float y=ImPlot::PlotToPixels(0,z.price).y;
        const float top=ImPlot::PlotToPixels(0,z.high).y;
        const float bottom=ImPlot::PlotToPixels(0,z.low).y;
        const float bar=width*static_cast<float>(z.usd/profile.max_usd);
        // Opposite halves preserve side when both share a reported price bin.
        const float a=z.is_long?y:top, b=z.is_long?bottom:y;
        const auto tint=z.is_long?Theme::Tokens::ICEBERG_VIOLET:Theme::Tokens::UP;
        draw->AddRectFilled(ImVec2(right-bar,a),ImVec2(right,std::max(a+1,b)),Theme::u32(tint,.7f));
        if(hovered&&mouse.x>=right-width&&mouse.x<=right&&mouse.y>=top-2&&mouse.y<=bottom+2) {
            hover_frame=latest;hover_low=z.low;hover_high=z.high;
            hover_longs=0;hover_shorts=0;
            for(size_t j=0;j<profile.count;++j)if(profile.zones[j].price==z.price) {
                if(profile.zones[j].is_long)hover_longs+=profile.zones[j].usd;else hover_shorts+=profile.zones[j].usd;
            }
        }
        if(labels>=6||(z.is_long?long_labels:short_labels)>=3||y<pos.y+panel_height+line||y>pos.y+size.y-line)continue;
        bool overlaps=false;
        for(int j=0;j<labels;++j)if(std::abs(y-label_ys[j])<line*1.2f)overlaps=true;
        if(overlaps)continue;
        char amount[32],price[48],label[128];money(z.usd,amount,sizeof(amount));
        std::snprintf(price,sizeof(price),price_fmt,z.price);
        std::snprintf(label,sizeof(label),"%s  %s  %s  %+.2f%%",z.is_long?"Long":"Short",price,amount,100*(z.price/latest->mark_price-1));
        const auto text_size=ImGui::CalcTextSize(label);
        const ImVec2 text_pos(right-width-8-text_size.x,y-text_size.y/2);
        draw->AddRectFilled(ImVec2(text_pos.x-3,text_pos.y-2),ImVec2(text_pos.x+text_size.x+3,text_pos.y+text_size.y+2),Theme::u32(Theme::Tokens::PANEL));
        draw->AddText(text_pos,Theme::u32(tint),label);
        label_ys[labels++]=y;if(z.is_long)++long_labels;else ++short_labels;
    }
    char rows[4][220]{};
    char time[64]="",amount[32],unlocated[32],scale[32],floor[32];
    money(min_usd,floor,sizeof(floor));
    if(latest)format_time(latest->timestamp_ms,time,sizeof(time));
    std::snprintf(rows[0],sizeof(rows[0]),"HL %s | %s%s",coin,show_history?"Current + history":"Current exposure",cross_venue?" | HL prices":"");
    if(!latest)std::snprintf(rows[1],sizeof(rows[1]),"Waiting for a recorded HL snapshot");
    else {
        const auto& q=latest->census_quality;
        money(q.usable_notional_usd,amount,sizeof(amount));money(q.unlocated_notional_usd,unlocated,sizeof(unlocated));
        if(status==State::stale)std::snprintf(rows[1],sizeof(rows[1]),"Current exposure unavailable | report %.0fs old",(asof-latest->timestamp_ms)/1000.0);
        else if(status==State::unavailable)std::snprintf(rows[1],sizeof(rows[1]),"No usable levels | %u sampled positions",q.sampled_positions);
        else if(status==State::legacy)std::snprintf(rows[1],sizeof(rows[1]),"Historical sample | observation age unknown");
        else if(q.coverage_denominator_usd>0)std::snprintf(rows[1],sizeof(rows[1]),"Located %s | %.2f%% of HL exposure",amount,100*q.usable_notional_usd/q.coverage_denominator_usd);
        else std::snprintf(rows[1],sizeof(rows[1]),"Located %s | coverage denominator unknown",amount);
        if(q.version>0)std::snprintf(rows[2],sizeof(rows[2]),"%u/%u positions located | %s unlocated",q.usable_positions,q.sampled_positions,unlocated);
        else std::snprintf(rows[2],sizeof(rows[2]),"Coverage details unavailable for this old snapshot");
        if(current&&profile.count==0)std::snprintf(rows[3],sizeof(rows[3]),"No zones in view above %s | %s",floor,time);
        else if(current) {money(profile.max_usd,scale,sizeof(scale));std::snprintf(rows[3],sizeof(rows[3]),"%s | bars 0-%s | min %s",time,scale,floor);}
        else std::snprintf(rows[3],sizeof(rows[3]),"Last snapshot %s | history %s",time,show_history?"shown":"off");
    }
    float panel_width=0;for(auto& row:rows)panel_width=std::max(panel_width,ImGui::CalcTextSize(row).x);
    panel_width=std::min(panel_width,size.x-24);
    const float left=std::max(pos.x+8,right-panel_width-12);
    const float panel_top=pos.y+8;
    draw->AddRectFilled(ImVec2(left,panel_top),ImVec2(right,panel_top+panel_height),Theme::u32(Theme::Tokens::PANEL),4);
    draw->PushClipRect(ImVec2(left,panel_top),ImVec2(right,panel_top+panel_height),true);
    for(int i=0;i<4;++i)draw->AddText(ImVec2(left+6,panel_top+6+i*line),Theme::u32(i==0?Theme::Tokens::TX1:Theme::Tokens::TX2),rows[i]);
    draw->PopClipRect();
    const bool summary_hover=hovered&&mouse.x>=left&&mouse.x<=right&&mouse.y>=panel_top&&mouse.y<=panel_top+panel_height;
    ImPlot::PopPlotClipRect();
    if(summary_hover)hover_frame=latest;
    if(hovered&&hover_frame) {
        const auto& f=*hover_frame;const auto& q=f.census_quality;
        char stamp[64],low[48],high[48];format_time(f.timestamp_ms,stamp,sizeof(stamp));
        Theme::begin_tooltip();
        ImGui::Text("%s | HL %s | %s",summary_hover?"Profile snapshot":"Observed snapshot",coin,stamp);
        if(!summary_hover) {
            std::snprintf(low,sizeof(low),price_fmt,hover_low);std::snprintf(high,sizeof(high),price_fmt,hover_high);
            ImGui::Text("Price zone %s - %s",low,high);
            ImGui::Text("Zone only: long $%.0f | short $%.0f",hover_longs,hover_shorts);
        }
        ImGui::Text("Snapshot totals: long $%.0f | short $%.0f",f.total_long_risk_usd,f.total_short_risk_usd);
        std::snprintf(low,sizeof(low),price_fmt,f.mark_price);
        ImGui::Text("HL mark %s | distances use this snapshot's mark",low);
        if(q.version>0) {
            ImGui::Text("Located $%.0f / $%.0f (twice HL one-side OI)",q.usable_notional_usd,q.coverage_denominator_usd);
            ImGui::Text("Located %u / sampled %u positions | unlocated $%.0f",q.usable_positions,q.sampled_positions,q.unlocated_notional_usd);
            ImGui::Text("Price/mark-policy excluded $%.0f | stale resident $%.0f",q.far_filtered_notional_usd,q.stale_notional_usd);
            ImGui::Text("At snapshot: wallet age weighted %.0fs | p95 %.0fs",q.weighted_wallet_age_ms/1000,q.p95_wallet_age_ms/1000.0);
            if(f.census_observed_at_ms>0)ImGui::Text("Oldest wallet at snapshot %.0fs | at playhead %.0fs",(f.timestamp_ms-f.census_observed_at_ms)/1000.0,(asof-f.census_observed_at_ms)/1000.0);
            if(q.venue_received_at_ms>0)ImGui::Text("Mark/OI receipt %.0fs before snapshot",(f.timestamp_ms-q.venue_received_at_ms)/1000.0);
        }
        if(summary_hover&&current)ImGui::Text("In view $%.0f | below filter $%.0f | outside view $%.0f",profile.visible_usd,profile.filtered_usd,profile.outside_usd);
        ImGui::TextUnformatted(q.version==2?"Stable 0.25% logarithmic bins":"Legacy mark-relative bins");
        ImGui::TextUnformatted("Violet: long positions. Mint: short positions. Amounts are position notional.");
        ImGui::TextUnformatted("Selected wallet sample, not a confidence score or whole-venue inventory.");
        ImGui::TextUnformatted("Missing thresholds stay unknown. Collateral changes can move reported levels.");
        if(cross_venue)ImGui::TextUnformatted("Hyperliquid exposure over another venue's candles; triggers use HL mark.");
        Theme::end_tooltip();
    }
}
}
