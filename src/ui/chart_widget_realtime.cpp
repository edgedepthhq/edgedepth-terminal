#include "ui/chart_widget.h"
#include "ui/realtime_navigation.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/trade_at_price.h"
#include "core/orderbook_manager.h"
#include "core/entitlements.h"
#include "replayer/replay_manager.h"
#include "core/education_boot.h"
#include "ui/upsell_modal.h"
#include "rendering/theme.h"
#include <cmath>
#include <array>
#include "rendering/realtime_bubble.h"
#include "types/frame_profiler.h"

void ChartWidget::set_rt_mode(bool on) {
    if (on && Entitlements::hosted() && !Entitlements::is_pro() &&
        !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack()) {
        ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
            "Real-time depth and trade bubbles are a hosted Pro view.", "realtime_depth");
        return;
    }
    if (!on && rt_mode_ && liq_dense_field_) heatmap_enabled_ = false;
    rt_mode_ = on;
    if (on && !rt_archive_) rt_archive_ = RealtimeArchive::acquire(ctx_.candle_mgr(), ctx_.ob_mgr(), pair_);
    if (!on) rt_archive_.reset();
    if (on) {
        chart_type_ = ChartType::Line;
        rt_candles_ = false;
        rt_auto_price_ = true;
        rt_auto_fit_ = {};
        rt_price_window_ = {};
        rt_effective_multiplier_ = rt_bucket_multiplier_;
        rt_paused_ = false;
        heatmap_enabled_ = true;
        rt_span_ms_ = 60000;
        rt_unhealthy_since_ms_ = 0;
        if (!rt_flow_) {
            rt_flow_ = std::make_unique<TradeAtPriceAccumulator>();
            rt_flow_->init(pair_, ctx_.stream_mgr(), tick_size_, true);
        }
        ctx_.candle_mgr().set_follow_live(true);
        if (!rt_subscribed_) {
            rt_stream_mgr_ = &ctx_.stream_mgr();
            rt_stream_mgr_->subscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
            rt_subscribed_ = true;
        }
    } else if (rt_subscribed_) {
        rt_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
        rt_stream_mgr_ = nullptr;
        rt_flow_.reset();
        rt_subscribed_ = false;
    }
}

