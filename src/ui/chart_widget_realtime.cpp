#include "ui/chart_widget.h"
#include "core/candle_bubble_display.h"
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
#include <cstdio>
#include <emscripten.h>
#include "stream_handler.h"
#include <array>
#include "rendering/realtime_bubble.h"
#include "rendering/observed_liquidations.h"
#include "ui/indicators/volume_indicator.h"
#include "types/frame_profiler.h"

bool ChartWidget::rt_mode_locked() const {
    return Entitlements::hosted() && !Entitlements::is_pro() &&
        !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack();
}

void ChartWidget::set_rt_mode(bool on) {
    if (on && rt_mode_locked()) {
        ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
            "Live real-time mode (observed depth, every trade as a bubble, the spread) is a Pro view. Large prints still show as bubbles on your candles.", "realtime_depth");
        return;
    }
    // heatmap_enabled_ (candle depth heatmap) is left alone in both directions:
    // real-time depth has its own flag, so a round trip keeps the candle choice.
    // The chart type is not: real-time draws Line / 1s candles, so the type the
    // user had is captured on the rising edge and put back on exit (a re-arm
    // while already on, e.g. a replay context swap, keeps the captured type).
    if (on && !rt_mode_) rt_prev_chart_type_ = chart_type_;
    const bool leaving = !on && rt_mode_;
    if (leaving) liq_dense_field_ = true;
    rt_mode_ = on;
    if (leaving) apply_chart_type(rt_prev_chart_type_);
    if (on && !rt_archive_) rt_archive_ = RealtimeArchive::acquire(candles(), ctx_.ob_mgr(), pair_);
    // Keep recording across candle/RT switches. Chart closure or a context
    // change owns archive disposal and depth unsubscription.
    if (on) {
        chart_type_ = ChartType::Line;
        rt_candles_ = false;
        rt_auto_price_ = true;
        rt_auto_fit_ = {};
        rt_price_window_ = {};
        rt_effective_multiplier_ = rt_bucket_multiplier_;
        rt_paused_ = false;
        rt_paused_liquidations_.clear();
        rt_span_ms_ = realtime_default_span_ms;
        rt_unhealthy_since_ms_ = 0;
        if (!rt_flow_) {
            rt_flow_ = std::make_unique<TradeAtPriceAccumulator>();
            rt_flow_->init(pair_, ctx_.stream_mgr(), tick_size_, true);
        }
        candles().set_follow_live(true);
        if (!rt_subscribed_) {
            rt_stream_mgr_ = &ctx_.stream_mgr();
            rt_stream_mgr_->subscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
            rt_subscribed_ = true;
        }
    } else {
        rt_flow_.reset();
    }
}

