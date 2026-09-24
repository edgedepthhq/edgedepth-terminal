#pragma once
#include "indicator_base.h"
#include "volume_indicator.h"
#include "core/liquidity_response.h"
#include "core/display_time_zone.h"
#include "rendering/theme.h"
#include <functional>

namespace Indicators {
class AbsorptionIndicator : public IndicatorBase {
public:
    AbsorptionIndicator(liquidity_response::History& history,std::function<void()> inspect)
        : history_(history),inspect_(std::move(inspect)) {}
    const char* get_name() const override { return "Absorption"; }
    void update() override {}
    void clear() override { history_.reset(); }
    bool has_settings() const override { return true; }
    workspace::Json save_settings() const override { return {{"episode_count",count_}}; }
    void load_settings(const workspace::Json& j) override { workspace::read(j,"episode_count",count_); }
    void render_settings() override {
        ImGui::Checkbox("Show new episode counts",&count_);
        ImGui::TextWrapped("Otherwise show executed USD associated with qualifying episodes, at execution time. Qualification may associate earlier trades later; the as-of time is explicit.");
        ImGui::TextWrapped("Bid and ask are separate. Missing intervals are unavailable. This is observed replenishment, not hidden order size or a trading signal.");
        ImGui::TextWrapped("%s",history_.status.c_str());
    }
    ImPlotFormatter get_y_formatter() const override { return count_?format_count:format_volume_usdt; }
    void get_y_limits(double from,double to,double& low,double& high) const override {
        low=0;high=1;
        for(const auto& b:history_.bars)if(b.time>=from-history_.timeframe&&b.time<=to+history_.timeframe){
            if(b.bid)high=std::max(high,value(*b.bid)*1.65);
            if(b.ask)high=std::max(high,value(*b.ask)*1.65);
        }
    }
    void render_content(double from,double to) override {
        auto* draw=ImPlot::GetPlotDrawList();
        const auto pos=ImPlot::GetPlotPos();
        char cutoff[64]="";
        if(history_.ready)DisplayTimeZone::instance().format(history_.through,TimeZoneFormat::FullInspection,cutoff,sizeof(cutoff));
        ImPlot::PushPlotClipRect();
        const auto mouse=ImGui::GetMousePos();
        const double tf=double(history_.timeframe);
        const liquidity_response::ActivityBar* hover=nullptr;
        for(const auto& b:history_.bars){
            if(b.time+tf*.5<from||b.time-tf*.5>to)continue;
            auto column=[&](const std::optional<liquidity_response::ActivitySide>& side,bool bid){
                if(!side)return;
                const double center=double(b.time)+(bid?-.18:.18)*tf;
                const auto a=ImPlot::PlotToPixels(center-.14*tf,value(*side));
                const auto z=ImPlot::PlotToPixels(center+.14*tf,0);
                draw->AddRectFilled(a,z,Theme::u32(bid?Theme::Tokens::UP:Theme::Tokens::DOWN,.75f));
            };
            column(b.bid,true);column(b.ask,false);
            const auto left=ImPlot::PlotToPixels(double(b.time)-tf*.5,0);
            const auto right=ImPlot::PlotToPixels(double(b.time)+tf*.5,0);
            if(ImPlot::IsPlotHovered()&&mouse.x>=left.x&&mouse.x<right.x)hover=&b;
        }
        const bool observed=std::any_of(history_.bars.begin(),history_.bars.end(),[](const auto& bar){return bar.bid.has_value()||bar.ask.has_value();});
        const std::string label=history_.ready?(std::string(!observed?"No certified observations":count_?"New episodes":"Executed USD")+" | Bid / Ask | as of "+cutoff):history_.status;
        draw->AddText(ImVec2(pos.x+10,pos.y+28),Theme::u32(Theme::Tokens::TX2),label.c_str());
        ImPlot::PopPlotClipRect();
        if(hover){
            Theme::begin_tooltip();
            char stamp[64]="";DisplayTimeZone::instance().format(hover->time,TimeZoneFormat::FullInspection,stamp,sizeof(stamp));
            ImGui::TextUnformatted(stamp);
            auto detail=[&](const char* name,const std::optional<liquidity_response::ActivitySide>& side){
                if(!side){ImGui::Text("%s: unavailable",name);return;}
                ImGui::Text("%s: $%.0f executed | %d new episodes",name,side->executed,side->episodes);
                if(side->episodes>0)ImGui::Text("At detection: max %.1fs persistence | max %d refill cycles",double(side->persistence)/1000,side->cycles);
            };
            detail("Bid",hover->bid);detail("Ask",hover->ask);
            ImGui::TextWrapped("Observed through %s. Missing observations are not zero. Maxima may describe different episodes.",cutoff);
            ImGui::TextUnformatted("Click to inspect available episode evidence.");
            Theme::end_tooltip();
            if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
                std::vector<size_t> selected;
                for(size_t i=0;i<history_.episodes.size();i++){
                    const auto& e=history_.episodes[i];
                    if(e.start<hover->time+history_.timeframe&&e.updated>=hover->time)selected.push_back(i);
                }
                if(!selected.empty()){history_.select(selected);inspect_();}
            }
        }
    }
private:
    static int format_count(double value,char* buffer,int size,void*) {
        if(std::abs(value-std::round(value))>1e-6){if(size>0)buffer[0]='\0';return 0;}
        return std::snprintf(buffer,size,"%7.0f",value);
    }
    double value(const liquidity_response::ActivitySide& s) const { return count_?double(s.episodes):s.executed; }
    liquidity_response::History& history_;
    std::function<void()> inspect_;
    bool count_=false;
};
}