void ChartWidget::render_realtime_settings() {
    if (!ctx_.replay_mgr().is_active() && ImGui::Checkbox("Pause display", &rt_paused_)) {
        if (rt_paused_) {
            rt_paused_trades_ = ctx_.candle_mgr().realtime_trades().trades();
            if (rt_archive_) rt_archive_->cancel_view();
        }
        else rt_paused_trades_.clear();
    }
    ImGui::Checkbox("1s observed candles", &rt_candles_);
    ImGui::Checkbox("Trade-price line", &rt_trade_line_);
    chart_type_ = rt_candles_ ? ChartType::Candles : ChartType::Line;
    if (ImGui::Checkbox("Auto-fit visible history", &rt_auto_fit_history_)) {
        rt_auto_fit_ = {}; rt_price_window_ = {}; rt_auto_price_ = true;
        rt_effective_multiplier_ = rt_bucket_multiplier_;
    }
    if (ImGui::IsItemHovered()) Theme::tooltip("Fits observed trades and quotes in the visible time window. Automatically groups price rows to keep the linked DOM readable. Turn off for fixed-fidelity price following.");
    ImGui::Checkbox(rt_dom_linked_ ? "Follow price" : "Auto-fit price", &rt_auto_price_);
    if (ImGui::IsItemHovered()) Theme::tooltip(rt_dom_linked_
        ? rt_auto_fit_history_ ? "Fits observed price history with readable grouped rows. Drag or zoom the price axis to inspect manually; Follow price resumes fitting." : "Keeps the market in the central half of a readable price window. Drag vertically to stop following. Zoom out stops before numbers overlap; choose coarser fidelity for a wider price range."
        : "Turn off to zoom or drag the price axis. Turn on to fit observed prices again.");
    ImGui::Checkbox("Trade bubbles", &rt_bubbles_);
    ImGui::Checkbox("Extend current depth", &rt_extend_depth_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Projects the last synchronized sampled book into the right margin. This is a held current book, not future orders or recorded history. Turn off to leave the margin clear.");
    ImGui::Checkbox("Auto market size", &rt_auto_bubbles_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Uses the 75th percentile of received trade values over the last 60 seconds. Settles after 32 records and stays fixed until Recalibrate bubble sizes; independent of zoom. One bubble per record, with no inferred fills.");
    if (rt_auto_bubbles_) {
        ImGui::Text("Auto minimum: %.4g quote%s", rt_bubble_scale_.minimum(),
            rt_bubble_scale_.settled() ? " (fixed)" : " (warming up)");
        if (ImGui::Button("Recalibrate bubble sizes")) rt_bubble_scale_ = {};
    }
    ImGui::BeginDisabled(rt_auto_bubbles_);
    ImGui::SetNextItemWidth(150);
    ImGui::InputFloat("Minimum trade value", &rt_min_notional_, 1000, 10000, "%.0f");
    if (!std::isfinite(rt_min_notional_)) rt_min_notional_ = 10000;
    rt_min_notional_ = std::max(1.0f, rt_min_notional_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Price x quantity in quote units. One bubble per received record; the exchange may aggregate fills. Radius starts at 3px and is capped at 12px. Sizes above 16 times the minimum share the cap.");
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 350);
    ImGui::TextUnformatted("Session history: 30-minute target, browser storage permitting");
    if (rt_archive_) {
        const auto& a = *rt_archive_;
        ImGui::Text("Retained: %.1f minutes / %.2f MiB (origin %.2f / 256 MiB)",
            double(std::max(int64_t(0), a.last-a.first))/60000, a.bytes/1048576, a.total_bytes/1048576);
        if (a.first) {
            char first[64]={}, last[64]={};
            DisplayTimeZone::instance().format(a.first, TimeZoneFormat::DateTimeSeconds, first, sizeof(first));
            DisplayTimeZone::instance().format(a.last, TimeZoneFormat::DateTimeSeconds, last, sizeof(last));
            ImGui::Text("%s to %s", first, last);
        }
        if (rt_history_view_) ImGui::Text("History: %.1fs depth bins; %zu markers from %zu trades",
            double(rt_loaded_step_)/1000, rt_archive_trades_.size(), a.view_trade_count);
        if(!a.startup_status.empty()) {
            ImGui::TextWrapped("%s",a.startup_status.c_str());
            ImGui::TextWrapped("Startup depth: 500ms observations, up to 128 native levels per side. Trade coverage can be shorter during bursts. Current DOM uses live depth. Startup trades do not enter CVD or alerts.");
        }
        ImGui::TextUnformatted(a.error.empty() ? "Recording observed depth and received trades" : a.error.c_str());
        if (rt_trade_tail_waiting_) ImGui::TextUnformatted("Refreshing older trades; current trades continue");
        if (a.dropped) ImGui::Text("Capture overload: %zu records missed; depth gaps preserved", a.dropped);
        if (ImGui::Button("Whole session")) { rt_span_ms_ = double(RealtimeArchive::target_ms) / 0.88; ctx_.candle_mgr().set_follow_live(true); }
        ImGui::SameLine();
        if (ImGui::Button("Return live")) { rt_span_ms_ = 60000; rt_auto_price_ = true; rt_price_window_ = {}; rt_auto_fit_ = {}; ctx_.candle_mgr().set_follow_live(true); }
        if (ImGui::Button("Clear history")) { rt_archive_->reset(); }
        ImGui::TextUnformatted("Local session only. Closing RT clears its archive. Storage may be evicted.");
    }
    ImGui::Text("%s: %d native ticks per row", rt_auto_fit_history_ && rt_dom_linked_ ? "Automatic grouping" : "Fixed grouping", rt_effective_multiplier_);
    ImGui::TextUnformatted("Minimum fidelity: Layers > Depth settings");
    ImGui::PopTextWrapPos();
}

void ChartWidget::on_rewind(int64_t) {
    // The replay owner restores/replays depth separately. Never keep future
    // samples or GPU cells from the preceding traversal.
    rt_dom_frame_ = {};
    rt_auto_fit_ = {}; rt_price_window_ = {};
    rt_archive_samples_.clear(); rt_archive_trades_.clear();
    rt_trade_tail_waiting_ = false;
    rt_history_view_ = false; rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
    rt_quote_ = {};
    if (rt_flow_) rt_flow_->reset();
    rt_unhealthy_since_ms_ = 0;
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
    update_realtime_archive_view();
    if (!(tick_size_ > 0) || rt_paused_) return;
    if (!rt_renderer_) {
        rt_renderer_ = std::make_unique<ShaderHeatmapRenderer>();
        rt_renderer_->configure_realtime(tick_size_);
        rt_pending_.reserve(RealtimeDepthHistory::max_samples);
        rt_prices_.reserve(1024);
    }
    const int64_t clock = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    if (rt_flow_) {
        rt_flow_->set_tick_size(tick_size_);
        rt_flow_->advance_to(clock);
    }
    if (clock != rt_clock_ms_ || !ctx_.replay_mgr().is_paused())
        rt_quote_ = ctx_.ob_mgr().realtime_quote(pair_, clock);
    rt_clock_ms_ = clock;
    rt_renderer_->set_observation_clock_ms(clock);
    rt_book_valid_ = ctx_.ob_mgr().copy_realtime_since(pair_, rt_serial_, rt_pending_);
    // Samples past the as-of clock stay pending.
    for (const auto& sample : rt_pending_) {
        if (sample->timestamp_ms > clock) break;
        rt_serial_ = sample->serial;
        if (sample->timestamp_ms <= clock - RealtimeDepthHistory::retention_ms) continue;
        rt_prices_.clear();
        for (const auto& level : sample->levels) rt_prices_[level.price] += float(level.size);
        if (!rt_history_view_ || realtime_live_edge()) rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask) * 0.5);
        rt_latest_ = sample;
        rt_samples_.push_back(sample);
        if (rt_history_view_ && realtime_live_edge() && sample->timestamp_ms > rt_loaded_to_)
            rt_archive_samples_.push_back(sample);
        while (rt_archive_samples_.size() > 4096) rt_archive_samples_.pop_front();
        while (rt_samples_.size() > RealtimeDepthHistory::max_samples || rt_samples_.front()->timestamp_ms <= clock - RealtimeDepthHistory::retention_ms)
            rt_samples_.pop_front();
    }
    if (!rt_book_valid_ && rt_latest_) {
        // Discard the final sampling bin on interruption rather than carrying
        // it through an invalid sequence inside that bin.
        if (!rt_history_view_ || realtime_live_edge()) rt_renderer_->invalidate_observation(rt_latest_->timestamp_ms);
        if (!rt_samples_.empty() && rt_samples_.back() == rt_latest_) rt_samples_.pop_back();
        if (!rt_archive_samples_.empty() && rt_archive_samples_.back() == rt_latest_)
            rt_archive_samples_.pop_back();
        rt_latest_.reset();
    }
    const bool healthy = rt_book_valid_ && rt_latest_ && clock - rt_latest_->timestamp_ms <= 15000;
    if (healthy || ctx_.replay_mgr().is_active()) rt_unhealthy_since_ms_ = 0;
    else {
        if (!rt_unhealthy_since_ms_) rt_unhealthy_since_ms_ = clock;
        if (clock - rt_unhealthy_since_ms_ >= 3000)
            ctx_.stream_mgr().refresh_orderbook({pair_, Terminal::Stream::Orderbook, 0}, clock);
    }
    rt_renderer_->set_observation_hold((!rt_history_view_ || realtime_live_edge()) && rt_book_valid_ && rt_latest_ &&
        clock - rt_latest_->timestamp_ms <= 15000 ? clock : 0);

}