// Every control and disclosure the panel had is still here; the prose moved
// into hover tooltips, so the panel reads as controls first. Missing-data
// states (capture overload, records not plotted, archive errors) stay visible
// as one line each, and only when they apply.
void ChartWidget::render_realtime_settings() {
    auto tip = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Theme::begin_tooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            Theme::end_tooltip();
        }
    };
    auto return_live = [this]() {
        rt_span_ms_ = realtime_default_span_ms; rt_auto_price_ = true;
        rt_price_window_ = {}; rt_auto_fit_ = {};
        candles().set_follow_live(true);
    };

    Theme::section_label("VIEW");
    if (!ctx_.replay_mgr().is_active() && ImGui::Checkbox("Pause display", &rt_paused_)) {
        if (rt_paused_) {
            rt_paused_trades_ = candles().realtime_trades().trades();
            rt_paused_liquidations_ = ctx_.liq_heatmap_mgr().observed_events(pair_);
            if (rt_archive_) rt_archive_->cancel_view();
            rt_query_from_ = 0; // A cancelled request is not loaded coverage.
        }
        else { rt_paused_trades_.clear(); rt_paused_liquidations_.clear(); }
    }
    ImGui::Checkbox("1s candles", &rt_candles_);
    tip("One-second candles built from observed trades, in place of the trade-price line.");
    ImGui::Checkbox("Trade-price line", &rt_trade_line_);
    chart_type_ = rt_candles_ ? ChartType::Candles : ChartType::Line;
    ImGui::Checkbox("Depth ladder", &rt_dom_shown_);
    tip("Order book ladder attached to the right of the chart: resting depth and traded volume on the chart's own price rows and clock.");
    ImGui::Checkbox(rt_dom_linked_ ? "Follow price" : "Auto-fit price", &rt_auto_price_);
    tip(rt_dom_linked_
        ? rt_auto_fit_history_ ? "Fits observed price history with readable grouped rows. Drag or zoom the price axis to inspect manually; Follow price resumes fitting." : "Keeps the market in the central half of a readable price window. Drag vertically to stop following. Zoom out stops before numbers overlap; choose coarser fidelity for a wider price range."
        : "Turn off to zoom or drag the price axis. Turn on to fit observed prices again.");
    if (ImGui::Checkbox("Auto-fit visible history", &rt_auto_fit_history_)) {
        rt_auto_fit_ = {}; rt_price_window_ = {}; rt_auto_price_ = true;
        rt_effective_multiplier_ = rt_bucket_multiplier_;
    }
    tip("Fits observed trades and quotes in the visible time window, grouping price rows to keep the depth ladder readable. Turn off for fixed-fidelity price following.");
    if (ImGui::Button("Return live")) return_live();
    tip("Back to the most recent 30 seconds, following price. Zoom out or drag left for older retained context; the view never changes recorded detail.");
    if (rt_archive_) {
        ImGui::SameLine();
        if (ImGui::Button("Whole session")) { rt_span_ms_ = double(RealtimeArchive::target_ms) / 0.88; candles().set_follow_live(true); }
        tip("Zoom out to everything retained this session.");
    }

    Theme::section_label("DEPTH");
    ImGui::Checkbox("Extend current depth", &rt_extend_depth_);
    tip("Projects the last synchronized sampled book into the right margin. This is a held current book, not future orders or recorded history.");
    ImGui::TextDisabled("%s: %d native ticks per row",
        rt_auto_fit_history_ && rt_dom_linked_ ? "Automatic grouping" : "Fixed grouping", rt_effective_multiplier_);
    tip("Brightness is mean quantity per native tick; wider rows keep their totals in the DOM, and thin walls may fade on zoom-out. Time bins average received samples. Set the minimum fidelity in Layers > Depth settings.");

    Theme::section_label("TRADES");
    ImGui::Checkbox("Trade bubbles", &rt_bubbles_);
    tip("Fixed 100ms groups at each exact price, with 100ms of delivery settling. The eight busiest prices per window are selected once and frozen for 30 minutes; zoom never regroups them. Changing size or filter redraws the layer.");
    ImGui::Checkbox("Auto size", &rt_auto_bubbles_);
    tip("Uses the 75th percentile of received trade values over the last 60 seconds. Settles after 32 records or five seconds, then stays fixed until Recalibrate; independent of zoom.");
    if (rt_auto_bubbles_) {
        ImGui::SameLine();
        ImGui::TextDisabled("min %.4g%s", rt_bubble_scale_.minimum(),
            rt_bubble_scale_.settled() ? "" : " (warming up)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Recalibrate")) rt_bubble_scale_ = {};
    } else {
        ImGui::SetNextItemWidth(130);
        ImGui::InputFloat("Minimum value", &rt_min_notional_, 1000, 10000, "%.0f");
        if (!std::isfinite(rt_min_notional_)) rt_min_notional_ = 10000;
        rt_min_notional_ = std::max(1.0f, rt_min_notional_);
        tip("Price x quantity in quote units, summed over one 100ms window at one exact price; the exchange may aggregate fills. Two-sided takers show the smaller side as an inner disc. Radius runs 3 to 12px; sizes above 16x the minimum share the cap.");
    }
    if (rt_archive_) {
        const auto& b = rt_archive_->bubbles();
        const unsigned long long skipped = b.approximate_records + b.late_records + b.overflow_records;
        if (skipped) {
            ImGui::TextDisabled("Not plotted as bubbles: %llu records", skipped);
            char why[320];
            snprintf(why, sizeof(why),
                "%llu in multi-price history summaries, %llu late for already closed groups, %llu over the pending capture budget. Original records remain in session history when storage is available.",
                (unsigned long long)b.approximate_records, (unsigned long long)b.late_records,
                (unsigned long long)b.overflow_records);
            tip(why);
        }
    }

    Theme::section_label("LIQUIDATIONS");
    ImGui::Checkbox("Reported liquidations", &liq_observed_enabled_);
    tip("Diamonds at reported liquidations. Binance reports are censored; missing liquidations are not estimated.");
    if (liq_observed_enabled_) {
        ImGui::SetNextItemWidth(130);
        ImGui::InputFloat("Minimum notional", &liq_obs_min_usd_, 100, 1000, "%.0f");
        if (!std::isfinite(liq_obs_min_usd_)) liq_obs_min_usd_ = 0;
        liq_obs_min_usd_ = std::clamp(liq_obs_min_usd_, 0.0f, 1e15f);
        tip("Filters the diamonds only; the activity strip includes all retained reports in view.");
    }
    ImGui::Checkbox("Activity strip", &rt_liq_strip_);

    if (!rt_archive_) return;
    const auto& a = *rt_archive_;
    Theme::section_label("SESSION");
    ImGui::TextDisabled("Recorded %.1f min \xc2\xb7 %.2f MiB",
        double(std::max(int64_t(0), a.last - a.first)) / 60000, a.bytes / 1048576);
    {
        std::string detail = "Local session history, 30-minute target, browser storage permitting. "
                             "Switching to candles keeps recording; closing the chart clears it. Storage may be evicted.";
        char line[160];
        snprintf(line, sizeof(line), "\nOrigin storage %.2f / 256 MiB.", a.total_bytes / 1048576);
        detail += line;
        if (a.first) {
            char first[64] = {}, last[64] = {};
            DisplayTimeZone::instance().format(a.first, TimeZoneFormat::DateTimeSeconds, first, sizeof(first));
            DisplayTimeZone::instance().format(a.last, TimeZoneFormat::DateTimeSeconds, last, sizeof(last));
            snprintf(line, sizeof(line), "\n%s to %s", first, last);
            detail += line;
        }
        snprintf(line, sizeof(line), "\nBubble groups retained: %zu; filtered before display: %zu.",
                 a.bubbles().size(), a.bubbles().filtered_groups);
        detail += line;
        if (rt_history_view_) {
            snprintf(line, sizeof(line), "\nHistory: %.1fs depth bins; %zu markers from %zu trades.",
                     double(rt_loaded_step_) / 1000, rt_archive_trades_.size(), a.view_trade_count);
            detail += line;
        }
        if (!a.startup_status.empty()) {
            detail += "\n\n" + a.startup_status;
            snprintf(line, sizeof(line), "\nStartup received up to %d price levels per side.", a.startup_max_levels_per_side);
            detail += line;
            detail += "\nStartup depth: 500ms observations, up to 512 native levels per side on supported feeds. "
                      "Busy-market prefixes group observed trades by side over 100ms, preserving quantity and price range; "
                      "zoom cannot recover individual trades there. Current DOM uses live depth. Startup trades do not enter CVD or alerts.";
        }
        tip(detail.c_str());
    }
    if (!a.error.empty()) ImGui::TextColored(Theme::Tokens::WARN, "%s", a.error.c_str());
    if (a.dropped) ImGui::TextColored(Theme::Tokens::WARN, "Capture overload: %zu records missed; depth gaps kept", a.dropped);
    if (rt_trade_tail_waiting_) ImGui::TextDisabled("Refreshing older trades; current trades continue");
    if (ImGui::SmallButton("Clear history")) rt_archive_->reset();
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
    // Candle-mode large prints belong to the traversal being discarded too.
    candle_bubble_history_.reset();
    flow_history_.reset();
    candle_prints_.clear();
    candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
    candle_bubble_scale_ = {}; candle_bubble_scale_since_ms_ = 0;
    candle_bubble_auto_floor_ = 0; candle_bubble_size_reference_ = 0;
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
    if (clock != rt_clock_ms_ || !ctx_.replay_mgr().is_paused()) {
        const auto quote = ctx_.ob_mgr().realtime_quote_as_of(pair_, clock);
        rt_quote_ = quote.quote;
        rt_quote_bid_since_ms_ = quote.bid_since_ms;
        rt_quote_ask_since_ms_ = quote.ask_since_ms;
    }
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
        if (!rt_history_view_ || realtime_live_edge()) rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask) * 0.5, sample->source_bucket_ticks);
        rt_latest_ = sample;
        rt_samples_.push_back(sample);
        if (rt_history_view_ && realtime_live_edge() && sample->timestamp_ms > rt_loaded_to_)
            append_realtime_depth_sample(rt_archive_samples_, sample, rt_loaded_step_);
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
        clock - rt_latest_->timestamp_ms <= 15000 ? clock : 0,
        rt_latest_ ? rt_latest_->timestamp_ms : 0);

}

