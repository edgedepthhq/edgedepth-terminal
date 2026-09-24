#pragma once
#include "core/exposure_summary.h"
#include "rendering/exposure_overlay.h"

namespace exposure {
inline void summary_interval_label(int64_t width,char* text,size_t size) {
    if (width%86400000==0) std::snprintf(text,size,"%lld day",static_cast<long long>(width/86400000));
    else if (width%3600000==0) std::snprintf(text,size,"%lld h",static_cast<long long>(width/3600000));
    else if (width%60000==0) std::snprintf(text,size,"%lld min",static_cast<long long>(width/60000));
    else std::snprintf(text,size,"%.1f s",width/1000.);
}
inline void summary_coverage_text(const SummaryColumn& column) {
    const double duration=double(column.duration());
    ImGui::Text("Data available %.1f%% | observed flat %.1f%%",100*(column.exposure+column.flat)/duration,100*column.flat/duration);
    if (column.exposure>0) {
        ImGui::Text("Located during observed time: long %.2f%% | short %.2f%%",100*(1-column.unknown[0]/column.exposure),100*(1-column.unknown[1]/column.exposure));
        ImGui::Text("Unlocated during observed time: long %.2f%% | short %.2f%%",100*column.unknown[0]/column.exposure,100*column.unknown[1]/column.exposure);
    } else ImGui::TextUnformatted("Inventory placement unavailable: no non-flat exposure observations");
    ImGui::Text("Unavailable: missing %.1fs | expired %.1fs",column.unavailable[0]/1000.,column.unavailable[1]/1000.);
    ImGui::Text("Before capture %.1fs | not retained %.1fs",column.unavailable[2]/1000.,column.unavailable[3]/1000.);
}
inline void render_summary_inspection(const Summary& summary) {
    char interval[32];summary_interval_label(summary.scope.width,interval,sizeof(interval));
    ImGui::Text("%s averages | %zu time cells",interval,summary.columns.size());
    ImGui::TextWrapped("Colour is average exposure over each whole cell. Cells do not show exact band start/end times or the survival of individual positions.");
    ImGui::TextWrapped("Amber coverage strip: some data unavailable. Grey: full data coverage. Dotted band edge: the bin was reported for only part of the cell.");
    ImGui::Text("Recorded range [%lld, %lld) UTC ms",static_cast<long long>(summary.scope.from),static_cast<long long>(summary.scope.to));
    if (ImGui::CollapsingHeader("Inspect averaged coverage")) {
        ImGui::Text("Pinned sequences %llu to %llu",static_cast<unsigned long long>(summary.first),static_cast<unsigned long long>(summary.through));
        ImGui::TextWrapped("Display summary only. Zoom to exact recorded frames for event timing. Unknown inventory is separate from unavailable data.");
        if (!summary.definition.is_null()) ImGui::TextWrapped("Source: %s | scenario: %s",summary.definition["source"].get_ref<const std::string&>().c_str(),summary.scope.identity.scenario.c_str());
        SummaryColumn total;total.start=summary.scope.from;total.end=summary.scope.to;
        for (const auto& c:summary.columns) {
            total.exposure+=c.exposure;total.flat+=c.flat;
            for (int i=0;i<4;++i) total.unavailable[i]+=c.unavailable[i];
            for (int i=0;i<2;++i) total.unknown[i]+=c.unknown[i];
        }
        summary_coverage_text(total);
    }
}
inline size_t render_summary_overlay(const Summary& summary,int64_t cutoff,const Display& display={},const char* price_format="%.12g",Visibility* visibility=nullptr) {
    if (!summary_at(summary,cutoff)) return 0; // Cannot crop a future-aware average.
    auto* draw=ImPlot::GetPlotDrawList();
    const auto limits=ImPlot::GetPlotLimits();
    const auto plot_pos=ImPlot::GetPlotPos(),plot_size=ImPlot::GetPlotSize();
    const auto mouse=ImPlot::GetPlotMousePos();
    const auto mouse_pixels=ImGui::GetIO().MousePos;
    const bool hovered=ImPlot::IsPlotHovered();
    size_t count=0;
    ImPlot::PushPlotClipRect();
    for (const auto& column:summary.columns) {
        if (column.end<=limits.X.Min || column.start>=limits.X.Max) continue;
        const float left=std::max(plot_pos.x,ImPlot::PlotToPixels(double(column.start),limits.Y.Max).x);
        const float right=std::min(plot_pos.x+plot_size.x,ImPlot::PlotToPixels(double(column.end),limits.Y.Max).x);
        const float strip_y=plot_pos.y+plot_size.y-6;
        // A coverage indicator, not a claim about where within the cell a gap lay.
        draw->AddRectFilled(ImVec2(left,strip_y),ImVec2(right,strip_y+4),Theme::u32(column.missing()>0?Theme::Tokens::WARN:Theme::Tokens::TX3));
        if (hovered && mouse.x>=column.start && mouse.x<column.end && mouse_pixels.y>=strip_y-3) {
            Theme::begin_tooltip();ImGui::TextUnformatted("Coverage within this time cell");summary_coverage_text(column);Theme::end_tooltip();
        }
        for (const auto& cell:column.cells) {
            const auto& band=cell.band;
            if (visibility) visibility->observe(band,display,limits.Y.Min,limits.Y.Max);
            if (!shown(band,display) || band.high<limits.Y.Min || band.low>limits.Y.Max) continue;
            const auto a=ImPlot::PlotToPixels(double(column.start),band.high),b=ImPlot::PlotToPixels(double(column.end),band.low);
            draw->AddRectFilled(a,b,band_color(band.mass,display));++count;
            // Filled rectangles preserve exact cell bounds. Repeated outlines
            // overpower narrow candles and turn minute cells into vertical stripes.
            if (cell.active_ms<column.duration() && b.y-a.y>=2) {
                for (float x=left;x<right;x+=6)
                    draw->AddLine(ImVec2(x,b.y-1),ImVec2(std::min(x+2,right),b.y-1),Theme::u32(Theme::Tokens::TX1));
            }
            if (hovered && mouse.x>=column.start && mouse.x<column.end && mouse.y>=band.low && mouse.y<band.high && mouse_pixels.y<strip_y-3) {
                Theme::begin_tooltip();
                ImGui::Text("%s average %.3f%% of one side OI",band.side==0?"Long":"Short",100*band.mass);
                ImGui::Text("Cell duration %.1fs | bin reported %.1fs",column.duration()/1000.,cell.active_ms/1000.);
                ImGui::TextUnformatted("Price bounds");ImGui::SameLine();ImGui::Text(price_format,band.low);ImGui::SameLine();ImGui::Text(price_format,band.high);
                summary_coverage_text(column);
                ImGui::TextUnformatted("Reported duration does not identify continuous positions.");
                Theme::end_tooltip();
            }
        }
    }
    ImPlot::PopPlotClipRect();return count;
}
} // namespace exposure
