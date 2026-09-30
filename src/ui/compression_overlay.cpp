#include "ui/compression_overlay.h"
#include "core/compression_geometry.h"
#include "core/research_url.h"
#include "rendering/theme.h"
#include "implot.h"
#include <nlohmann/json.hpp>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace compression {
namespace {
Geometry geometry;
std::string symbol, last_json;
bool enabled = false;
void read() {
#ifdef __EMSCRIPTEN__
    static int frame = -1;
    if (frame == ImGui::GetFrameCount()) return;
    frame = ImGui::GetFrameCount();
    // Main-thread bridge, shared by all charts; decode only when snapshot/toggle changes.
    const char* raw = emscripten_run_script_string("JSON.stringify(window.__EDGEDEPTH_COMPRESSION__ || null)");
    if (!raw || last_json == raw) return;
    last_json = raw; enabled = false; geometry = {}; symbol.clear();
    const auto j = nlohmann::json::parse(raw, nullptr, false);
    if (!j.is_object() || !j.contains("v") || !j["v"].is_number_integer() || j["v"] != 1 || !j.contains("venue") || j["venue"] != "binancef") return;
    const char* numeric[] = {"asof","peak","touch","origin","supportFrom","supportTo","price","slope","supportPrice","supportSlope"};
    for (const auto* k : numeric) if (!j.contains(k) || !j[k].is_number()) return;
    if (!j.contains("symbol") || !j["symbol"].is_string() || !j.contains("enabled") || !j["enabled"].is_boolean()) return;
    geometry = {j["asof"],j["peak"],j["touch"],j["origin"],j["supportFrom"],j["supportTo"],j["price"],j["slope"],j["supportPrice"],j["supportSlope"]};
    symbol = j["symbol"].get<std::string>(); enabled = j["enabled"].get<bool>() && geometry.valid();
#endif
}
void open(const Terminal::Pair& pair, const char* tf) {
#ifdef __EMSCRIPTEN__
    const auto market = research_url::normalize_symbol(pair.symbol);
    EM_ASM({window.dispatchEvent(new CustomEvent('edgedepth:compression-open', {detail:{venue:'binancef',symbol:UTF8ToString($0),tf:UTF8ToString($1)}}));}, market.c_str(), tf);
#else
    (void)pair; (void)tf;
#endif
}
}
void menu(const Terminal::Pair& pair, bool replay) {
    if (pair.exchange != "binancef") return;
    if (Theme::begin_menu_group("Compression")) {
        if (replay) Theme::menu_note("Open a saved setup from its investigation to inspect it in the replay sidebar.");
        else {
            if (Theme::menu_toggle("Inspect 1h compression", false, false)) open(pair, "1h");
            if (Theme::menu_toggle("Inspect 4h compression", false, false)) open(pair, "4h");
            Theme::menu_note("Experimental watchlist: admin access. Recognition and alert eligibility are separate.");
        }
        Theme::end_menu_group();
    }
}
void render(const Terminal::Pair& pair, int64_t clock) {
    read();
    if (!enabled || pair.exchange != "binancef" || research_url::normalize_symbol(pair.symbol) != symbol || !geometry.visible(clock)) return;
    const auto lim = ImPlot::GetPlotLimits();
    const double end = std::min(lim.X.Max, static_cast<double>(clock));
    const double begin = std::max(lim.X.Min, static_cast<double>(geometry.peak));
    if (end <= begin) return;
    auto* draw = ImPlot::GetPlotDrawList();
    const auto resistance_color = ImGui::ColorConvertFloat4ToU32(Theme::Tokens::LOGO);
    const auto support_color = ImGui::ColorConvertFloat4ToU32(Theme::Colors::TEXT_SECONDARY);
    ImPlot::PushPlotClipRect();
    auto line = [&](double start, bool support) {
        if (start >= end) return;
        const auto p1 = ImPlot::PlotToPixels(start, support ? geometry.support(start) : geometry.resistance(start));
        const auto p2 = ImPlot::PlotToPixels(end, support ? geometry.support(end) : geometry.resistance(end));
        draw->AddLine(p1, p2, support ? support_color : resistance_color, 2.0f);
        const auto mouse = ImGui::GetMousePos();
        const float dx = p2.x-p1.x, dy = p2.y-p1.y;
        const float len = dx*dx+dy*dy;
        const float u = len > 0 ? std::clamp(((mouse.x-p1.x)*dx+(mouse.y-p1.y)*dy)/len,0.0f,1.0f) : 0;
        const float ex = mouse.x-p1.x-u*dx, ey = mouse.y-p1.y-u*dy;
        if (ImPlot::IsPlotHovered() && ex*ex+ey*ey < 36) {
            Theme::tooltip("Compression: click to inspect the saved structure");
#ifdef __EMSCRIPTEN__
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                EM_ASM({window.dispatchEvent(new CustomEvent('edgedepth:compression-inspect'));});
#endif
        }
    };
    line(begin, false); line(std::max(begin, static_cast<double>(geometry.support_from)), true);
    for (const auto t : {geometry.peak, geometry.touch})
        if (t >= begin && t <= end) draw->AddCircle(ImPlot::PlotToPixels(static_cast<double>(t), geometry.resistance(static_cast<double>(t))), 4, resistance_color, 0, 2);
    for (const auto t : {geometry.support_from, geometry.support_to})
        if (t >= begin && t <= end) draw->AddCircle(ImPlot::PlotToPixels(static_cast<double>(t), geometry.support(static_cast<double>(t))), 4, support_color, 0, 2);
    ImPlot::PopPlotClipRect();
}
}
