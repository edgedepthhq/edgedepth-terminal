#pragma once
#include "core/exposure_publications.h"
#include "rendering/theme.h"
#include "implot.h"

namespace exposure {
struct Display {
    float minimum_percent=0.01f;
    float peak_percent=25.0f;
    int side=0; // Both, long, short. Never sum sides or scenarios.
    float opacity=0.45f; // Background heatmap; candles remain the foreground.
};
inline bool shown(const Band& b,const Display& display) {
    return b.mass*100>=display.minimum_percent && (display.side==0 || b.side==display.side-1);
}
inline ImU32 band_color(double mass,const Display& display) {
    // Fixed, inspectable square-root contrast. Panning/new frames cannot rescale.
    const float intensity=float(std::sqrt(std::clamp(mass*100/std::max(1.f,display.peak_percent),0.,1.)));
    auto color=ImPlot::SampleColormap(intensity,ImPlotColormap_Viridis);
    color.w=std::clamp(display.opacity,0.f,1.f);
    return ImGui::ColorConvertFloat4ToU32(color);
}
inline void render_display_controls(Display& display) {
    const bool wide=ImGui::GetContentRegionAvail().x>=820;
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("Position side",&display.side,"Both\0Long\0Short\0");
    if (wide) ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("Hide below",&display.minimum_percent,0,10,"%.2f%% OI");
    if (wide) ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("Brightest at",&display.peak_percent,1,100,"%.0f%% OI");
    if (ImGui::Button("Inspect small bands")) { display.minimum_percent=0.01f;display.peak_percent=1.f; }
    ImGui::TextWrapped("Inspection uses a fixed 0.01%% minimum and 1%% colour scale. Price auto-fit includes qualifying bands. The native 7%% buffer is an uncalibrated assumption.");
    ImGui::TextUnformatted("Band concentration | fixed square-root color scale | share of ONE side's OI");
    const auto pos=ImGui::GetCursorScreenPos();
    const float width=std::min(360.f,ImGui::GetContentRegionAvail().x);
    auto* draw=ImGui::GetWindowDrawList();
    for (int i=0;i<64;++i) {
        const double fraction=double(i)/63;
        draw->AddRectFilled(ImVec2(pos.x+width*i/64,pos.y),ImVec2(pos.x+width*(i+1)/64,pos.y+8),
            band_color(fraction*fraction*display.peak_percent/100,display));
    }
    ImGui::Dummy(ImVec2(width,10));
    ImGui::Text("0%%          %.1f%%          %.1f%%          %.0f%%+",
        display.peak_percent/9,display.peak_percent*4/9,display.peak_percent);
}
struct Visibility {
    size_t reported=0,filtered=0,outside=0;
    void observe(const Band& band,const Display& display,double low,double high) {
        ++reported;
        if (!shown(band,display)) ++filtered;
        else if (band.high<low || band.low>high) ++outside;
    }
};
// Draw immutable price intervals without blur, interpolation or moving levels.
inline size_t render_overlay(const Timeline& timeline,int64_t clock,const Display& display={},Visibility* visibility=nullptr) {
    auto* draw=ImPlot::GetPlotDrawList();
    const auto limits=ImPlot::GetPlotLimits();
    size_t count=0;
    ImPlot::PushPlotClipRect();
    rectangles(timeline,clock,[&](const Publication& publication,const Band& band,int64_t start,int64_t end) {
        if (double(end)<=limits.X.Min || double(start)>=limits.X.Max) return;
        if (visibility) visibility->observe(band,display,limits.Y.Min,limits.Y.Max);
        if (!shown(band,display) || band.high<limits.Y.Min || band.low>limits.Y.Max) return;
        const ImVec2 a=ImPlot::PlotToPixels(double(start),band.high);
        const ImVec2 b=ImPlot::PlotToPixels(double(end),band.low);
        // No minimum-height expansion or visible-range normalization.
        draw->AddRectFilled(a,b,band_color(band.mass,display));
        const auto mouse=ImPlot::GetPlotMousePos();
        if (ImPlot::IsPlotHovered() && mouse.x>=double(start) && mouse.x<double(end) && mouse.y>=band.low && mouse.y<band.high) {
            Theme::begin_tooltip();
            ImGui::Text("%s positions | %.3f%% of one side OI",band.side==0?"Long":"Short",band.mass*100);
            ImGui::Text("Price [%.12g, %.12g)",band.low,band.high);
            ImGui::Text("Scenario %s | sequence %llu",timeline.identity().scenario.c_str(),static_cast<unsigned long long>(publication.sequence));
            Theme::end_tooltip();
        }
        ++count;
    });
    ImPlot::PopPlotClipRect();
    return count;
}
inline void render_inspection(const Timeline& timeline,int64_t clock) {
    const auto state=timeline.at(clock);
    ImGui::Text("State: %s",state.status);
    if (!state.reason.empty()) ImGui::TextWrapped("%s",state.reason.c_str());

    ImGui::TextWrapped("Scenarios are alternatives, never additive exposure. Sensitivity is not confidence or probability.");
    if (!state.publication) return;
    const auto& p=*state.publication;
    ImGui::Text("Sequence: %llu | publication: %lld | expiry: %lld UTC ms",
        static_cast<unsigned long long>(p.sequence),static_cast<long long>(p.published),static_cast<long long>(p.expires));
    if (std::string_view(state.status)!="available") return;
    const auto& f=p.frame;
    ImGui::Text("Unknown long %.2f%% | short %.2f%%",100*f["unknown_long_short"][0].get<double>(),100*f["unknown_long_short"][1].get<double>());
    if (ImGui::CollapsingHeader("Inspect recorded frame")) {
        ImGui::Text("Decision: %lld | input availability: %lld UTC ms",
            static_cast<long long>(f["as_of_ms"].get<int64_t>()),static_cast<long long>(f["available_at_ms"].get<int64_t>()));
        if (f.contains("public_provenance") && !f["public_provenance"].is_null()) {
            const auto& provenance=f["public_provenance"];
            ImGui::TextWrapped("Public-input fixed-buffer scenario; not account liquidation levels or a calibrated forecast.");
            ImGui::Text("Assumptions valid: %lld to %lld UTC ms",
                static_cast<long long>(provenance["assumptions"]["valid_from_ms"].get<int64_t>()),
                static_cast<long long>(provenance["assumptions"]["valid_until_ms"].get<int64_t>()));
            ImGui::TextWrapped("Assumption digest (verified by backend): %s",provenance["assumptions_sha256"].get_ref<const std::string&>().c_str());
        }
        const char* names[]={"OI","Trade","Mark"};
        for (int i=0;i<3;++i) {
            const auto& c=f["oi_trade_mark_clocks"][i];
            ImGui::Text("%s event %lld | received %lld",names[i],static_cast<long long>(c["event_ms"].get<int64_t>()),static_cast<long long>(c["available_ms"].get<int64_t>()));
        }
        const auto& s=f["scenario"];
        ImGui::Text("Survival %s | turnover %.3f | buffer %.3f | maintenance %.4f",
            s["survival"].get_ref<const std::string&>().c_str(),s["turnover"].get<double>(),s["buffer"].get<double>(),s["maintenance"].get<double>());
        ImGui::TextWrapped("%s | %s",f["status"].get_ref<const std::string&>().c_str(),f["coverage"].get_ref<const std::string&>().c_str());
        for (int side=0;side<2;++side) {
            const auto& reasons=f["unknown_reasons_long_short"][side];
            for (auto it=reasons.begin();it!=reasons.end();++it)
                ImGui::Text("%s unknown: %s %.3f%%",side==0?"Long":"Short",it.key().c_str(),it.value().get<double>()*100);
        }
        ImGui::TextUnformatted("Exact bins: [low, high), fraction_of_one_side_oi");
        ImGui::BeginChild("bins",ImVec2(0,90),ImGuiChildFlags_Borders);
        for (const auto& b:p.bands)
            ImGui::Text("%s #%lld [%.12g, %.12g) mass %.12g",b.side==0?"long":"short",static_cast<long long>(b.index),b.low,b.high,b.mass);
        ImGui::EndChild();
    }
}
} // namespace exposure