const std::deque<Terminal::Trade>& ChartWidget::realtime_trades() const {
    if (rt_history_view_) return rt_archive_trades_;
    return rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades();
}

void ChartWidget::render_realtime() {
    ProfileScope profile("RT overlays");
    const auto limits = ImPlot::GetPlotLimits();
    if (!candles().follow_live()) rt_span_ms_ = std::clamp(limits.X.Size(), 5000.0, (double(RealtimeArchive::target_ms) / 0.88));
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const auto& trades = realtime_trades();
    if (rt_depth_enabled_ && rt_extend_depth_ && (!rt_history_view_ || realtime_live_edge()) && rt_book_valid_ && rt_latest_ &&
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
    auto draw_book = [&](const RealtimeDepthHistory::SamplePtr& sample) {
        if (sample->timestamp_ms < limits.X.Min) { previous_book = sample.get(); return; }
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
    };
    for (const auto& sample : realtime_samples()) {
        if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > limits.X.Max) break;
        draw_book(sample);
    }
    // Coarse archive bins omit the newest observations within their last bin.
    // Draw the retained raw tail without changing the bounded archive grid.
    if (rt_history_view_ && realtime_live_edge() && previous_book) {
        if (!rt_samples_.empty() && rt_samples_.front()->timestamp_ms > previous_book->timestamp_ms &&
            rt_samples_.front()->serial > 1) previous_book = nullptr;
        for (const auto& sample : rt_samples_) {
            if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > limits.X.Max) break;
            if (previous_book && sample->timestamp_ms <= previous_book->timestamp_ms) continue;
            draw_book(sample);
        }
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
        rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades(), rt_clock_ms_);
    const double minimum = rt_auto_bubbles_
        ? (rt_bubble_scale_.settled() ? rt_bubble_scale_.minimum() : 0) : rt_min_notional_;
    if (rt_bubbles_ && minimum > 0 && rt_archive_) {
        // Immutable capture publications, not a ranking of the current viewport.
        // Neither archive refreshes nor camera movement can change a marker.
        rt_archive_->bubbles().for_each(int64_t(limits.X.Min), int64_t(limits.X.Max), rt_clock_ms_,
            [&](const RealtimeBubbleHistory::Bubble& m) {
                if (m.value() < minimum) return;
                const ImVec4& major = m.buy_led() ? Theme::Tokens::UP : Theme::Tokens::DOWN;
                const ImVec4& minor = m.buy_led() ? Theme::Tokens::DOWN : Theme::Tokens::UP;
                RealtimeBubble::draw(*dl, ImPlot::PlotToPixels(double(m.timestamp_ms), m.price),
                    RealtimeBubble::radius(m.value(), minimum), major, Theme::u32(Theme::Tokens::BASE),
                    RealtimeBubble::Fill::Opaque, float(m.minor()), minor);
            });
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
            const double current_price = side ? rt_dom_frame_.ask() : rt_dom_frame_.bid();
            // The step sits where this side's price changed, not where the
            // newest ticker arrived, so it holds its place in time.
            const int64_t since = side ? rt_quote_ask_since_ms_ : rt_quote_bid_since_ms_;
            const double quote_ms = double(std::clamp(since > 0 ? since : rt_quote_.timestamp_ms, book.timestamp_ms, rt_clock_ms_));
            const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
            dl->AddLine(ImPlot::PlotToPixels(double(book.timestamp_ms), price),
                ImPlot::PlotToPixels(quote_ms, price), color, 1.25f);
            dl->AddLine(ImPlot::PlotToPixels(quote_ms, price),
                ImPlot::PlotToPixels(quote_ms, current_price), color, 1.25f);
            dl->AddLine(ImPlot::PlotToPixels(quote_ms, current_price),
                ImPlot::PlotToPixels(double(rt_clock_ms_), current_price), color, 1.25f);
        }
        for (int side = 0; side < 2; ++side) {
            const double price = side ? rt_dom_frame_.ask() : rt_dom_frame_.bid();
            const float y = ImPlot::PlotToPixels(0, price).y;
            const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
            for (float x = edge; x < right; x += 10) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5, right), y), color);
        }
    }
    const char* note = rt_archive_ && rt_archive_->loading ? "Loading RT history..." : rt_archive_ && !rt_archive_->error.empty() ? "RT archive stopped: open RT settings for storage status" : rt_history_view_ ? (rt_archive_ && rt_archive_->loading ?
        "RT history loading / recording continues" : "RT history: mean depth bins / fixed 100ms trade bubbles") : rt_paused_ ? "RT paused / session recording continues" : !fresh ? "RT: waiting for fresh synchronized depth" :
        "RT: sampled book held between updates / fixed 100ms trade bubbles";
    const ImVec2 pos = ImPlot::GetPlotPos();
    const float note_y = pos.y + (ctx_.replay_mgr().is_active() ? 60.0f : 34.0f);
    if (!realtime_samples().empty() && realtime_samples().front()->timestamp_ms >= limits.X.Min &&
        realtime_samples().front()->timestamp_ms <= std::min(limits.X.Max, double(rt_clock_ms_))) {
        const float start = ImPlot::PlotToPixels(double(realtime_samples().front()->timestamp_ms), 0).x;
        dl->AddLine(ImVec2(start, note_y + 26), ImVec2(start, pos.y + ImPlot::GetPlotSize().y),
            Theme::u32(Theme::Tokens::TX2, 0.45f));
        dl->AddText(ImVec2(start + 6, note_y + 18), Theme::u32(Theme::Tokens::TX2), "Observed depth starts here");
    }
    char density_note[384];
    if (rt_bubbles_ && rt_archive_ && rt_archive_->bubbles().filtered_groups > 0) {
        std::snprintf(density_note, sizeof(density_note), "%s / up to 8 prices per 100ms, selected once", note);
        note = density_note;
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
    const double native_bucket = renderer.get_native_bucket_size();
    const auto limits = ImPlot::GetPlotLimits();
    int mult = heatmap_bucket_multiplier_;
    if (native_bucket > 0 && heatmap_adapt_to_zoom_) {
        // Round UP to readable rows, with no 100x ceiling on volatile markets.
        const double rows = std::clamp(double(ImPlot::GetPlotSize().y) / 2.5, 1.0, 900.0);
        const double needed = std::ceil(limits.Y.Size() / (native_bucket * rows));
        const int groups = int(std::clamp(std::ceil(needed / mult), 1.0, 1000000.0));
        mult *= groups;
        const int current = renderer.get_bucket_multiplier();
        if (mult < current && mult > current * 0.7) mult = current;
    }
    renderer.set_bucket_multiplier(mult);
    renderer.set_candle_price_window(limits.Y.Min, limits.Y.Max);

}