const std::deque<Terminal::Trade>& ChartWidget::realtime_trades() const {
    if (rt_history_view_) return rt_archive_trades_;
    return rt_paused_ ? rt_paused_trades_ : ctx_.candle_mgr().realtime_trades().trades();
}

void ChartWidget::render_realtime() {
    ProfileScope profile("RT overlays");
    const auto limits = ImPlot::GetPlotLimits();
    if (!ctx_.candle_mgr().follow_live()) rt_span_ms_ = std::clamp(limits.X.Size(), 5000.0, (double(RealtimeArchive::target_ms) / 0.88));
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const auto& trades = realtime_trades();
    if (heatmap_enabled_ && rt_extend_depth_ && (!rt_history_view_ || realtime_live_edge()) && rt_book_valid_ && rt_latest_ &&
        rt_latest_->timestamp_ms <= rt_clock_ms_ && rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000 &&
        limits.X.Max > rt_clock_ms_) {
        const float edge = std::max(ImPlot::GetPlotPos().x,
            ImPlot::PlotToPixels(double(rt_clock_ms_), 0).x);
        dl->AddLine(ImVec2(edge, ImPlot::GetPlotPos().y),
            ImVec2(edge, ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y), Theme::u32(Theme::Tokens::TX2, 0.35f));
        dl->AddText(ImVec2(edge + 5, ImPlot::GetPlotPos().y + 5), Theme::u32(Theme::Tokens::TX2), "Current depth");
    }

    ImVec2 previous{};
    int64_t previous_ms = 0;
    // Last point per screen pixel bounds line geometry to the plot width.
    if (rt_trade_line_ && !rt_candles_ && !(rt_history_view_ && rt_archive_->view_trades_grouped)) for (const auto& trade : trades) {
        if (trade.timestamp_ms < limits.X.Min) continue;
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
    for (const auto& sample : realtime_samples()) {
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
                const ImU32 halo = Theme::u32(Theme::Tokens::BASE);
                dl->AddLine(a, b, halo, 3.5f);
                dl->AddLine(b, c, halo, 3.5f);
                dl->AddLine(a, b, color, 1.5f);
                dl->AddLine(b, c, color, 1.5f);
            }
        }
        previous_book = sample.get();
    }
    if (rt_candles_ && !(rt_history_view_ && rt_archive_->view_trades_grouped)) {
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
    // Calibrate from original received records even while displaying an archive.
    // Grouped archive markers are sums, not samples of individual trade sizes.
    if (rt_auto_bubbles_) rt_bubble_scale_.update(
        rt_paused_ ? rt_paused_trades_ : ctx_.candle_mgr().realtime_trades().trades(), rt_clock_ms_);
    const double minimum = rt_auto_bubbles_ ? rt_bubble_scale_.minimum() : rt_min_notional_;
    if (rt_bubbles_ && minimum > 0) {
        rt_trade_view_.build(trades, int64_t(limits.X.Min), std::min(rt_clock_ms_, int64_t(limits.X.Max)),
            minimum, rt_history_view_ && rt_archive_->view_trades_grouped);
        for (size_t i=0;i<rt_trade_view_.count;++i) {
            const auto& trade = rt_trade_view_.records[i];
            const auto& color = trade.is_buy ? Theme::Tokens::UP : Theme::Tokens::DOWN;
            if (rt_trade_view_.grouped && rt_trade_view_.high[i]>rt_trade_view_.low[i])
                dl->AddLine(ImPlot::PlotToPixels(double(trade.timestamp_ms), rt_trade_view_.low[i]),
                    ImPlot::PlotToPixels(double(trade.timestamp_ms), rt_trade_view_.high[i]), Theme::u32(color, 0.5f));
            RealtimeBubble::draw(*dl, ImPlot::PlotToPixels(double(trade.timestamp_ms), trade.price),
                RealtimeBubble::radius(trade.price * trade.qty, minimum), color, Theme::u32(Theme::Tokens::BASE));
        }
    }
    const bool fresh = rt_book_valid_ && rt_latest_ &&
        rt_latest_->timestamp_ms <= rt_clock_ms_ && rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000;
    rt_dom_frame_.book = rt_latest_;
    rt_dom_frame_.quote = rt_quote_;
    rt_dom_frame_.frame = ImGui::GetFrameCount();
    rt_dom_frame_.clock_ms = rt_clock_ms_;
    rt_dom_frame_.native_tick = rt_renderer_ ? rt_renderer_->get_native_bucket_size() : 0;
    rt_dom_frame_.automatic_grouping = rt_auto_fit_history_ && rt_dom_linked_;
    rt_dom_frame_.bucket_ticks = rt_renderer_ ? rt_renderer_->get_bucket_multiplier() : rt_bucket_multiplier_;
    rt_dom_frame_.price_min = limits.Y.Min;
    rt_dom_frame_.price_max = limits.Y.Max;
    rt_dom_frame_.top = ImPlot::GetPlotPos().y;
    rt_dom_frame_.bottom = rt_dom_frame_.top + ImPlot::GetPlotSize().y;
    rt_dom_frame_.synchronized = rt_book_valid_;
    rt_dom_frame_.replay = ctx_.replay_mgr().is_active();
    rt_dom_frame_.flow = rt_flow_.get();
    rt_dom_frame_.paused = rt_paused_ || ctx_.replay_mgr().is_paused();
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
        for (int side = 0; side < 2; ++side) {
            const double price = side ? rt_dom_frame_.ask() : rt_dom_frame_.bid();
            const float y = ImPlot::PlotToPixels(0, price).y;
            const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
            for (float x = edge; x < right; x += 10) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5, right), y), color);
        }
    }
    const char* note = rt_archive_ && rt_archive_->loading ? "Loading RT history..." : rt_archive_ && !rt_archive_->error.empty() ? "RT archive stopped: open RT settings for storage status" : rt_history_view_ ? (rt_archive_ && rt_archive_->loading ?
        "RT history loading / recording continues" : "RT history: mean depth bins / grouped trade volume when dense / zoom for detail") : rt_paused_ ? "RT paused / session recording continues" : !fresh ? "RT: waiting for fresh synchronized depth" :
        (rt_trade_view_.grouped && rt_bubbles_ ? "RT: grouped trade volume at average prices / zoom for individual records" :
         "RT: sampled book held between updates / bubbles are trade records");
    const ImVec2 pos = ImPlot::GetPlotPos();
    const float note_y = pos.y + (ctx_.replay_mgr().is_active() ? 60.0f : 34.0f);
    if (!realtime_samples().empty() && realtime_samples().front()->timestamp_ms >= limits.X.Min &&
        realtime_samples().front()->timestamp_ms <= std::min(limits.X.Max, double(rt_clock_ms_))) {
        const float start = ImPlot::PlotToPixels(double(realtime_samples().front()->timestamp_ms), 0).x;
        dl->AddLine(ImVec2(start, note_y + 26), ImVec2(start, pos.y + ImPlot::GetPlotSize().y),
            Theme::u32(Theme::Tokens::TX2, 0.45f));
        dl->AddText(ImVec2(start + 6, note_y + 18), Theme::u32(Theme::Tokens::TX2), "Observed depth starts here");
    }
    const ImVec2 note_size = ImGui::CalcTextSize(note);
    dl->AddRectFilled(ImVec2(pos.x + 8, note_y - 3),
        ImVec2(pos.x + 16 + note_size.x, note_y + note_size.y + 3),
        Theme::u32(Theme::Tokens::BASE, 0.9f), 3);
    dl->AddText(ImVec2(pos.x + 12, note_y), Theme::u32(Theme::Tokens::TX1), note);
    if(rt_archive_ && !rt_archive_->startup_status.empty() && !rt_paused_ && fresh && rt_archive_->error.empty())
        dl->AddText(ImVec2(pos.x+12,note_y+36),Theme::u32(Theme::Tokens::TX2),rt_archive_->startup_status.c_str());
    if (ctx_.replay_mgr().is_loading()) {
        const ImVec2 size = ImPlot::GetPlotSize();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::u32(Theme::Tokens::BASE));
        const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
        const float angle = float(ImGui::GetTime() * 4.0);
        dl->PathArcTo(center, 12.0f, angle, angle + 4.5f, 24);
        dl->PathStroke(Theme::u32(Theme::Tokens::BRAND_TX), 0, 2.5f);
        const char* label = "Loading replay...";
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y + 24),
                    Theme::u32(Theme::Tokens::TX1), label);
    }
    ImPlot::PopPlotClipRect();
}

