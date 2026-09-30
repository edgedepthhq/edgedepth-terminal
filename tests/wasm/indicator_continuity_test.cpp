#ifdef NDEBUG
#undef NDEBUG
#endif
#include <span>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "stream_handler.h"
#include "core/candle_manager.h"
#include "ui/indicators/volume_indicator.h"
#include "ui/indicators/vpin_indicator.h"
#include "rendering/theme.h"

extern "C" EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(EMSCRIPTEN_WEBSOCKET_T, const char*) { return EMSCRIPTEN_RESULT_SUCCESS; }
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_get_ready_state(EMSCRIPTEN_WEBSOCKET_T, unsigned short* state) { *state = 1; return EMSCRIPTEN_RESULT_SUCCESS; }
namespace Theme {
ImU32 get_buy_color_u32(uint8_t a) { return IM_COL32(0, 200, 160, a); }
ImU32 get_sell_color_u32(uint8_t a) { return IM_COL32(240, 80, 120, a); }
void tooltip(const char*, ...) {}
void begin_tooltip() { ImGui::BeginTooltip(); }
void end_tooltip() { ImGui::EndTooltip(); }
namespace Fonts {
ImFont* label() { return ImGui::GetFont(); }
ImFont* mono_sm() { return ImGui::GetFont(); }
}
}
static Terminal::Candle candle(int64_t t, double volume) {
    return {1, 10, 10, 10, 10, volume, volume, 0, 1, 0, t, true};
}
int main() {
    StreamManager stream(1);
    CandleManager cm({"binancef", "btcusdt"}, 60, stream);
    cm.mark_ready_for_replay();
    Indicators::VolumeIndicator volume;
    cm.handle_candle(candle(60000, 2));
    volume.sync_candles(cm);
    assert(volume.get_bar_count() == cm.count());
    cm.handle_candle(candle(120000, 3));
    volume.sync_candles(cm);
    double value, lo, hi;
    assert(volume.get_bar_count() == 2);
    assert(volume.get_latest_value(value) && value == 30);
    // A corrected closed candle does not change the count.
    cm.handle_candle(candle(120000, 7));
    volume.sync_candles(cm);
    assert(volume.get_bar_count() == 2);
    assert(volume.get_latest_value(value) && value == 70);
    std::vector<Terminal::Candle> older{candle(30000, 9)};
    cm.handle_candle_batch(older);
    volume.sync_candles(cm);
    volume.get_y_limits(0, 40000, lo, hi);
    assert(std::abs(hi - 99) < 1e-6);
    cm.handle_trade({10, 4, 181000, true});
    volume.sync_candles(cm);
    assert(volume.get_latest_value(value) && value == 40);
    cm.handle_trade({10, 5, 241000, true});
    volume.sync_candles(cm);
    volume.get_y_limits(180000, 180000, lo, hi);
    assert(std::abs(hi - 44) < 1e-6); // former building bar survives rollover
    cm.reset_for_seek(180000);
    volume.sync_candles(cm);
    assert(volume.get_bar_count() == 0 && !volume.get_latest_value(value));
    std::vector<Terminal::Candle> seek_batch{candle(60000, 6)};
    cm.handle_candle_batch(seek_batch);
    volume.sync_candles(cm);
    assert(volume.get_latest_value(value) && value == 60);
    cm.change_timeframe(300);
    volume.sync_candles(cm);
    assert(volume.get_bar_count() == 0 && volume.get_timeframe() == 300);
    volume.clear();
    assert(!volume.get_latest_value(value));

    Indicators::VPINIndicator vpin;
    vpin.set_points({{1000, .14f, .8f, .99f, 1, 0}, {2000, .38f, .2f, .96f, 2, 2}, {5000, .95f, .1f, 1, 3, 3}});
    vpin.set_observed_until(3000);
    assert(vpin.get_latest_value(value) && std::abs(value - .38) < 1e-6);
    vpin.get_y_limits(1500, 6000, lo, hi);
    assert(lo == 0 && hi == 1);
    vpin.load_settings({{"fit_visible", true}});
    assert(vpin.save_settings()["fit_visible"] == true);
    vpin.get_y_limits(1500, 6000, lo, hi);
    assert(lo < .14 && hi > .38 && hi < .5); // entering shelf included; future spike excluded
    vpin.load_settings({{"imbalance", true}});
    vpin.get_y_limits(1500, 6000, lo, hi);
    assert(hi > .8);
    vpin.set_observed_until(500);
    assert(!vpin.get_latest_value(value));
    vpin.get_y_limits(0, 6000, lo, hi);
    assert(lo == 0 && hi == 1);

    ImGui::CreateContext(); ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 650); io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (bool dense : {false, true}) {
        std::vector<Series::VPINPoint> points;
        for (int i = 0; i < (dense ? 10000 : 30); ++i)
            points.push_back({1000 + i, .15f + (i % 7) * .04f, .1f, .99f, 1, 2});
        vpin.set_points(points); vpin.set_observed_until(12000);
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(950, 600));
        ImGui::Begin("indicator render regression");
        if (ImPlot::BeginPlot("VPIN", ImVec2(850, 250))) {
            ImPlot::SetupAxesLimits(0, 20000, 0, 1, ImGuiCond_Always);
            // Fit-visible mode uses automatic ticks. Finish axes/grid first so
            // the bounds check measures only indicator observation geometry.
            ImPlot::SetupFinish();
            const auto* dl = ImPlot::GetPlotDrawList();
            const int before = dl->VtxBuffer.Size;
            vpin.render_content(0, 20000);
            assert(dl->VtxBuffer.Size > before);
            // Below the header, no observation geometry may extend into the future.
            const float cutoff = ImPlot::PlotToPixels(12000, 0).x;
            const float header_bottom = ImPlot::GetPlotPos().y + 25;
            for (int i = before; i < dl->VtxBuffer.Size; ++i)
                if (dl->VtxBuffer[i].pos.y > header_bottom)
                    assert(dl->VtxBuffer[i].pos.x <= cutoff + 2);
            ImPlot::EndPlot();
        }
        ImGui::End(); ImGui::Render();
        assert(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    ImPlot::DestroyContext(); ImGui::DestroyContext();
    std::puts("PASS: volume rollover/corrections/backfill/reset and VPIN scale/clock/rendering");
}
