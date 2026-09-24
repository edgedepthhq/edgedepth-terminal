#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ui/indicators/cvd_indicator.h"
#include "imgui.h"
#include "implot.h"
#include <cassert>
#include <cstdio>
namespace Theme {
ImU32 get_buy_color_u32(uint8_t alpha) { return IM_COL32(0, 255, 0, alpha); }
ImU32 get_sell_color_u32(uint8_t alpha) { return IM_COL32(255, 0, 0, alpha); }
}
int main() {
    Indicators::CVDIndicator cvd;
    cvd.add_candle(60000, 5, 2);
    cvd.add_candle(120000, 4, 2);
    cvd.update();
    double latest = 0;
    assert(cvd.get_latest_value(latest) && latest == 5);
    cvd.set_history_unavailable(true);
    cvd.update();
    assert(!cvd.get_latest_value(latest));
    cvd.set_building_candle(180000, 10, 0);
    cvd.update();
    assert(!cvd.get_latest_value(latest)); // a live print cannot fill missing history

    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(700, 500));
    ImGui::Begin("availability fixture");
    if (ImPlot::BeginPlot("CVD", ImVec2(600, 400))) {
        cvd.render_content(0, 180000); // empty/unavailable series still draws its notice
        ImPlot::EndPlot();
    }
    ImGui::End();
    ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount > 0);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    cvd.clear();
    cvd.add_candle(60000, 5, 2);
    cvd.set_building_candle(120000, 4, 2);
    cvd.set_building_unavailable(true);
    cvd.update();
    assert(!cvd.get_latest_value(latest));
    cvd.set_building_unavailable(false);
    cvd.set_building_candle(120000, 4, 2);
    cvd.update();
    assert(cvd.get_latest_value(latest) && latest == 5);
    std::puts("PASS: CVD refuses missing history and clears unavailable live values");
}