void ChartWidget::configure_depth_fidelity(ShaderHeatmapRenderer& renderer) {
    if (rt_mode_) {
        // The chart owns one display grid for both historical depth and DOM.
        // Archive queries retain the selected minimum grouping for later detail.
        renderer.set_bucket_multiplier(rt_effective_multiplier_);
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


void ChartWidget::capture_realtime_archive() {
    if (!rt_archive_) rt_archive_ = RealtimeArchive::acquire(ctx_.candle_mgr(), ctx_.ob_mgr(), pair_);
    const int64_t clock = ctx_.replay_mgr().is_active() ? ctx_.replay_mgr().interpolated_time_ms() :
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rt_archive_->update(clock);
    if(!ctx_.replay_mgr().is_active())rt_archive_->request_startup(ctx_.stream_mgr());
}

void ChartWidget::rebuild_realtime_view() {
    if (!rt_renderer_) return;
    rt_renderer_->clear_realtime_view(rt_history_view_ ? rt_loaded_step_ : 100);
    rt_renderer_->set_observation_clock_ms(rt_clock_ms_);
    for (const auto& sample : realtime_samples()) {
        rt_prices_.clear();
        for (const auto& level : sample->levels) rt_prices_[level.price] += float(level.size);
        rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask)*0.5);
    }
}