void ChartWidget::capture_realtime_archive() {
    if (!rt_archive_) rt_archive_ = RealtimeArchive::acquire(candles(), ctx_.ob_mgr(), pair_);
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
        rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask)*0.5, sample->source_bucket_ticks);
    }
}

bool ChartWidget::realtime_live_edge() const {
    return realtime_view_has_live_edge(candles().follow_live(), last_visible_range_.X.Max, rt_clock_ms_);
}

void ChartWidget::update_realtime_archive_view() {
    if (!rt_archive_ || !rt_renderer_ || rt_clock_ms_<=0) return;
    if (rt_archive_generation_ != rt_archive_->generation) {
        rt_archive_generation_ = rt_archive_->generation;
        rt_archive_samples_.clear(); rt_archive_trades_.clear(); rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
        if (rt_history_view_) rebuild_realtime_view();
    }
    const bool follow = candles().follow_live();
    const int64_t from = follow ? rt_clock_ms_ - int64_t(rt_span_ms_ * 0.88) : int64_t(last_visible_range_.X.Min);
    const int64_t to = follow ? rt_clock_ms_ : std::min(rt_clock_ms_, int64_t(last_visible_range_.X.Max));
    const auto& recent_trades = rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades();
    const bool trades_retired = recent_trades.size() == RealtimeTradeHistory::max_trades &&
        recent_trades.front().timestamp_ms > from && rt_archive_->first < recent_trades.front().timestamp_ms;
    const bool seeded = rt_archive_->startup_first>0 && from<rt_archive_->startup_end;
    const bool history = to > from && (seeded || (rt_archive_ && rt_archive_->recovered_in(from, to)) || trades_retired || to-from > 290000 || from < rt_clock_ms_ - 290000);
    if (!history) {
        rt_trade_tail_waiting_ = false;
        // Keep the loaded archive and any pending response while inspecting
        // the recent ring. Zooming back out must not start with empty history.
        if (rt_history_view_) {
            rt_history_view_ = false;
            rebuild_realtime_view();
        }
        return;
    }
    if (!rt_history_view_ && rt_loaded_to_ > 0) {
        rt_history_view_ = true;
        if (realtime_live_edge())
            append_realtime_depth_tail(rt_archive_samples_, rt_samples_, rt_loaded_to_, rt_clock_ms_, rt_loaded_step_);
        rebuild_realtime_view();
    }
    const int64_t step = std::max(int64_t(seeded || rt_archive_->recovered_in(from, to) ? 500 : 100), ((to-from+179999)/180000)*100);
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
            if (realtime_live_edge())
                append_realtime_depth_tail(rt_archive_samples_, rt_samples_, rt_loaded_to_, rt_clock_ms_, rt_loaded_step_);
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
        if (rt_archive_->query(aligned,to,rt_clock_ms_,step,tick_size_*rt_bucket_multiplier_,tick_size_)) {
            rt_query_from_=aligned;rt_query_to_=to;rt_query_step_=step;rt_query_at_=now;rt_query_multiplier_=rt_bucket_multiplier_;
        }
    }
}


