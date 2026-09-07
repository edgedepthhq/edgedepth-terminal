#include "ui/chart_widget.h"
#include "core/candle_manager.h"
#include "core/orderbook_manager.h"
#include "core/entitlements.h"
#include "replayer/replay_manager.h"
#include "core/education_boot.h"
#include "ui/upsell_modal.h"
#include "rendering/theme.h"
#include <cmath>

void ChartWidget::set_rt_mode(bool on) {
    if (on && Entitlements::hosted() && !Entitlements::is_pro() &&
        !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack()) {
        ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
            "Real-time depth and trade bubbles are a hosted Pro view.", "realtime_depth");
        return;
    }
    rt_mode_ = on;
    if (on) {
        chart_type_ = ChartType::Line;
        rt_candles_ = false;
        rt_paused_ = false;
        heatmap_enabled_ = true;
        rt_span_ms_ = 60000;
        ctx_.candle_mgr().set_follow_live(true);
        if (!rt_subscribed_) {
            ctx_.stream_mgr().subscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
            rt_subscribed_ = true;
        }
    } else if (rt_subscribed_) {
        ctx_.stream_mgr().unsubscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
        rt_subscribed_ = false;
    }
}

void ChartWidget::render_realtime_settings() {
    if (!ctx_.replay_mgr().is_active() && ImGui::Checkbox("Pause display", &rt_paused_)) {
        if (rt_paused_) rt_paused_trades_ = ctx_.candle_mgr().realtime_trades().trades();
        else rt_paused_trades_.clear();
    }
    ImGui::Checkbox("1s observed candles", &rt_candles_);
    ImGui::Checkbox("Trade-price line", &rt_trade_line_);
    chart_type_ = rt_candles_ ? ChartType::Candles : ChartType::Line;
    ImGui::Checkbox("Trade bubbles", &rt_bubbles_);
    ImGui::Checkbox("Auto market size", &rt_auto_bubbles_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Uses the 75th percentile of received trade values over the last 60 seconds. Updates gradually every 5 seconds; independent of zoom. One bubble per record, with no inferred fills.");
    if (rt_auto_bubbles_) ImGui::Text("Auto minimum: %.4g quote", rt_bubble_scale_.minimum());
    ImGui::BeginDisabled(rt_auto_bubbles_);
    ImGui::SetNextItemWidth(150);
    ImGui::InputFloat("Minimum trade value", &rt_min_notional_, 1000, 10000, "%.0f");
    if (!std::isfinite(rt_min_notional_)) rt_min_notional_ = 10000;
    rt_min_notional_ = std::max(1.0f, rt_min_notional_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Price x quantity in quote units. One bubble per received record; the exchange may aggregate fills. Radius starts at 7px and is capped at 28px.");
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Depth: 100ms samples, 2 minutes retained");
    ImGui::TextUnformatted("Fixed price fidelity: Layers > Depth settings");
}

void ChartWidget::on_rewind(int64_t) {
    // The replay owner restores/replays depth separately. Never keep future
    // samples or GPU cells from the preceding traversal.
    rt_renderer_.reset();
    rt_latest_.reset();
    rt_pending_.clear();
    rt_samples_.clear();
    rt_serial_ = 0;
    rt_paused_ = false;
    rt_paused_trades_.clear();
    rt_book_valid_ = false;
    rt_bubble_scale_ = {};
}

void ChartWidget::update_realtime() {
    if (rt_generation_ != ctx_.ob_mgr().realtime_generation()) {
        on_rewind(0);
        rt_generation_ = ctx_.ob_mgr().realtime_generation();
    }
    if (!(tick_size_ > 0) || rt_paused_) return;
    if (!rt_renderer_) {
        rt_renderer_ = std::make_unique<ShaderHeatmapRenderer>();
        rt_renderer_->configure_realtime(tick_size_);
        rt_pending_.reserve(1200);
        rt_prices_.reserve(1024);
    }
    const int64_t clock = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    rt_renderer_->set_observation_clock_ms(clock);
    rt_book_valid_ = ctx_.ob_mgr().copy_realtime_since(pair_, rt_serial_, rt_pending_);
    // Samples past the as-of clock stay pending.
    for (const auto& sample : rt_pending_) {
        if (sample->timestamp_ms > clock) break;
        rt_serial_ = sample->serial;
        if (sample->timestamp_ms <= clock - RealtimeDepthHistory::retention_ms) continue;
        rt_prices_.clear();
        for (const auto& level : sample->levels) rt_prices_[level.price] += float(level.size);
        rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask) * 0.5);
        rt_latest_ = sample;
        rt_samples_.push_back(sample);
        while (rt_samples_.size() > 1200 || rt_samples_.front()->timestamp_ms <= clock - 120000)
            rt_samples_.pop_front();
    }
    if (!rt_book_valid_ && rt_latest_) {
        // Discard the final sampling bin on interruption rather than carrying
        // it through an invalid sequence inside that bin.
        rt_renderer_->invalidate_observation(rt_latest_->timestamp_ms);
        if (!rt_samples_.empty() && rt_samples_.back() == rt_latest_) rt_samples_.pop_back();
        rt_latest_.reset();
    }
    rt_renderer_->set_observation_hold(rt_book_valid_ && rt_latest_ &&
        clock - rt_latest_->timestamp_ms <= 15000 ? clock : 0);

}

