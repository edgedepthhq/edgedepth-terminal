// ═══════════════════════════════════════════════════════════════════════════════
// chart_widget_scrub_preview.cpp - ghost scrub-preview candle pass
//
// While the user aims a scrub on the replay transport (hover with intent delay,
// or an active drag), this pass renders the PreviewCandleStore's candles AHEAD
// of the playhead so they know exactly where to scrub:
//   - playhead → aimed-at time: stronger ghost (the "what you'd have watched")
//   - beyond the aimed-at time: faint ghost (context for aiming further)
//   - a vertical line at the aimed-at time (the landing marker)
// Up/down tint is preserved at low alpha deliberately: direction IS the
// information users scrub by. Reads ONLY the preview store - never
// CandleManager - and mirrors plot_candles' three zoom tiers so a fitted map
// view stays cheap. Lives in its own translation unit per the God-file rule
// (chart_widget.cpp is slated for splits, not growth).
// ═══════════════════════════════════════════════════════════════════════════════

#include "ui/chart_widget.h"
#include "core/footprint_manager.h"     // thin-body tier when footprint is active
#include "core/preview_candle_store.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include "implot.h"
#include <algorithm>

namespace {
// Ghost alpha tiers (0..255). Tuned for bg-0 with the liq Field behind;
// values reviewed in the screenshot round.
constexpr uint8_t kGhostAlphaAimed  = 82;   // playhead → aim (~0.32)
constexpr uint8_t kGhostAlphaBeyond = 36;   // beyond the aim (~0.14)
}  // namespace

void ChartWidget::render_scrub_preview(double visible_x_min, double visible_x_max) {
    auto& replay = ctx_.replay_mgr();
    const int64_t preview_ms = replay.scrub_preview_ms();
    if (preview_ms <= 0) return;
    const PreviewCandleStore* pv = replay.preview_candles();
    if (!pv || !pv->ready()) return;

    const auto& ts = pv->timestamps();
    const auto& opens = pv->opens();
    const auto& highs = pv->highs();
    const auto& lows = pv->lows();
    const auto& closes = pv->closes();

    const double tf_ms = static_cast<double>(pv->timeframe_sec()) * 1000.0;
    if (tf_ms <= 0.0) return;
    const double half_width = tf_ms * 0.35;  // same body/gap ratio as plot_candles

    // Ghosts exist strictly AFTER the playhead: candles whose period has begun
    // are real (finalized or building) and already drawn by plot_candles.
    const double play_ms = static_cast<double>(replay.interpolated_time_ms());
    const double from = std::max(play_ms, visible_x_min - tf_ms);
    const double to = visible_x_max;
    if (to <= from) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const auto plot_max = ImVec2(plot_min.x + plot_size.x, plot_min.y + plot_size.y);

    // Same three zoom tiers as plot_candles, computed against the PREVIEW tf
    // (the store may be coarser than the chart when the backend snapped the
    // request up to fit its batch cap).
    const double visible_candles =
        (visible_x_max - visible_x_min) / tf_ms;
    const double pixels_per_candle =
        visible_candles > 0.0 ? plot_size.x / visible_candles : plot_size.x;
    const bool bar_mode = pixels_per_candle < 2.0;
    const bool force_thin = !bar_mode && (pixels_per_candle < 5.0 ||
                                          ctx_.footprint_mgr().enabled);

    ImPlot::PushPlotClipRect();

    // First preview candle strictly after the playhead (period not yet begun).
    size_t i = static_cast<size_t>(
        std::distance(ts.begin(), std::upper_bound(ts.begin(), ts.end(), play_ms)));

    if (bar_mode) {
        // Aggregate candles sharing a pixel column into one H/L line, split by
        // tier at the aim point (a column is "aimed" while it starts before it).
        int last_px_col = -1;
        double col_high = -1e18, col_low = 1e18;
        double col_open = 0, col_close = 0;
        bool col_started = false;
        bool col_aimed = true;
        auto flush_col = [&]() {
            if (!col_started) return;
            const uint8_t a = col_aimed ? kGhostAlphaAimed : kGhostAlphaBeyond;
            const ImU32 col = (col_close >= col_open)
                ? Theme::get_buy_color_u32(a)
                : Theme::get_sell_color_u32(a);
            const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
            const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
            draw_list->AddLine(
                ImVec2(static_cast<float>(last_px_col), top.y),
                ImVec2(static_cast<float>(last_px_col), bot.y),
                col, 1.0f);
        };
        for (; i < ts.size(); i++) {
            if (ts[i] > to) break;
            const ImVec2 px = ImPlot::PlotToPixels(ts[i], closes[i]);
            const int px_col = static_cast<int>(px.x);
            if (px_col != last_px_col && col_started) {
                flush_col();
                col_high = -1e18;
                col_low = 1e18;
                col_started = false;
            }
            if (!col_started) {
                col_open = opens[i];
                col_started = true;
                col_aimed = ts[i] <= static_cast<double>(preview_ms);
            }
            col_high = std::max(col_high, highs[i]);
            col_low = std::min(col_low, lows[i]);
            col_close = closes[i];
            last_px_col = px_col;
        }
        flush_col();
    } else {
        for (; i < ts.size(); i++) {
            if (ts[i] > to) break;
            const uint8_t a = ts[i] <= static_cast<double>(preview_ms)
                ? kGhostAlphaAimed : kGhostAlphaBeyond;
            draw_single_candle(draw_list, ts[i], opens[i], closes[i],
                               lows[i], highs[i], half_width,
                               plot_min, plot_max, force_thin, a);
        }
    }

    // Landing marker: a vertical accent line at the aimed-at time, full plot
    // height, subtle enough to sit under the crosshair.
    {
        const ImVec2 aim_px = ImPlot::PlotToPixels(static_cast<double>(preview_ms), 0.0);
        if (aim_px.x >= plot_min.x && aim_px.x <= plot_max.x) {
            draw_list->AddLine(ImVec2(aim_px.x, plot_min.y),
                               ImVec2(aim_px.x, plot_max.y),
                               Theme::u32(Theme::Tokens::BRAND, 0.55f), 1.0f);
        }
    }

    ImPlot::PopPlotClipRect();
}