const std::vector<Terminal::Liquidation>& ChartWidget::realtime_liquidations() const {
    return rt_mode_ && rt_paused_ ? rt_paused_liquidations_ : ctx_.liq_heatmap_mgr().observed_events(pair_);
}

void ChartWidget::render_realtime_liquidation_strip() {
    const auto& events = realtime_liquidations();
    const auto view = observed_liquidations::aggregate(events, int64_t(last_visible_range_.X.Min),
        int64_t(last_visible_range_.X.Max), rt_clock_ms_);
    char shorts[32] = {}, longs[32] = {};
    Indicators::format_volume_usdt(view.shorts, shorts, sizeof(shorts), nullptr);
    Indicators::format_volume_usdt(view.longs, longs, sizeof(longs), nullptr);
    ImGui::Text("Reported notional | shorts +%s / longs -%s | %.3gs bins",
        shorts + std::strspn(shorts, " "), longs + std::strspn(longs, " "), double(view.step_ms)/1000);
    ImGui::TextColored(Theme::Tokens::TX2, "%s", view.count == 0 ? "No reports in this retained window; completeness unknown"
        : pair_.exchange == "binancef" ? "Binance reports are censored; received notional only"
        : "Received notional only; feed coverage may be incomplete");
    if (!ImPlot::BeginPlot("##RTLiqActivity", ImVec2(-1, 90),
            ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) return;
    // Real-time has no indicator pane below, so this strip owns the time axis.
    ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
    ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoTickLabels);
    ImPlot::SetupAxisLimits(ImAxis_X1, last_visible_range_.X.Min, last_visible_range_.X.Max, ImGuiCond_Always);
    const double scale = std::max(1.0, view.peak) * 1.1;
    ImPlot::SetupAxisLimits(ImAxis_Y1, -scale, scale, ImGuiCond_Always);
    ImPlot::SetupAxisFormat(ImAxis_X1, [](double value, char* out, int size, void*) {
        DisplayTimeZone::instance().format(int64_t(value), TimeZoneFormat::TimeSeconds, out, size);
        return int(std::strlen(out));
    });
    ImPlot::SetupFinish();
    auto* draw = ImPlot::GetPlotDrawList();
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const float zero = ImPlot::PlotToPixels(0, 0).y;
    ImPlot::PushPlotClipRect();
    draw->AddLine(ImVec2(pos.x, zero), ImVec2(pos.x+size.x, zero), Theme::u32(Theme::Tokens::BD1));
    const observed_liquidations::Bin* hovered = nullptr;
    const double mouse_time = ImPlot::GetPlotMousePos().x;
    for (size_t i = 0; i < view.size; ++i) {
        const auto& bin = view.bins[i];
        if (!bin.count) continue;
        const int64_t end = std::min(bin.start_ms + view.step_ms, rt_clock_ms_);
        const float left = ImPlot::PlotToPixels(double(bin.start_ms), 0).x;
        const float right = std::max(left+1, ImPlot::PlotToPixels(double(end), 0).x);
        if (bin.shorts > 0) draw->AddRectFilled(ImVec2(left, ImPlot::PlotToPixels(0,bin.shorts).y), ImVec2(right,zero), Theme::u32(Theme::Tokens::UP));
        if (bin.longs > 0) draw->AddRectFilled(ImVec2(left,zero), ImVec2(right,ImPlot::PlotToPixels(0,-bin.longs).y), Theme::u32(Theme::Tokens::DOWN));
        if (mouse_time >= bin.start_ms && mouse_time < bin.start_ms + view.step_ms) hovered = &bin;
    }
    ImPlot::PopPlotClipRect();
    if (hovered && ImPlot::IsPlotHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%u reports | shorts +%.2f | longs -%.2f", hovered->count, hovered->shorts, hovered->longs);
        ImGui::TextUnformatted("Reported notional; no estimate of unreported liquidations.");
        ImGui::EndTooltip();
    }
    ImPlot::EndPlot();
}