const std::deque<Terminal::Trade>& ChartWidget::realtime_trades() const {
    return rt_paused_ ? rt_paused_trades_ : ctx_.candle_mgr().realtime_trades().trades();
}

void ChartWidget::render_realtime() {
    const auto limits = ImPlot::GetPlotLimits();
    if (!ctx_.candle_mgr().follow_live()) rt_span_ms_ = std::clamp(limits.X.Size(), 5000.0, 120000.0);
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const auto& trades = realtime_trades();
    ImVec2 previous{};
    int64_t previous_ms = 0;
    // Last point per screen pixel bounds line geometry to the plot width.
    for (const auto& trade : trades) {
        if (trade.timestamp_ms < limits.X.Min || trade.timestamp_ms <= rt_clock_ms_ - 120000) continue;
        if (trade.timestamp_ms > rt_clock_ms_ || trade.timestamp_ms > limits.X.Max) break;
        const ImVec2 point = ImPlot::PlotToPixels(double(trade.timestamp_ms), trade.price);
        if (rt_trade_line_ && !rt_candles_ && previous_ms && point.x >= previous.x + 1.0f &&
            trade.timestamp_ms - previous_ms <= 1000)
            dl->AddLine(previous, point, Theme::u32(Theme::Tokens::TX1), 1.0f);
        if (!previous_ms || point.x >= previous.x + 1.0f || trade.timestamp_ms - previous_ms > 1000) {
            previous = point; previous_ms = trade.timestamp_ms;
        }
    }
    // Historical best bid/ask are steps at original observation timestamps.
    // Quiet intervals hold the prior quote; a new synchronization epoch breaks it.
    const RealtimeDepthHistory::Sample* previous_book = nullptr;
    for (const auto& sample : rt_samples_) {
        if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > limits.X.Max) break;
        if (sample->timestamp_ms < limits.X.Min) { previous_book = sample.get(); continue; }
        if (previous_book && !sample->segment_start) {
            for (int side = 0; side < 2; ++side) {
                const double before = side ? previous_book->ask : previous_book->bid;
                const double after = side ? sample->ask : sample->bid;
                const ImVec2 a = ImPlot::PlotToPixels(double(previous_book->timestamp_ms), before);
                const ImVec2 b = ImPlot::PlotToPixels(double(sample->timestamp_ms), before);
                const ImVec2 c = ImPlot::PlotToPixels(double(sample->timestamp_ms), after);
                const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
                dl->AddLine(a, b, color, 1.25f);
                dl->AddLine(b, c, color, 1.25f);
            }
        }
        previous_book = sample.get();
    }
    if (rt_candles_) {
        // One-second OHLC from retained trade records only. Empty seconds stay
        // absent; a paused frame uses its frozen records and the same as-of clock.
        Terminal::Candle candle{};
        bool have = false;
        auto flush = [&]() {
            if (!have) return;
            const ImVec2 hi = ImPlot::PlotToPixels(double(candle.timestamp_ms) + 500, candle.high);
            const ImVec2 lo = ImPlot::PlotToPixels(double(candle.timestamp_ms) + 500, candle.low);
            const ImVec2 open = ImPlot::PlotToPixels(double(candle.timestamp_ms) + 150, candle.open);
            const ImVec2 close = ImPlot::PlotToPixels(double(candle.timestamp_ms) + 850, candle.close);
            const ImU32 color = Theme::u32(candle.close >= candle.open ? Theme::Tokens::UP : Theme::Tokens::DOWN);
            dl->AddLine(hi, lo, color);
            dl->AddRectFilled(ImVec2(open.x, std::min(open.y, close.y)),
                ImVec2(close.x, std::max(open.y + 1, close.y)), color);
        };
        for (const auto& trade : trades) {
            if (trade.timestamp_ms < limits.X.Min - 1000) continue;
            if (trade.timestamp_ms > rt_clock_ms_ || trade.timestamp_ms > limits.X.Max) break;
            const int64_t bucket = trade.timestamp_ms / 1000 * 1000;
            if (!have || bucket != candle.timestamp_ms) {
                flush(); candle = {}; candle.timestamp_ms = bucket;
                candle.open = candle.high = candle.low = candle.close = trade.price; have = true;
            } else {
                candle.high = std::max(candle.high, trade.price);
                candle.low = std::min(candle.low, trade.price); candle.close = trade.price;
            }
        }
        flush();
    }
    if (rt_auto_bubbles_) rt_bubble_scale_.update(trades, rt_clock_ms_);
    const double minimum = rt_auto_bubbles_ ? rt_bubble_scale_.minimum() : rt_min_notional_;
    int bubbles = 0;
    if (rt_bubbles_) for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
        const auto& trade = *it;
        if (trade.timestamp_ms > rt_clock_ms_ || trade.timestamp_ms > limits.X.Max) continue;
        if (trade.timestamp_ms < limits.X.Min || trade.timestamp_ms <= rt_clock_ms_ - 120000) break;
        const double notional = trade.price * trade.qty;
        if (!(minimum > 0) || notional < minimum || !std::isfinite(notional)) continue;
        if (++bubbles > 1500) break;
        const ImVec2 point = ImPlot::PlotToPixels(double(trade.timestamp_ms), trade.price);
        const float radius = std::min(28.0f, 7.0f * float(std::sqrt(notional / minimum)));
        auto color = trade.is_buy ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        color.w = 0.72f;
        dl->AddCircleFilled(point, radius, ImGui::GetColorU32(color), 24);
        color.w = 1.0f;
        dl->AddCircle(point, radius, ImGui::GetColorU32(color), 24, 1.0f);
    }
    const bool fresh = rt_book_valid_ && rt_latest_ &&
        rt_latest_->timestamp_ms <= rt_clock_ms_ && rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000;
    if (fresh && limits.X.Max > rt_clock_ms_) {
        const auto& book = *rt_latest_;
        const float right = ImPlot::GetPlotPos().x + ImPlot::GetPlotSize().x;
        const float edge = ImPlot::PlotToPixels(double(rt_clock_ms_), book.bid).x;
        for (int side = 0; side < 2; ++side) {
            const double price = side ? book.ask : book.bid;
            const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
            dl->AddLine(ImPlot::PlotToPixels(double(book.timestamp_ms), price),
                ImPlot::PlotToPixels(double(rt_clock_ms_), price), color, 1.25f);
        }
        const float width = std::min(70.0f, std::max(0.0f, right - edge - 5));
        double max_qty = 0;
        for (const auto& level : book.levels)
            if (level.price >= limits.Y.Min && level.price <= limits.Y.Max) max_qty = std::max(max_qty, level.size);
        if (max_qty > 0) for (const auto& level : book.levels) {
            if (level.price < limits.Y.Min || level.price > limits.Y.Max) continue;
            const float y = ImPlot::PlotToPixels(0, level.price).y;
            auto color = level.price <= book.bid ? Theme::Tokens::UP : Theme::Tokens::DOWN;
            color.w = 0.45f;
            dl->AddLine(ImVec2(right - width * float(level.size / max_qty), y), ImVec2(right, y),
                ImGui::GetColorU32(color), 2.0f);
        }
        for (int side = 0; side < 2; ++side) {
            const double price = side ? book.ask : book.bid;
            const float y = ImPlot::PlotToPixels(0, price).y;
            const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
            for (float x = edge; x < right; x += 10) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5, right), y), color);
        }
    }
    const char* note = rt_paused_ ? "RT paused / feed continues within the 2-minute retention limit" : !fresh ? "RT: waiting for fresh synchronized depth" :
        (bubbles > 1500 ? "RT: newest 1,500 qualifying trade records shown" :
         "RT: sampled book held between updates / bubbles are trade records");
    const ImVec2 pos = ImPlot::GetPlotPos();
    const float note_y = pos.y + (ctx_.replay_mgr().is_active() ? 60.0f : 34.0f);
    if (!rt_samples_.empty() && rt_samples_.front()->timestamp_ms >= limits.X.Min &&
        rt_samples_.front()->timestamp_ms <= std::min(limits.X.Max, double(rt_clock_ms_))) {
        const float start = ImPlot::PlotToPixels(double(rt_samples_.front()->timestamp_ms), 0).x;
        dl->AddLine(ImVec2(start, note_y + 26), ImVec2(start, pos.y + ImPlot::GetPlotSize().y),
            Theme::u32(Theme::Tokens::TX2, 0.45f));
        dl->AddText(ImVec2(start + 6, note_y + 18), Theme::u32(Theme::Tokens::TX2), "Observed depth starts here");
    }
    dl->AddText(ImVec2(pos.x + 12, note_y), Theme::u32(Theme::Tokens::TX2), note);
    ImPlot::PopPlotClipRect();
}

