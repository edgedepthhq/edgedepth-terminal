#include "chart_widget.h"
#include "core/candle_manager.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include <chrono>

void ChartWidget::update_reference_context() {
    if (!session_vwap_ && !previous_day_ && !previous_week_ && !vwap_anchor_ms_) return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(now).count();
    const auto& replay = ctx_.replay_mgr();
    const int64_t asof = replay.is_active() ? replay.interpolated_time_ms() :
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const auto* cm_ptr = &ctx_.candle_mgr();
    const auto tf_now = cm_ptr->timeframe_seconds();
    if (reference_manager_ != cm_ptr || reference_tf_ != tf_now) {
        reference_manager_ = cm_ptr; reference_tf_ = tf_now; reference_update_time_ = -1;
    }
    // Rewind invalidates immediately; regular data corrections refresh at 4 Hz.
    if (seconds - reference_update_time_ < 0.25 && asof >= reference_asof_) return;
    reference_update_time_ = seconds; reference_asof_ = asof;
    const auto& cm = ctx_.candle_mgr();
    const auto tf = cm.timeframe_seconds() * 1000;
    const auto day = reference_context::day_start(asof);
    const auto week = reference_context::week_start(asof);
    if (session_vwap_) reference_context::vwap(cm.candles(), day, asof, tf, session_vwap_data_);
    if (vwap_anchor_ms_) {
        // Keep the chosen epoch fixed. A coarser timeframe that cannot express
        // it must not silently include trades from before the anchor.
        reference_context::vwap(cm.candles(), vwap_anchor_ms_, asof, tf, anchored_vwap_data_);
    }
    if (previous_day_) previous_day_data_ = reference_context::levels(
        cm.candles(), day - reference_context::day_ms, day, tf);
    if (previous_week_) previous_week_data_ = reference_context::levels(
        cm.candles(), week - 7 * reference_context::day_ms, week, tf);
}

void ChartWidget::render_reference_context() {
    if (!ct_allows_time_overlays(chart_type_)) return;
    const auto line = [&](const char* label, const reference_context::Series& data, ImVec4 color) {
        if (data.times.empty()) return;
        ImPlotSpec style; style.LineColor = color; style.LineWeight = 1.5f;
        ImPlot::PlotLine(label, data.times.data(), data.values.data(), static_cast<int>(data.times.size()), style);
    };
    if (session_vwap_) line("Session VWAP (closed HLC3)", session_vwap_data_, Theme::Tokens::BRAND);
    if (vwap_anchor_ms_) line("Anchored VWAP (closed HLC3)", anchored_vwap_data_, Theme::Tokens::TX2);
    const auto levels = [&](const char* prefix, const reference_context::Levels& data, int64_t start) {
        if (!data.complete) return;
        const auto bounds = ImPlot::GetPlotLimits();
        const double xs[2] = {std::max(bounds.X.Min, static_cast<double>(start)), std::min(bounds.X.Max, static_cast<double>(reference_asof_))};
        if (xs[1] < xs[0]) return;
        const double values[] = {data.high, data.low, data.close};
        const char* suffixes[] = {"H", "L", "C"};
        for (int i = 0; i < 3; ++i) {
            char label[16]; snprintf(label, sizeof(label), "%s%s", prefix, suffixes[i]);
            const double ys[2] = {values[i], values[i]};
            ImPlotSpec style; style.LineColor = Theme::Tokens::TX3;
            ImPlot::PlotLine(label, xs, ys, 2, style);
            ImPlot::PlotText(label, xs[1], values[i], ImVec2(-18,-8));
        }
    };
    if (previous_day_) levels("PD", previous_day_data_, reference_context::day_start(reference_asof_));
    if (previous_week_) levels("PW", previous_week_data_, reference_context::week_start(reference_asof_));
}