// ═══════════════════════════════════════════════════════════════════════════
// Trade bubbles on candles (free)
//
// Real-time mode draws every received trade; this draws only the LARGE ones,
// over any time-based candle view, for every plan. Same marker and
// signed colours. Candle displays use their own compressed reference and
// screen-space selection so wide history cannot become a wall of equal discs.
// ═══════════════════════════════════════════════════════════════════════════

double ChartWidget::candle_bubble_floor() const {
    if (candle_bubble_min_ > 0) return candle_bubble_min_;
    return candle_bubble_auto_floor_ > 0 ? candle_bubble_auto_floor_ : candle_bubble_history_.auto_floor;
}

void ChartWidget::collect_candle_prints() {
    const auto& trades = candles().realtime_trades().trades();
    if (trades.empty()) return;
    const int64_t clock = trades.back().timestamp_ms;
    // Time went backwards (replay seek, context swap): the retained prints
    // belong to a traversal that no longer exists.
    if (clock < candle_prints_seen_ms_) {
        candle_prints_.clear();
        candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
        candle_bubble_scale_ = {}; candle_bubble_scale_since_ms_ = 0;
        candle_bubble_auto_floor_ = 0; candle_bubble_size_reference_ = 0;
    }
    // Fix the reference once warm. Panning and new history never resize old
    // prints. The user can explicitly recalibrate from the layer controls.
    if (!candle_bubble_scale_since_ms_) candle_bubble_scale_since_ms_ = clock;
    if (!(candle_bubble_auto_floor_ > 0)) {
        if (candle_bubble_history_.auto_floor > 0)
            candle_bubble_auto_floor_ = candle_bubble_history_.auto_floor;
        else {
            candle_bubble_scale_.update(trades, clock);
            if (candle_bubble_scale_.settled() ||
                (candle_bubble_scale_.minimum() > 0 && clock - candle_bubble_scale_since_ms_ >= kCandleBubbleWarmMs)) {
                candle_bubble_auto_floor_ = candle_bubble_scale_.minimum() * kCandleBubbleMult;
                // Recover the still-retained initial sample when auto first warms.
                if (candle_bubble_min_ == 0) {
                    candle_prints_.clear(); candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
                }
            }
        }
    }
    const double floor = candle_bubble_floor();
    // Fold in every trade newer than the last one scanned. Same-millisecond
    // ties are told apart by arrival position: the ring appends in order for
    // every venue, so the count already scanned at that millisecond is the
    // only identity needed (Bybit prints carry no numeric aggregate ID).
    auto it = std::upper_bound(trades.begin(), trades.end(), candle_prints_seen_ms_,
        [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
    if (candle_prints_seen_ms_ > 0) {
        auto tie = std::lower_bound(trades.begin(), it, candle_prints_seen_ms_,
            [](const Terminal::Trade& t, int64_t ts) { return t.timestamp_ms < ts; });
        size_t index = 0;
        for (; tie != it; ++tie, ++index)
            if (index >= candle_prints_seen_count_ && floor > 0 &&
                tie->price * tie->qty >= floor)
                candle_prints_.push_back(*tie);
    }
    for (; it != trades.end(); ++it)
        if (floor > 0 && it->price * it->qty >= floor) candle_prints_.push_back(*it);
    candle_prints_seen_ms_ = clock;
    {
        const auto first = std::lower_bound(trades.begin(), trades.end(), clock,
            [](const Terminal::Trade& a, int64_t ts) { return a.timestamp_ms < ts; });
        const auto last = std::upper_bound(first, trades.end(), clock,
            [](int64_t ts, const Terminal::Trade& a) { return ts < a.timestamp_ms; });
        candle_prints_seen_count_ = size_t(last - first);
    }
    // Same-ms ties appended after the main run can land out of order; the
    // renderer binary-searches by time, so keep the deque sorted.
    if (candle_prints_.size() > 1 &&
        candle_prints_[candle_prints_.size() - 2].timestamp_ms > candle_prints_.back().timestamp_ms)
        std::stable_sort(candle_prints_.begin(), candle_prints_.end(),
            [](const Terminal::Trade& a, const Terminal::Trade& b) { return a.timestamp_ms < b.timestamp_ms; });
    while (candle_prints_.size() > kCandlePrintsMax ||
           (!candle_prints_.empty() && candle_prints_.front().timestamp_ms < clock - kCandlePrintsKeepMs))
        candle_prints_.pop_front();
}

void ChartWidget::render_candle_bubbles() {
    const double floor = candle_bubble_floor();
    const auto limits = ImPlot::GetPlotLimits();
    // A candle bar is drawn CENTRED on its period's open time (draw_single_candle
    // spans x +/- 0.35 tf around timestamps[i]), while the period's trades run
    // from open to open + tf. Drawn at their raw time, the second half of every
    // bar's prints landed over the NEXT bar, at prices that bar never touched.
    // Shifting each print back by half a period puts it inside its own bar.
    const double shift_ms = double(candles().timeframe_seconds()) * 500.0;
    const bool history = candle_bubble_history_enabled_ && Entitlements::hosted() && Entitlements::is_pro() &&
        pair_.exchange == "binancef" && !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack();
    const int64_t from_ms = int64_t(limits.X.Min + shift_ms), to_ms = int64_t(limits.X.Max + shift_ms);
    if (history) {
        auto req = candle_bubble_history_.request(pair_,from_ms,to_ms,int64_t(emscripten_date_now()),emscripten_get_now());
        if (!req.is_null() && !req.empty()) {
            const char* token = emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
            req["data"]["entitlement_token"] = token ? token : "";
            ctx_.stream_mgr().send_message(req.dump());
        }
        int loaded = 0, observed = 0;
        candle_bubble_history_.coverage(from_ms,to_ms,loaded,observed);
        char status[160];
        if (!candle_bubble_history_.error.empty())
            snprintf(status,sizeof(status),"Bubbles: %s",candle_bubble_history_.error.c_str());
        else if (candle_bubble_history_.loading()) snprintf(status,sizeof(status),"Bubbles: loading recent history...");
        else snprintf(status,sizeof(status),"Bubbles: %d/%d loaded minutes have records | recent 6h | max 16/min",observed,loaded);
        auto* draw = ImPlot::GetPlotDrawList();
        const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
        const ImVec2 text(pos.x+8,pos.y+size.y-ImGui::GetFontSize()-8);
        const ImVec2 text_size = ImGui::CalcTextSize(status);
        ImPlot::PushPlotClipRect();
        draw->AddRectFilled(ImVec2(text.x-3,text.y-2),ImVec2(text.x+text_size.x+3,text.y+text_size.y+2),Theme::u32(Theme::Tokens::BASE,0.9f));
        draw->AddText(text,Theme::u32(Theme::Tokens::TX2),status);
        ImPlot::PopPlotClipRect();
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseHoveringRect(text,ImVec2(text.x+text_size.x,text.y+text_size.y)))
            Theme::tooltip("Historical selection: up to 16 largest original records per minute, filtered by Minimum value.\n"
                "Only the recent six hours are queried; the latest two minutes use live observations.\n"
                "A minute with records is not certified complete. Blank minutes may be quiet, missing or not loaded.\n"
                "Exchange records can aggregate fills. Overlapping smaller records are hidden; zoom for detail. Sizes use a fixed log-compressed reference.");
    }
    if (!(floor > 0)) return;
    const auto first = std::lower_bound(candle_prints_.begin(), candle_prints_.end(), int64_t(limits.X.Min + shift_ms),
        [](const Terminal::Trade& t, int64_t ts) { return t.timestamp_ms < ts; });
    const auto last = std::upper_bound(first, candle_prints_.end(), int64_t(limits.X.Max + shift_ms),
        [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
    auto* dl = ImPlot::GetPlotDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MixedBubbles::Marker* hovered = nullptr;
    ImPlot::PushPlotClipRect();
    // Qualifying prints keep their identities up to here; the mixed view then
    // merges the ones that share a bar and a price, so a level both takers hit
    // in one bar is one two-tone bubble, and the strongest-first selection
    // below removes crowding among the merged markers.
    static std::vector<const Terminal::Trade*> order;
    order.clear();
    for (auto it = first; it != last; ++it) {
        const double notional = it->price * it->qty;
        if (notional < floor || it->price < limits.Y.Min || it->price > limits.Y.Max) continue;
        if (!history || !candle_bubble_history_.contains(it->agg_trade_id)) order.push_back(&*it);
    }
    if (history) candle_bubble_history_.append_visible(order,from_ms,to_ms);
    order.erase(std::remove_if(order.begin(),order.end(),[&](const auto* t){
        return t->price*t->qty < floor || t->price < limits.Y.Min || t->price > limits.Y.Max;
    }),order.end());
    const size_t qualifying_count=order.size();
    candle_bubble_view_.build(order.begin(), order.end(), int64_t(shift_ms) * 2, tick_size_, 0.0);
    static std::vector<const MixedBubbles::Marker*> ranked;
    ranked.clear();
    for (size_t i = 0; i < candle_bubble_view_.count; ++i) ranked.push_back(&candle_bubble_view_.markers[i]);
    std::sort(ranked.begin(), ranked.end(), [](const MixedBubbles::Marker* a, const MixedBubbles::Marker* b) {
        if (a->value() != b->value()) return a->value() > b->value();
        if (a->timestamp_ms != b->timestamp_ms) return a->timestamp_ms < b->timestamp_ms;
        return a->price < b->price;
    });
    if (!(candle_bubble_size_reference_ > 0)) {
        if (candle_bubble_history_.size_reference > 0) candle_bubble_size_reference_=candle_bubble_history_.size_reference;
        else if (!history || !candle_bubble_history_.error.empty() || (!candle_bubble_history_.loading() && !candle_bubble_history_.tiles.empty())) {
            // Local/replay observations use their warmed market reference.
            candle_bubble_size_reference_=candle_bubble_auto_floor_;
        }
    }
    const double reference = candle_bubble_size_reference_;
    // Warm the market reference even with a manual visibility floor.
    if (!(reference > 0)) { ImPlot::PopPlotClipRect(); return; }
    static std::vector<CandleBubbleDisplay::Marker> selected;
    selected.clear();
    const auto plot_pos = ImPlot::GetPlotPos(), plot_size = ImPlot::GetPlotSize();
    for (size_t i=0;i<ranked.size();++i) {
        const auto* m=ranked[i];
        const auto c=ImPlot::PlotToPixels(double(m->timestamp_ms)-shift_ms,m->price);
        const float radius=CandleBubbleDisplay::radius(m->value(),reference);
        // Offscreen candidates cannot suppress a visible execution.
        if(c.x<plot_pos.x || c.x>plot_pos.x+plot_size.x || c.y<plot_pos.y || c.y>plot_pos.y+plot_size.y) continue;
        CandleBubbleDisplay::retain(selected,{i,c.x,c.y,radius},plot_size.x);
    }
    for (const auto& marker : selected) {
        const MixedBubbles::Marker* m = ranked[marker.index];
        const ImVec2 c = ImPlot::PlotToPixels(double(m->timestamp_ms) - shift_ms, m->price);
        const float r = CandleBubbleDisplay::radius(m->value(), reference);
        const ImVec4& major = m->buy_led() ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        const ImVec4& minor = m->buy_led() ? Theme::Tokens::DOWN : Theme::Tokens::UP;
        RealtimeBubble::draw(*dl, c, r, major, Theme::u32(Theme::Tokens::BASE),
                             RealtimeBubble::Fill::Translucent, float(m->minor()), minor);
        const float dx = mouse.x - c.x, dy = mouse.y - c.y;
        // Last hit is the visible topmost disc, including its circular edge.
        if (dx * dx + dy * dy <= r * r) hovered = m;
    }
    if (hovered && ImPlot::IsPlotHovered()) {
        const auto anchor = ImPlot::PlotToPixels(double(hovered->timestamp_ms)-shift_ms,hovered->price);
        const float right=plot_pos.x+plot_size.x;
        for(float x=anchor.x;x<right;x+=10)
            dl->AddLine(ImVec2(x,anchor.y),ImVec2(std::min(x+5,right),anchor.y),Theme::u32(Theme::Tokens::TX1,0.6f));
    }
    ImPlot::PopPlotClipRect();
    if (hovered && ImPlot::IsPlotHovered()) {
        char time[64] = {}, price[64] = {};
        DisplayTimeZone::instance().format(hovered->timestamp_ms, TimeZoneFormat::DateTimeSeconds, time, sizeof(time));
        snprintf(price, sizeof(price), fmt_.price_fmt, hovered->price);
        Theme::begin_tooltip();
        if (hovered->buy > 0 && hovered->sell > 0)
            ImGui::Text("Large prints, both takers: %.0f%% bought / %.0f%% sold",
                100.0 * hovered->buy / hovered->value(), 100.0 * hovered->sell / hovered->value());
        else ImGui::TextUnformatted(hovered->buy_led() ? "Large buy (taker bought)" : "Large sell (taker sold)");
        ImGui::Text("%s | %s %s", time, pair_.exchange.c_str(), pair_.symbol.c_str());
        ImGui::Text("Price %s | value %.0f (bought %.0f, sold %.0f) | %u record%s", price, hovered->value(),
            hovered->buy, hovered->sell, hovered->count, hovered->count == 1 ? "" : "s");
        ImGui::Text("%.1fx display reference (%.0f quote value)", hovered->value()/reference,reference);
        ImGui::Text("Showing %zu bubbles from %zu qualifying records; zoom for detail",selected.size(),qualifying_count);
        if (candle_bubble_history_.size_reference>0)
            ImGui::TextUnformatted("Reference: initial history selection P90, held while panning.");
        else ImGui::TextUnformatted("Reference: warmed live trade-size threshold.");
        ImGui::TextUnformatted("Log-compressed sizes. Price guide starts at this level.");
        ImGui::TextUnformatted("Records sharing this bar and price are one bubble; the exchange may aggregate fills.");
        Theme::end_tooltip();
    }
}