void ChartWidget::configure_depth_fidelity(ShaderHeatmapRenderer& renderer) {
    if (rt_mode_) {
        // Keep historical price groups fixed as the live price axis auto-fits.
        renderer.set_bucket_multiplier(heatmap_bucket_multiplier_);
        return;
    }
    // Viewport-adaptive bucket multiplier: ensure each heatmap cell
    // is at least min_cell_px pixels tall. This adapts to any coin at
    // any zoom level - BTC on 1m stays at native resolution, LABUSDT
    // on 4h (300%+ range) aggregates into visible bands automatically.
    // The UHD/HD/SD combo controls min_cell_px (detail preference).
    const double native_bucket = renderer.get_native_bucket_size();
    if (native_bucket > 0 && heatmap_adapt_to_zoom_) {
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        const double visible_range = limits.Y.Max - limits.Y.Min;
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const float plot_height_px = plot_size.y;

        // UHD/HD/SD/LD/ULD → minimum pixels per cell
        // bucket_multipliers[] = {1, 2, 5, 10, 20} maps to detail tiers
        constexpr float detail_min_px[] = {1.5f, 2.5f, 4.0f, 6.0f, 10.0f};
        int detail_idx = 0;
        for (int i = 0; i < 5; i++) {
            const int bm[] = {1, 2, 5, 10, 20};
            if (heatmap_bucket_multiplier_ == bm[i]) { detail_idx = i; break; }
        }
        const float min_cell_px = detail_min_px[detail_idx];

        const int native_rows = static_cast<int>(visible_range / native_bucket);
        const int max_visible_rows = std::max(1, static_cast<int>(plot_height_px / min_cell_px));
        int adaptive_mult = std::max(1, native_rows / max_visible_rows);

        // Snap to clean values to avoid constant GPU rebuilds
        const int snap[] = {1, 2, 3, 5, 8, 10, 15, 20, 30, 50, 75, 100};
        int best = 1;
        for (int v : snap) {
            if (v <= adaptive_mult) best = v;
        }

        best = std::max(best, heatmap_bucket_multiplier_);
        // Hysteresis: only update if significantly different
        const int current = renderer.get_bucket_multiplier();
        if (best != current &&
            (best > current * 1.3 || best < current * 0.7 || current <= 1)) {
            renderer.set_bucket_multiplier(best);
        }
    } else {
        renderer.set_bucket_multiplier(heatmap_bucket_multiplier_);
    }

}