bool ChartWidget::realtime_live_edge() const {
    return realtime_view_has_live_edge(ctx_.candle_mgr().follow_live(), last_visible_range_.X.Max, rt_clock_ms_);
}

void ChartWidget::update_realtime_archive_view() {
    if (!rt_archive_ || !rt_renderer_ || rt_clock_ms_<=0) return;
    if (rt_archive_generation_ != rt_archive_->generation) {
        rt_archive_generation_ = rt_archive_->generation;
        rt_archive_samples_.clear(); rt_archive_trades_.clear(); rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
        if (rt_history_view_) rebuild_realtime_view();
    }
    const bool follow = ctx_.candle_mgr().follow_live();
    const int64_t from = follow ? rt_clock_ms_ - int64_t(rt_span_ms_ * 0.88) : int64_t(last_visible_range_.X.Min);
    const int64_t to = follow ? rt_clock_ms_ : std::min(rt_clock_ms_, int64_t(last_visible_range_.X.Max));
    const auto& recent_trades = rt_paused_ ? rt_paused_trades_ : ctx_.candle_mgr().realtime_trades().trades();
    const bool trades_retired = recent_trades.size() == RealtimeTradeHistory::max_trades &&
        recent_trades.front().timestamp_ms > from && rt_archive_->first < recent_trades.front().timestamp_ms;
    const bool seeded = rt_archive_->startup_first>0 && from<rt_archive_->startup_end;
    const bool history = to > from && (seeded || trades_retired || to-from > 290000 || from < rt_clock_ms_ - 290000);
    if (!history) {
        rt_trade_tail_waiting_ = false;
        int64_t discarded_step=100;
        rt_archive_->take_view(rt_archive_samples_, rt_archive_trades_, discarded_step);
        if (rt_history_view_) {
            rt_history_view_ = false; rt_archive_samples_.clear(); rt_archive_trades_.clear();
            rebuild_realtime_view(); rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
        }
        return;
    }
    const int64_t step = std::max(int64_t(seeded ? 500 : 100), ((to-from+179999)/180000)*100);
    const int64_t aligned = from / step * step;
    int64_t received_step = 100;
    std::deque<RealtimeDepthHistory::SamplePtr> received_samples;
    std::deque<Terminal::Trade> received_trades;
    const auto displayed_count = rt_archive_->view_trade_count;
    const bool displayed_grouped = rt_archive_->view_trades_grouped;
    if (rt_archive_->take_view(received_samples, received_trades, received_step)) {
        // Navigation may have changed while a worker query was in flight.
        const bool tail_already_retired = realtime_live_edge() &&
            realtime_trade_tail_retired(recent_trades, rt_query_to_);
        if (tail_already_retired || step != rt_query_step_ || rt_query_multiplier_ != rt_bucket_multiplier_ || !realtime_query_start_matches(follow, aligned, rt_query_from_, step)) {
            rt_archive_->view_trade_count = displayed_count;
            rt_archive_->view_trades_grouped = displayed_grouped;
            rt_query_from_ = 0;
        } else {
            // Keep the displayed data until a matching replacement is complete.
            rt_archive_samples_.swap(received_samples);
            rt_archive_trades_.swap(received_trades);
            rt_history_view_ = true;
            rt_loaded_to_ = rt_query_to_; rt_loaded_step_ = received_step;
            // Depth delivered while the query ran has already advanced rt_serial_.
            // Preserve that tail before rebuilding the GPU view.
            if (realtime_live_edge()) for (const auto& sample : rt_samples_)
                if (sample->timestamp_ms > rt_loaded_to_ && sample->timestamp_ms <= rt_clock_ms_)
                    rt_archive_samples_.push_back(sample);
            while (rt_archive_samples_.size() > 4096) rt_archive_samples_.pop_front();
            rebuild_realtime_view();
        }
    }
    // Join the query's as-of snapshot to the recent source by a strict time
    // boundary. Equal timestamps stay together in the archive snapshot; late
    // records at/before its cutoff appear on the next query, never deduplicated.
    bool tail_ready = true;
    if (realtime_live_edge() && rt_loaded_to_ > 0)
        tail_ready = refresh_realtime_trade_tail(rt_archive_trades_, recent_trades,
                                                rt_loaded_to_, rt_clock_ms_);
    rt_trade_tail_waiting_ = !tail_ready;
    if (!tail_ready && bound_realtime_trade_tail(rt_archive_trades_, recent_trades, rt_trade_view_))
        rt_archive_->view_trades_grouped = true;
    const double now = emscripten_get_now();
    if (!rt_archive_->loading && (!tail_ready || rt_query_from_==0 || step!=rt_query_step_ || rt_query_multiplier_!=rt_bucket_multiplier_ ||
        !realtime_query_start_matches(follow, aligned, rt_query_from_, step) || (!rt_paused_ && to>rt_query_to_ && now-rt_query_at_>5000))) {
        if (rt_archive_->query(aligned,to,rt_clock_ms_,step,tick_size_*rt_bucket_multiplier_)) {
            rt_query_from_=aligned;rt_query_to_=to;rt_query_step_=step;rt_query_at_=now;rt_query_multiplier_=rt_bucket_multiplier_;
        }
    }
}
