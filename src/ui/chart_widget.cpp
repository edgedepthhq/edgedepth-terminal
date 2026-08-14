// ═══════════════════════════════════════════════════════════════════════════════
// chart_widget.cpp - REFACTORED: Rendering + UI only
//
// ChartWidget no longer owns candle data. It reads from CandleManager via
// const accessors. All candle lifecycle (build, finalize, batch, scroll-load)
// is in CandleManager.
//
// Removed: handle_trade, handle_candle, handle_candle_batch,
//          build_candle_from_trade, finalize_current_candle,
//          get_candle_timestamp, initial_load, request_historical_candles,
//          check_and_load_more, should_load_more_candles, rebuild_cache,
//          plot_candlesticks, plot_candles_simple (replaced by plot_candles)
//
// Removed dependency: custom_implot.h/.cpp (draw_single_candle is inline)
// ═══════════════════════════════════════════════════════════════════════════════

#include "ui/chart_widget.h"
#include "ui/price_profile_renderer.h"
#include "ui/custom_implot.h"
#include "core/footprint_manager.h"
#include "core/heatmap_colormap.h"
#include "core/entitlements.h"
#include "core/stream_presence.h"
#include "core/display_time_zone.h"
#include "ui/upsell_modal.h"
#include "ui/research_moment_panel.h"  // right-click "Investigate this minute"
#include "core/research_url.h"         // the 60000 minute floor + UTC labels
#include "education/recorder_runtime.h"  // suppress the status chip in produced clips
#include "rendering/theme.h"
#include "rendering/app_shell.h"
#include "rendering/menu.h"            // "+ widget" toolbar menu → picker / add-request
#include "core/education_boot.h"       // is_embedded()/is_pack() gate the add path
#include "core/recorder_glue.h"        // ClipRecorder::focus_active gates the rail
#include "ui/drawing/drawing_toolbar.h"  // in-chart rail + Draw dropdown
#include "ui/drawing/drawing_icons.h"
#include "implot.h"
#include <cmath>
#include "implot_internal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
#include <ctime>
#include <ranges>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "indicators/volume_indicator.h"
#include "replayer/replay_manager.h"
#include "core/preview_candle_store.h"  // scrub-preview (ghost candles) y-fit
#include "core/ticker_manager.h"        // HL market registry for the census pill grey-out
#include "types/frame_profiler.h"
#include "education/chart_projection.h"

// Single global chart-coordinate snapshot for the lesson spotlight overlay.
namespace edu {
ChartProjection& chart_projection() {
    static ChartProjection p;
    return p;
}
}

// "Investigate this minute" - shared by the candle (##ChartCtx) and Renko
// (##RenkoCtx) context menus, placed ABOVE the replay block in both (it is a
// read, not a replay). Enabled only for Binance USDT-M: that is all the C++
// side can honestly know - tier, record edge and absent readings belong to
// the web surface, which already says the right thing for each.
static void render_investigate_menu_item(const Terminal::Pair& pair, int64_t minute_ms) {
    const bool on_record = (pair.exchange == "binancef");
    const bool enabled = on_record && minute_ms > 0;
    std::string shortcut;
    if (enabled) shortcut = research_url::minute_label_utc(minute_ms);
    if (ImGui::MenuItem("Investigate this minute",
                        shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled)) {
        ui::ResearchMomentPanel::instance().open(pair.symbol, minute_ms);
        ImGui::CloseCurrentPopup();
    }
    if (!on_record && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Theme::tooltip("The record covers Binance USDT-M");
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════════

// P2e census: resolve the HL census pair ({"hl", UNDERLYING}) for any chart pair.
// The census feed is keyed by underlying coin, so any venue's chart of the same
// underlying subscribes the same subject. HL-native pairs are their own underlying
// (uppercase coins, "BTC"); binancef perps are lowercase "<base><quote>" - strip the
// stable-quote suffix and uppercase. Binance's 1000-bundled tickers map to HL's
// k-prefix ("1000pepe" → "kPEPE"; both price 1000 units, so USD scales align).
static Terminal::Pair hl_census_pair_for(const Terminal::Pair& pair) {
    if (pair.exchange == "hl") return pair;
    std::string base = pair.symbol;
    for (const char* q : {"usdt", "usdc", "fdusd", "busd"}) {
        const std::string_view qv{q};
        if (base.size() > qv.size() && std::string_view(base).ends_with(qv)) {
            base.resize(base.size() - qv.size());
            break;
        }
    }
    std::string sym;
    sym.reserve(base.size() + 1);
    if (base.rfind("1000", 0) == 0) { sym += 'k'; base.erase(0, 4); }
    for (char c : base)
        sym += (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    return {"hl", sym};
}

ChartWidget::ChartWidget(
    const Terminal::Pair& pair,
    const AppContext& ctx,
    double tick_size)
    : pair_(pair)
    , ctx_(ctx)
    , last_heatmap_update_ms_(std::chrono::steady_clock::now())
    , tick_size_(tick_size)
{
    title_tf_seconds_ = ctx_.candle_mgr().timeframe_seconds();
    timeframe_label_ = timeframe_to_string(title_tf_seconds_);
    // Visible prefix carries the TF; identity after "###" is TF-independent so the
    // chart stays docked across TF changes (matches layout.cpp's chart dock id).
    title_ = "Chart " + pair.exchange + " " + pair.symbol + " " + timeframe_label_ +
             "###chart_" + pair.exchange + "_" + pair.symbol;
    fmt_ = PriceFormatter::from_tick_and_step(tick_size_, 0.001);

    // P2e: the census layer defaults ON for HL-native pairs - there the census IS
    // the ground-truth predictive layer (the modelled heatmap has no HL publisher).
    // On other venues it's the opt-in cross-venue overlay (the pill).
    liq_census_pair_ = hl_census_pair_for(pair_);
    liq_census_enabled_ = (pair_.exchange == "hl");

    // Subscribe to Volume stream for CVD intra-candle wicks
    {
        volume_sub_tf_ms_ = ctx_.candle_mgr().timeframe_seconds() * 1000;
        StreamKey vol_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        StreamHandler<Terminal::Volume> vol_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Volume& v) {
                static_cast<ChartWidget*>(ptr)->handle_volume(v);
            }
        };
        ctx_.stream_mgr().subscribe_volume(vol_key, vol_handler);
        volume_subscribed_ = true;
    }

    // Subscribe to Stats stream for funding rate data
    {
        stats_sub_tf_ms_ = ctx_.candle_mgr().timeframe_seconds() * 1000;
        StreamKey stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        StreamHandler<Terminal::Stat> stat_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Stat& s) {
                static_cast<ChartWidget*>(ptr)->handle_stat_for_chart(s);
            }
        };
        ctx_.stream_mgr().subscribe_stats(stat_key, stat_handler);
        stats_subscribed_ = true;
    }

    // Subscribe to the discrete @forceOrder liquidation stream (WS4 Observed
    // markers). TF-independent (timeframe 0); live frames come from the
    // LIQUIDATIONS NATS stream, replay frames from the archive bundle / the
    // liquidation_events DB seed. Events land in LiquidationHeatmapManager.
    {
        StreamKey liq_key{pair_, Terminal::Stream::Liquidations, 0};
        StreamHandler<Terminal::Liquidation> liq_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Liquidation& l) {
                auto* w = static_cast<ChartWidget*>(ptr);
                w->ctx_.liq_heatmap_mgr().add_observed_event(w->pair_, l);
            }
        };
        ctx_.stream_mgr().subscribe_liquidations(liq_key, liq_handler);
        liq_events_subscribed_ = true;
    }

    // Research rollout only. This is an explicit UI gate; the dedicated stream id
    // remains separate from ordinary pattern traffic and the server retains
    // authority over delivery.
    if (Entitlements::is_research()) {
        const StreamKey pattern_key{pair_, Terminal::Stream::PatternAdmin, 0};
        StreamHandler<Terminal::PatternOverlay> pattern_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::PatternOverlay& pattern) {
                static_cast<ChartWidget*>(ptr)->handle_pattern_overlay(pattern);
            }
        };
        pattern_stream_mgr_ = &ctx_.stream_mgr();
        pattern_stream_mgr_->subscribe_patterns(pattern_key, pattern_handler);
        pattern_subscribed_ = true;
    }
}

ChartWidget::~ChartWidget() {
    if (volume_subscribed_) {
        StreamKey vol_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        ctx_.stream_mgr().unsubscribe_volume(vol_key, this);
    }
    if (stats_subscribed_) {
        StreamKey stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        ctx_.stream_mgr().unsubscribe_stats(stat_key, this);
    }
    if (liq_events_subscribed_) {
        StreamKey liq_key{pair_, Terminal::Stream::Liquidations, 0};
        ctx_.stream_mgr().unsubscribe_liquidations(liq_key, this);
    }
    if (pattern_subscribed_ && pattern_stream_mgr_) {
        const StreamKey pattern_key{pair_, Terminal::Stream::PatternAdmin, 0};
        pattern_stream_mgr_->unsubscribe_patterns(pattern_key, this);
    }
    if (liq_heatmap_subscribed_) {
        const StreamKey key{pair_, Terminal::Stream::LiquidationHeatmap, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
    }
    if (liq_census_subscribed_) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
    }
}

bool ChartWidget::is_loading() const {
    return ctx_.candle_mgr().is_loading();
}

std::string ChartWidget::timeframe_to_string(int64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m";
    if (seconds < 86400) return std::to_string(seconds / 3600) + "h";
    return std::to_string(seconds / 86400) + "d";
}


// Frame Update
void ChartWidget::update() {
    ProfileScope _ps("ChartUpd");
    if (chart_type_ == ChartType::Renko) {
        // Renko's last_visible_range_.X is a BRICK-INDEX domain - never feed that
        // to CandleManager (its scroll-load thresholds are timestamps). Use the
        // visible bricks' TIME window published by render_chart_renko instead.
        if (renko_view_t1_ms_ == 0) return;
        ctx_.candle_mgr().update(static_cast<double>(renko_view_t0_ms_),
                                 static_cast<double>(renko_view_t1_ms_));
    } else {
        if (last_visible_range_.X.Max == 0 || last_visible_range_.Y.Max == 0 ||
            last_visible_range_.X.Min == 0) {
            return;
        }
        ctx_.candle_mgr().update(last_visible_range_.X.Min, last_visible_range_.X.Max);
    }

    // CLIP_FACTORY P2: one-shot view overrides from the recorder script.
    // Layer state is the produced clip's visual grammar; scripts pin it
    // explicitly instead of inheriting whatever this build's defaults are.
    if (!recorder_view_applied_ && edu::RecorderRuntime::instance().active()) {
        if (const auto* v = edu::RecorderRuntime::instance().view()) {
            if (v->liq_field >= 0 && liq_dense_field_ != (v->liq_field != 0)) {
                liq_dense_field_ = v->liq_field != 0;
                liq_shelf_cache_ts_ = -1;
            }
            if (v->liq_levels >= 0 && liq_heatmap_enabled_ != (v->liq_levels != 0))
                toggle_liquidation_heatmap();
            if (v->liq_profile >= 0) liq_profile_enabled_ = v->liq_profile != 0;
            if (v->liq_observed >= 0) liq_observed_enabled_ = v->liq_observed != 0;
            if (v->ob_depth >= 0) heatmap_enabled_ = v->ob_depth != 0;
            if (v->vpvr >= 0 && vpvr_enabled_ != (v->vpvr != 0)) {
                vpvr_enabled_ = v->vpvr != 0;
                ctx_.vpvr_mgr().set_enabled(vpvr_enabled_);
                if (vpvr_enabled_) { vpvr_last_request_start_ = 0; vpvr_last_request_end_ = 0; }
            }
        }
        recorder_view_applied_ = true;
    }

    // ── Client-side price alerts ──────────────────────────────────────────
    // Fire a browser notification + in-app toast + beep when the live price
    // crosses a user-set level (added from the chart right-click menu, live
    // mode only). One-shot: the level clears once it fires. Session-scoped.
    if (!alerts_.empty()) {
        const double px = ctx_.candle_mgr().last_close_price();
        if (px > 0.0 && alert_last_price_ > 0.0) {
            for (size_t i = 0; i < alerts_.size();) {
                const PriceAlert& a = alerts_[i];
                const bool crossed = a.arm_above
                    ? (px >= a.price && alert_last_price_ < a.price)
                    : (px <= a.price && alert_last_price_ > a.price);
                if (crossed) {
                    char pbuf[32];
                    fmt_.format_price(pbuf, sizeof(pbuf), a.price);
                    std::string sym = pair_.symbol;
                    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    alert_toast_msg_ = sym + " crossed " + pbuf;
                    alert_toast_until_ = ImGui::GetTime() + 6.0;
#ifdef __EMSCRIPTEN__
                    EM_ASM({
                        try {
                            var msg = UTF8ToString($0);
                            if (window.Notification && Notification.permission === 'granted')
                                new Notification('EdgeDepth alert', { body: msg });
                            var AC = window.AudioContext || window.webkitAudioContext;
                            if (AC) { var c = new AC(); var o = c.createOscillator();
                                var g = c.createGain(); o.frequency.value = 880;
                                o.connect(g); g.connect(c.destination); g.gain.value = 0.06;
                                o.start(); o.stop(c.currentTime + 0.18); }
                        } catch (e) {}
                    }, alert_toast_msg_.c_str());
#endif
                    alerts_.erase(alerts_.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
        }
        if (px > 0.0) alert_last_price_ = px;
    } else {
        const double px = ctx_.candle_mgr().last_close_price();
        if (px > 0.0) alert_last_price_ = px;
    }

    if (!ctx_.candle_mgr().empty()) {
        auto* vol_ind = indicator_mgr_.get_indicator_of_type<Indicators::VolumeIndicator>();
        if (vol_ind && vol_ind->get_bar_count() == 0 && ctx_.candle_mgr().is_initial_load_complete()) {
            vol_ind->clear();
            populate_volume_data(vol_ind);
            vol_ind->update();
        }
        update_volume_indicators();

        // CVD: populate on first data availability, update building candle each frame
        auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
        if (cvd_ind && cvd_ind->is_visible() && ctx_.candle_mgr().is_initial_load_complete()) {
            // Repopulate when candle count changes (new candle finalized or after clear)
            const size_t current_count = ctx_.candle_mgr().count();
            if (current_count > 0 && cvd_ind->bar_count() != current_count) {
                cvd_ind->clear();
                populate_cvd_data(cvd_ind);
            }
            update_cvd_indicator();
        }

        // RSI: populate on first data, repopulate when candle count changes
        auto* rsi_ind = indicator_mgr_.get_indicator_of_type<Indicators::RSIIndicator>();
        if (rsi_ind && rsi_ind->is_visible() && ctx_.candle_mgr().is_initial_load_complete()) {
            const size_t current_count = ctx_.candle_mgr().count();
            if (current_count > 0 && rsi_ind->bar_count() != current_count) {
                rsi_ind->clear();
                for (const auto& candle : ctx_.candle_mgr().candles())
                    rsi_ind->add_candle(candle.timestamp_ms, candle.close);
            }
            if (ctx_.candle_mgr().has_building_candle()) {
                const auto& bc = ctx_.candle_mgr().building_candle();
                rsi_ind->set_building_candle(bc.timestamp_ms, bc.close);
            }
            rsi_ind->update();
        }

        // MACD: same pattern
        auto* macd_ind = indicator_mgr_.get_indicator_of_type<Indicators::MACDIndicator>();
        if (macd_ind && macd_ind->is_visible() && ctx_.candle_mgr().is_initial_load_complete()) {
            const size_t current_count = ctx_.candle_mgr().count();
            if (current_count > 0 && macd_ind->bar_count() != current_count) {
                macd_ind->clear();
                for (const auto& candle : ctx_.candle_mgr().candles())
                    macd_ind->add_candle(candle.timestamp_ms, candle.close);
            }
            if (ctx_.candle_mgr().has_building_candle()) {
                const auto& bc = ctx_.candle_mgr().building_candle();
                macd_ind->set_building_candle(bc.timestamp_ms, bc.close);
            }
            macd_ind->update();
        }

        // Funding Rate: populate from stats stream cache
        auto* funding_ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
        if (funding_ind && funding_ind->is_visible() && ctx_.candle_mgr().is_initial_load_complete()) {
            const size_t current_count = ctx_.candle_mgr().count();
            const bool have_funding_data = !funding_cache_.empty();
            if (current_count > 0 && have_funding_data &&
                (funding_ind->bar_count() != current_count || funding_data_dirty_)) {
                funding_ind->clear();
                populate_funding_data(funding_ind);
                funding_data_dirty_ = false;
            }
            update_funding_indicator();
        }

        // OI: populate from stats stream cache, repopulate when candles change or history arrives
        auto* oi_ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
        if (oi_ind && oi_ind->is_visible() && ctx_.candle_mgr().is_initial_load_complete()) {
            const size_t current_count = ctx_.candle_mgr().count();
            // Only populate when we have OI data to work with (avoid spamming empty populate
            // while waiting for the async historical OI batch to arrive)
            const bool have_oi_data = !oi_ohlc_cache_.empty() || !oi_cache_.empty();
            if (current_count > 0 && have_oi_data &&
                (oi_ind->bar_count() != current_count || oi_data_dirty_)) {
                oi_ind->clear();
                populate_oi_data(oi_ind);
                oi_data_dirty_ = false;
            }
            update_oi_indicator();
        }

        // VPIN / Toxicity: subscribe + history request + revision-gated
        // repopulate from the shared SeriesCache
        {
            auto* vpin_ind = indicator_mgr_.get_indicator_of_type<Indicators::VPINIndicator>();
            if (vpin_ind && vpin_ind->is_visible()) {
                update_vpin_indicator();
            }
        }
    }
    // Heatmap - request once candles are loaded (WS guaranteed connected).
    // Skipped in Renko: the time-keyed overlays do not draw there, and
    // update_heatmap() reads last_visible_range_ as TIME (brick indices in Renko).
    if (heatmap_enabled_ && ct_allows_time_overlays(chart_type_) &&
        ctx_.candle_mgr().is_initial_load_complete()) {
        request_heatmap_data();  // First-time request, deferred from constructor

        update_heatmap();
        update_live_heatmap_from_orderbook();
    }
    // Liquidation heatmap - subscribe + request historical once WS is connected
    // During replay, skip live subscriptions (data arrives via replay binary stream)
    const bool is_replay = ctx_.candle_mgr().replay_start_time_ms() > 0;
    if (liq_heatmap_enabled_ && ct_allows_time_overlays(chart_type_) &&
        ctx_.candle_mgr().is_initial_load_complete()) {
        if (!liq_heatmap_subscribed_ && !is_replay) {
            const StreamKey key{pair_, Terminal::Stream::LiquidationHeatmap, 0};
            ctx_.stream_mgr().send_subscribe(key);
            liq_heatmap_subscribed_ = true;
        }
        if (!liq_timeline_requested_) {
            request_liq_heatmap_data();
        }
        // Scroll-back: load more liq heatmap data when user scrolls/zooms out
        if (liq_timeline_requested_) {
            update_liq_heatmap_scroll();
        }
    }
    // Liq Levels HL (census, P2e) - live-only subscribe on the UNDERLYING-keyed HL
    // pair (works from any venue's chart of the same underlying). No historical
    // request: the stream is deliver-last-per-subject (loads instantly on subscribe)
    // with a 5-min live window; replay has no census data.
    if (liq_census_enabled_ && ct_allows_time_overlays(chart_type_) &&
        ctx_.candle_mgr().is_initial_load_complete() &&
        !liq_census_subscribed_ && !is_replay) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_subscribe(key);
        liq_census_subscribed_ = true;
    }
    indicator_mgr_.update_all();
}

// Timeframe Change
void ChartWidget::set_liq_opacity(float v) {
    liq_opacity_ = std::clamp(v, 0.0f, 1.0f);
    if (auto* recon = ctx_.liq_heatmap_mgr().get_timeline_reconstructor(pair_))
        recon->set_opacity(liq_opacity_);
}

void ChartWidget::change_timeframe(const int new_tf_seconds)
{
    ctx_.candle_mgr().change_timeframe(new_tf_seconds);
    // The previous viewport is expressed in the old timeframe's domain. Reusing
    // it for even one frame makes the new timeframe's Y fit sample the wrong
    // candle window and can leave volatile symbols vertically compressed.
    last_visible_range_ = {};
    x_axis_min_ = 0.0;
    x_axis_max_ = 0.0;
    stored_x_min_ = 0.0;
    stored_x_max_ = 0.0;
    // Update ReplayManager's timeframe so scrubber ticks, candle-boundary
    // snapping, and skip amounts use the correct timeframe.
    if (ctx_.replayer) {
        ctx_.replay_mgr().set_timeframe_ms(static_cast<int64_t>(new_tf_seconds) * 1000);
    }
    ctx_.heatmap_mgr().clear(pair_, heatmap_mode_);
    heatmap_data_requested_ = false;
    heatmap_loaded_timeframe_ = 0;
    // Liq map is TIMEFRAME-INDEPENDENT. Snapshots are price-level bands keyed by
    // {exchange,symbol}+timestamp (raw_timeline_protos_) at a fixed ~5-min cadence -
    // NOT bucketed by candle TF. The chart TF only changes the x-axis mapping at
    // render; the GPU timeline (and rails) reconstruct from the cached protos via the
    // same path set_leverage_mask() uses. So do NOT clear or re-request on TF change:
    // that reset forced request_liq_heatmap_data() -> a full-history server re-query
    // (handleGetHistoricalLiqHeatmap, 15s ctx timeout) on EVERY TF change, which is the
    // 20s "load delay / sometimes no-load" bug. Keep the cache; if a higher TF widens
    // the view past loaded data, update_liq_heatmap_scroll() fills the gap incrementally.

    // Clear and reset indicators for new timeframe
    auto* vol_ind = indicator_mgr_.get_indicator_of_type<Indicators::VolumeIndicator>();
    if (vol_ind) {
        vol_ind->clear();
        vol_ind->set_timeframe(new_tf_seconds);
    }
    auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
    if (cvd_ind) {
        cvd_ind->clear();
        cvd_ind->set_timeframe(new_tf_seconds);
    }
    // Clear CVD wick cache and re-subscribe Volume stream for new timeframe
    cvd_wick_cache_.clear();
    if (volume_subscribed_) {
        StreamKey old_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        ctx_.stream_mgr().unsubscribe_volume(old_key, this);
    }
    {
        volume_sub_tf_ms_ = static_cast<int64_t>(new_tf_seconds) * 1000;
        StreamKey vol_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        StreamHandler<Terminal::Volume> vol_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Volume& v) {
                static_cast<ChartWidget*>(ptr)->handle_volume(v);
            }
        };
        ctx_.stream_mgr().subscribe_volume(vol_key, vol_handler);
        volume_subscribed_ = true;
    }
    // Re-subscribe stats for new timeframe + clear funding cache
    funding_cache_.clear();
    if (stats_subscribed_) {
        StreamKey old_stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        ctx_.stream_mgr().unsubscribe_stats(old_stat_key, this);
    }
    {
        stats_sub_tf_ms_ = static_cast<int64_t>(new_tf_seconds) * 1000;
        StreamKey stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        StreamHandler<Terminal::Stat> stat_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Stat& s) {
                static_cast<ChartWidget*>(ptr)->handle_stat_for_chart(s);
            }
        };
        ctx_.stream_mgr().subscribe_stats(stat_key, stat_handler);
        stats_subscribed_ = true;
    }
    // Clear and reset funding indicator for new timeframe
    funding_history_requested_ = false;
    funding_data_dirty_ = false;
    auto* funding_ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
    if (funding_ind) {
        funding_ind->clear();
        funding_ind->set_timeframe(new_tf_seconds);
        // Re-request historical funding for new timeframe
        funding_history_requested_ = true;
        const int count = static_cast<int>(ctx_.candle_mgr().count());
        ctx_.stream_mgr().request_historical_funding(pair_, new_tf_seconds, count > 0 ? count : 2880);
    }
    // Clear and reset OI indicator for new timeframe
    oi_ohlc_cache_.clear();
    oi_cache_.clear();
    oi_history_requested_ = false;
    oi_data_dirty_ = false;
    auto* oi_ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
    if (oi_ind) {
        oi_ind->clear();
        oi_ind->set_timeframe(new_tf_seconds);
        request_historical_oi(); // Re-request for new timeframe
    }
}

// Main Render
void ChartWidget::render() {
    if (!is_open) return;

    // Keep the tab's visible TF in sync with the live timeframe (catches every
    // TF-change path, not just the toolbar). Cheap int compare; the string is only
    // rebuilt on an actual change. The docking identity (after "###") is unchanged.
    {
        const int64_t tf = ctx_.candle_mgr().timeframe_seconds();
        if (tf != title_tf_seconds_) {
            title_tf_seconds_ = tf;
            timeframe_label_  = timeframe_to_string(tf);
            title_ = "Chart " + pair_.exchange + " " + pair_.symbol + " " + timeframe_label_ +
                     "###chart_" + pair_.exchange + "_" + pair_.symbol;
        }
    }

    ImGuiWindowClass window_class;
    window_class.ClassId = ImHashStr("ChartWindow");
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_CentralNode;

    if (hints.use_hints) {
        ImGui::SetNextWindowPos(ImVec2(hints.pos_x, hints.pos_y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(hints.size_x, hints.size_y), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowClass(&window_class);

    if (!ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    render_controls();
    crosshair_state_ = CrosshairState();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    // In-chart drawing rail (2026-08-06: moved off the viewport edge so it
    // sits directly against the chart). It reserves its width from the plot;
    // the body child keeps the indicator panes right of the rail too. Gated
    // like the old full_shell chrome - drawings themselves render everywhere.
    const auto& edu_boot = EducationBoot::instance();
    const bool show_rail = !edu_boot.is_embedded() && !edu_boot.is_pack() &&
                           !ClipRecorder::focus_active() &&
                           !edu::RecorderRuntime::instance().active();
    if (show_rail) {
        drawing::render_chart_rail(ctx_.drawing_mgr(),
                                   ImGui::GetContentRegionAvail().y);
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::BeginChild("##chart_body", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

    const float total_height = ImGui::GetContentRegionAvail().y;
    // Indicator pane (tabbed, design .indi-pane): 0 / header-only / INDI_PANE_H.
    // Renko skips the time-aligned indicator pane (render_indicators early-returns),
    // so reserve no height for it - the brick chart takes the full area.
    const float indicator_total_height =
        (chart_type_ == ChartType::Renko) ? 0.0f : indicator_mgr_.pane_height();
    const float chart_height = std::max(100.0f, total_height - indicator_total_height);

    chart_allocated_height_ = chart_height;
    indicator_allocated_height_ = indicator_total_height;
    crosshair_state_.is_active = false;

    // SPEC §0.2: the chart + every stacked pane share ONE right-gutter
    // column - ImPlot equalizes the y-axis widths across the group, so all
    // plots have identical x-extents. This is also what makes the crosshair
    // math exact: render_crosshair maps time→pixels with a single transform
    // spanning chart-left → last-pane-right, which is only valid when the
    // extents match (mixed per-pane axis label widths caused the 2026-07-04
    // "mouse vs time-pointer horizontal gap" once panes stacked by default).
    const bool plots_aligned = ImPlot::BeginAlignedPlots("##chart_pane_align");
    render_chart();
    // Restore the ImPlot input map before the indicator subplots BeginPlot -
    // they must never see the drawing layer's overrides. Idempotent.
    drawing_layer_.end_frame();
    {
        ProfileScope _ps("Indics");
        render_indicators();
    }
    if (plots_aligned) ImPlot::EndAlignedPlots();

    if (crosshair_state_.is_active) {
        render_crosshair(crosshair_state_.plot_pos);
    }

    ImGui::EndChild();  // ##chart_body
    ImGui::PopStyleVar();

    ImGui::End();
}


// Chart Rendering - reads from CandleManager
void ChartWidget::render_chart() {
    // Renko abandons the time X-axis for a brick-index domain; it is a fully
    // self-contained render path (own BeginPlot/EndPlot) so the time-based path
    // below is untouched. See render_chart_renko.
    if (chart_type_ == ChartType::Renko) { render_chart_renko(); return; }

    const auto& timestamps = ctx_.candle_mgr().timestamps();
    if (timestamps.empty() && !ctx_.candle_mgr().has_building_candle()) {
        ImGui::Text("No candle data available");
        return;
    }
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    // Calculate X range. When no finalized candles exist yet (fresh/volatile
    // symbol still loading history, or replay before the first drip) the old
    // code anchored x_min at 0.0, so the axis spanned 1970 to now with a single
    // dot at the live edge: the "no history / broken" look on RT load. Anchor the
    // window to the live edge instead. Reached identically by live and replay,
    // which both arrive here with only a building candle / ticks at first.
    double x_min, x_max;
    if (!timestamps.empty()) {
        x_min = timestamps.front();
        x_max = timestamps.back();
    } else {
        int64_t anchor_ms;
        if (ctx_.candle_mgr().has_building_candle()) {
            anchor_ms = ctx_.candle_mgr().building_candle().timestamp_ms;
        } else if (!ctx_.candle_mgr().tick_times().empty()) {
            anchor_ms = ctx_.candle_mgr().tick_times().back();
        } else if (ctx_.replay_mgr().is_active()) {
            anchor_ms = ctx_.replay_mgr().interpolated_time_ms();
        } else {
            anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        const int64_t span_ms =
            static_cast<int64_t>(ctx_.candle_mgr().visible_candles_for_timeframe()) *
            tf_sec * 1000LL;
        x_max = static_cast<double>(anchor_ms);
        x_min = static_cast<double>(anchor_ms - span_ms);
    }
    if (ctx_.candle_mgr().has_building_candle()) {
        x_max = std::max(x_max, static_cast<double>(ctx_.candle_mgr().building_candle().timestamp_ms));
    }
    const double x_padding = static_cast<double>(tf_sec) * 1000.0 * 3.0;
    x_max += x_padding;

    // During replay, clamp x_max so heatmaps/overlays don't render past the
    // current playback position. Allows a small padding (1 candle) for the
    // building candle, but prevents data from leaking into "future" chart area.
    if (ctx_.replay_mgr().is_active()) {
        const double replay_time = static_cast<double>(ctx_.replay_mgr().interpolated_time_ms());
        const double replay_max = replay_time + static_cast<double>(tf_sec) * 1000.0 * 2.0;
        x_max = std::min(x_max, replay_max);
    }
    // Scrub preview (ghost candles): while the user aims a scrub on the
    // transport, extend x_max PAST the playhead clamp to the aimed-at time
    // plus a small lead, bounded by the replay window end. The view grows as
    // the user sweeps right and snaps back to the clamp when the preview
    // disarms - the clamp above stays authoritative whenever no preview is up.
    const int64_t scrub_preview_ms = ctx_.replay_mgr().scrub_preview_ms();
    if (scrub_preview_ms > 0) {
        const double preview_lead = static_cast<double>(tf_sec) * 1000.0 * 6.0;
        const double window_end = static_cast<double>(ctx_.replay_mgr().info().end_time_ms)
                                  + x_padding;
        const double preview_max = std::min(
            static_cast<double>(scrub_preview_ms) + preview_lead, window_end);
        x_max = std::max(x_max, preview_max);
    }

    if (ctx_.candle_mgr().follow_live()) {
        const size_t visible_candles = ctx_.candle_mgr().visible_candles_for_timeframe();
        if (ctx_.candle_mgr().count() > visible_candles) {
            const auto& candles = ctx_.candle_mgr().candles();
            x_min = static_cast<double>(candles[candles.size() - visible_candles].timestamp_ms);
        }
    }

    // CLIP_FACTORY: script-pinned visible span. The post-seek batch fit spans
    // however much history the seed happened to serve - a produced clip must
    // not inherit that. Pin the X window to the last viewSpanMinutes behind
    // the live edge - but ONLY from shot 0 onward (in_shot): pinning during
    // boot/seek/prime races the initial candle batch (malformed building
    // candle + request thrash), so those phases keep default behavior.
    if (edu::RecorderRuntime::instance().in_shot()) {
        if (const auto* rv = edu::RecorderRuntime::instance().view();
            rv && rv->view_span_min > 0) {
            x_min = x_max - static_cast<double>(rv->view_span_min) * 60000.0;
        }
    }
    // Last-resort guard on a degenerate X range. Every branch above assumes
    // x_min < x_max, and a feed that lands a bogus candle at the back of the
    // series breaks that: an all-zero candle at timestamp 0 takes x_max to 0
    // against a real epoch x_min. ImPlot then draws an empty plot on an
    // inverted axis, and nothing clears it except a timeframe change, so the
    // terminal looks broken for as long as the user leaves it alone. The candle
    // manager now rejects that candle at the door; this is the backstop that
    // keeps ANY such feed from blanking the chart permanently. Written as
    // !(x_max > x_min) so a NaN from the same class of bug is caught too.
    if (!(x_max > x_min)) {
        const int64_t anchor_ms = ctx_.replay_mgr().is_active()
            ? ctx_.replay_mgr().interpolated_time_ms()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
        const double span_ms =
            static_cast<double>(ctx_.candle_mgr().visible_candles_for_timeframe()) *
            static_cast<double>(tf_sec) * 1000.0;
        x_max = static_cast<double>(anchor_ms) + x_padding;
        x_min = x_max - span_ms;
    }

    // Real-time view (RT), any chart type: engage follow-live so the chart
    // streams and keeps the live edge current. We re-arm follow-live ONLY on the
    // rising edge of rt_mode_ (rt_was_on_ edge-detect) - not every frame - so the
    // normal follow_live() X/Y path (above / below) takes over and ImPlot zoom +
    // pan stay fully interactive (the first user pan/scroll clears the latch in
    // handle_plot_interaction). No per-frame axis pin here.
    if (rt_mode_ && !rt_was_on_) {
        ctx_.candle_mgr().set_follow_live(true);
    }
    rt_was_on_ = rt_mode_;
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10, 10));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 1.0f);
    if (ImPlot::BeginPlot("##Price", ImVec2(-1, chart_allocated_height_),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        // Hide x-axis labels on main chart when the indicator pane shows a
        // plot below (the pane owns the time axis then)
        const bool has_subplots = indicator_mgr_.pane_expanded();
        // TPO mode always shows date labels on main chart x-axis
        const bool hide_x_labels = has_subplots && chart_type_ != ChartType::TPO;
        ImPlotAxisFlags x_flags = hide_x_labels ? ImPlotAxisFlags_NoTickLabels : ImPlotAxisFlags_None;
        ImPlot::SetupAxis(ImAxis_X1, nullptr, x_flags);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisFormat(ImAxis_Y1, fmt_.price_fmt);

        // Beyond this density candles and liquidation cells collapse below a
        // useful pixel width. ImPlot applies this constraint during wheel zoom,
        // while the explicit clamp below also repairs an already-oversized view.
        constexpr double kMinVisibleCandles = 8.0;
        constexpr double kMaxVisibleCandles = 1440.0;
        const double timeframe_ms = static_cast<double>(tf_sec) * 1000.0;
        if (chart_type_ != ChartType::TPO) {
            ImPlot::SetupAxisZoomConstraints(
                ImAxis_X1,
                kMinVisibleCandles * timeframe_ms,
                kMaxVisibleCandles * timeframe_ms);
        }

        // TPO mode: override x-axis formatter to show dates only (no scientific notation)
        if (chart_type_ == ChartType::TPO) {
            ImPlot::SetupAxisFormat(ImAxis_X1, [](double value, char* buff, int size, void*) -> int {
                if (!DisplayTimeZone::instance().format(static_cast<int64_t>(value),
                        TimeZoneFormat::MonthDay, buff, static_cast<size_t>(size))) {
                    return snprintf(buff, size, "--/--");
                }
                return static_cast<int>(std::strlen(buff));
            }, nullptr);
        }

        // Keep the viewport between 8 and 1,440 candles. When it is already
        // outside those bounds, force the repaired range for this frame.
        const double min_x_span = kMinVisibleCandles * timeframe_ms;
        const double max_x_span = kMaxVisibleCandles * timeframe_ms;
        bool zoom_clamped = false;
        if (!ctx_.candle_mgr().follow_live() && chart_type_ != ChartType::TPO) {
            const double span = last_visible_range_.X.Max - last_visible_range_.X.Min;
            if (span > 0.0 && (span < min_x_span || span > max_x_span)) {
                const double clamped_span = std::clamp(span, min_x_span, max_x_span);
                const double center = (last_visible_range_.X.Min + last_visible_range_.X.Max) * 0.5;
                last_visible_range_.X.Min = center - clamped_span * 0.5;
                last_visible_range_.X.Max = center + clamped_span * 0.5;
                zoom_clamped = true;
            }
        }

        // TPO zoom-out: apply for multiple frames to ensure ImPlot processes
        if (tpo_zoom_pending_ && chart_type_ == ChartType::TPO && !timestamps.empty()) {
            double latest = timestamps.back();
            double span_ms = 5.0 * 24.0 * 3600.0 * 1000.0;
            double tpo_x_min = latest - span_ms;
            double tpo_x_max = latest + span_ms * 0.05;
            ImPlot::SetupAxisLimits(ImAxis_X1, tpo_x_min, tpo_x_max, ImPlotCond_Always);
            last_visible_range_.X.Min = tpo_x_min;
            last_visible_range_.X.Max = tpo_x_max;
            if (--tpo_zoom_frames_ <= 0) {
                tpo_zoom_pending_ = false;
            }
        } else if (chart_type_ == ChartType::TPO) {
            // TPO mode: don't follow live - let user control zoom freely
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
        } else if (ctx_.candle_mgr().follow_live() || zoom_clamped) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                zoom_clamped ? last_visible_range_.X.Min : x_min,
                zoom_clamped ? last_visible_range_.X.Max : x_max,
                ImPlotCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
        }
        // Y-axis from VISIBLE candles only.
        // timestamps are sorted ascending → binary-search the visible index
        // range instead of scanning ALL loaded candles (up to 200k after
        // scroll-back) every frame. FPS item, 2026-07-05.
        const bool use_follow_range = ctx_.candle_mgr().follow_live() &&
                                      chart_type_ != ChartType::TPO;
        const double visible_x_min = use_follow_range || last_visible_range_.X.Min <= 0
            ? x_min : last_visible_range_.X.Min;
        const double visible_x_max = use_follow_range || last_visible_range_.X.Max <= 0
            ? x_max : last_visible_range_.X.Max;
        double y_min = std::numeric_limits<double>::infinity();
        double y_max = -std::numeric_limits<double>::infinity();
        const auto& lows = ctx_.candle_mgr().lows();
        const auto& highs = ctx_.candle_mgr().highs();
        {
            ProfileScope _ps("YLimits");
            const auto lo_it = std::lower_bound(timestamps.begin(), timestamps.end(), visible_x_min);
            const auto hi_it = std::upper_bound(lo_it, timestamps.end(), visible_x_max);
            const size_t i0 = static_cast<size_t>(lo_it - timestamps.begin());
            const size_t i1 = static_cast<size_t>(hi_it - timestamps.begin());
            for (size_t i = i0; i < i1; i++) {
                y_min = std::min(y_min, lows[i]);
                y_max = std::max(y_max, highs[i]);
            }
        }
        // Include building candle if visible
        if (ctx_.candle_mgr().has_building_candle()) {
            const auto& bc = ctx_.candle_mgr().building_candle();
            if (bc.timestamp_ms >= visible_x_min && bc.timestamp_ms <= visible_x_max) {
                y_min = std::min(y_min, bc.low);
                y_max = std::max(y_max, bc.high);
            }
        }
        // Heikin Ashi: frame the DRAWN HA candles, not the real ones - HA bodies
        // sit inside the real range, so real extents would leave dead space (or,
        // for the building HA candle, clip). Recompute purely from HA extents;
        // if HA has no data in range this leaves y at infinity and the fallback
        // below covers it. Real-candle modes are untouched.
        if (chart_type_ == ChartType::HeikinAshi) {
            y_min = std::numeric_limits<double>::infinity();
            y_max = -std::numeric_limits<double>::infinity();
            ha_visible_extents(visible_x_min, visible_x_max, y_min, y_max);
        }
        // Scrub preview: the ghost candles ahead of the playhead must join the
        // y-fit, or the very move being previewed clips off-screen. Scans the
        // preview store over [playhead, right edge]; right edge is this frame's
        // extended x_max under follow-live, the user's own range otherwise.
        if (scrub_preview_ms > 0) {
            if (const auto* pv = ctx_.replay_mgr().preview_candles();
                pv && pv->ready()) {
                const double ghost_from =
                    static_cast<double>(ctx_.replay_mgr().interpolated_time_ms());
                const double ghost_to =
                    ctx_.candle_mgr().follow_live() ? x_max : visible_x_max;
                double plo = 0.0, phi = 0.0;
                if (ghost_to > ghost_from &&
                    pv->minmax_in_range(ghost_from, ghost_to, plo, phi)) {
                    y_min = std::min(y_min, plo);
                    y_max = std::max(y_max, phi);
                }
            }
        }
        // Fallback
        if (y_min == std::numeric_limits<double>::infinity()) {
            const size_t candle_count = std::min({timestamps.size(), lows.size(), highs.size()});
            if (candle_count > 0) {
                // A scroll can briefly outrun the history request. Fit a small
                // neighbourhood around the nearest loaded candle instead of the
                // entire store, which made one old spike flatten the current view.
                const double center = (visible_x_min + visible_x_max) * 0.5;
                const auto nearest = std::lower_bound(
                    timestamps.begin(), timestamps.begin() + candle_count, center);
                size_t pivot = static_cast<size_t>(nearest - timestamps.begin());
                if (pivot == candle_count) pivot = candle_count - 1;
                constexpr size_t kFallbackRadius = 8;
                const size_t i0 = pivot > kFallbackRadius ? pivot - kFallbackRadius : 0;
                const size_t i1 = std::min(candle_count, pivot + kFallbackRadius + 1);
                for (size_t i = i0; i < i1; ++i) {
                    y_min = std::min(y_min, lows[i]);
                    y_max = std::max(y_max, highs[i]);
                }
            } else {
                // No candles at all yet: anchor a thin band around the best known
                // price so a fresh/volatile symbol never shows a degenerate 0..1
                // axis on RT load. Building close, else last close, else newest tick.
                double ref = ctx_.candle_mgr().last_close_price();
                if (ref <= 0.0 && ctx_.candle_mgr().has_building_candle())
                    ref = ctx_.candle_mgr().building_candle().close;
                if (ref <= 0.0 && !ctx_.candle_mgr().tick_prices().empty())
                    ref = ctx_.candle_mgr().tick_prices().back();
                if (ref > 0.0) { y_min = ref * 0.995; y_max = ref * 1.005; }
                else { y_min = 0; y_max = 1; }
            }
        }
        const double y_span = y_max - y_min;
        const double y_padding = y_span > 0.0
            ? y_span * 0.08
            : std::max(std::abs(y_min) * 0.001, tick_size_ * 4.0);
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min - y_padding, y_max + y_padding,
                                ImPlotCond_Always);
        if (!hide_x_labels) {
            // In TPO mode, always use last_visible_range_ for tick generation
            // to avoid stale visible_x_min/max on initial frames
            if (chart_type_ == ChartType::TPO && last_visible_range_.X.Min > 0) {
                setup_time_axis_ticks(last_visible_range_.X.Min, last_visible_range_.X.Max);
            } else {
                setup_time_axis_ticks(visible_x_min, visible_x_max);
            }
        }
        stored_x_min_ = visible_x_min;
        stored_x_max_ = visible_x_max;

        // Publish the chart's coordinate system for the lesson spotlight overlay.
        // GetPlotPos/GetPlotSize give the plot-area rect that PlotToPixels maps
        // into; ranges match the axes set above. Read later in the frame by the
        // foreground-drawlist lesson overlay (linear projection, axes are linear).
        {
            const ImVec2 ppos = ImPlot::GetPlotPos();
            const ImVec2 psz  = ImPlot::GetPlotSize();
            edu::ChartProjection& cp = edu::chart_projection();
            cp.plot_min = ppos;
            cp.plot_max = ImVec2(ppos.x + psz.x, ppos.y + psz.y);
            cp.x_min_ms = visible_x_min;
            cp.x_max_ms = visible_x_max;
            cp.y_min = y_min - y_padding;
            cp.y_max = y_max + y_padding;
            cp.valid = true;
        }
        // ── Render layers (back to front)
        // 1. Heatmap (background)
        // Determine replay cutoff for heatmap rendering
        const int64_t heatmap_replay_cutoff = ctx_.replay_mgr().is_active()
            ? ctx_.replay_mgr().interpolated_time_ms() : 0;

        if (heatmap_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("OBHeat");
            auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
            if (reconstructor && reconstructor->has_data()) {
                reconstructor->set_time_offset_ms(heatmap_time_offset_ms_);

                // Viewport-adaptive bucket multiplier: ensure each heatmap cell
                // is at least min_cell_px pixels tall. This adapts to any coin at
                // any zoom level - BTC on 1m stays at native resolution, LABUSDT
                // on 4h (300%+ range) aggregates into visible bands automatically.
                // The UHD/HD/SD combo controls min_cell_px (detail preference).
                const double native_bucket = reconstructor->get_native_bucket_size();
                if (native_bucket > 0) {
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

                    // Hysteresis: only update if significantly different
                    const int current = reconstructor->get_bucket_multiplier();
                    if (best != current &&
                        (best > current * 1.3 || best < current * 0.7 || current <= 1)) {
                        reconstructor->set_bucket_multiplier(best);
                    }
                } else {
                    reconstructor->set_bucket_multiplier(heatmap_bucket_multiplier_);
                }

                reconstructor->set_replay_cutoff_ms(heatmap_replay_cutoff);
                const int64_t candle_ms = tf_sec * 1000;
                const double visible_span = visible_x_max - visible_x_min;
                const double vis_candles = visible_span / static_cast<double>(candle_ms);
                const bool show_labels = (vis_candles < 45);
                reconstructor->render_cells(candle_ms, heatmap_sensitivity_, show_labels);
            }
            render_heatmap_tooltip();
        }
        // 1.5 Liquidation timeline heatmap (the predictive shader map) -- ENABLED 2026-06-27.
        //     Re-enabled on the L1 footprint-located estimator field (peaky + persistent on the
        //     scoreboard + offline render). Per-level brightness is the per-tier USD with a
        //     sqrt→pow(0.6)→decaying-max contrast curve + viewport-adaptive Low/Peak
        //     (convert_to_heatmap_snapshot / render_liq_timeline); GL_NEAREST + extend-levels give
        //     the crisp MMT-style streaks. The discrete rails draw on top as the decision layer.
        // 1.55 Liq Heatmap = dense candle×leverage projection Field (flow-weighted, MMT-style).
        //      Background layer: drawn BEFORE rails + candles so it's the predictive-map backdrop.
        //      Client-computed from CandleManager → universal + deterministic on every symbol/TF.
        if (liq_dense_field_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqField");
            render_liq_dense_field();
        }
        // 1.6 Liq Levels - discrete OI/positioning liquidation levels (rails), an
        //     independent layer on top of the Field.
        if (liq_heatmap_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqRails");
            render_liquidation_heatmap(visible_x_min, visible_x_max);
        }
        // 1.62 Liq Levels HL - REAL predictive liq levels (HL census, P2e). Drawn
        //      over the modelled layers: ground truth outranks the model. Underlying-
        //      keyed, so it also overlays non-HL charts of the same underlying.
        if (liq_census_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqCensus");
            render_liq_census();
        }
        // 1.65 Liq Profile - smoothed price-marginal of the Field; independent
        //      toggle, candle-derived (needs no liq subscription or snapshot).
        if (liq_profile_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqProf");
            render_liq_profile();
        }
        // 1.7 VPVR profile overlay (sidebar histogram bars)
        if (vpvr_enabled_) {
            // Request profile data when visible range changes
            const int64_t tf_ms = static_cast<int64_t>(tf_sec) * 1000;
            const int64_t start_ms = static_cast<int64_t>(visible_x_min);
            const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms;
            ctx_.vpvr_mgr().request_profile(
                pair_.symbol, start_ms, end_ms,
                compute_vpvr_tick_per_row(
                    ImPlot::GetPlotLimits().Y.Max - ImPlot::GetPlotLimits().Y.Min,
                    ImPlot::GetPlotSize().y),
                &ctx_.stream_mgr());
            render_vpvr_profile();
        }
        // 2. Foreground price geometry - dispatch over ChartType.
        //    Candles / Footprint draw the REAL OHLC arrays; Heikin Ashi draws the
        //    derived HA arrays through the same plot_candles tiers; Line strokes a
        //    close-price polyline; TPO renders market-profile columns.
        {
            ProfileScope _ps("Candles");
            // Real building-candle OHLC (used by Candles / FP).
            BuildingOHLC bld{0, 0, 0, 0, false};
            if (ctx_.candle_mgr().has_building_candle()) {
                const auto& bc = ctx_.candle_mgr().building_candle();
                bld = {bc.open, bc.high, bc.low, bc.close, true};
            }
            switch (chart_type_) {
                case ChartType::TPO:
                    render_tpo(visible_x_min, visible_x_max);
                    break;
                case ChartType::Line:
                    plot_line(visible_x_min, visible_x_max);
                    break;
                case ChartType::HeikinAshi: {
                    // Heikin Ashi: same plot_candles tiers, HA arrays as the source.
                    // The building HA candle is derived per frame from the last
                    // finalized HA pair (falls back to the seed when none exists).
                    const auto& ha_o = ctx_.candle_mgr().ha_opens();
                    const auto& ha_c = ctx_.candle_mgr().ha_closes();
                    BuildingOHLC ha_bld{0, 0, 0, 0, false};
                    if (ctx_.candle_mgr().has_building_candle()) {
                        const auto& bc = ctx_.candle_mgr().building_candle();
                        const double hb_close = (bc.open + bc.high + bc.low + bc.close) * 0.25;
                        const double hb_open  = (!ha_o.empty())
                            ? (ha_o.back() + ha_c.back()) * 0.5
                            : (bc.open + bc.close) * 0.5;
                        const double hb_high = std::max({bc.high, hb_open, hb_close});
                        const double hb_low  = std::min({bc.low,  hb_open, hb_close});
                        ha_bld = {hb_open, hb_high, hb_low, hb_close, true};
                    }
                    plot_candles(visible_x_min, visible_x_max,
                                 ctx_.candle_mgr().ha_opens(), ctx_.candle_mgr().ha_highs(),
                                 ctx_.candle_mgr().ha_lows(),  ctx_.candle_mgr().ha_closes(),
                                 ha_bld);
                    break;
                }
                case ChartType::Candles:
                case ChartType::FootprintCluster:
                case ChartType::FootprintProfile:
                case ChartType::Renko:
                default:
                    plot_candles(visible_x_min, visible_x_max,
                                 ctx_.candle_mgr().opens(), ctx_.candle_mgr().highs(),
                                 ctx_.candle_mgr().lows(),  ctx_.candle_mgr().closes(), bld);
                    break;
            }
        }
        // 2.05 Ghost scrub-preview candles (replay only, while the user aims a
        //      scrub on the transport). Drawn over the real candles, under the
        //      Observed markers; reads ONLY the PreviewCandleStore. Skipped for
        //      Line (polyline ghost is a follow-up) and TPO/Renko (no time axis).
        if (scrub_preview_ms > 0 && chart_type_ != ChartType::Line &&
            ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("ScrubPreview");
            render_scrub_preview(visible_x_min, std::max(visible_x_max, x_max));
        }
        // 2.1 Observed - real @forceOrder liquidation markers (WS4). Drawn OVER the
        //     candles: liquidations fire at traded prices, so an under-candle layer
        //     would be occluded by the very candles that consumed them.
        if (liq_observed_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqObs");
            render_liq_observed();
        }
        draw_ohlc_readout();
        // v2 3b: the LIVE status pill is redundant (top-bar Live toggle + status bar).
        // Keep the chip only in replay, where it shows the playback position.
        // No status chip while the clip recorder drives: it would burn the
        // CAPTURE speed (pre-retime) and the wall-position clock into the
        // produced video (CLIP_FACTORY P2).
        if (ctx_.replay_mgr().is_active() && !edu::RecorderRuntime::instance().active())
            draw_status_chip();
        // 2.5 Footprint overlay (per-candle volume grid / profile sidebar)
        if (ctx_.footprint_mgr().enabled) {
            ProfileScope _ps("Footprint");
            if (ctx_.footprint_mgr().mode == FootprintManager::Mode::Profile) {
                render_footprint_profile(visible_x_min, visible_x_max);
            } else {
                render_footprint_overlay(visible_x_min, visible_x_max);
            }
        }
        // 3. Admin pattern context. Live-only and already gated at subscription.
        if (!ctx_.replay_mgr().is_active() && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("Patterns");
            render_pattern_overlay(visible_x_min, visible_x_max);
        }
        // 3.5 Current-price axis tag (green if current candle bullish, red if bearish).
        //     Uses the REAL candle close (never HA) so the tag tracks true price in
        //     every non-TPO mode.
        if (chart_type_ != ChartType::TPO) {
            double cur_price = 0.0;
            bool have_price = false, bullish = true;
            if (ctx_.candle_mgr().has_building_candle()) {
                const auto& bc = ctx_.candle_mgr().building_candle();
                cur_price = bc.close;
                bullish = bc.close >= bc.open;
                have_price = true;
            } else if (!ctx_.candle_mgr().closes().empty()) {
                const auto& cl = ctx_.candle_mgr().closes();
                const auto& op = ctx_.candle_mgr().opens();
                cur_price = cl.back();
                bullish = cl.back() >= op.back();
                have_price = true;
            }
            if (have_price) {
                char price_buf[32];
                fmt_.format_price(price_buf, sizeof(price_buf), cur_price);
                const ImU32 tag_col = bullish ? Theme::get_buy_color_u32(255)
                                              : Theme::get_sell_color_u32(255);
                CustomImPlot::DrawAxisValueTag(cur_price, price_buf, tag_col, true);
            }
        }
        // Drawing tools: placement/hit-test/drag + render, and the input-map
        // override for this frame. Runs BEFORE handle_plot_interaction so
        // captures_mouse() gates the chart's own handlers the same frame.
        drawing_layer_.render_in_plot(ctx_, fmt_,
                                      ct_allows_time_overlays(chart_type_),
                                      tf_sec);
        // 4. Interaction
        handle_plot_interaction();
        // Update viewport state for next frame
        last_visible_range_ = ImPlot::GetPlotLimits();
        x_axis_min_ = last_visible_range_.X.Min;
        x_axis_max_ = last_visible_range_.X.Max;
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}


// ═══════════════════════════════════════════════════════════════════════════════
// Renko (ChartType::Renko) - price-driven bricks on a brick-index X-axis.
// Self-contained render path; the time-axis render_chart() is untouched.
// See core/renko_builder.h.
// ═══════════════════════════════════════════════════════════════════════════════

// Resolved brick size in price units. Manual override wins; otherwise the auto
// size resolves ONCE from the latest price and freezes (recomputing per frame as
// price moves would repaint the whole brick sequence - the plan's determinism /
// "store the resolved size" requirement).
double ChartWidget::renko_resolved_size() {
    if (renko_brick_ticks_ > 0) {                       // manual override
        renko_resolved_ticks_ = renko_brick_ticks_;
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    }
    if (renko_resolved_ticks_ > 0)                       // auto, already frozen
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    // Auto = volatility-adaptive ATR(14) of the loaded candles - the TradingView /
    // Binance "Renko [ATR(14), x]" default. Resolved ONCE and frozen so it stays
    // replay-stable (the deferred-ATR concern was CONTINUOUS recompute/repaint;
    // freezing avoids that). A fixed %-of-price default was ~6x too small on a
    // volatile symbol, which is what produced the wall of tiny, noisy bricks.
    const double atr = renko_atr(14);
    if (atr > 0.0) {
        renko_resolved_ticks_ = std::max(1, static_cast<int>(std::llround(atr / tick_size_)));
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    }
    // ATR not ready yet (too few candles) → temporary %-of-price size, NOT frozen,
    // so it re-resolves to ATR next frame once candles have loaded.
    double ref = ctx_.candle_mgr().last_close_price();
    if (ref <= 0.0 && !ctx_.candle_mgr().closes().empty())
        ref = ctx_.candle_mgr().closes().back();
    const int tmp = RenkoBuilder::auto_brick_ticks(ref, tick_size_);
    return static_cast<double>(std::max(1, tmp)) * tick_size_;
}

// Average True Range over the most recent `period` CLOSED candles (price units).
// 0 when there are too few candles.
double ChartWidget::renko_atr(int period) const {
    const auto& highs  = ctx_.candle_mgr().highs();
    const auto& lows   = ctx_.candle_mgr().lows();
    const auto& closes = ctx_.candle_mgr().closes();
    const size_t n = std::min({highs.size(), lows.size(), closes.size()});
    if (period < 1 || n < static_cast<size_t>(period) + 1) return 0.0;
    double sum = 0.0;
    for (size_t i = n - static_cast<size_t>(period); i < n; ++i) {
        const double tr = std::max({highs[i] - lows[i],
                                    std::fabs(highs[i] - closes[i - 1]),
                                    std::fabs(lows[i]  - closes[i - 1])});
        sum += tr;
    }
    return sum / static_cast<double>(period);
}

// Rebuild the committed bricks only when the closed-candle signature / brick
// size / timeframe change, then fold the building candle in per frame.
void ChartWidget::ensure_renko() {
    const double bsize = renko_resolved_size();
    const auto& closes = ctx_.candle_mgr().closes();
    const auto& ts     = ctx_.candle_mgr().timestamps();
    const int64_t tf   = ctx_.candle_mgr().timeframe_seconds();
    const size_t  n    = std::min(closes.size(), ts.size());
    // Key on the last CLOSED candle's TIMESTAMP, never its close. The client
    // mutates a closed candle's close during replay (finalize-replace, late
    // trades, 15s-tick merges); keying on close would re-run build() and - with
    // the append-only builder ignoring the tail anyway - do nothing useful, while
    // keying on close in a full-rebuild world was what un-printed bricks. The
    // timestamp only changes when a genuinely new candle closes (append) or the
    // set is reloaded (seek / timeframe), which is exactly when we must fold.
    const int64_t last_t = n ? static_cast<int64_t>(ts[n - 1]) : 0;

    const bool dirty = (n != renko_sig_count_) || (last_t != renko_sig_time_) ||
                       (bsize != renko_sig_size_) || (tf != renko_sig_tf_);
    if (dirty && bsize > 0.0) {
        // close_times parallel to closes (both from the same candle deque). The
        // rebuild is infrequent (candle finalize / scroll-load), so the transient
        // int64 copy of the timestamps is fine. build() is append-only - it folds
        // only the candles newer than the ones already consumed.
        static thread_local std::vector<int64_t> times;
        times.clear();
        times.reserve(n);
        for (size_t i = 0; i < n; ++i) times.push_back(static_cast<int64_t>(ts[i]));
        renko_.build(closes, times, bsize);
        renko_sig_count_ = n;
        renko_sig_time_  = last_t;
        renko_sig_size_  = bsize;
        renko_sig_tf_    = tf;
    }

    // Live leading edge: fold the building (unfinished) candle into a SEPARATE
    // provisional list. These bricks repaint as live price moves - a retrace can
    // remove the rightmost one, then it re-forms. That IS correct Renko: Binance's
    // live Renko (and others) repaint the forming brick the same way. They render
    // SOLID (see plot_renko), identical to committed bricks - the only earlier
    // mistake was drawing them hollow. Crucially they live in building_, NOT
    // bricks_, so a repaint here can never touch a PUBLISHED (closed-candle) brick;
    // those stay append-only and permanent. On candle close they hand off to
    // committed. fold_building() clears the list first, so with no building candle
    // (passing the last committed close) it simply stays empty.
    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        renko_.fold_building(bc.close, bc.timestamp_ms);
    } else {
        renko_.fold_building(n > 0 ? closes[n - 1] : 0.0,
                             n > 0 ? static_cast<int64_t>(ts[n - 1]) : 0);
    }
}

// Filled brick rectangles over the visible brick-index slice [i0, i1).
void ChartWidget::plot_renko(size_t i0, size_t i1) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const size_t n_total = renko_.total_count();
    if (n_total == 0) return;
    i1 = std::min(i1, n_total);
    if (i0 >= i1) return;

    // Pixels per brick-index unit → adaptive tier. Zoomed out, a per-brick gap +
    // sub-pixel width render bricks as dotted slivers; the compressed tier drops
    // the gap and floors each brick at 1px so the staircase stays a solid band.
    const double px_per_brick = ImPlot::PlotToPixels(1.0, 0.0).x
                              - ImPlot::PlotToPixels(0.0, 0.0).x;
    const bool compressed = px_per_brick < 4.0;
    const double kGap = compressed ? 0.0 : 0.08;

    ImPlot::PushPlotClipRect();
    // All bricks draw SOLID - committed (closed-candle) AND the live provisional
    // ones (building candle, indices >= renko_.bricks().size()). No hollow: the
    // live leading edge looks identical to the rest; it just repaints as price
    // moves (correct Renko), while committed bricks stay permanent (append-only).
    for (size_t i = i0; i < i1; ++i) {
        const RenkoBrick& b = renko_.at(i);
        const double xl = static_cast<double>(i) + kGap;
        const double xr = static_cast<double>(i) + 1.0 - kGap;
        const double y_top = std::max(b.open, b.close);
        const double y_bot = std::min(b.open, b.close);
        const ImVec2 p_tl = ImPlot::PlotToPixels(xl, y_top);  // higher price → smaller y
        const ImVec2 p_br = ImPlot::PlotToPixels(xr, y_bot);
        float top_y = p_tl.y, bot_y = p_br.y;
        if (bot_y - top_y < 1.0f) bot_y = top_y + 1.0f;  // never let a brick vanish (thin)
        float left_x = p_tl.x, right_x = p_br.x;
        if (right_x - left_x < 1.0f) right_x = left_x + 1.0f;  // …or (narrow) at zoom-out
        const ImU32 fill = b.up ? Theme::get_buy_color_u32(255)
                                : Theme::get_sell_color_u32(255);
        dl->AddRectFilled(ImVec2(left_x, top_y), ImVec2(right_x, bot_y), fill);
    }
    ImPlot::PopPlotClipRect();
}

// X ticks EVENLY spaced by brick index (the TradingView Renko look): uniform
// on-screen spacing, each label showing that brick's actual close time. Renko
// time is non-uniform, so the labels are NOT round clock values - but the SPACING
// is consistent, which is what matters (the earlier wall-clock scheme gave clean
// times but wildly uneven gaps, since a quiet stretch packs few bricks into a
// long time and a busy one packs many into a short time).
void ChartWidget::setup_renko_axis_ticks(double vis_i_min, double vis_i_max) const {
    const size_t n_total = renko_.total_count();
    if (n_total == 0) return;
    size_t i0 = (vis_i_min <= 0.0) ? 0 : std::min(n_total - 1, static_cast<size_t>(std::floor(vis_i_min)));
    size_t i1 = (vis_i_max <= 0.0) ? n_total : std::min(n_total, static_cast<size_t>(std::ceil(vis_i_max)));
    if (i1 <= i0) return;
    const size_t span = i1 - i0;

    // Stride by a round number of BRICKS → ~8 evenly-spaced labels.
    size_t stride = std::max<size_t>(1, span / 8);
    static constexpr size_t nice[] = {1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 30, 40,
                                      50, 75, 100, 150, 200, 300, 500, 1000, 2000};
    for (size_t v : nice) { if (v >= stride) { stride = v; break; } }

    renko_tick_pos_.clear();
    renko_tick_lbls_.clear();
    renko_tick_ptrs_.clear();
    size_t start = ((i0 + stride - 1) / stride) * stride;  // first multiple of stride >= i0
    char prev_day[16]{};
    for (size_t i = start; i < i1; i += stride) {
        const RenkoBrick& b = renko_.at(i);
        renko_tick_pos_.push_back(static_cast<double>(i) + 0.5);
        char day[16]{};
        DisplayTimeZone::instance().format(b.close_time_ms, TimeZoneFormat::DateOnly,
                                            day, sizeof(day));
        char lbl[32];
        const bool day_changed = prev_day[0] && std::strcmp(day, prev_day) != 0;
        DisplayTimeZone::instance().format(b.close_time_ms,
            (day_changed || !prev_day[0]) ? TimeZoneFormat::MonthDayTime
                                           : TimeZoneFormat::TimeMinutes,
            lbl, sizeof(lbl));
        std::snprintf(prev_day, sizeof(prev_day), "%s", day);
        renko_tick_lbls_.emplace_back(lbl);
    }
    // Second pass: storage is final → c_str() pointers are stable.
    for (const auto& s : renko_tick_lbls_) renko_tick_ptrs_.push_back(s.c_str());
    if (!renko_tick_pos_.empty())
        ImPlot::SetupAxisTicks(ImAxis_X1, renko_tick_pos_.data(),
            static_cast<int>(renko_tick_pos_.size()), renko_tick_ptrs_.data(), false);
}

void ChartWidget::render_chart_renko() {
    const bool follow = ctx_.candle_mgr().follow_live();

    // ── Scroll-load view stability (capture BEFORE ensure_renko may rebuild) ──
    // A historical prepend shifts every brick index right; anchor the view to the
    // left-edge brick's TIME so we can shift the axis back afterwards and keep the
    // same bricks on screen (otherwise the view jumps + the loader keeps re-firing).
    int64_t anchor_time = 0;
    double  anchor_frac = 0.0;
    if (!follow && renko_.total_count() > 0) {
        const double xm = last_visible_range_.X.Min;
        size_t ai; double frac;
        if (xm <= 0.0) { ai = 0; frac = xm; }  // at / left-of the oldest brick
        else {
            ai   = std::min(renko_.total_count() - 1, static_cast<size_t>(std::floor(xm)));
            frac = xm - std::floor(xm);
        }
        anchor_time = renko_.at(ai).close_time_ms;
        anchor_frac = frac;
    }

    ensure_renko();
    const size_t n_total = renko_.total_count();
    const double bsize   = renko_.brick_size();

    // brick-index X cannot be time-projected → mark the lesson/studio projection
    // invalid so the spotlight + capture never misproject in Renko (v1).
    edu::chart_projection().valid = false;

    if (n_total == 0 || bsize <= 0.0) {
        ImGui::Text("Renko: waiting for candle data...");
        return;
    }

    // Re-anchor after a possible prepend: find the anchored brick's NEW index and
    // shift the stored view by the delta so it lands back on the same bricks.
    bool renko_reanchor = false;
    if (anchor_time > 0 && !follow) {
        const auto& br = renko_.bricks();
        size_t lo = 0, hi = br.size();  // first committed brick with time >= anchor_time
        while (lo < hi) { const size_t mid = (lo + hi) / 2;
            if (br[mid].close_time_ms < anchor_time) lo = mid + 1; else hi = mid; }
        const size_t new_ai = br.empty() ? 0 : std::min(lo, br.size() - 1);
        const double delta = (static_cast<double>(new_ai) + anchor_frac) - last_visible_range_.X.Min;
        if (delta > 0.5) {  // bricks were prepended → shift the view right to match
            last_visible_range_.X.Min += delta;
            last_visible_range_.X.Max += delta;
            renko_reanchor = true;
        }
    }

    // ── X range: brick-index domain; follow-live pins the newest brick.
    constexpr double kRenkoVisibleBricks = 140.0;
    const double x_pad = 2.0;
    const double x_max = static_cast<double>(n_total) + x_pad;
    double x_min = 0.0;
    if (follow && static_cast<double>(n_total) > kRenkoVisibleBricks)
        x_min = static_cast<double>(n_total) - kRenkoVisibleBricks;

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10, 10));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 1.0f);
    if (ImPlot::BeginPlot("##Price", ImVec2(-1, chart_allocated_height_),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisFormat(ImAxis_Y1, fmt_.price_fmt);

        // Min zoom: never collapse below ~8 bricks.
        const double min_x_span = 8.0;
        bool zoom_clamped = false;
        if (!follow && last_visible_range_.X.Max - last_visible_range_.X.Min > 0 &&
            last_visible_range_.X.Max - last_visible_range_.X.Min < min_x_span) {
            const double center = (last_visible_range_.X.Min + last_visible_range_.X.Max) * 0.5;
            last_visible_range_.X.Min = center - min_x_span * 0.5;
            last_visible_range_.X.Max = center + min_x_span * 0.5;
            zoom_clamped = true;
        }
        if (renko_reanchor) {
            // Prepend just happened - force the shifted range so the view stays on
            // the same bricks (one frame; ImPlot keeps it afterwards under Once).
            ImPlot::SetupAxisLimits(ImAxis_X1, last_visible_range_.X.Min,
                                    last_visible_range_.X.Max, ImPlotCond_Always);
        } else if (follow || zoom_clamped) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                zoom_clamped ? last_visible_range_.X.Min : x_min,
                zoom_clamped ? last_visible_range_.X.Max : x_max,
                ImPlotCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
        }

        // Visible brick-index slice.
        const double vis_i_min = last_visible_range_.X.Min > 0 ? last_visible_range_.X.Min : x_min;
        const double vis_i_max = last_visible_range_.X.Max > 0 ? last_visible_range_.X.Max : x_max;
        size_t i0 = (vis_i_min <= 0.0) ? 0 : std::min(n_total, static_cast<size_t>(std::floor(vis_i_min)));
        size_t i1 = (vis_i_max <= 0.0) ? n_total : std::min(n_total, static_cast<size_t>(std::ceil(vis_i_max)) + 1);
        if (i1 < i0) i1 = i0;

        // Live price (REAL close, never a brick level) - drives the forming brick,
        // the current-price tag, AND the Y-limits (so the forming tip never clips).
        double cur_price = 0.0; bool have_price = false, bullish = true;
        if (ctx_.candle_mgr().has_building_candle()) {
            const auto& bc = ctx_.candle_mgr().building_candle();
            cur_price = bc.close; bullish = bc.close >= bc.open; have_price = true;
        } else if (!ctx_.candle_mgr().closes().empty()) {
            const auto& cl = ctx_.candle_mgr().closes();
            const auto& op = ctx_.candle_mgr().opens();
            cur_price = cl.back(); bullish = cl.back() >= op.back(); have_price = true;
        }

        // Y-limits from the visible bricks' price extents (fallback: whole series).
        double y_min = std::numeric_limits<double>::infinity();
        double y_max = -std::numeric_limits<double>::infinity();
        for (size_t i = i0; i < i1; ++i) {
            const RenkoBrick& b = renko_.at(i);
            y_min = std::min(y_min, std::min(b.open, b.close));
            y_max = std::max(y_max, std::max(b.open, b.close));
        }
        if (!std::isfinite(y_min)) {
            for (size_t i = 0; i < n_total; ++i) {
                const RenkoBrick& b = renko_.at(i);
                y_min = std::min(y_min, std::min(b.open, b.close));
                y_max = std::max(y_max, std::max(b.open, b.close));
            }
            if (!std::isfinite(y_min)) { y_min = 0.0; y_max = 1.0; }
        }
        if (have_price) { y_min = std::min(y_min, cur_price); y_max = std::max(y_max, cur_price); }
        const double y_pad = (y_max - y_min) * 0.08 + bsize * 0.5;
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min - y_pad, y_max + y_pad, ImPlotCond_Always);

        setup_renko_axis_ticks(vis_i_min, vis_i_max);

        // Publish the visible bricks' TIME window so update() feeds CandleManager
        // a time range (never brick indices) for scroll-load / history.
        if (i1 > i0) {
            // Left-edge brick's TIME drives the historical loader (its threshold is
            // candle-time based). When scrolled to the oldest bricks this sits near
            // the oldest candle, so one batch loads; the re-anchor above then keeps
            // the view on the same bricks, moving the left edge far enough from the
            // (now much older) oldest candle that the loader stops - one batch per
            // scroll gesture, exactly like the candle chart. NO oldest-candle
            // override (that reported "at the edge" forever → back-loaded one tiny
            // batch per cooldown, thrashing the view + ticks).
            renko_view_t0_ms_ = renko_.at(i0).close_time_ms;
            renko_view_t1_ms_ = renko_.at(i1 - 1).close_time_ms;
        }

        plot_renko(i0, i1);

        // Current-price axis tag.
        if (have_price) {
            char pb[32]; fmt_.format_price(pb, sizeof(pb), cur_price);
            const ImU32 tc = bullish ? Theme::get_buy_color_u32(255)
                                     : Theme::get_sell_color_u32(255);
            CustomImPlot::DrawAxisValueTag(cur_price, pb, tc, true);
        }

        // Minimal Renko readout (top-left): brick size in price + resolved ticks.
        {
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImVec2 pos = ImPlot::GetPlotPos(); pos.x += 10.0f; pos.y += 8.0f;
            const ImU32 lbl = Theme::u32(Theme::Tokens::TX4);
            const ImU32 val = Theme::u32(Theme::Tokens::TX1);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            auto field = [&](const char* label, const char* v) {
                dl->AddText(pos, lbl, label);
                pos.x += ImGui::CalcTextSize(label).x + 4.0f;
                dl->AddText(pos, val, v);
                pos.x += ImGui::CalcTextSize(v).x + 12.0f;
            };
            char bs[32]; fmt_.format_price(bs, sizeof(bs), bsize);
            char tk[48];
            if (renko_brick_ticks_ == 0)
                snprintf(tk, sizeof(tk), "ATR(14) \xc2\xb7 %d ticks", renko_resolved_ticks_);
            else
                snprintf(tk, sizeof(tk), "%d ticks", renko_resolved_ticks_);
            field("RENKO", bs);
            dl->AddText(pos, lbl, tk);
            pos.x += ImGui::CalcTextSize(tk).x + 10.0f;
            // Gear affordance → opens the brick-size config (also right-clickable
            // on the chart-type dropdown's Renko row).
            const float gr = 6.0f;
            const ImVec2 gc(pos.x + gr, pos.y + ImGui::GetFontSize() * 0.5f);
            const bool gear_hov = ImGui::IsMouseHoveringRect(
                ImVec2(gc.x - gr - 3, gc.y - gr - 3), ImVec2(gc.x + gr + 3, gc.y + gr + 3));
            const ImU32 gcol = gear_hov ? Theme::u32(Theme::Tokens::BRAND)
                                        : Theme::u32(Theme::Tokens::TX3);
            for (int t = 0; t < 8; ++t) {  // cog teeth
                const float ang = static_cast<float>(t) / 8.0f * 6.2831853f;
                dl->AddLine(ImVec2(gc.x + cosf(ang) * gr * 0.68f, gc.y + sinf(ang) * gr * 0.68f),
                            ImVec2(gc.x + cosf(ang) * gr,         gc.y + sinf(ang) * gr), gcol, 1.6f);
            }
            dl->AddCircle(gc, gr * 0.60f, gcol, 12, 1.4f);
            renko_gear_pos_ = ImVec2(gc.x - 30.0f, gc.y + gr + 8.0f);  // popup anchor
            if (gear_hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                open_renko_settings_ = true;
            ImGui::PopFont();
        }

        if (ctx_.replay_mgr().is_active() && !edu::RecorderRuntime::instance().active())
            draw_status_chip();

        // ── Interaction: follow-live latch + price crosshair + REPLAY-FROM.
        //    Renko has no time X-axis, but each brick carries its source candle's
        //    close_time_ms, so "replay from" maps the brick under the cursor back
        //    to a real time and reuses the exact candle-chart entry points (R key
        //    + right-click menu). Measure / alerts / shift-drag range-select stay
        //    off in Renko v1.
        auto brick_time_at = [&](double px) -> int64_t {
            const size_t nt = renko_.total_count();
            if (nt == 0) return 0;
            double xi = std::floor(px);
            if (xi < 0.0) xi = 0.0;
            size_t bi = static_cast<size_t>(xi);
            if (bi >= nt) bi = nt - 1;
            return renko_.at(bi).close_time_ms;
        };
        if (ImPlot::IsPlotHovered()) {
            if (ImGui::GetIO().MouseWheel != 0 ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ctx_.candle_mgr().set_follow_live(false);
            }
            const ImPlotPoint m = ImPlot::GetPlotMousePos();
            // R key → replay from the brick under the cursor → focused view.
            if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                !ImGui::GetIO().WantTextInput && !ctx_.replay_mgr().is_active()) {
                const int64_t t = brick_time_at(m.x);
                if (t > 0)
                    ctx_.replay_mgr().open_focused_replay_at(pair_.symbol, t);
            }
            // Right-click → context menu (replay actions) anchored at the brick time.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                context_menu_time_ms_ = brick_time_at(m.x);
                // Renko bricks carry their source-candle epoch time, so the
                // research minute floors THAT (never the brick index m.x).
                context_menu_minute_ms_ = research_url::floor_minute_ms(context_menu_time_ms_);
                context_menu_price_   = m.y;
                ImGui::OpenPopup("##RenkoCtx");
            }
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            const ImVec2 pmin = ImPlot::GetPlotPos();
            const ImVec2 psz  = ImPlot::GetPlotSize();
            const ImVec2 mp   = ImGui::GetMousePos();
            const ImU32 ch = Theme::u32(Theme::Tokens::TX3, 0.45f);
            dl->AddLine(ImVec2(pmin.x, mp.y), ImVec2(pmin.x + psz.x, mp.y), ch, 1.0f);
            dl->AddLine(ImVec2(mp.x, pmin.y), ImVec2(mp.x, pmin.y + psz.y), ch, 1.0f);
            char pb[32]; fmt_.format_price(pb, sizeof(pb), m.y);
            CustomImPlot::DrawAxisValueTag(m.y, pb, Theme::u32(Theme::Tokens::TX3), true);
            // X-axis TIME tag: the brick under the cursor → its source-candle time
            // (Renko has no time axis, so map via close_time_ms). Matches the candle
            // chart's crosshair time pill; drawn on the foreground list so it shows
            // below the plot rect.
            const int64_t bt = brick_time_at(m.x);
            if (bt > 0) {
                char tl[48]{};
                if (!DisplayTimeZone::instance().format(bt, TimeZoneFormat::DateTimeSeconds,
                                                         tl, sizeof(tl))) {
                    snprintf(tl, sizeof(tl), "?");
                }
                ImDrawList* fdl = ImGui::GetForegroundDrawList();
                const ImVec2 tsz = ImGui::CalcTextSize(tl);
                constexpr float tp = 6.0f;
                const float by = pmin.y + psz.y;
                ImVec2 bmin(mp.x - tsz.x * 0.5f - tp, by + 4.0f);
                ImVec2 bmax(mp.x + tsz.x * 0.5f + tp, by + 4.0f + tsz.y + tp * 2.0f);
                bmin.x = std::max(bmin.x, pmin.x + 2.0f);
                bmax.x = std::min(bmax.x, pmin.x + psz.x - 2.0f);
                fdl->AddRectFilled(bmin, bmax, IM_COL32(45, 45, 48, 240), 2.0f);
                fdl->AddRect(bmin, bmax, IM_COL32(100, 100, 105, 255), 2.0f, 0, 1.0f);
                fdl->AddText(ImVec2((bmin.x + bmax.x - tsz.x) * 0.5f, bmin.y + tp),
                             IM_COL32(230, 230, 235, 255), tl);
            }
        }
        // Renko context menu (rendered every frame so it stays open once opened) -
        // replay actions keyed to the brick's source-candle time, plus copy price.
        // 450 wide (measured): narrower clipped the MenuItem shortcut values
        // the replay datetime, the copy price). The widest label ("Replay
        // range (shift-drag to select)") sets the label column; the replay
        // datetime needs ~120px after it.
        ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 11));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Tokens::ELEV);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX1);
        if (ImGui::BeginPopup("##RenkoCtx")) {
            // Menu chrome is monospace (JetBrains Mono) to match the design (1f).
            ImGui::PushFont(Theme::Fonts::mono_sm());
            // Investigate this minute - same placement rule as the candle
            // menu: the read sits above the replay block.
            render_investigate_menu_item(pair_, context_menu_minute_ms_);
            ImGui::Separator();
            // Replay actions (entitlement-gated inside): Start Replay · <time>,
            // Stop Replay · Esc. No range (shift-drag select is off in Renko).
            ctx_.replay_mgr().render_chart_context_menu(
                pair_.symbol, context_menu_time_ms_,
                ctx_.candle_mgr().timeframe_seconds(), 0, 0);
            ImGui::Separator();
            char pbc[32]; fmt_.format_price(pbc, sizeof(pbc), context_menu_price_);
            if (ImGui::MenuItem("Copy price", pbc)) ImGui::SetClipboardText(pbc);
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(5);

        last_visible_range_ = ImPlot::GetPlotLimits();
        x_axis_min_ = last_visible_range_.X.Min;
        x_axis_max_ = last_visible_range_.X.Max;
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}

// Renko brick-size config (opened by right-clicking the Renko chart-type row).
void ChartWidget::render_renko_settings_popup() {
    if (renko_gear_pos_.x > 0.0f || renko_gear_pos_.y > 0.0f)
        ImGui::SetNextWindowPos(renko_gear_pos_, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("renko_settings")) {
        ImGui::PushFont(Theme::Fonts::ui());
        ImGui::TextUnformatted("Renko brick size");
        ImGui::PopFont();
        ImGui::Spacing();
        bool is_auto = (renko_brick_ticks_ == 0);
        if (ImGui::Checkbox("Auto (ATR-14, per-symbol)", &is_auto)) {
            if (is_auto) { renko_brick_ticks_ = 0; renko_resolved_ticks_ = 0; }
            else         { renko_brick_ticks_ = std::max(1, renko_resolved_ticks_); }
            renko_sig_size_ = -1.0;  // force a committed rebuild next frame
        }
        if (!is_auto) {
            int ticks = renko_brick_ticks_;
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputInt("brick (ticks)", &ticks, 1, 10)) {
                renko_brick_ticks_ = std::max(1, ticks);
                renko_resolved_ticks_ = renko_brick_ticks_;
                renko_sig_size_ = -1.0;
            }
        }
        char pb[32];
        const double sz = static_cast<double>(renko_resolved_ticks_) * tick_size_;
        fmt_.format_price(pb, sizeof(pb), sz);
        ImGui::TextDisabled("Brick = %s  (%d ticks)", pb, renko_resolved_ticks_);
        ImGui::EndPopup();
    }
}


// Candle Drawing - reads SoA from CandleManager
// (replaces both plot_candles_simple and CustomImPlot::PlotCandlestick)

// OHLCV readout - top-left overlay, hovered candle (or latest when idle)
void ChartWidget::draw_ohlc_readout() const {
    const auto& ts = ctx_.candle_mgr().timestamps();
    if (ts.empty()) return;
    const auto& opens  = ctx_.candle_mgr().opens();
    const auto& highs  = ctx_.candle_mgr().highs();
    const auto& lows   = ctx_.candle_mgr().lows();
    const auto& closes = ctx_.candle_mgr().closes();
    const auto& vols   = ctx_.candle_mgr().volumes();

    // hovered candle, else most recent
    size_t idx = ts.size() - 1;
    if (ImPlot::IsPlotHovered()) {
        const double mx = ImPlot::GetPlotMousePos().x;
        const auto it = std::ranges::upper_bound(ts, mx);
        if (it != ts.begin()) {
            idx = static_cast<size_t>(std::distance(ts.cbegin(), it)) - 1;
        }
    }
    if (idx >= ts.size()) return;

    const bool up = closes[idx] >= opens[idx];
    const ImU32 val_col = up ? Theme::get_buy_color_u32(255)
                             : Theme::get_sell_color_u32(255);
    const ImU32 lbl_col = Theme::u32(Theme::Tokens::TX4);

    char num[32];
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImVec2 pos = ImPlot::GetPlotPos();
    pos.x += 10.0f;
    pos.y += 8.0f;

    ImGui::PushFont(Theme::Fonts::mono_sm());
    auto field = [&](const char* label, double value, bool is_vol) {
        dl->AddText(pos, lbl_col, label);
        pos.x += ImGui::CalcTextSize(label).x + 4.0f;
        if (is_vol) snprintf(num, sizeof(num), "%.0f", value);
        else        snprintf(num, sizeof(num), fmt_.price_fmt, value);
        dl->AddText(pos, val_col, num);
        pos.x += ImGui::CalcTextSize(num).x + 10.0f;
    };
    if (chart_type_ == ChartType::Line) {
        // Line mode shows Close only (the real close - the line IS the close).
        field("C", closes[idx], false);
    } else {
        field("O", opens[idx], false);
        field("H", highs[idx], false);
        field("L", lows[idx], false);
        field("C", closes[idx], false);
        field("V", vols[idx], true);
    }
    ImGui::PopFont();
}

// LIVE/REPLAY status pill, drawn just under the OHLC readout (top-left of plot).
void ChartWidget::draw_status_chip() const {
    using namespace Theme;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 plot = ImPlot::GetPlotPos();
    const bool replay = ctx_.replay_mgr().is_active();

    std::string sym = pair_.symbol;
    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);

    char text[128];
    if (replay) {
        char clk[16];
        DisplayTimeZone::instance().format(ctx_.replay_mgr().interpolated_time_ms(),
                                            TimeZoneFormat::TimeSeconds,
                                            clk, sizeof(clk));
        char spd[16];
        ReplayManager::format_speed(ctx_.replay_mgr().current_speed(), spd, sizeof(spd));
        snprintf(text, sizeof(text), "REPLAY \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s", sym.c_str(), clk, spd);
    } else {
        char clk[16];
        DisplayTimeZone::instance().format(static_cast<int64_t>(time(nullptr)) * 1000,
                                            TimeZoneFormat::TimeSeconds,
                                            clk, sizeof(clk));
        const float fr = ImGui::GetIO().Framerate;
        snprintf(text, sizeof(text), "LIVE \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %.0f FPS", sym.c_str(), clk, fr);
    }

    const ImVec4 fg = replay ? Tokens::WARN : Tokens::UP;
    ImGui::PushFont(Fonts::mono_sm());
    const ImVec2 tsz = ImGui::CalcTextSize(text);
    const float padx = 9.0f, h = 22.0f, dotr = 3.0f;
    const ImVec2 a(plot.x + 10.0f, plot.y + 30.0f);
    const ImVec2 b(a.x + padx + dotr * 2.0f + 6.0f + tsz.x + padx, a.y + h);
    dl->AddRectFilled(a, b, u32(fg, 0.12f), Radius::R2);
    dl->AddRect(a, b, u32(fg, 0.40f), Radius::R2, 0, 1.0f);
    const float cy = (a.y + b.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(a.x + padx + dotr, cy), dotr, u32(fg));
    dl->AddText(ImVec2(a.x + padx + dotr * 2.0f + 6.0f, cy - tsz.y * 0.5f), u32(fg), text);
    ImGui::PopFont();
}

void ChartWidget::plot_candles(double visible_x_min, double visible_x_max,
                               const std::vector<double>& src_opens,
                               const std::vector<double>& src_highs,
                               const std::vector<double>& src_lows,
                               const std::vector<double>& src_closes,
                               const BuildingOHLC& bld) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const double timeframe_ms = static_cast<double>(tf_sec) * 1000.0;
    const double half_width = timeframe_ms * 0.35;  // ~70% body, ~30% gap (was 0.49 = no gap)
    const bool fp_thin = ctx_.footprint_mgr().enabled;  // Thin candle body when FP active
    const ImVec2 plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const auto plot_max = ImVec2(plot_min.x + plot_size.x, plot_min.y + plot_size.y);

    // Three rendering tiers based on zoom level:
    //   Full candle mode: >=5px per candle (body + wick, normal width)
    //   Thin candle mode: 2-5px per candle (1px wick + thin body, preserves H/L)
    //   Bar mode: <2px per candle (vertical H/L line per pixel column, OHLC style)
    const double visible_candles = (visible_x_max - visible_x_min) / timeframe_ms;
    const double pixels_per_candle = plot_size.x / visible_candles;
    enum class CandleRenderMode { Full, Thin, Bar };
    const CandleRenderMode mode = (pixels_per_candle >= 5.0) ? CandleRenderMode::Full
                                : (pixels_per_candle >= 2.0) ? CandleRenderMode::Thin
                                :                              CandleRenderMode::Bar;

    ImPlot::PushPlotClipRect();
    // X axis is always the REAL timestamps; OHLC comes from the passed source
    // spans (real arrays for Candles/FP, derived HA arrays for Heikin Ashi).
    const auto& timestamps = ctx_.candle_mgr().timestamps();
    const auto& opens = src_opens;
    const auto& closes = src_closes;
    const auto& lows = src_lows;
    const auto& highs = src_highs;
    // §3C - prefer the live building candle over any finalized candle sharing its
    // timestamp. A transient finalized+building coexistence (e.g. the wall-clock
    // finalizer racing a late server partial on 5m/15m) otherwise leaves the STALE
    // finalized candle drawn while the live price tag (= building close) floats above
    // it. Skip the stale finalized one in the loops below and ALWAYS draw the building
    // candle, so the rendered live candle's close sits on the price tag.
    const double building_x = ctx_.candle_mgr().has_building_candle()
        ? static_cast<double>(ctx_.candle_mgr().building_candle().timestamp_ms) : -1.0;
    // Safety: OHLC spans must be parallel to the timestamps (HA cache is built
    // from the same deque, so this holds; guard against a transient mismatch).
    const size_t n_draw = std::min({timestamps.size(), opens.size(),
                                    highs.size(), lows.size(), closes.size()});
    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    if (mode == CandleRenderMode::Bar) {
        // Bar mode: aggregate candles that share the same pixel column.
        // Draw a vertical line from min(low) to max(high), colored by net direction.
        int last_px_col = -1;
        double col_high = -1e18, col_low = 1e18;
        double col_open = 0, col_close = 0;
        bool col_started = false;
        for (size_t i = start_idx; i < n_draw; i++) {
            if (timestamps[i] > visible_x_max) break;
            if (timestamps[i] == building_x) continue;  // live building candle drawn below
            const ImVec2 px = ImPlot::PlotToPixels(timestamps[i], closes[i]);
            const int px_col = static_cast<int>(px.x);
            if (px_col != last_px_col && col_started) {
                // Flush previous pixel column
                const ImU32 col = (col_close >= col_open)
                    ? Theme::get_buy_color_u32(255)
                    : Theme::get_sell_color_u32(255);
                const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
                const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
                draw_list->AddLine(
                    ImVec2(static_cast<float>(last_px_col), top.y),
                    ImVec2(static_cast<float>(last_px_col), bot.y),
                    col, 1.0f);
                col_high = -1e18; col_low = 1e18;
                col_started = false;
            }
            if (!col_started) {
                col_open = opens[i];
                col_started = true;
            }
            col_high = std::max(col_high, highs[i]);
            col_low = std::min(col_low, lows[i]);
            col_close = closes[i];
            last_px_col = px_col;
        }
        // Flush final column
        if (col_started) {
            const ImU32 col = (col_close >= col_open)
                ? Theme::get_buy_color_u32(255)
                : Theme::get_sell_color_u32(255);
            const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
            const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
            draw_list->AddLine(
                ImVec2(static_cast<float>(last_px_col), top.y),
                ImVec2(static_cast<float>(last_px_col), bot.y),
                col, 1.0f);
        }
    } else {
        // Full or Thin candle mode
        const bool force_thin = (mode == CandleRenderMode::Thin) || fp_thin;
        for (size_t i = start_idx; i < n_draw; i++) {
            if (timestamps[i] > visible_x_max) break;
            if (timestamps[i] == building_x) continue;  // live building candle drawn below
            draw_single_candle(draw_list,
                               timestamps[i], opens[i], closes[i], lows[i], highs[i],
                               half_width, plot_min, plot_max, force_thin);
        }
    }
    // Draw building candle. Any finalized candle sharing this timestamp was skipped in
    // the loops above (see building_x), so we ALWAYS draw the live building candle here -
    // it owns the right edge and its close sits on the price tag. (Previously this was
    // skipped whenever a finalized candle shared the ts, which left the STALE finalized
    // candle showing while the live price floated above it - the 5m/15m "price not
    // pinned / building candle invisible" bug.)
    if (bld.valid && building_x >= 0.0) {
        const double live_x = building_x;
        if (live_x >= visible_x_min && live_x <= visible_x_max) {
            if (mode == CandleRenderMode::Bar) {
                // Draw as vertical line matching bar mode
                const ImU32 col = (bld.close >= bld.open)
                    ? Theme::get_buy_color_u32(255)
                    : Theme::get_sell_color_u32(255);
                const ImVec2 top = ImPlot::PlotToPixels(live_x, bld.high);
                const ImVec2 bot = ImPlot::PlotToPixels(live_x, bld.low);
                draw_list->AddLine(top, bot, col, 1.0f);
            } else {
                const bool force_thin = (mode == CandleRenderMode::Thin) || fp_thin;
                draw_single_candle(draw_list,
                                   live_x, bld.open, bld.close, bld.low, bld.high,
                                   half_width, plot_min, plot_max, force_thin);
            }
        }
    }
    ImPlot::PopPlotClipRect();
}

// Line chart (ChartType::Line) - a close-price polyline vs time. Real-time
// resolution (§5.1): the recent-tick ring buffer (CandleManager::tick_times /
// tick_prices) supplies points across the window it covers, so the line moves
// tick-by-tick; for the region OLDER than the oldest retained tick we fall back
// to finalized candle closes; the building candle's (timestamp, close) is the
// final live point. When points-per-pixel is high, the stream is min/max
// decimated to one vertical segment per pixel column so a fast market stays a
// clean, cheap line. Stroked with the up/accent Theme token; culled to the rect.
void ChartWidget::plot_line(double visible_x_min, double visible_x_max) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    // Pixel clipping is handled by PushPlotClipRect below; culling here is by the
    // visible X range in add_point().
    const auto& timestamps = ctx_.candle_mgr().timestamps();
    const auto& closes     = ctx_.candle_mgr().closes();
    const auto& tick_times  = ctx_.candle_mgr().tick_times();
    const auto& tick_prices = ctx_.candle_mgr().tick_prices();

    // Oldest retained tick time: candle closes cover strictly OLDER than this so
    // the two sources do not double-plot the same region.
    const bool have_ticks = !tick_times.empty();
    const int64_t tick_floor_ms = have_ticks
        ? tick_times.front() : std::numeric_limits<int64_t>::max();

    // Per-pixel-column min/max decimation: as we walk points left→right we keep
    // one column open at a time, emitting its min and max (in plot Y) before
    // moving to the next column. This preserves spikes at ~2 points per column.
    line_pts_.clear();
    int    cur_col   = std::numeric_limits<int>::min();
    ImVec2 col_first{}, col_min{}, col_max{};
    bool   col_open  = false;

    auto flush_col = [&]() {
        if (!col_open) return;
        // Emit in a stable order so the polyline stays monotone in X: first the
        // column's entry point, then its extreme-low and extreme-high pixels.
        line_pts_.push_back(col_first);
        if (col_min.y != col_first.y) line_pts_.push_back(col_min);
        if (col_max.y != col_first.y && col_max.y != col_min.y) line_pts_.push_back(col_max);
        col_open = false;
    };
    auto add_point = [&](double x_ms, double price) {
        if (x_ms < visible_x_min || x_ms > visible_x_max) return;  // X cull
        const ImVec2 px = ImPlot::PlotToPixels(x_ms, price);
        const int col = static_cast<int>(px.x);
        if (col != cur_col) {
            flush_col();
            cur_col  = col;
            col_first = px;
            col_min   = px;
            col_max   = px;
            col_open  = true;
        } else {
            if (px.y < col_min.y) col_min = px;   // lower pixel y = higher price
            if (px.y > col_max.y) col_max = px;
        }
    };

    // 1) Candle-close fallback for the region older than the oldest tick.
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min);
        size_t i0 = 0;
        if (it != timestamps.begin())
            i0 = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        const size_t n = std::min(timestamps.size(), closes.size());
        for (size_t i = i0; i < n; i++) {
            if (timestamps[i] > visible_x_max) break;
            if (static_cast<int64_t>(timestamps[i]) >= tick_floor_ms) break;  // ticks own this
            add_point(timestamps[i], closes[i]);
        }
    }

    // 2) Tick region (newest, high-resolution). Binary-search the first visible.
    if (have_ticks) {
        const auto it = std::lower_bound(tick_times.begin(), tick_times.end(),
                                         static_cast<int64_t>(visible_x_min));
        size_t j0 = static_cast<size_t>(std::distance(tick_times.begin(), it));
        const size_t n = std::min(tick_times.size(), tick_prices.size());
        for (size_t j = j0; j < n; j++) {
            if (static_cast<double>(tick_times[j]) > visible_x_max) break;
            add_point(static_cast<double>(tick_times[j]), tick_prices[j]);
        }
    }

    // 3) Building candle close = the final live point (right edge).
    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        add_point(static_cast<double>(bc.timestamp_ms), bc.close);
    }

    flush_col();

    if (line_pts_.size() >= 2) {
        ImPlot::PushPlotClipRect();
        draw_list->AddPolyline(line_pts_.data(), static_cast<int>(line_pts_.size()),
                               Theme::u32(Theme::Tokens::UP), 0, 1.5f);
        ImPlot::PopPlotClipRect();
    }

    // RT (real-time view): a small filled dot on the live edge - the newest
    // point (building candle close, else newest tick, else last candle close).
    // Subtle marker so the eye lands on "now". Only in RT mode.
    if (rt_mode_) {
        double edge_x = 0.0, edge_y = 0.0;
        bool have_edge = false;
        if (ctx_.candle_mgr().has_building_candle()) {
            const auto& bc = ctx_.candle_mgr().building_candle();
            edge_x = static_cast<double>(bc.timestamp_ms);
            edge_y = bc.close;
            have_edge = true;
        } else if (have_ticks) {
            edge_x = static_cast<double>(tick_times.back());
            edge_y = tick_prices.back();
            have_edge = true;
        } else if (!timestamps.empty() && !closes.empty()) {
            edge_x = timestamps.back();
            edge_y = closes.back();
            have_edge = true;
        }
        if (have_edge && edge_x >= visible_x_min && edge_x <= visible_x_max) {
            const ImVec2 edge_px = ImPlot::PlotToPixels(edge_x, edge_y);
            ImPlot::PushPlotClipRect();
            draw_list->AddCircleFilled(edge_px, 3.25f, Theme::u32(Theme::Tokens::UP));
            ImPlot::PopPlotClipRect();
        }
    }
}

// Heikin Ashi visible extents (Phase 2): min HA low / max HA high over the
// visible window (plus the per-frame building HA candle), so the Y-limits frame
// the drawn HA candles and nothing clips. Leaves out params untouched when the
// HA cache has no data in range (caller keeps its real-candle extents then).
void ChartWidget::ha_visible_extents(double visible_x_min, double visible_x_max,
                                     double& y_min, double& y_max) const {
    const auto& ts      = ctx_.candle_mgr().timestamps();
    const auto& ha_low  = ctx_.candle_mgr().ha_lows();
    const auto& ha_high = ctx_.candle_mgr().ha_highs();
    if (ts.empty()) return;
    const size_t n = std::min({ts.size(), ha_low.size(), ha_high.size()});
    const auto lo_it = std::lower_bound(ts.begin(), ts.end(), visible_x_min);
    const auto hi_it = std::upper_bound(lo_it, ts.end(), visible_x_max);
    size_t i0 = static_cast<size_t>(lo_it - ts.begin());
    size_t i1 = std::min(n, static_cast<size_t>(hi_it - ts.begin()));
    for (size_t i = i0; i < i1; i++) {
        y_min = std::min(y_min, ha_low[i]);
        y_max = std::max(y_max, ha_high[i]);
    }
    // Building HA candle (computed per frame in render_chart; recompute the pair
    // here from the last finalized HA values to include it in the frame).
    if (ctx_.candle_mgr().has_building_candle() && n > 0) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        if (bc.timestamp_ms >= visible_x_min && bc.timestamp_ms <= visible_x_max) {
            const auto& ha_open  = ctx_.candle_mgr().ha_opens();
            const auto& ha_close = ctx_.candle_mgr().ha_closes();
            const size_t last = n - 1;
            const double prev_o = ha_open[last];
            const double prev_c = ha_close[last];
            const double hb_close = (bc.open + bc.high + bc.low + bc.close) * 0.25;
            const double hb_open  = (prev_o + prev_c) * 0.5;
            const double hb_high  = std::max({bc.high, hb_open, hb_close});
            const double hb_low   = std::min({bc.low,  hb_open, hb_close});
            y_min = std::min(y_min, hb_low);
            y_max = std::max(y_max, hb_high);
        }
    }
}

void ChartWidget::draw_single_candle(
    ImDrawList* draw_list,
    const double x, const double open, const double close,
    const double low, const double high,
    const double half_width, const ImVec2& plot_min, const ImVec2& plot_max,
    bool thin_body, uint8_t alpha)
{
    const bool bullish = close >= open;
    const ImU32 body_color = bullish
        ? Theme::get_buy_color_u32(alpha)
        : Theme::get_sell_color_u32(alpha);
    const ImU32 wick_color = body_color;

    const ImVec2 high_px = ImPlot::PlotToPixels(x, high);
    const ImVec2 low_px = ImPlot::PlotToPixels(x, low);
    const ImVec2 open_px = ImPlot::PlotToPixels(x, open);
    const ImVec2 close_px = ImPlot::PlotToPixels(x, close);

    // When footprint is active, use a thin candle body (~30% width)
    const double effective_half = thin_body ? half_width * 0.15 : half_width;
    const ImVec2 left_px = ImPlot::PlotToPixels(x - effective_half, 0);
    const ImVec2 right_px = ImPlot::PlotToPixels(x + effective_half, 0);

    float body_top = std::min(open_px.y, close_px.y);
    float body_bottom = std::max(open_px.y, close_px.y);

    // Minimum height for doji/flat candles
    if (body_bottom - body_top < 2.0f) {
        float center_y = (body_top + body_bottom) * 0.5f;
        body_top = center_y - 1.0f;
        body_bottom = center_y + 1.0f;
    }
    // Cull off-screen candles
    if (right_px.x < plot_min.x || left_px.x > plot_max.x) return;
    // Wick first (behind body)
    draw_list->AddLine(high_px, low_px, wick_color, 1.0f);
    // Body on top
    draw_list->AddRectFilled(
        ImVec2(left_px.x, body_top),
        ImVec2(right_px.x, body_bottom),
        body_color
    );
}


// Controls / Toolbar

void ChartWidget::render_controls() {
    // v2 chart toolbar (Component 3a): one 44px bar directly above the chart.
    // Left to right: chart-type icon button (single-select menu), layers icon
    // button (multi-select checklist + active-count badge), a vertical divider,
    // then a right-aligned Indicators + Settings group. Timeframes arrive in 3b.
    // Accent teal is the only on-state hue; controls are bg-1 with line-2 borders
    // (accent border on hover/open). Numbers use JetBrains Mono, chrome uses Hanken.
    // The two style pushes below are kept only to balance the trailing
    // PopStyleVar(2); the bar itself is drawn via the window draw list.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));

    using namespace Theme;

    // Bar geometry: a 44px band (8 top + 28 control + 8 bottom), bg-1 fill, with a
    // line-1 hairline along the bottom (drawn last) against the chart canvas.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float  ww = ImGui::GetContentRegionAvail().x;
    const float  bar_h = Theme::Layout::PILLSROW_H;         // 44
    const float  ctrl_y = bp.y + (bar_h - 28.0f) * 0.5f;    // centre 28px controls in the band
    dl->AddRectFilled(bp, ImVec2(bp.x + ww, bp.y + bar_h), Theme::u32(Theme::Tokens::PANEL));

    // Toolbar draw helpers ------------------------------------------------------
    // Small down-caret (a filled triangle), centred on (cx, cy).
    auto draw_caret = [](ImDrawList* d, float cx, float cy, ImU32 col) {
        d->AddTriangleFilled(ImVec2(cx - 3.0f, cy - 1.5f), ImVec2(cx + 3.0f, cy - 1.5f),
                             ImVec2(cx, cy + 2.5f), col);
    };
    // Two-stroke check mark inside a box at (bx, by), nominal size s, colour col.
    auto draw_check = [](ImDrawList* d, float bx, float by, float s, ImU32 col) {
        d->AddLine(ImVec2(bx + s * 0.24f, by + s * 0.52f), ImVec2(bx + s * 0.42f, by + s * 0.72f), col, 1.5f);
        d->AddLine(ImVec2(bx + s * 0.42f, by + s * 0.72f), ImVec2(bx + s * 0.78f, by + s * 0.30f), col, 1.5f);
    };
    // Chart-type glyph: 1:1 port of the design SVGs into a 14x14 box at (ox, oy).
    // chart_type_ index: 0 Candles, 1 FP Cluster, 2 FP Profile, 3 Heikin, 4 Line, 5 TPO.
    auto draw_ctype_glyph = [](ImDrawList* d, int t, float ox, float oy, ImU32 col) {
        const float sw = 1.2f;
        switch (t) {
            case 0:  // Candles: filled body + wick, hollow body + wick
                d->AddLine(ImVec2(ox + 4.0f, oy + 1.5f), ImVec2(ox + 4.0f, oy + 12.5f), col, sw);
                d->AddRectFilled(ImVec2(ox + 2.3f, oy + 4.0f), ImVec2(ox + 5.7f, oy + 10.0f), col);
                d->AddLine(ImVec2(ox + 10.0f, oy + 2.5f), ImVec2(ox + 10.0f, oy + 11.5f), col, sw);
                d->AddRect(ImVec2(ox + 8.3f, oy + 5.5f), ImVec2(ox + 11.7f, oy + 9.5f), col, 0.0f, 0, 1.1f);
                break;
            case 3:  // Heikin Ashi: single centred wick + wide body
                d->AddLine(ImVec2(ox + 7.0f, oy + 1.5f), ImVec2(ox + 7.0f, oy + 12.5f), col, sw);
                d->AddRectFilled(ImVec2(ox + 4.3f, oy + 4.0f), ImVec2(ox + 9.7f, oy + 10.0f), col);
                break;
            case 4: {  // Line: zig-zag polyline
                const ImVec2 pts[5] = {
                    ImVec2(ox + 1.0f, oy + 10.0f), ImVec2(ox + 4.5f, oy + 6.0f),
                    ImVec2(ox + 7.0f, oy + 8.5f),  ImVec2(ox + 10.0f, oy + 3.5f),
                    ImVec2(ox + 13.0f, oy + 5.5f) };
                d->AddPolyline(pts, 5, col, 0, 1.4f);
                break;
            }
            case 1:  // Footprint Cluster: outlined 2x3 grid of cells
                d->AddRect(ImVec2(ox + 1.5f, oy + 2.0f), ImVec2(ox + 12.5f, oy + 12.0f), col, 0.0f, 0, 1.0f);
                d->AddLine(ImVec2(ox + 7.0f, oy + 2.0f), ImVec2(ox + 7.0f, oy + 12.0f), col, 1.0f);
                d->AddLine(ImVec2(ox + 1.5f, oy + 5.3f), ImVec2(ox + 12.5f, oy + 5.3f), col, 1.0f);
                d->AddLine(ImVec2(ox + 1.5f, oy + 8.7f), ImVec2(ox + 12.5f, oy + 8.7f), col, 1.0f);
                break;
            case 2:  // Footprint Profile: left-anchored horizontal histogram
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 2.5f),  ImVec2(ox + 10.0f, oy + 4.0f),  col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 5.25f), ImVec2(ox + 8.0f,  oy + 6.75f), col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 8.0f),  ImVec2(ox + 12.0f, oy + 9.5f),  col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 10.75f),ImVec2(ox + 6.5f,  oy + 12.25f),col);
                break;
            case 5:  // TPO: market-profile bell of stacked columns
                d->AddRectFilled(ImVec2(ox + 2.0f,  oy + 6.0f), ImVec2(ox + 4.0f,  oy + 8.0f),  col);
                d->AddRectFilled(ImVec2(ox + 4.5f,  oy + 4.0f), ImVec2(ox + 6.5f,  oy + 10.0f), col);
                d->AddRectFilled(ImVec2(ox + 7.0f,  oy + 2.5f), ImVec2(ox + 9.0f,  oy + 11.5f), col);
                d->AddRectFilled(ImVec2(ox + 9.5f,  oy + 5.0f), ImVec2(ox + 11.5f, oy + 9.0f),  col);
                break;
            case 6:  // Renko: stepped bricks (staircase - filled, hollow, filled)
                d->AddRectFilled(ImVec2(ox + 1.0f,  oy + 8.0f),  ImVec2(ox + 5.0f,  oy + 11.5f), col);
                d->AddRect(ImVec2(ox + 5.5f,  oy + 4.5f), ImVec2(ox + 9.5f,  oy + 8.0f), col, 0.0f, 0, 1.1f);
                d->AddRectFilled(ImVec2(ox + 10.0f, oy + 1.0f),  ImVec2(ox + 14.0f, oy + 4.5f),  col);
                break;
        }
    };
    // Layers stack glyph: three stacked diamonds in a 15x15 box at (ox, oy).
    auto draw_layers_glyph = [](ImDrawList* d, float ox, float oy, ImU32 col) {
        const float k = 15.0f / 16.0f;
        auto P = [&](float x, float y) { return ImVec2(ox + x * k, oy + y * k); };
        const ImVec2 top[4] = { P(8, 2), P(14.5f, 5), P(8, 8), P(1.5f, 5) };
        d->AddPolyline(top, 4, col, ImDrawFlags_Closed, 1.2f);
        const ImVec2 mid[3] = { P(1.5f, 8), P(8, 11), P(14.5f, 8) };
        d->AddPolyline(mid, 3, col, 0, 1.2f);
        const ImVec2 bot[3] = { P(1.5f, 11), P(8, 14), P(14.5f, 11) };
        d->AddPolyline(bot, 3, col, 0, 1.2f);
    };

    // Layer on-count drives the badge and the layers-menu header.
    const int layers_on = (liq_dense_field_ ? 1 : 0) + (liq_heatmap_enabled_ ? 1 : 0)
                        + (liq_profile_enabled_ ? 1 : 0) + (liq_observed_enabled_ ? 1 : 0)
                        + (liq_census_enabled_ ? 1 : 0)
                        + (heatmap_enabled_ ? 1 : 0) + (vpvr_enabled_ ? 1 : 0);

    auto chart_type_label = [](ChartType type) -> const char* {
        switch (type) {
            case ChartType::Candles:          return "Candles";
            case ChartType::HeikinAshi:       return "Heikin Ashi";
            case ChartType::Line:             return "Line";
            case ChartType::FootprintCluster: return "Footprint cluster";
            case ChartType::FootprintProfile: return "Footprint profile";
            case ChartType::TPO:              return "TPO";
            case ChartType::Renko:            return "Renko";
        }
        return "Chart";
    };

    // The full labels are deliberately visible at normal terminal widths. The
    // previous icon-only controls hid seven working chart views behind an
    // unlabeled candlestick glyph. Compact docks keep the old glyph-only shape.
    const bool show_control_labels = ww >= 980.0f;

    // Left group: chart-view button, layers button, divider ---------------------
    const float caret_w = 9.0f;
    float cx = bp.x + 12.0f;   // left gutter (bar padding 0 12)
    ImVec2 ct_anchor, ly_anchor;

    // Timeframe segment (favourites bar + caret -> grouped dropdown), reusing the
    // shell control so the bare terminal matches the /terminal?event= chrome. Sits
    // left of the chart-type button; render_chart_tf draws it + returns its width.
    {
        ImGui::SetCursorScreenPos(ImVec2(cx, bp.y + (bar_h - 30.0f) * 0.5f));
        cx += AppShell::render_chart_tf(this) + 12.0f;
    }

    // Vertical divider between the timeframe group and the chart-type/layers group.
    {
        const float dy = bp.y + (bar_h - 22.0f) * 0.5f;
        dl->AddLine(ImVec2(cx, dy), ImVec2(cx, dy + 22.0f), Theme::u32(Theme::Tokens::BD1), 1.0f);
        cx += 1.0f + 12.0f;
    }

    // Chart-view button (active glyph + current view name + caret).
    {
        const char* active_label = chart_type_label(chart_type_);
        ImGui::PushFont(Theme::Fonts::ui());
        const float label_w = show_control_labels ? ImGui::CalcTextSize(active_label).x : 0.0f;
        ImGui::PopFont();
        const float label_span = show_control_labels ? (8.0f + label_w) : 0.0f;
        const float w = 9.0f + 14.0f + label_span + 6.0f + caret_w + 9.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##ct_btn", ImVec2(w, 28.0f));
        const bool open = ImGui::IsPopupOpen("chart_type_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        dl->AddRect(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(hot ? Theme::Tokens::BRAND : Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
        draw_ctype_glyph(dl, static_cast<int>(chart_type_), cx + 9.0f, ctrl_y + 7.0f,
                         Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2));
        float caret_x = cx + 9.0f + 14.0f;
        if (show_control_labels) {
            ImGui::PushFont(Theme::Fonts::ui());
            dl->AddText(ImVec2(caret_x + 8.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                        active_label);
            ImGui::PopFont();
            caret_x += 8.0f + label_w;
        }
        draw_caret(dl, caret_x + 6.0f + caret_w * 0.5f, ctrl_y + 14.0f,
                   Theme::u32(Theme::Tokens::TX3));
        if (ImGui::IsItemHovered()) Theme::tooltip("Chart view: %s", active_label);
        if (clicked && !open) ImGui::OpenPopup("chart_type_popup");
        ct_anchor = ImVec2(cx, ctrl_y + 28.0f);
        cx += w + 12.0f;
    }

    // Layers button (stack glyph + plain-language label + count + caret).
    {
        char cb[8]; snprintf(cb, sizeof(cb), "%d", layers_on);
        ImGui::PushFont(Theme::Fonts::mono_sm());
        const float num_w = ImGui::CalcTextSize(cb).x;
        ImGui::PopFont();
        ImGui::PushFont(Theme::Fonts::ui());
        const float layers_label_w = show_control_labels ? ImGui::CalcTextSize("Layers").x : 0.0f;
        ImGui::PopFont();
        const float layers_label_span = show_control_labels ? (8.0f + layers_label_w) : 0.0f;
        const float chip_w = num_w + 10.0f;                          // badge chip (padding ~1 5)
        const float badge_span = (layers_on > 0) ? (chip_w + 6.0f) : 0.0f;
        const float w = 9.0f + 15.0f + layers_label_span + 6.0f + badge_span + caret_w + 9.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##ly_btn", ImVec2(w, 28.0f));
        const bool open = ImGui::IsPopupOpen("layers_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        dl->AddRect(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(hot ? Theme::Tokens::BRAND : Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
        draw_layers_glyph(dl, cx + 9.0f, ctrl_y + 6.5f,
                          Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2));
        float bx = cx + 9.0f + 15.0f + 6.0f;
        if (show_control_labels) {
            ImGui::PushFont(Theme::Fonts::ui());
            dl->AddText(ImVec2(bx, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                        "Layers");
            ImGui::PopFont();
            bx += layers_label_w + 8.0f;
        }
        if (layers_on > 0) {
            const float chip_h = 14.0f, chip_y = ctrl_y + (28.0f - chip_h) * 0.5f;
            dl->AddRectFilled(ImVec2(bx, chip_y), ImVec2(bx + chip_w, chip_y + chip_h),
                              Theme::u32(Theme::Tokens::BRAND), Theme::Radius::R1);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            dl->AddText(ImVec2(bx + (chip_w - num_w) * 0.5f, chip_y + (chip_h - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(Theme::Tokens::BRAND_INK), cb);
            ImGui::PopFont();
            bx += chip_w + 6.0f;
        }
        draw_caret(dl, bx + caret_w * 0.5f, ctrl_y + 14.0f, Theme::u32(Theme::Tokens::TX3));
        if (ImGui::IsItemHovered()) Theme::tooltip("%d chart layer%s active",
                                                       layers_on, layers_on == 1 ? "" : "s");
        if (clicked && !open) ImGui::OpenPopup("layers_popup");
        ly_anchor = ImVec2(cx, ctrl_y + 28.0f);
        cx += w + 12.0f;
    }

    // "+ widget" button (beside the layers control). This is the ONLY widget-
    // creation entry now (the old topbar "+" was removed): living in the chart
    // toolbar means it renders in EVERY chrome, including the embedded /demo +
    // event replays where the native topbar is suppressed.
    {
        const char* label = "+ widget";
        const float tw = ImGui::CalcTextSize(label).x;
        const float w = 10.0f + tw + 10.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##add_widget_btn", ImVec2(w, 28.0f));
        const bool open = ImGui::IsPopupOpen("add_widget_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        dl->AddRect(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(hot ? Theme::Tokens::BRAND : Theme::Tokens::BD2),
                    Theme::Radius::R2, 0, 1.0f);
        dl->AddText(ImVec2(cx + 10.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2), label);
        if (ImGui::IsItemHovered()) Theme::tooltip("Add a widget");
        if (clicked && !open) ImGui::OpenPopup("add_widget_popup");
        cx += w + 12.0f;
    }

    // Draw button - the rail's tool list as a dropdown (icons left of labels).
    // Gated with the in-chart rail: drawing chrome hides in embedded education
    // chromes, pack mode and clip-recorder focus.
    {
        const auto& eb = EducationBoot::instance();
        const bool show_draw_menu = !eb.is_embedded() && !eb.is_pack() &&
                                    !ClipRecorder::focus_active() &&
                                    !edu::RecorderRuntime::instance().active();
        if (show_draw_menu) {
            const char* label = "Draw";
            const float tw = ImGui::CalcTextSize(label).x;
            const float w = 10.0f + 15.0f + 6.0f + tw + 10.0f;
            ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
            const bool clicked =
                ImGui::InvisibleButton("##draw_menu_btn", ImVec2(w, 28.0f));
            const bool open = ImGui::IsPopupOpen("chart_draw_popup");
            const bool hot  = ImGui::IsItemHovered() || open;
            dl->AddRect(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                        Theme::u32(hot ? Theme::Tokens::BRAND : Theme::Tokens::BD2),
                        Theme::Radius::R2, 0, 1.0f);
            drawing::draw_ui_icon(
                dl, drawing::UiIcon::Pencil,
                ImVec2(cx + 10.0f + 7.5f, ctrl_y + 14.0f), 6.0f,
                Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                1.4f);
            dl->AddText(ImVec2(cx + 10.0f + 15.0f + 6.0f,
                               ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX
                                       : Theme::Tokens::TX2),
                        label);
            if (ImGui::IsItemHovered()) Theme::tooltip("Drawing tools");
            if (clicked && !open) ImGui::OpenPopup("chart_draw_popup");
            cx += w + 12.0f;

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Theme::Radius::R3);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
            if (ImGui::BeginPopup("chart_draw_popup")) {
                drawing::render_tool_menu_rows(ctx_.drawing_mgr());
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }
    }

    // Add-widget menu (single-select). In the live terminal a symbol-bearing
    // choice opens the symbol picker (multi-symbol, same as the old topbar +); in
    // the embedded single-symbol replays it files a request for THIS chart's
    // symbol. Global widgets carry no symbol, so they always file a request.
    if (ImGui::BeginPopup("add_widget_popup")) {
        using PW = Menu::SymbolPickerState::PendingWidget;
        const bool embedded = EducationBoot::instance().is_embedded()
                           || EducationBoot::instance().is_pack();
        auto add = [&](PW type) {
            const bool global_widget = type == PW::PaperTrading
                                    || type == PW::ReplayLibrary;
            if (!global_widget && !embedded) {
                Menu::g_symbol_picker.pending = type;
                Menu::g_symbol_picker.open = true;
                Menu::g_symbol_picker.search_buf[0] = '\0';
                Menu::g_symbol_picker.replace_mode = false;
            } else {
                Menu::g_widget_add_request.type = type;
                Menu::g_widget_add_request.pair = pair_;
                Menu::g_widget_add_request.fmt = fmt_;
                Menu::g_widget_add_request.tick_size = tick_size_;
                Menu::g_widget_add_request.pending = true;
            }
        };
        if (ImGui::MenuItem("Chart"))     add(PW::Charts);
        if (ImGui::MenuItem("Orderbook")) add(PW::Orderbook);
        if (ImGui::MenuItem("DOM"))       add(PW::DOM);
        if (ImGui::MenuItem("Trades"))    add(PW::Trades);
        if (ImGui::MenuItem("Stats"))     add(PW::Stats);
        if (ImGui::MenuItem("Debug"))     add(PW::Debug);
        ImGui::Separator();
        if (ImGui::MenuItem("Paper Trading")) add(PW::PaperTrading);
        if (!embedded && ImGui::MenuItem("Replay Library")) add(PW::ReplayLibrary);
        ImGui::EndPopup();
    }

    // (RT toggle relocated to the TIMEFRAME dropdown - app_shell render_tf_menu.
    //  It now applies to every chart type, not just the Line, so the old Line-
    //  only toolbar pill was removed.)


    // Layer toggles + LIQ LEV now live inside the layers menu (defined below).

    // Orderbook-heatmap settings - opened by right-clicking the Heatmap pill.
    if (ImGui::BeginPopup("hm_settings")) {
        ImGui::SetNextItemWidth(120);
        int offset_sec = static_cast<int>(heatmap_time_offset_ms_ / 1000);
        if (ImGui::SliderInt("##HMOffset", &offset_sec, -120, 60, "Offset: %ds")) {
            heatmap_time_offset_ms_ = static_cast<int64_t>(offset_sec) * 1000;
        }
        if (ImGui::IsItemHovered()) {
            Theme::tooltip("Adjust heatmap time alignment.\nNegative = shift left, Positive = shift right");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        const int bucket_multipliers[] = {1, 2, 5, 10, 20};
        constexpr int NUM_BUCKETS = 5;
        const char* bucket_labels[] = {"UHD", "HD", "SD", "LD", "ULD"};
        int current_bucket_idx = 0;
        for (int i = 0; i < NUM_BUCKETS; i++) {
            if (heatmap_bucket_multiplier_ == bucket_multipliers[i]) {
                current_bucket_idx = i;
                break;
            }
        }
        if (ImGui::Combo("##HMBucket", &current_bucket_idx, bucket_labels, NUM_BUCKETS)) {
            heatmap_bucket_multiplier_ = bucket_multipliers[current_bucket_idx];
        }
        if (ImGui::IsItemHovered()) {
            auto* bucket_recon = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
            double native_bucket = bucket_recon ? bucket_recon->get_native_bucket_size() : 0;
            double display_bucket = native_bucket * heatmap_bucket_multiplier_;
            char tip[128];
            if (display_bucket >= 1.0)
                snprintf(tip, sizeof(tip), "Bucket: $%.0f", display_bucket);
            else if (display_bucket >= 0.01)
                snprintf(tip, sizeof(tip), "Bucket: $%.2f", display_bucket);
            else
                snprintf(tip, sizeof(tip), "Bucket: $%.6f", display_bucket);
            Theme::tooltip("%s", tip);
        }
        ImGui::EndPopup();
    }
    // ═══════════════════════════════════════════════════════════════════════
    // Chart View dropdown: one base rendering mode at a time.
    // ═══════════════════════════════════════════════════════════════════════
    // Shared floating-chrome style for both menus: bg-1, 1px line-2 border,
    // radius 6, padding 4, plus a soft drop shadow (floating menus are the one
    // place rounding + shadow are allowed).
    auto push_menu_style = []() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    };
    auto pop_menu_style = []() { ImGui::PopStyleColor(2); ImGui::PopStyleVar(4); };
    auto draw_menu_shadow = []() {
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
        d->PushClipRectFullScreen();
        for (int i = 5; i >= 1; --i) {
            const float e = (float)i * 2.0f;
            d->AddRect(ImVec2(a.x - e, a.y - e + 3.0f), ImVec2(b.x + e, b.y + e + 3.0f),
                       IM_COL32(0, 0, 0, 14), Theme::Radius::R3 + e, 0, 1.6f);
        }
        d->PopClipRect();
    };
    // Popover header row (Hanken micro-label, text-3).
    auto menu_header = [](const char* s) {
        const float base_x = ImGui::GetCursorPosX();
        ImGui::SetCursorPos(ImVec2(base_x + 5.0f, ImGui::GetCursorPosY() + 4.0f));
        ImGui::PushFont(Theme::Fonts::label());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
        ImGui::TextUnformatted(s);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorPosX(base_x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
    };

    // Chart-view menu: price views first, then order-flow views. Each row says
    // what changes, so the menu works as a compact feature inventory too.
    ImGui::SetNextWindowPos(ImVec2(ct_anchor.x, ct_anchor.y + 4.0f));
    ImGui::SetNextWindowSize(ImVec2(286.0f, 0.0f));
    push_menu_style();
    if (ImGui::BeginPopup("chart_type_popup")) {
        draw_menu_shadow();
        menu_header("CHART VIEW \xc2\xb7 7 OPTIONS");
        struct CtRow { const char* label; const char* detail; int idx; };
        const CtRow rows[7] = {
            {"Candles", "Standard OHLC bars", 0},
            {"Heikin Ashi", "Smoothed candles for trend structure", 3},
            {"Line", "Close price without candle noise", 4},
            {"Renko", "Price movement without fixed time bars", 6},
            {"Footprint cluster", "Bid and ask volume at every price", 1},
            {"Footprint profile", "Traded-volume shape inside each bar", 2},
            {"TPO market profile", "Time spent at each price", 5} };
        ImDrawList* d = ImGui::GetWindowDrawList();
        for (int r = 0; r < 7; ++r) {
            if (r == 0 || r == 4) {
                if (r == 4) {
                    const ImVec2 sep = ImGui::GetCursorScreenPos();
                    d->AddLine(ImVec2(sep.x + 5.0f, sep.y), ImVec2(sep.x + 273.0f, sep.y),
                               Theme::u32(Theme::Tokens::BD1), 1.0f);
                }
                menu_header(r == 0 ? "PRICE" : "ORDER FLOW");
            }
            const int idx = rows[r].idx;
            const bool active = (chart_type_ == static_cast<ChartType>(idx));
            const float row_w = ImGui::GetContentRegionAvail().x;
            const float row_h = 44.0f;
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGui::PushID(r);
            const bool clicked = ImGui::InvisibleButton("##ctrow", ImVec2(row_w, row_h));
            const bool hov = ImGui::IsItemHovered();
            const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            ImGui::PopID();
            if (hov) d->AddRectFilled(rp, ImVec2(rp.x + row_w, rp.y + row_h),
                                      Theme::u32(Theme::Tokens::ELEV), Theme::Radius::R2);
            draw_ctype_glyph(d, idx, rp.x + 9.0f, rp.y + (row_h - 14.0f) * 0.5f,
                             Theme::u32(Theme::Tokens::TX2));
            ImGui::PushFont(Theme::Fonts::ui());
            d->AddText(ImVec2(rp.x + 32.0f, rp.y + 7.0f),
                       Theme::u32(Theme::Tokens::TX1), rows[r].label);
            ImGui::PopFont();
            ImGui::PushFont(Theme::Fonts::mono_sm());
            d->AddText(ImVec2(rp.x + 32.0f, rp.y + 24.0f),
                       Theme::u32(Theme::Tokens::TX3), rows[r].detail);
            ImGui::PopFont();
            if (active)
                draw_check(d, rp.x + row_w - 22.0f, rp.y + (row_h - 12.0f) * 0.5f, 12.0f,
                           Theme::u32(Theme::Tokens::BRAND_TX));
            if (clicked) {
                const ChartType prev = chart_type_;
                chart_type_ = static_cast<ChartType>(idx);
                auto& fp = ctx_.footprint_mgr();
                fp.enabled = (idx == 1 || idx == 2);
                if (idx == 1) fp.mode = FootprintManager::Mode::SellsBuys;
                if (idx == 2) fp.mode = FootprintManager::Mode::Profile;
                if (idx == 5) { tpo_zoom_pending_ = true; tpo_zoom_frames_ = 3; }
                // Renko uses a brick-index X-axis; entering or leaving it switches
                // the X domain (time <-> brick index). Snap to the live edge and
                // drop the stale opposite-domain axis limits so nothing renders
                // off-screen on the first frame after the switch.
                if (idx == 6 || prev == ChartType::Renko) {
                    ctx_.candle_mgr().set_follow_live(true);
                    last_visible_range_.X.Min = last_visible_range_.X.Max = 0.0;
                    last_visible_range_.Y.Min = last_visible_range_.Y.Max = 0.0;
                    renko_view_t0_ms_ = renko_view_t1_ms_ = 0;
                }
                ImGui::CloseCurrentPopup();
            }
            if ((idx == 1 || idx == 2) && rclick) {
                ImGui::CloseCurrentPopup();
                footprint_settings_panel_.open();
            }
            if (idx == 5 && rclick) {
                ImGui::CloseCurrentPopup();
                tpo_settings_panel_.open();
            }
            if (idx == 6 && rclick) {
                ImGui::CloseCurrentPopup();
                ImGui::OpenPopup("renko_settings");
            }
        }
        {
            const ImVec2 fp = ImGui::GetCursorScreenPos();
            d->AddLine(ImVec2(fp.x + 5.0f, fp.y), ImVec2(fp.x + 273.0f, fp.y),
                       Theme::u32(Theme::Tokens::BD1), 1.0f);
            ImGui::PushFont(Theme::Fonts::label());
            d->AddText(ImVec2(fp.x + 9.0f, fp.y + 9.0f), Theme::u32(Theme::Tokens::TX3),
                       "RIGHT-CLICK A VIEW FOR ITS SETTINGS");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(278.0f, 28.0f));
        }
        ImGui::EndPopup();
    }
    pop_menu_style();

    // Renko brick-size config popup (opened by right-clicking the Renko row OR
    // the on-chart gear; the gear defers OpenPopup to here so it runs at window
    // scope rather than inside BeginPlot).
    if (open_renko_settings_) { ImGui::OpenPopup("renko_settings"); open_renko_settings_ = false; }
    render_renko_settings_popup();

    // Layers menu (multi-select) + a liquidation leverage sub-section.
    ImGui::SetNextWindowPos(ImVec2(ly_anchor.x, ly_anchor.y + 4.0f));
    ImGui::SetNextWindowSize(ImVec2(250.0f, 0.0f));
    push_menu_style();
    if (ImGui::BeginPopup("layers_popup")) {
        draw_menu_shadow();
        char hdr[32]; snprintf(hdr, sizeof(hdr), "LAYERS \xc2\xb7 %d ON", layers_on);
        menu_header(hdr);
        ImDrawList* d = ImGui::GetWindowDrawList();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float row_h = 30.0f;
        const bool pro = Entitlements::is_pro();
        auto layer_section = [&](const char* label) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushFont(Theme::Fonts::label());
            d->AddText(ImVec2(p.x + 9.0f, p.y + 8.0f),
                       Theme::u32(Theme::Tokens::TX3), label);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(row_w, 25.0f));
        };

        // One checklist row (14px checkbox + label). `locked` = Pro-gated: shows a
        // padlock and, on click, funnels into the upsell instead of toggling.
        auto layer_row = [&](const char* label, bool on, bool locked) -> bool {
            ImGui::PushID(label);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##lr", ImVec2(row_w, row_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) d->AddRectFilled(rp, ImVec2(rp.x + row_w, rp.y + row_h),
                                      Theme::u32(Theme::Tokens::ELEV), Theme::Radius::R2);
            const float bx = rp.x + 9.0f, by = rp.y + (row_h - 14.0f) * 0.5f;
            if (locked) {
                d->AddRect(ImVec2(bx + 1.0f, by + 6.0f), ImVec2(bx + 11.0f, by + 13.0f),
                           Theme::u32(Theme::Tokens::TX3), 1.0f, 0, 1.0f);
                d->PathArcTo(ImVec2(bx + 6.0f, by + 6.0f), 3.0f, 3.14159265f, 6.2831853f, 8);
                d->PathStroke(Theme::u32(Theme::Tokens::TX3), 0, 1.0f);
            } else if (on) {
                d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + 14.0f, by + 14.0f),
                                 Theme::u32(Theme::Tokens::BRAND), 2.0f);
                draw_check(d, bx, by, 14.0f, Theme::u32(Theme::Tokens::BRAND_INK));
            } else {
                d->AddRect(ImVec2(bx, by), ImVec2(bx + 14.0f, by + 14.0f),
                           Theme::u32(Theme::Tokens::BD2), 2.0f, 0, 1.0f);
            }
            ImGui::PushFont(Theme::Fonts::ui());
            const ImU32 lc = Theme::u32(locked ? Theme::Tokens::TX3
                                               : (on ? Theme::Tokens::TX1 : Theme::Tokens::TX2));
            d->AddText(ImVec2(bx + 23.0f, rp.y + (row_h - ImGui::GetFontSize()) * 0.5f), lc, label);
            ImGui::PopFont();
            return clicked;
        };

        layer_section("LIQUIDATIONS");

        // Liquidation Heatmap = the client Field (free).
        if (layer_row("Liquidation heatmap", liq_dense_field_, false)) {
            liq_dense_field_ = !liq_dense_field_;
            liq_shelf_cache_ts_ = -1;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_liq_settings_ = true;
        // Liq Levels = OI rails via the gated liq_heatmaps stream (Pro).
        if (layer_row("Liquidation levels", pro && liq_heatmap_enabled_, !pro)) {
            if (pro) toggle_liquidation_heatmap();
            else ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer);
        }
        // Modelled levels are a hosted stream. Once enabled on a feed that
        // never delivers it, the layer stays invisible; say why on hover
        // and point at the layers that do work here (see stream_presence.h).
        if (ImGui::IsItemHovered() && StreamPresence::instance().absent(
                static_cast<uint32_t>(Terminal::Stream::LiquidationHeatmap))) {
            Theme::tooltip("Modelled levels ride EdgeDepth's hosted feed, which this\n"
                           "feed has not delivered. The liquidation heatmap and\n"
                           "profile are computed client-side and still work.");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_liq_settings_ = true;
        // Liq Levels HL = REAL predictive levels from the HL census (Pro, P2e) -
        // ground truth, never folded into the modelled "Liq Levels" above. Keyed
        // by underlying, so it's offered on any venue's chart; greyed (inert, no
        // padlock) when the underlying has no HL market. The ticker24h hl feed is
        // the market registry - before it loads, "unknown" stays offered and data
        // arrival decides (the layer's own NO DATA badge covers the miss).
        {
            const bool hl_known = TickerManager::instance().get("hl", liq_census_pair_.symbol) != nullptr;
            const bool hl_missing = !hl_known && TickerManager::instance().has_data();
            if (layer_row(hl_missing ? "Hyperliquid liq levels \xc2\xb7 n/a" : "Hyperliquid liq levels",
                          pro && liq_census_enabled_ && !hl_missing, !pro)) {
                if (!pro) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer);
                else if (!hl_missing) toggle_liq_census();
            }
        }
        // Liq Profile = price-marginal of the Field (free).
        if (layer_row("Liquidation profile", liq_profile_enabled_, false))
            liq_profile_enabled_ = !liq_profile_enabled_;
        // Observed = real @forceOrder prints via the liquidations stream (Pro).
        if (layer_row("Observed liquidations", pro && liq_observed_enabled_, !pro)) {
            if (pro) liq_observed_enabled_ = !liq_observed_enabled_;
            else ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer);
        }

        {
            const ImVec2 sep = ImGui::GetCursorScreenPos();
            d->AddLine(ImVec2(sep.x + 5.0f, sep.y), ImVec2(sep.x + row_w - 5.0f, sep.y),
                       Theme::u32(Theme::Tokens::BD1), 1.0f);
        }
        layer_section("MARKET STRUCTURE");

        // OB Depth = orderbook depth heatmap (free). Right-click opens its settings.
        {
            const bool c = layer_row("Order book depth", heatmap_enabled_, false);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("hm_settings");
            if (c) heatmap_enabled_ = !heatmap_enabled_;
        }
        // VPVR = volume profile visible range (free). Right-click opens its settings.
        {
            const bool c = layer_row("Volume profile (VPVR)", vpvr_enabled_, false);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_vpvr_settings_ = true;
            if (c) {
                vpvr_enabled_ = !vpvr_enabled_;
                ctx_.vpvr_mgr().set_enabled(vpvr_enabled_);
                if (vpvr_enabled_) { vpvr_last_request_start_ = 0; vpvr_last_request_end_ = 0; }
            }
        }

        // LIQ LEV sub-section: leverage tiers (Pro). Drives BOTH the field and the
        // levels via the reconstructor leverage mask.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        { const ImVec2 sp = ImGui::GetCursorScreenPos();
          d->AddLine(ImVec2(sp.x + 2.0f, sp.y), ImVec2(sp.x + row_w - 2.0f, sp.y),
                     Theme::u32(Theme::Tokens::BD1), 1.0f); }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        menu_header("LIQUIDATION LEVERAGE");
        {
            struct Lev { const char* label; bool* flag; };
            Lev levs[4] = { {"25\xc3\x97", &liq_lev_25x_}, {"50\xc3\x97", &liq_lev_50x_},
                            {"75\xc3\x97", &liq_lev_75x_}, {"100\xc3\x97", &liq_lev_100x_} };
            const float gap = 6.0f;
            const float chip_w = (row_w - gap * 3.0f) / 4.0f;
            const float chip_h = 24.0f;
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(row_w, chip_h));   // reserve the chip band so the popup auto-grows its bounds
            bool changed = false, lock_hit = false;
            for (int i = 0; i < 4; ++i) {
                const float x = base.x + i * (chip_w + gap);
                ImGui::PushID(600 + i);
                ImGui::SetCursorScreenPos(ImVec2(x, base.y));
                const bool clicked = ImGui::InvisibleButton("##lev", ImVec2(chip_w, chip_h));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool on = pro && *levs[i].flag;
                d->AddRectFilled(ImVec2(x, base.y), ImVec2(x + chip_w, base.y + chip_h),
                    Theme::u32(on ? Theme::Tokens::BRAND_SOFT
                                  : (hov ? Theme::Tokens::ELEV : Theme::Tokens::PANEL)), Theme::Radius::R2);
                d->AddRect(ImVec2(x, base.y), ImVec2(x + chip_w, base.y + chip_h),
                    Theme::u32(on ? Theme::Tokens::BRAND : Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
                ImGui::PushFont(Theme::Fonts::mono_sm());
                const float tw = ImGui::CalcTextSize(levs[i].label).x;
                d->AddText(ImVec2(x + (chip_w - tw) * 0.5f, base.y + (chip_h - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(on ? Theme::Tokens::BRAND_TX
                                  : (pro ? Theme::Tokens::TX2 : Theme::Tokens::TX4)), levs[i].label);
                ImGui::PopFont();
                if (clicked) { if (pro) { *levs[i].flag = !*levs[i].flag; changed = true; } else lock_hit = true; }
            }
            if (lock_hit) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer);
            if (changed) {
                uint8_t mask = 0;
                if (liq_lev_5x_)   mask |= 0x01;
                if (liq_lev_10x_)  mask |= 0x02;
                if (liq_lev_25x_)  mask |= 0x04;
                if (liq_lev_50x_)  mask |= 0x08;
                if (liq_lev_75x_)  mask |= 0x10;
                if (liq_lev_100x_) mask |= 0x20;
                ctx_.liq_heatmap_mgr().set_leverage_mask(mask);
            }
        }
        ImGui::EndPopup();
    }
    pop_menu_style();

    // ═══════════════════════════════════════════════════════════════════════
    // Indicators popup: subplots only. Anything drawn on the main chart belongs
    // in Layers, so one capability has one home and one active count.
    // ═══════════════════════════════════════════════════════════════════════
    // Right group: Indicators + Settings, right-aligned to the bar's right edge.
    // The button carries an active-count chip and the menu stays open for
    // multi-selection.
    int indi_active_n = 0;
    if (indicator_mgr_.has_indicator_of_type<Indicators::VolumeIndicator>())      ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::CVDIndicator>())         ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::RSIIndicator>())         ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::MACDIndicator>())        ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::FundingRateIndicator>()) ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::OIIndicator>())          ++indi_active_n;
    if (indicator_mgr_.has_indicator_of_type<Indicators::VPINIndicator>())        ++indi_active_n;
    char indi_nbuf[8];
    snprintf(indi_nbuf, sizeof(indi_nbuf), "%d", indi_active_n);

    ImGui::PushFont(Theme::Fonts::ui());
    const float rg_indic_txt = ImGui::CalcTextSize("Indicators").x;
    const float rg_set_txt   = ImGui::CalcTextSize("Settings").x;
    ImGui::PopFont();
    float indi_chip_w = 0.0f;
    if (indi_active_n > 0) {
        ImGui::PushFont(Theme::Fonts::mono_sm());
        indi_chip_w = 7.0f + ImGui::CalcTextSize(indi_nbuf).x + 10.0f;  // gap + chip
        ImGui::PopFont();
    }
    const float rg_indic_w = rg_indic_txt + 11.0f + 13.0f + 7.0f + indi_chip_w + 11.0f;
    const float rg_set_w   = rg_set_txt + 12.0f;       // borderless text button
    const float rg_right   = bp.x + ww - 12.0f;        // right gutter 12
    const float rg_set_x   = rg_right - rg_set_w;
    const float rg_indic_x = rg_set_x - 12.0f - rg_indic_w;
    {
        ImGui::SetCursorScreenPos(ImVec2(rg_indic_x, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##indic_btn", ImVec2(rg_indic_w, 28.0f));
        const bool open = ImGui::IsPopupOpen("indicators_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        if (hot)
            dl->AddRectFilled(ImVec2(rg_indic_x, ctrl_y), ImVec2(rg_indic_x + rg_indic_w, ctrl_y + 28.0f),
                              Theme::u32(Theme::Tokens::BRAND_SOFT));
        dl->AddRect(ImVec2(rg_indic_x, ctrl_y), ImVec2(rg_indic_x + rg_indic_w, ctrl_y + 28.0f),
                    Theme::u32(hot ? Theme::Tokens::BRAND : Theme::Tokens::BD2), 0.0f, 0, 1.0f);
        // Mini bar-chart glyph (1k) + the label.
        const ImU32 icol = Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2);
        {
            const float ix = rg_indic_x + 11.0f, base = ctrl_y + 20.5f;   // bottom baseline
            const float bars[4] = {6.0f, 11.0f, 8.0f, 13.0f};
            for (int b = 0; b < 4; ++b) {
                const float bx = ix + b * 3.5f;
                dl->AddRectFilled(ImVec2(bx, base - bars[b]), ImVec2(bx + 2.2f, base), icol);
            }
        }
        ImGui::PushFont(Theme::Fonts::ui());
        dl->AddText(ImVec2(rg_indic_x + 11.0f + 13.0f + 7.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    icol, "Indicators");
        ImGui::PopFont();
        if (indi_active_n > 0) {
            // solid accent count chip (1k)
            ImGui::PushFont(Theme::Fonts::mono_sm());
            const ImVec2 cts = ImGui::CalcTextSize(indi_nbuf);
            const float chx = rg_indic_x + 11.0f + 13.0f + 7.0f + rg_indic_txt + 7.0f;
            const float chh = 15.0f, chy = ctrl_y + (28.0f - chh) * 0.5f;
            dl->AddRectFilled(ImVec2(chx, chy), ImVec2(chx + cts.x + 10.0f, chy + chh),
                              Theme::u32(Theme::Tokens::BRAND));
            dl->AddText(ImVec2(chx + 5.0f, chy + (chh - cts.y) * 0.5f),
                        Theme::u32(Theme::Tokens::BRAND_INK), indi_nbuf);
            ImGui::PopFont();
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Add or remove indicator subplots");
        if (clicked && !open) ImGui::OpenPopup("indicators_popup");

        ImGui::SetNextWindowPos(ImVec2(rg_indic_x, ctrl_y + 28.0f + 4.0f));
        ImGui::SetNextWindowSize(ImVec2(312, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginPopup("indicators_popup")) {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const float pww = ImGui::GetWindowWidth();

            // ── header: INDICATORS ..... N ACTIVE ────────────────────
            {
                const ImVec2 hp = ImGui::GetCursorScreenPos();
                ImGui::PushFont(Theme::Fonts::label());
                pdl->AddText(ImVec2(hp.x + 14.0f, hp.y + 11.0f),
                             Theme::u32(Theme::Tokens::TX2), "INDICATORS");
                char ab[24];
                snprintf(ab, sizeof(ab), "%d ACTIVE", indi_active_n);
                const float aw = ImGui::CalcTextSize(ab).x;
                pdl->AddText(ImVec2(hp.x + pww - 14.0f - aw, hp.y + 11.0f),
                             Theme::u32(indi_active_n > 0 ? Theme::Tokens::BRAND_TX
                                                          : Theme::Tokens::TX3), ab);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(pww, 32.0f));
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                pdl->AddLine(lp, ImVec2(lp.x + pww, lp.y), Theme::u32(Theme::Tokens::BD1));
            }

            // section label (micro, text-3); optional hairline above
            auto indi_section = [&](const char* s, bool hairline_above) {
                if (hairline_above) {
                    ImGui::Dummy(ImVec2(pww, 6.0f));
                    const ImVec2 lp = ImGui::GetCursorScreenPos();
                    pdl->AddLine(lp, ImVec2(lp.x + pww, lp.y), Theme::u32(Theme::Tokens::BD1));
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushFont(Theme::Fonts::label());
                pdl->AddText(ImVec2(p.x + 14.0f, p.y + 10.0f), Theme::u32(Theme::Tokens::TX3), s);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(pww, 26.0f));
            };

            // one indicator row - label + square toggle switch (1k). Toggles
            // flip live; the menu stays open for multi-change. Returns 0 none,
            // 1 left-click (toggle), 2 right-click (settings).
            auto indi_row = [&](const char* label, bool active, const char* tip) -> int {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float rh = 32.0f;
                ImGui::PushID(label);
                const bool row_clicked = ImGui::InvisibleButton("##irow", ImVec2(pww, rh));
                const bool hov = ImGui::IsItemHovered();
                const bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                ImGui::PopID();
                if (hov)
                    pdl->AddRectFilled(ImVec2(p.x + 1.0f, p.y), ImVec2(p.x + pww - 1.0f, p.y + rh),
                                       Theme::u32(Theme::Tokens::ELEV));
                ImGui::PushFont(Theme::Fonts::mono());
                pdl->AddText(ImVec2(p.x + 14.0f, p.y + (rh - ImGui::GetFontSize()) * 0.5f),
                             Theme::u32(active ? Theme::Tokens::TX1 : Theme::Tokens::TX2), label);
                ImGui::PopFont();
                // toggle: 24x12 track, 8x8 knob, square (1k)
                const float tw = 24.0f, th2 = 12.0f;
                const ImVec2 t0(p.x + pww - 14.0f - tw, p.y + (rh - th2) * 0.5f);
                if (active) {
                    pdl->AddRectFilled(t0, ImVec2(t0.x + tw, t0.y + th2),
                                       Theme::u32(Theme::Tokens::BRAND_SOFT));
                    pdl->AddRect(t0, ImVec2(t0.x + tw, t0.y + th2),
                                 Theme::u32(Theme::Tokens::BRAND_LINE), 0.0f, 0, 1.0f);
                    pdl->AddRectFilled(ImVec2(t0.x + tw - 10.0f, t0.y + 2.0f),
                                       ImVec2(t0.x + tw - 2.0f, t0.y + 10.0f),
                                       Theme::u32(Theme::Tokens::BRAND));
                } else {
                    pdl->AddRectFilled(t0, ImVec2(t0.x + tw, t0.y + th2),
                                       Theme::u32(Theme::Tokens::ELEV));
                    pdl->AddRect(t0, ImVec2(t0.x + tw, t0.y + th2),
                                 Theme::u32(Theme::Tokens::BD1), 0.0f, 0, 1.0f);
                    pdl->AddRectFilled(ImVec2(t0.x + 2.0f, t0.y + 2.0f),
                                       ImVec2(t0.x + 10.0f, t0.y + 10.0f),
                                       Theme::u32(Theme::Tokens::TX3));
                }
                if (hov && tip) Theme::tooltip("%s", tip);
                return rclick ? 2 : (row_clicked ? 1 : 0);
            };

            indi_section("SUBPLOTS \xC2\xB7 RENDER IN ORDER", false);

            // Volume
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::VolumeIndicator>();
                if (indi_row("Volume", active, nullptr) == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::VolumeIndicator>();
                    else add_volume_indicator();
                }
            }

            // CVD
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::CVDIndicator>();
                if (indi_row("CVD", active, "Cumulative Volume Delta") == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::CVDIndicator>();
                    else add_cvd_indicator();
                }
            }

            // RSI
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::RSIIndicator>();
                if (indi_row("RSI", active, nullptr) == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::RSIIndicator>();
                    else add_rsi_indicator();
                }
            }

            // MACD
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::MACDIndicator>();
                if (indi_row("MACD", active, nullptr) == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::MACDIndicator>();
                    else add_macd_indicator();
                }
            }

            // Funding Rate
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::FundingRateIndicator>();
                if (indi_row("Funding Rate", active,
                             "Funding rate histogram (blue=longs pay, red=shorts pay)") == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::FundingRateIndicator>();
                    else add_funding_rate_indicator();
                }
            }

            // Open Interest
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::OIIndicator>();
                if (indi_row("Open Interest", active,
                             "Open interest candlesticks (green=OI up, red=OI down)") == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::OIIndicator>();
                    else add_oi_indicator();
                }
            }

            // VPIN / Toxicity (Indicators V1 Wave B - TRADER+)
            {
                const bool active = indicator_mgr_.has_indicator_of_type<Indicators::VPINIndicator>();
                if (indi_row("VPIN \xC2\xB7 Toxicity", active,
                             "Flow toxicity (VPIN, volume clock) with HMM regime coloring\nStep-hold shelves are honest: quiet symbols hold") == 1) {
                    if (active) indicator_mgr_.remove_indicator_of_type<Indicators::VPINIndicator>();
                    else add_vpin_indicator();
                }
            }

            // ── footer: hint strip (1k) ──────────────────────────────
            {
                ImGui::Dummy(ImVec2(pww, 4.0f));
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                pdl->AddLine(lp, ImVec2(lp.x + pww, lp.y), Theme::u32(Theme::Tokens::BD1));
                ImGui::PushFont(Theme::Fonts::label());
                pdl->AddText(ImVec2(lp.x + 14.0f, lp.y + 9.0f), Theme::u32(Theme::Tokens::TX3),
                             "TOGGLES FLIP LIVE \xC2\xB7 MENU STAYS OPEN");
                const char* esc = "ESC CLOSE";
                const float ew = ImGui::CalcTextSize(esc).x;
                pdl->AddText(ImVec2(lp.x + pww - 14.0f - ew, lp.y + 9.0f),
                             Theme::u32(Theme::Tokens::TX3), esc);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(pww, 28.0f));
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Right-aligned: Indicators + Settings (borderless). LOW->HIGH legend dropped in v2.
    // ═══════════════════════════════════════════════════════════════════════
    {
        // Settings: borderless text button (text-3), opens the liq Style/Intensity
        // panel. The old LOW->HIGH liq colormap legend was removed with the v2 bar
        // (the SPEC has no legend); rehome it onto the chart or into this panel later.
        ImGui::SetCursorScreenPos(ImVec2(rg_set_x, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##settings_btn", ImVec2(rg_set_w, 28.0f));
        const bool hot = ImGui::IsItemHovered();
        ImGui::PushFont(Theme::Fonts::ui());
        dl->AddText(ImVec2(rg_set_x + 6.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX3), "Settings");
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            Theme::tooltip("Chart settings: liquidation heatmap style and intensity");
        if (clicked) liq_settings_panel_.open();
    }

    // ── Deferred settings popup opens (from right-click in Indicators) ──
    if (open_liq_settings_) {
        liq_settings_panel_.open();
        open_liq_settings_ = false;
    }
    if (open_vpvr_settings_) {
        vpvr_settings_panel_.open();
        open_vpvr_settings_ = false;
    }
    // ── Settings popups (opened by right-clicking indicator items) ────────
    // These must be at toolbar scope, not inside the Indicators popup.
    // Liq heatmap settings - the design floating panel (Style / Intensity),
    // wired straight to the Field's chart_widget.h knobs. Low/Peak/tick-per-row/
    // half-life are baked into the cache at build → invalidate on change.
    liq_settings_panel_.set_panel_height(300.0f);
    if (liq_settings_panel_.begin()) {
        if (liq_settings_panel_.tab("Style")) {
            ImGui::Text("Colormap");
            auto cmap_btn = [&](const char* label, HeatmapColormap::LiqMap m, bool same_line) {
                if (same_line) ImGui::SameLine(0.0f, 5.0f);
                const bool on = (HeatmapColormap::liq_map() == m);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? Theme::Tokens::BRAND_SOFT
                                                          : Theme::Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Theme::Tokens::BRAND_SOFT
                                                                 : Theme::Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? Theme::Tokens::BRAND_TX
                                                        : Theme::Tokens::TX2);
                if (ImGui::Button(label)) HeatmapColormap::set_liq_map(m);
                ImGui::PopStyleColor(4);
            };
            cmap_btn("Ember",   HeatmapColormap::LiqMap::Ember,   false);
            cmap_btn("Inferno", HeatmapColormap::LiqMap::Inferno, true);
            cmap_btn("Magma",   HeatmapColormap::LiqMap::Magma,   true);
            cmap_btn("Viridis", HeatmapColormap::LiqMap::Viridis, true);
            ImGui::Spacing();
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Opacity", &liq_field_opacity_, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Field opacity - the alpha curve handles the melt\n"
                                  "(design 0.90)");
        }
        if (liq_settings_panel_.tab("Intensity")) {
            bool rebuild = false;
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Gamma", &liq_field_gamma_, 0.5f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Shaping on the log-mapped intensity - higher darkens\n"
                                  "the mid-band, deepens the black toe (design 2.2)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Low", &liq_field_lo_pct_, 0.0f, 0.90f, "p%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Intensity-Low clip percentile - higher culls weak plots\n"
                                  "into black gaps; lower fills movers denser");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Peak", &liq_field_hi_pct_, 0.950f, 1.000f, "p%.4f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Intensity-Peak clip percentile - higher makes yellow\n"
                                  "ignition rarer (design 0.9985)");
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Noise floor", &liq_field_floor_, 0.0f, 0.10f, "%.3f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Rendered-intensity floor - culls sub-visible fuel (design 0.02)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Tick-per-row", &liq_field_bps_, 2.0f, 20.0f, "%.0f bps");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Price-row height in bps of its own price (log grid)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Half-life", &liq_field_halflife_h_, 0.0f, 72.0f, "%.0f h");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Age-decay half-life on standing fuel - stale far-history\n"
                                  "dims sooner; 0 disables (design 16h, replay-safe)");
            if (rebuild) liq_field_sig_ts_ = -1;   // Low/Peak/bps/half-life bake at cache build
        }
        liq_settings_panel_.end();
    }
    // VPVR settings (tabbed panel)
    vpvr_settings_panel_.set_panel_height(280.0f);
    if (vpvr_settings_panel_.begin()) {
        if (vpvr_settings_panel_.tab("Display")) {
            const char* vp_modes[] = { "Standard", "Total Volume", "Total Delta" };
            int vp_mode_idx = static_cast<int>(ctx_.vpvr_mgr().mode());
            ImGui::SetNextItemWidth(130);
            if (ImGui::Combo("Mode##vp", &vp_mode_idx, vp_modes, 3)) {
                ctx_.vpvr_mgr().set_mode(static_cast<VolumeProfileManager::Mode>(vp_mode_idx));
            }
            ImGui::Spacing();
            bool vp_poc = ctx_.vpvr_mgr().show_poc();
            if (ImGui::Checkbox("POC Line##vp", &vp_poc)) ctx_.vpvr_mgr().set_show_poc(vp_poc);
            bool vp_va = ctx_.vpvr_mgr().show_vah_val();
            if (ImGui::Checkbox("VAH/VAL Lines##vp", &vp_va)) ctx_.vpvr_mgr().set_show_vah_val(vp_va);
            bool vp_vals = ctx_.vpvr_mgr().show_values();
            if (ImGui::Checkbox("Show Values##vp", &vp_vals)) ctx_.vpvr_mgr().set_show_values(vp_vals);
        }
        if (vpvr_settings_panel_.tab("Sizing")) {
            int vp_width_pct = static_cast<int>(ctx_.vpvr_mgr().width_pct() * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(130);
            if (ImGui::SliderInt("Width##vp", &vp_width_pct, 5, 50, "%d%%")) {
                ctx_.vpvr_mgr().set_width_pct(static_cast<float>(vp_width_pct) / 100.0f);
            }
            ImGui::Spacing();
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (vpvr_ticks_per_row_ == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.2f)", vpvr_ticks_per_row_ * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##vptpr") && vpvr_ticks_per_row_ > 0) {
                vpvr_ticks_per_row_ = std::max(0, vpvr_ticks_per_row_ - 5);
                vpvr_last_request_start_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##vptpr")) {
                vpvr_ticks_per_row_ += 5;
                vpvr_last_request_start_ = 0;
            }
            ImGui::SetNextItemWidth(60);
            int vptpr = vpvr_ticks_per_row_;
            if (ImGui::InputInt("##vptpr_input", &vptpr, 0, 0)) {
                vpvr_ticks_per_row_ = std::max(0, vptpr);
                vpvr_last_request_start_ = 0;
            }
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Price levels per row (0 = auto)");
            }
        }
        vpvr_settings_panel_.end();
    }
    // ── Footprint settings (always available) ──────────────────────────
    render_footprint_settings_popup();
    // ── TPO settings ────────────────────────────────────────────────────
    tpo_settings_panel_.set_panel_height(300.0f);
    if (tpo_settings_panel_.begin()) {
        auto& tpo = ctx_.tpo_mgr();
        if (tpo_settings_panel_.tab("General")) {
            const char* periods[] = { "Daily (24h)", "Weekly (168h)" };
            int period_idx = (tpo.session_period_hours == 168) ? 1 : 0;
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Session Period", &period_idx, periods, 2)) {
                tpo.session_period_hours = (period_idx == 1) ? 168 : 24;
                tpo.clear(pair_.symbol);
            }
            ImGui::Checkbox("Session Header", &tpo.show_session_header);
            ImGui::Checkbox("Highlight Start/End", &tpo.highlight_start_end);
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Profile Spacing", &tpo.profile_spacing, 1, 5);
            tpo.profile_spacing = std::max(0, std::min(20, tpo.profile_spacing));
        }
        if (tpo_settings_panel_.tab("Session")) {
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (tpo.ticks_per_row_setting == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.1f)", tpo.ticks_per_row_setting * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##tpotpr") && tpo.ticks_per_row_setting > 0) {
                tpo.ticks_per_row_setting = std::max(0, tpo.ticks_per_row_setting - 50);
                tpo.clear(pair_.symbol);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##tpotpr")) {
                tpo.ticks_per_row_setting += 50;
                tpo.clear(pair_.symbol);
            }
            ImGui::SetNextItemWidth(80);
            int tpr = tpo.ticks_per_row_setting;
            if (ImGui::InputInt("##tpotpr_input", &tpr, 0, 0)) {
                tpo.ticks_per_row_setting = std::max(0, tpr);
                tpo.clear(pair_.symbol);
            }
            ImGui::Spacing();
            ImGui::Checkbox("POC Ray", &tpo.show_poc_ray);
            ImGui::Checkbox("VAH/VAL Rays", &tpo.show_vah_val_rays);
            ImGui::Checkbox("Poor High/Low", &tpo.show_poor_high_low);
            ImGui::Checkbox("Initial Balance", &tpo.show_initial_balance);
        }
        if (tpo_settings_panel_.tab("Value Area")) {
            ImGui::SetNextItemWidth(80);
            ImGui::SliderFloat("Value Area %", &tpo.value_area_pct,
                               0.50f, 0.90f, "%.2f");
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Percentage of total blocks for value area\n"
                                  "Standard: 0.70 (70%%)");
            }
        }
        if (tpo_settings_panel_.tab("Single Prints")) {
            ImGui::Checkbox("Show Single Prints", &tpo.show_single_prints);
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Highlight rows with exactly 1 block\n"
                                  "where adjacent rows have >1 block.\n"
                                  "Indicates inefficient price movement.");
            }
        }
        tpo_settings_panel_.end();
    }
    // Bottom hairline (line-1) against the chart, then drop the layout cursor to
    // the bar's bottom so the chart begins directly below the 44px band.
    dl->AddLine(ImVec2(bp.x, bp.y + bar_h - 0.5f), ImVec2(bp.x + ww, bp.y + bar_h - 0.5f),
                Theme::u32(Theme::Tokens::BD1), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bar_h));

    ImGui::PopStyleVar(2);
}

void ChartWidget::handle_plot_interaction() {
    // While the drawing layer owns the mouse (armed tool, placement, drag, or
    // hover over a drawing), the chart's own click/drag handlers stand down.
    const bool draw_cap = drawing_layer_.captures_mouse();

    // Capture the chart plot rect EVERY frame, not only when the chart is
    // hovered: the indicator subplots reuse first_plot_min/last_plot_max for
    // the shared crosshair, and hovering a pane while these still held the
    // frame-start reset (0,0) dragged the vertical line to the window's
    // left edge. render_tabbed overwrites last_plot_max with the bottom
    // pane's rect right after this.
    crosshair_state_.first_plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    crosshair_state_.first_plot_max = ImVec2(
        crosshair_state_.first_plot_min.x + plot_size.x,
        crosshair_state_.first_plot_min.y + plot_size.y
    );
    crosshair_state_.last_plot_max = crosshair_state_.first_plot_max;

    if (ImPlot::IsPlotHovered()) {
        crosshair_state_.is_active = true;
        crosshair_state_.plot_pos = ImPlot::GetPlotMousePos();
        crosshair_state_.chart_hovered = true;
        crosshair_state_.indicator_hovered = false;

        crosshair_state_.hovered_plot_min = crosshair_state_.first_plot_min;
        crosshair_state_.hovered_plot_max = crosshair_state_.first_plot_max;

        const ImPlotRect limits = ImPlot::GetPlotLimits();
        crosshair_state_.hovered_y_min = limits.Y.Min;
        crosshair_state_.hovered_y_max = limits.Y.Max;

        // Detect user interaction → stop following live (a drawing drag is
        // not a pan - the layer suppresses the pan, so keep follow-live)
        if (ImGui::GetIO().MouseWheel != 0 ||
            (!draw_cap && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            ctx_.candle_mgr().set_follow_live(false);
        }

        // ─── Replay: Shift+drag to select time range ────────────
        if (!draw_cap && ImGui::GetIO().KeyShift &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            replay_selection_.active = true;
            replay_selection_.start_ms = static_cast<int64_t>(crosshair_state_.plot_pos.x);
            replay_selection_.end_ms = replay_selection_.start_ms;
        }
        if (replay_selection_.active && ImGui::GetIO().KeyShift &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            replay_selection_.end_ms = static_cast<int64_t>(crosshair_state_.plot_pos.x);
        }
        if (replay_selection_.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            replay_selection_.active = false;
            // Selection remains stored for context menu use
        }

        // ─── R key: replay from crosshair → focused view ───────────────
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
            !ImGui::GetIO().WantTextInput &&
            !ctx_.replay_mgr().is_active()) {
            // Snap to candle boundary so replay starts at exact candle start
            int64_t raw_time = static_cast<int64_t>(crosshair_state_.plot_pos.x);
            int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000;
            int64_t cursor_time_ms = (raw_time / tf_ms) * tf_ms;
            if (cursor_time_ms > 0)
                ctx_.replay_mgr().open_focused_replay_at(pair_.symbol, cursor_time_ms);
        }

        // ─── Right-click → open context menu ────────────────────
        // In TPO mode, right-click is handled by render_tpo() for session context
        if (!draw_cap && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            chart_type_ != ChartType::TPO) {
            // Capture timestamp NOW while plot is still hovered.
            // Once the popup opens, ImPlot no longer reports hovered and
            // crosshair_state_ gets zeroed on the next frame.
            // Snap to candle boundary (floor) so replay starts at exact candle start
            {
                int64_t raw_time = static_cast<int64_t>(crosshair_state_.plot_pos.x);
                int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000;
                context_menu_time_ms_ = (raw_time / tf_ms) * tf_ms;
                // Research reads a MINUTE: floor the SAME capture to 60000, in
                // this block, while the plot is still hovered. Reusing the
                // timeframe floor above would research a minute up to hours
                // away from the click on a 4H chart (the spec's named trap).
                context_menu_minute_ms_ = research_url::floor_minute_ms(raw_time);
                context_menu_price_ = crosshair_state_.plot_pos.y;
            }
            ImGui::OpenPopup("##ChartCtx");
        }
    }

    // ─── Measure ruler, price-alert lines + toast (drawn in the plot) ───────
    {
        ImDrawList* pdl = ImPlot::GetPlotDrawList();
        const ImVec2 p_min = ImPlot::GetPlotPos();
        const ImVec2 p_sz  = ImPlot::GetPlotSize();
        const float  p_r   = p_min.x + p_sz.x;
        const double x_mid = (x_axis_min_ + x_axis_max_) * 0.5;

        // Alert level lines (dashed) with a right-edge price pill + ✕ to remove.
        for (size_t i = 0; i < alerts_.size();) {
            const double ap = alerts_[i].price;
            const ImVec2 pt = ImPlot::PlotToPixels(ImPlotPoint(x_mid, ap));
            const float ly = pt.y;
            if (ly >= p_min.y && ly <= p_min.y + p_sz.y) {
                const ImU32 acol = Theme::u32(Theme::Tokens::WARN);
                for (float x = p_min.x; x < p_r - 58.0f; x += 8.0f)
                    pdl->AddLine(ImVec2(x, ly), ImVec2(std::min(x + 4.0f, p_r - 58.0f), ly), acol, 1.0f);
                char apb[32]; fmt_.format_price(apb, sizeof(apb), ap);
                char lbl[40]; snprintf(lbl, sizeof(lbl), "\xe2\x97\x89 %s", apb);
                const ImVec2 ts = ImGui::CalcTextSize(lbl);
                const ImVec2 bx0(p_r - ts.x - 34.0f, ly - ts.y * 0.5f - 3.0f);
                const ImVec2 bx1(p_r - 16.0f, ly + ts.y * 0.5f + 3.0f);
                pdl->AddRectFilled(bx0, bx1, Theme::u32(Theme::Tokens::PANEL), 3.0f);
                pdl->AddRect(bx0, bx1, acol, 3.0f, 0, 1.0f);
                pdl->AddText(ImVec2(bx0.x + 6.0f, ly - ts.y * 0.5f), acol, lbl);
                const ImVec2 xc(p_r - 8.0f, ly);
                pdl->AddText(ImVec2(xc.x - 4.0f, ly - ts.y * 0.5f), Theme::u32(Theme::Tokens::TX3), "\xc3\x97");
                const bool over_x = ImGui::IsMouseHoveringRect(ImVec2(xc.x - 8.0f, ly - 8.0f),
                                                               ImVec2(xc.x + 8.0f, ly + 8.0f));
                if (over_x && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    alerts_.erase(alerts_.begin() + static_cast<long>(i));
                    continue;
                }
            }
            ++i;
        }

        // Measure ruler: anchor → cursor, live Δ readout. Click/Esc clears it.
        if (measure_.active) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyShift &&
                 ImPlot::IsPlotHovered())) {
                measure_.active = false;
            } else {
                const double t1 = crosshair_state_.plot_pos.x;
                const double q1 = crosshair_state_.plot_pos.y;
                const double dprice = q1 - measure_.p0;
                const double dpct = measure_.p0 != 0.0 ? (dprice / measure_.p0) * 100.0 : 0.0;
                const int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000;
                const int64_t dt_ms = std::llabs(static_cast<int64_t>(t1) - measure_.t0_ms);
                const long bars = tf_ms > 0 ? static_cast<long>(dt_ms / tf_ms) : 0;
                const ImVec2 a = ImPlot::PlotToPixels(ImPlotPoint((double)measure_.t0_ms, measure_.p0));
                const ImVec2 b = ImPlot::PlotToPixels(ImPlotPoint(t1, q1));
                const bool up = dprice >= 0.0;
                const ImU32 col  = up ? Theme::u32(Theme::Tokens::UP) : Theme::u32(Theme::Tokens::DOWN);
                const ImU32 fill = up ? IM_COL32(47, 214, 173, 34) : IM_COL32(238, 92, 120, 34);
                const ImVec2 r0(std::min(a.x, b.x), std::min(a.y, b.y));
                const ImVec2 r1(std::max(a.x, b.x), std::max(a.y, b.y));
                pdl->AddRectFilled(r0, r1, fill);
                pdl->AddRect(r0, r1, col, 0.0f, 0, 1.0f);
                char l1[64], l2[72], pb[32];
                fmt_.format_price(pb, sizeof(pb), std::fabs(dprice));
                snprintf(l1, sizeof(l1), "%s%.2f%%  %s%s", up ? "+" : "-", std::fabs(dpct),
                         up ? "+" : "-", pb);
                const long long s = static_cast<long long>(dt_ms / 1000);
                snprintf(l2, sizeof(l2), "%ld bars  \xc2\xb7  %02lld:%02lld:%02lld", bars,
                         s / 3600, (s % 3600) / 60, s % 60);
                const ImVec2 ts1 = ImGui::CalcTextSize(l1);
                const ImVec2 ts2 = ImGui::CalcTextSize(l2);
                const float bw = std::max(ts1.x, ts2.x) + 16.0f;
                const float bh = ts1.y + ts2.y + 12.0f;
                ImVec2 lb(b.x + 12.0f, b.y - bh - 8.0f);
                if (lb.x + bw > p_r) lb.x = b.x - bw - 12.0f;
                if (lb.y < p_min.y) lb.y = b.y + 12.0f;
                pdl->AddRectFilled(lb, ImVec2(lb.x + bw, lb.y + bh), Theme::u32(Theme::Tokens::PANEL), 4.0f);
                pdl->AddRect(lb, ImVec2(lb.x + bw, lb.y + bh), col, 4.0f, 0, 1.0f);
                pdl->AddText(ImVec2(lb.x + 8.0f, lb.y + 5.0f), col, l1);
                pdl->AddText(ImVec2(lb.x + 8.0f, lb.y + 5.0f + ts1.y + 2.0f),
                             Theme::u32(Theme::Tokens::TX2), l2);
            }
        }

        // Alert toast (top-center of the plot, ~6s).
        if (alert_toast_until_ > ImGui::GetTime() && !alert_toast_msg_.empty()) {
            const ImVec2 ts = ImGui::CalcTextSize(alert_toast_msg_.c_str());
            const float bw = ts.x + 44.0f, bh = ts.y + 14.0f;
            const ImVec2 t0(p_min.x + (p_sz.x - bw) * 0.5f, p_min.y + 12.0f);
            pdl->AddRectFilled(t0, ImVec2(t0.x + bw, t0.y + bh), Theme::u32(Theme::Tokens::ELEV), 5.0f);
            pdl->AddRect(t0, ImVec2(t0.x + bw, t0.y + bh), Theme::u32(Theme::Tokens::WARN), 5.0f, 0, 1.0f);
            pdl->AddCircleFilled(ImVec2(t0.x + 16.0f, t0.y + bh * 0.5f), 3.5f, Theme::u32(Theme::Tokens::WARN));
            pdl->AddText(ImVec2(t0.x + 28.0f, t0.y + 7.0f), Theme::u32(Theme::Tokens::TX1),
                         alert_toast_msg_.c_str());
        }
    }

    // ─── Context menu popup (must be outside IsPlotHovered block) ────
    // 450 wide (measured): narrower clipped the MenuItem shortcut values
    // the replay datetime, the copy price). The widest label ("Replay
    // range (shift-drag to select)") sets the label column; the replay
    // datetime needs ~120px after it.
    ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 11));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX1);
    if (ImGui::BeginPopup("##ChartCtx")) {
        // Menu chrome is monospace (JetBrains Mono) to match the design (1f).
        ImGui::PushFont(Theme::Fonts::mono_sm());
        // Soft drop shadow (floating menus get rounding + shadow).
        {
            ImDrawList* d = ImGui::GetWindowDrawList();
            const ImVec2 wa = ImGui::GetWindowPos();
            const ImVec2 wb(wa.x + ImGui::GetWindowSize().x, wa.y + ImGui::GetWindowSize().y);
            d->PushClipRectFullScreen();
            for (int i = 5; i >= 1; --i) {
                const float e = static_cast<float>(i) * 2.0f;
                d->AddRect(ImVec2(wa.x - e, wa.y - e + 3.0f), ImVec2(wb.x + e, wb.y + e + 3.0f),
                           IM_COL32(0, 0, 0, 14), Theme::Radius::R3 + e, 0, 1.6f);
            }
            d->PopClipRect();
        }

        const bool replaying = ctx_.replay_mgr().is_active();
        char price_buf[32];
        fmt_.format_price(price_buf, sizeof(price_buf), context_menu_price_);

        // Add alert - live chart only (an alert on a historical replay is meaningless).
        if (!replaying) {
            if (ImGui::MenuItem("Add alert", price_buf)) {
                PriceAlert al;
                al.price = context_menu_price_;
                al.arm_above = (context_menu_price_ > ctx_.candle_mgr().last_close_price());
                alerts_.push_back(al);
                alert_last_price_ = ctx_.candle_mgr().last_close_price();
#ifdef __EMSCRIPTEN__
                EM_ASM({
                    if (window.Notification && Notification.permission === 'default')
                        Notification.requestPermission();
                });
#endif
                ImGui::CloseCurrentPopup();
            }
        }

        // Measure ruler - anchor here; readout follows the cursor (click/Esc clears).
        if (ImGui::MenuItem("Measure from here")) {
            measure_.active = true;
            measure_.t0_ms  = context_menu_time_ms_;
            measure_.p0     = context_menu_price_;
            ImGui::CloseCurrentPopup();
        }

        // Investigate this minute - a READ, not a replay, so it sits above the
        // replay block. Uses the MINUTE capture (context_menu_minute_ms_), never
        // the timeframe-floored replay timestamp beside it.
        render_investigate_menu_item(pair_, context_menu_minute_ms_);

        ImGui::Separator();

        // Replay actions (entitlement-gated inside): Start Replay · <time>,
        // Replay range…, Stop Replay · Esc.
        ctx_.replay_mgr().render_chart_context_menu(
            pair_.symbol,
            context_menu_time_ms_,
            ctx_.candle_mgr().timeframe_seconds(),
            replay_selection_.start_ms,
            replay_selection_.end_ms
        );

        ImGui::Separator();

        if (ImGui::MenuItem("Copy price", price_buf)) {
            ImGui::SetClipboardText(price_buf);
        }

        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}

// Crosshair

void ChartWidget::render_crosshair(const ImPlotPoint& mouse_pos) const {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    // Snap to nearest candle timestamp
    const double timeframe_ms = static_cast<double>(tf_sec) * 1000.0;
    const double snapped_time = std::round(mouse_pos.x / timeframe_ms) * timeframe_ms;
    const float line_top = crosshair_state_.first_plot_min.y;
    const float line_bottom = crosshair_state_.last_plot_max.y;
    const double x_range = x_axis_max_ - x_axis_min_;
    if (x_range <= 0) return;
    const double x_normalized = (snapped_time - x_axis_min_) / x_range;
    const float plot_width = crosshair_state_.last_plot_max.x - crosshair_state_.first_plot_min.x;
    const float snap_x = crosshair_state_.first_plot_min.x + static_cast<float>(x_normalized * plot_width);
    // Y position based on hovered plot
    float mouse_y = 0.0f;
    double y_range = crosshair_state_.hovered_y_max - crosshair_state_.hovered_y_min;
    if (y_range > 0) {
        const double y_normalized = (mouse_pos.y - crosshair_state_.hovered_y_min) / y_range;
        const float plot_height = crosshair_state_.hovered_plot_max.y - crosshair_state_.hovered_plot_min.y;
        mouse_y = crosshair_state_.hovered_plot_max.y - static_cast<float>(y_normalized * plot_height);
    }
    ImU32 crosshair_color = IM_COL32(128, 128, 128, 180);
    float dash_length = 4.0f;
    float gap_length = 4.0f;
    // Vertical dashed line
    for (float y = line_top; y < line_bottom; y += dash_length + gap_length) {
        float y_end = std::min(y + dash_length, line_bottom);
        draw_list->AddLine(ImVec2(snap_x, y), ImVec2(snap_x, y_end), crosshair_color, 1.0f);
    }
    // Horizontal dashed line (within hovered plot only)
    float h_left = crosshair_state_.hovered_plot_min.x;
    float h_right = crosshair_state_.hovered_plot_max.x;
    for (float x = h_left; x < h_right; x += dash_length + gap_length) {
        float x_end = std::min(x + dash_length, h_right);
        draw_list->AddLine(ImVec2(x, mouse_y), ImVec2(x_end, mouse_y), crosshair_color, 1.0f);
    }
    // Price label on Y-axis (main chart)
    if (crosshair_state_.chart_hovered && y_range > 0) {
        char price_label[32];
        snprintf(price_label, sizeof(price_label), fmt_.price_fmt, mouse_pos.y);
        const ImVec2 price_text_size = ImGui::CalcTextSize(price_label);
        constexpr float padding = 4.0f;
        auto price_bg_min = ImVec2(
            crosshair_state_.hovered_plot_max.x + 2.0f,
            mouse_y - price_text_size.y * 0.5f - padding
        );
        auto price_bg_max = ImVec2(
            price_bg_min.x + price_text_size.x + padding * 2,
            mouse_y + price_text_size.y * 0.5f + padding
        );
        draw_list->AddRectFilled(price_bg_min, price_bg_max, IM_COL32(45, 45, 48, 240), 2.0f);
        draw_list->AddRect(price_bg_min, price_bg_max, IM_COL32(100, 100, 105, 255), 2.0f, 0, 1.0f);
        draw_list->AddText(
            ImVec2(price_bg_min.x + padding, price_bg_min.y + padding),
            IM_COL32(230, 230, 235, 255),
            price_label
        );
    }

    // Y-axis value label on indicator subplots
    if (crosshair_state_.indicator_hovered && y_range > 0) {
        char val_label[32];
        // Use the hovered indicator's own Y-axis formatter if available
        if (crosshair_state_.indicator_y_formatter) {
            crosshair_state_.indicator_y_formatter(mouse_pos.y, val_label, sizeof(val_label), nullptr);
        } else {
            // Fallback: generic abbreviated format
            const double abs_val = std::abs(mouse_pos.y);
            if (abs_val >= 1e9)       snprintf(val_label, sizeof(val_label), "%.2fB", mouse_pos.y / 1e9);
            else if (abs_val >= 1e6)  snprintf(val_label, sizeof(val_label), "%.2fM", mouse_pos.y / 1e6);
            else if (abs_val >= 1e3)  snprintf(val_label, sizeof(val_label), "%.1fK", mouse_pos.y / 1e3);
            else                      snprintf(val_label, sizeof(val_label), "%.1f", mouse_pos.y);
        }
        const ImVec2 val_text_size = ImGui::CalcTextSize(val_label);
        constexpr float padding = 4.0f;
        auto val_bg_min = ImVec2(
            crosshair_state_.hovered_plot_max.x + 2.0f,
            mouse_y - val_text_size.y * 0.5f - padding
        );
        auto val_bg_max = ImVec2(
            val_bg_min.x + val_text_size.x + padding * 2,
            mouse_y + val_text_size.y * 0.5f + padding
        );
        draw_list->AddRectFilled(val_bg_min, val_bg_max, IM_COL32(45, 45, 48, 240), 2.0f);
        draw_list->AddRect(val_bg_min, val_bg_max, IM_COL32(100, 100, 105, 255), 2.0f, 0, 1.0f);
        draw_list->AddText(
            ImVec2(val_bg_min.x + padding, val_bg_min.y + padding),
            IM_COL32(230, 230, 235, 255),
            val_label
        );
    }

    // Time label on X-axis
    char time_label[48];
    if (!DisplayTimeZone::instance().format(static_cast<int64_t>(snapped_time),
                                             TimeZoneFormat::DateTimeSeconds,
                                             time_label, sizeof(time_label))) {
        snprintf(time_label, sizeof(time_label), "Invalid Time");
    }

    const ImVec2 time_text_size = ImGui::CalcTextSize(time_label);
    constexpr float t_padding = 6.0f;

    auto time_bg_min = ImVec2(
        snap_x - time_text_size.x * 0.5f - t_padding,
        line_bottom + 4.0f
    );
    auto time_bg_max = ImVec2(
        snap_x + time_text_size.x * 0.5f + t_padding,
        line_bottom + 4.0f + time_text_size.y + t_padding * 2
    );

    time_bg_min.x = std::max(time_bg_min.x, crosshair_state_.first_plot_min.x + 2.0f);
    time_bg_max.x = std::min(time_bg_max.x, crosshair_state_.last_plot_max.x - 2.0f);

    draw_list->AddRectFilled(time_bg_min, time_bg_max, IM_COL32(45, 45, 48, 240), 2.0f);
    draw_list->AddRect(time_bg_min, time_bg_max, IM_COL32(100, 100, 105, 255), 2.0f, 0, 1.0f);

    const float text_x = (time_bg_min.x + time_bg_max.x - time_text_size.x) * 0.5f;
    draw_list->AddText(
        ImVec2(text_x, time_bg_min.y + t_padding),
        IM_COL32(230, 230, 235, 255),
        time_label
    );
}


// Indicators

void ChartWidget::render_indicators() {
    // Renko uses a brick-index X-axis; the time-aligned indicator sub-pane would
    // not align to it (v1). Skip the pane entirely in Renko.
    if (chart_type_ == ChartType::Renko) return;
    if (indicator_mgr_.count() == 0) return;
    indicator_mgr_.render_tabbed(stored_x_min_, stored_x_max_, this,
                                 &crosshair_state_);
}

void ChartWidget::populate_volume_data(Indicators::VolumeIndicator* vol_ind) const {
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        const bool bullish = candle.close >= candle.open;
        // Convert base volume to quote volume (USDT) for display
        vol_ind->add_bar(candle.timestamp_ms, candle.volume * candle.close, bullish);
    }
}

void ChartWidget::add_volume_indicator() {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::VolumeIndicator>()) return;

    auto volume_ind = std::make_unique<Indicators::VolumeIndicator>();
    volume_ind->set_timeframe(ctx_.candle_mgr().timeframe_seconds());
    populate_volume_data(volume_ind.get());
    volume_ind->update();
    indicator_mgr_.add_indicator(std::move(volume_ind));
}

void ChartWidget::update_volume_indicators() {
    auto* vol_ind = indicator_mgr_.get_indicator_of_type<Indicators::VolumeIndicator>();
    if (!vol_ind) return;

    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        const bool bullish = bc.close >= bc.open;
        vol_ind->set_current_bar(bc.timestamp_ms, bc.volume * bc.close, bullish);
    }
    vol_ind->update();
}

void ChartWidget::add_cvd_indicator() {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::CVDIndicator>()) return;

    auto cvd_ind = std::make_unique<Indicators::CVDIndicator>();
    cvd_ind->set_timeframe(ctx_.candle_mgr().timeframe_seconds());
    populate_cvd_data(cvd_ind.get());
    cvd_ind->update();
    indicator_mgr_.add_indicator(std::move(cvd_ind));
}

void ChartWidget::populate_cvd_data(Indicators::CVDIndicator* cvd_ind) const {
    // Safety: ensure we start from empty state.
    // Callers should clear() before calling, but guard here too.
    if (cvd_ind->bar_count() > 0) {
        cvd_ind->clear();
    }
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        // Check if we have intra-candle CVD wick data from Volume stream
        auto it = cvd_wick_cache_.find(candle.timestamp_ms);
        if (it != cvd_wick_cache_.end()) {
            // Use Volume stream data with real intra-candle CVD high/low
            const double delta = (candle.vbuy - candle.vsell) * candle.close;
            cvd_ind->add_candle_with_cvd(candle.timestamp_ms, delta,
                                         it->second.cvd_high, it->second.cvd_low);
        } else {
            // Fallback: candle data only (no wicks)
            cvd_ind->add_candle(candle.timestamp_ms,
                                candle.vbuy * candle.close,
                                candle.vsell * candle.close);
        }
    }
}

void ChartWidget::handle_volume(const Terminal::Volume& vol) {
    // Store CVD wick data for the CVD indicator to pick up on next rebuild.
    // cvd/cvd_high/cvd_low are per-candle values from the VolumeActor.
    if (vol.cvd_high != 0 || vol.cvd_low != 0) {
        cvd_wick_cache_[vol.timestamp_ms] = CVDWickData{vol.cvd_high, vol.cvd_low};
    }
}

void ChartWidget::update_cvd_indicator() {
    auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
    if (!cvd_ind) return;

    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        cvd_ind->set_building_candle(bc.timestamp_ms,
                                     bc.vbuy * bc.close,
                                     bc.vsell * bc.close);
    }
    cvd_ind->update();
}

// ═══ Funding Rate Indicator ═══

void ChartWidget::handle_stat_for_chart(const Terminal::Stat& stat) {
    if (stat.funding != 0) {
        funding_cache_[stat.timestamp_ms] = stat.funding;
        latest_funding_rate_ = stat.funding;
        funding_data_dirty_ = true;
    }
    // Historical OI OHLC (from get_historical_oi batch)
    if (stat.oi_close > 0) {
        oi_ohlc_cache_[stat.timestamp_ms] = {stat.oi_open, stat.oi_high, stat.oi_low, stat.oi_close};
        latest_oi_usd_ = stat.oi_close;
        oi_data_dirty_ = true;
        // Debug: log first few historical OI arrivals
        static int oi_hist_count = 0;
        if (++oi_hist_count <= 5) {
        }
    }
    // Live OI snapshot (from live stat stream)
    if (stat.open_interest_usd > 0) {
        oi_cache_[stat.timestamp_ms] = stat.open_interest_usd;
        latest_oi_usd_ = stat.open_interest_usd;
    }
}

void ChartWidget::add_funding_rate_indicator() {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::FundingRateIndicator>()) return;

    // Request historical funding data if we haven't already
    if (!funding_history_requested_) {
        funding_history_requested_ = true;
        const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
        const int count = static_cast<int>(ctx_.candle_mgr().count());
        ctx_.stream_mgr().request_historical_funding(pair_, tf_sec, count > 0 ? count : 2880);
    }

    auto ind = std::make_unique<Indicators::FundingRateIndicator>();
    ind->set_timeframe(ctx_.candle_mgr().timeframe_seconds());
    populate_funding_data(ind.get());
    ind->update();
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::populate_funding_data(Indicators::FundingRateIndicator* ind) const {
    // Funding data comes from stats stream, keyed by candle timestamp.
    // For candles that don't have a matching stat, use the most recent known funding.
    double last_funding = 0;
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        auto it = funding_cache_.find(candle.timestamp_ms);
        if (it != funding_cache_.end()) {
            last_funding = it->second;
        }
        ind->add_bar(candle.timestamp_ms, last_funding);
    }
}

void ChartWidget::update_funding_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
    if (!ind) return;

    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        ind->set_building_bar(bc.timestamp_ms, latest_funding_rate_);
    }
    ind->update();
}

// ═══ OI Indicator ═══

void ChartWidget::add_oi_indicator() {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::OIIndicator>()) return;

    // Request historical OI if we haven't already
    request_historical_oi();

    auto ind = std::make_unique<Indicators::OIIndicator>();
    ind->set_timeframe(ctx_.candle_mgr().timeframe_seconds());
    populate_oi_data(ind.get());
    ind->update();
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::request_historical_oi() {
    if (oi_history_requested_) return;
    oi_history_requested_ = true;
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const int count = static_cast<int>(ctx_.candle_mgr().count());
    ctx_.stream_mgr().request_historical_oi(pair_, tf_sec, count > 0 ? count : 2880);
}

void ChartWidget::populate_oi_data(Indicators::OIIndicator* ind) const {
    double last_close = 0;
    int ohlc_hits = 0, live_hits = 0, fwd_fills = 0, gaps = 0;
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        // Prefer OHLC data from historical OI query
        auto ohlc_it = oi_ohlc_cache_.find(candle.timestamp_ms);
        if (ohlc_it != oi_ohlc_cache_.end()) {
            const auto& ohlc = ohlc_it->second;
            ind->add_candle_ohlc(candle.timestamp_ms, ohlc.open, ohlc.high, ohlc.low, ohlc.close);
            last_close = ohlc.close;
            ohlc_hits++;
            continue;
        }
        // Fallback: live OI snapshot (no wicks)
        auto it = oi_cache_.find(candle.timestamp_ms);
        if (it != oi_cache_.end()) {
            ind->add_oi(candle.timestamp_ms, it->second);
            last_close = it->second;
            live_hits++;
            continue;
        }
        // Forward-fill: use previous close as flat bar to avoid gaps
        if (last_close > 0) {
            ind->add_candle_ohlc(candle.timestamp_ms, last_close, last_close, last_close, last_close);
            fwd_fills++;
        } else {
            gaps++;
        }
    }
}

void ChartWidget::update_oi_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
    if (!ind) return;

    if (ctx_.candle_mgr().has_building_candle()) {
        const auto& bc = ctx_.candle_mgr().building_candle();
        if (latest_oi_usd_ > 0) {
            ind->set_building_oi(bc.timestamp_ms, latest_oi_usd_);
        }
    }
    ind->update();
}

// ═══ VPIN / Toxicity Indicator (Indicators V1 S1b - pathfinder) ═══

void ChartWidget::add_vpin_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::VPINIndicator>()) return;

    auto ind = std::make_unique<Indicators::VPINIndicator>();
    // Populate from whatever the SeriesCache already holds (live accrual /
    // replay seed); the update loop backfills via get_historical_vpin.
    if (ctx_.series) {
        ind->set_points(ctx_.series_mgr().vpin(pair_.symbol));
        vpin_populated_revision_ = ctx_.series_mgr().vpin_revision(pair_.symbol);
    }
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::update_vpin_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::VPINIndicator>();
    if (!ind || !ctx_.series) return;

    const bool is_replay = ctx_.candle_mgr().replay_start_time_ms() > 0;

    // Live STATE_VPIN subscribe - lazy, once, never during replay (the
    // replay StreamManager is subscribe-inert anyway; seeds deliver data).
    if (!vpin_subscribed_ && !is_replay) {
        const StreamKey key{pair_, Terminal::Stream::VPINState, 0};
        ctx_.stream_mgr().send_subscribe(key);
        vpin_subscribed_ = true;
    }

    // Chart-load history (F3 absolute range + chunk-until-covered).
    // The server keeps the MOST RECENT `count` rows of a window, so a wide
    // window truncates its OLD side - coverage must converge by chunking
    // [candle_front, series_oldest) until the gap closes, a chunk lands
    // empty (series floor), or the initial response is still in flight.
    // This also covers scroll-back: candle backfill moves the target left
    // and the same loop keeps chunking. end_ms of the initial request is 0
    // (server end = now; replay sessions clamp to the playback head).
    if (ctx_.candle_mgr().is_initial_load_complete() && !ctx_.candle_mgr().empty()) {
        const int64_t target_ms = ctx_.candle_mgr().candles().front().timestamp_ms;
        if (!vpin_history_requested_) {
            vpin_history_requested_ = true;
            vpin_hist_last_req_ = std::chrono::steady_clock::now();
            ctx_.stream_mgr().request_historical_vpin(pair_, target_ms, 0, 5000);
        } else if (!vpin_hist_exhausted_) {
            const int64_t oldest_ms = ctx_.series_mgr().vpin_oldest_ts(pair_.symbol);
            if (oldest_ms > 0 && oldest_ms > target_ms + 3600'000) {  // >1h uncovered
                const auto now = std::chrono::steady_clock::now();
                if (now - vpin_hist_last_req_ > std::chrono::seconds(2)) {
                    if (vpin_hist_last_oldest_ == 0 ||
                        oldest_ms < vpin_hist_last_oldest_ - 1000) {
                        // First chunk, or the last one made progress → next.
                        vpin_hist_last_oldest_ = oldest_ms;
                        vpin_hist_last_req_ = now;
                        ctx_.stream_mgr().request_historical_vpin(
                            pair_, target_ms, oldest_ms, 5000);
                    } else {
                        // A chunk landed with nothing older - stop asking.
                        vpin_hist_exhausted_ = true;
                    }
                }
            }
        }
    }

    // Repopulate the render cache only when the series actually changed.
    const uint64_t rev = ctx_.series_mgr().vpin_revision(pair_.symbol);
    if (rev != vpin_populated_revision_) {
        ind->set_points(ctx_.series_mgr().vpin(pair_.symbol));
        vpin_populated_revision_ = rev;
    }
}

void ChartWidget::add_rsi_indicator(int period) {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::RSIIndicator>()) return;

    auto rsi_ind = std::make_unique<Indicators::RSIIndicator>(period);
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        rsi_ind->add_candle(candle.timestamp_ms, candle.close);
    }
    rsi_ind->update();
    indicator_mgr_.add_indicator(std::move(rsi_ind));
}

void ChartWidget::add_macd_indicator(int fast, int slow, int signal) {
    if (ctx_.candle_mgr().empty()) return;
    if (indicator_mgr_.has_indicator_of_type<Indicators::MACDIndicator>()) return;

    auto macd_ind = std::make_unique<Indicators::MACDIndicator>(fast, slow, signal);
    macd_ind->set_timeframe(ctx_.candle_mgr().timeframe_seconds());
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        macd_ind->add_candle(candle.timestamp_ms, candle.close);
    }
    macd_ind->update();
    indicator_mgr_.add_indicator(std::move(macd_ind));
}


// Time Axis

void ChartWidget::setup_time_axis_ticks(double visible_x_min, double visible_x_max) const {
    generate_time_ticks(visible_x_min, visible_x_max);
}

void ChartWidget::generate_time_ticks(double visible_x_min, double visible_x_max) const {
    ProfileScope _ps("TimeTicks");
    // Cached tick set (FPS item, 2026-07-05). The label SET only changes when
    // the first visible tick / interval / count changes - under live-follow at
    // 160fps that's once per interval crossing, not per frame. Cache key holds
    // every format-affecting input (the audit's "zoom-dependent format"
    // concern: interval + count capture zoom; chart_type + tf capture mode).
    // Storage is static and only mutated on a key change, then ptrs are built
    // in a SECOND pass (emplace-as-you-go dangled SSO c_str()s on realloc).
    // ImPlot::SetupAxisTicks copies labels into the frame's Ticker each call,
    // so it still runs every frame - browser-Intl formatting is skipped.
    static std::vector<double> tick_positions;
    static std::vector<std::string> tick_labels_storage;
    static std::vector<const char*> tick_labels_ptrs;
    static int64_t ck_first = -1, ck_interval = -1, ck_tf = -1;
    static uint64_t ck_timezone_generation = 0;
    static int     ck_count = -1, ck_chart_type = -1;

    const auto submit = [&]() {
        if (!tick_positions.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_X1,
                tick_positions.data(),
                static_cast<int>(tick_positions.size()),
                tick_labels_ptrs.data(),
                false);
        }
    };

    // ── Resolve (first_tick, interval, count) arithmetically - no tm calls ──
    int64_t interval_s, first_tick_s;
    int64_t tf_sec = 0;
    if (chart_type_ == ChartType::TPO) {
        // TPO mode: daily ticks only, date labels (no intraday times)
        interval_s = 86400;
        const auto start_s = static_cast<int64_t>(visible_x_min / 1000.0);
        const int64_t aligned = (start_s / interval_s) * interval_s;
        first_tick_s = aligned;
        while (first_tick_s * 1000.0 < visible_x_min) first_tick_s += interval_s;
    } else {
        tf_sec = ctx_.candle_mgr().timeframe_seconds();
        const double visible_span = visible_x_max - visible_x_min;
        const double visible_candles = visible_span / (static_cast<double>(tf_sec) * 1000.0);
        interval_s = calculate_tick_interval(visible_candles);
        const auto start_s = static_cast<int64_t>(visible_x_min / 1000.0);
        first_tick_s = align_to_interval(start_s, interval_s);
        while (first_tick_s * 1000.0 < visible_x_min) first_tick_s += interval_s;
    }
    const auto end_s = static_cast<int64_t>(visible_x_max / 1000.0);
    int count = 0;
    if (first_tick_s <= end_s) {
        count = static_cast<int>((end_s - first_tick_s) / interval_s) + 1;
        // Honor the exact original bound (tick_ms <= visible_x_max)
        while (count > 0 &&
               (first_tick_s + static_cast<int64_t>(count - 1) * interval_s) * 1000.0 > visible_x_max) {
            count--;
        }
    }

    // ── Cache hit → resubmit stored arrays, skip regeneration ──
    if (first_tick_s == ck_first && interval_s == ck_interval && count == ck_count &&
        static_cast<int>(chart_type_) == ck_chart_type && tf_sec == ck_tf &&
        ck_timezone_generation == DisplayTimeZone::instance().generation() &&
        static_cast<int>(tick_positions.size()) == count) {
        submit();
        return;
    }
    ck_first = first_tick_s; ck_interval = interval_s; ck_count = count;
    ck_chart_type = static_cast<int>(chart_type_); ck_tf = tf_sec;
    ck_timezone_generation = DisplayTimeZone::instance().generation();

    tick_positions.clear();
    tick_labels_storage.clear();
    tick_labels_ptrs.clear();
    if (count <= 0) return;
    tick_positions.reserve(static_cast<size_t>(count));
    tick_labels_storage.reserve(static_cast<size_t>(count));
    tick_labels_ptrs.reserve(static_cast<size_t>(count));

    if (chart_type_ == ChartType::TPO) {
        for (int i = 0; i < count; i++) {
            const int64_t tick_s = first_tick_s + static_cast<int64_t>(i) * interval_s;
            tick_positions.push_back(static_cast<double>(tick_s) * 1000.0);
            char label[32];
            DisplayTimeZone::instance().format(tick_s * 1000,
                TimeZoneFormat::WeekdayMonthDay, label, sizeof(label));
            tick_labels_storage.emplace_back(label);
        }
    } else {
        char prev_day[16]{};
        for (int i = 0; i < count; i++) {
            const int64_t tick_s = first_tick_s + static_cast<int64_t>(i) * interval_s;
            tick_positions.push_back(static_cast<double>(tick_s) * 1000.0);

            char label[32];
            char day[16]{};
            DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::DateOnly,
                                                day, sizeof(day));

            const bool day_changed = prev_day[0] && std::strcmp(day, prev_day) != 0;

            if (day_changed || !prev_day[0]) {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::MonthDay,
                                                    label, sizeof(label));
            } else if (interval_s < 60) {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::TimeSeconds,
                                                    label, sizeof(label));
            } else {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::TimeMinutes,
                                                    label, sizeof(label));
            }

            std::snprintf(prev_day, sizeof(prev_day), "%s", day);
            tick_labels_storage.emplace_back(label);
        }
    }
    // Second pass: storage is final → c_str() pointers are stable.
    for (const auto& s : tick_labels_storage) tick_labels_ptrs.push_back(s.c_str());

    submit();
}

int64_t ChartWidget::calculate_tick_interval(double visible_candles) const {
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const double visible_span_seconds = visible_candles * static_cast<double>(tf_sec);
    // static constexpr - the old std::vector heap-allocated EVERY FRAME.
    static constexpr int64_t nice_intervals[] = {
        1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 300, 600, 900, 1200, 1800, 2700,
        3600, 5400, 7200, 10800, 14400, 18000, 21600,
        28800, 36000, 43200, 64800, 86400, 172800,
        259200, 432000, 604800, 1209600, 2592000, 5184000, 7776000
    };
    int min_ticks, max_ticks;
    if (visible_candles < 50) {
        min_ticks = 6; max_ticks = 12;
    } else if (visible_candles < 150) {
        min_ticks = 10; max_ticks = 18;
    } else if (visible_candles < 400) {
        min_ticks = 15; max_ticks = 27;
    } else if (visible_candles < 1000) {
        min_ticks = 12; max_ticks = 20;
    } else {
        min_ticks = 8; max_ticks = 15;
    }

    int64_t best_interval = nice_intervals[0];
    for (int64_t interval : nice_intervals) {
        const int tick_count = static_cast<int>(visible_span_seconds / static_cast<double>(interval));
        if (tick_count >= min_ticks && tick_count <= max_ticks) {
            best_interval = interval;
            break;
        }
        if (tick_count < min_ticks) break;
        best_interval = interval;
    }
    if (best_interval < 1) best_interval = 1;
    int final_tick_count = static_cast<int>(visible_span_seconds / static_cast<double>(best_interval));
    if (final_tick_count > 50) {
        for (const int64_t interval : nice_intervals) {
            if (interval > best_interval) {
                final_tick_count = static_cast<int>(visible_span_seconds / static_cast<double>(interval));
                if (final_tick_count <= 50) {
                    best_interval = interval;
                    break;
                }
            }
        }
    }
    return best_interval;
}

int64_t ChartWidget::align_to_interval(int64_t timestamp_s, int64_t interval_seconds) const {
    const auto start_time_t = static_cast<time_t>(timestamp_s);
    // Label tick positions remain UTC-aligned so a display preference never moves
    // a candle or marker. Only their rendered wall-clock labels are converted.
    tm* start_tm = gmtime(&start_time_t);

    if (interval_seconds == 30) {
        start_tm->tm_sec = (start_tm->tm_sec / 30) * 30;
        return timegm(start_tm);
    } else if (interval_seconds == 60) {
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds <= 1200 && interval_seconds % 60 == 0) {
        // 2m, 3m, 5m, 10m, 15m, 20m
        const int mins = static_cast<int>(interval_seconds / 60);
        start_tm->tm_min = (start_tm->tm_min / mins) * mins;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 1800) {
        start_tm->tm_min = (start_tm->tm_min / 30) * 30;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 2700) {
        start_tm->tm_min = (start_tm->tm_min / 45) * 45;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 3600) {
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 5400) {
        const int total_minutes = start_tm->tm_hour * 60 + start_tm->tm_min;
        const int rounded_minutes = (total_minutes / 90) * 90;
        start_tm->tm_hour = rounded_minutes / 60;
        start_tm->tm_min = rounded_minutes % 60;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds <= 43200 && interval_seconds % 3600 == 0) {
        // 2h, 3h, 4h, 5h, 6h, 8h, 10h, 12h
        const int hours = static_cast<int>(interval_seconds / 3600);
        start_tm->tm_hour = (start_tm->tm_hour / hours) * hours;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 86400) {
        start_tm->tm_hour = 0;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds % 86400 == 0) {
        const int day_interval = static_cast<int>(interval_seconds / 86400);
        start_tm->tm_mday = ((start_tm->tm_mday - 1) / day_interval) * day_interval + 1;
        start_tm->tm_hour = 0;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else {
        return (timestamp_s / interval_seconds) * interval_seconds;
    }
}


void ChartWidget::request_heatmap_data() {
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    if (heatmap_data_requested_ && heatmap_loaded_timeframe_ == tf_sec) return;
    // Unsubscribe from previous heatmap stream if switching timeframes
    if (heatmap_loaded_timeframe_ > 0) {
        StreamKey old_key{pair_, Terminal::Stream::Heatmap, heatmap_loaded_timeframe_};
        ctx_.stream_mgr().send_unsubscribe(old_key);
    }

    // During replay, use replay start time as anchor instead of wall clock
    int64_t anchor_ms;
    if (ctx_.candle_mgr().replay_start_time_ms() > 0) {
        anchor_ms = ctx_.candle_mgr().replay_start_time_ms();
    } else {
        const auto now = std::chrono::system_clock::now();
        anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    // Scale lookback to timeframe: ~500 candles worth of heatmap data
    // 1m → ~8h, 5m → ~42h, 15m → ~5d, 30m → ~10d
    const int64_t lookback_ms = std::min(
        static_cast<int64_t>(tf_sec) * 1000LL * 500,
        7200LL * 60 * 1000  // Cap at 5 days
    );
    const int64_t start_time_ms = anchor_ms - lookback_ms;
    ctx_.stream_mgr().request_historical_heatmap(
        pair_, heatmap_mode_, start_time_ms, anchor_ms, tf_sec
    );
    // Subscribe to live heatmap stream so completed candle periods get filled in
    StreamKey heatmap_key{pair_, Terminal::Stream::Heatmap, tf_sec};
    ctx_.stream_mgr().send_subscribe(heatmap_key);
    heatmap_data_requested_ = true;
    heatmap_loaded_timeframe_ = tf_sec;
}

void ChartWidget::update_heatmap() {
    if (!heatmap_enabled_ || ctx_.candle_mgr().empty()) return;

    const ImPlotRect limits = last_visible_range_;
    if (limits.X.Size() <= 0) return;

    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor) return;

    const int64_t available_min = reconstructor->get_min_time();
    const int64_t available_max = reconstructor->get_max_time();
    if (available_min == 0 || available_max == 0) return;

    const auto display_time_start = static_cast<int64_t>(limits.X.Min);
    const auto display_time_end = static_cast<int64_t>(limits.X.Max);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    constexpr int64_t threshold_ms = 30LL * 60 * 1000;

    // Zoom-aware threshold: when zoomed out far, the 30-minute threshold
    // is too small - the viewport edge can be hours past the data boundary.
    // Scale threshold to 20% of visible range or 30 min, whichever is larger.
    const int64_t vis_span = display_time_end - display_time_start;
    const int64_t dynamic_threshold = std::max(threshold_ms, vis_span / 5);

    // Max lookback per timeframe to keep memory bounded.
    const int64_t tf_ms_ob = ctx_.candle_mgr().timeframe_seconds() * 1000LL;
    const int64_t max_lookback_ob = (tf_ms_ob <= 60000)
        ? 3LL * 24 * 60 * 60 * 1000    // 3 days for 1m
        : 10LL * 24 * 60 * 60 * 1000;  // 10 days for 5m+
    const int64_t earliest_ob = now_ms - max_lookback_ob;

    bool missing_left = (display_time_start < available_min + dynamic_threshold)
                        && (available_min > earliest_ob);
    bool missing_right = (display_time_end > available_max - dynamic_threshold)
                         && (available_max < now_ms - 60000);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heatmap_rebuild_).count();

    // Faster cooldown, but prevent duplicate requests for the same range
    if ((missing_left || missing_right) && elapsed > 2000) {
        const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
        // Cap chunk at 1000 candles to stay within memory budget.
        // The timeline caps at 5000 entries, so loading 1000 at a time
        // fills progressively without wasting bandwidth on data that gets evicted.
        const int64_t chunk_candles = std::clamp(
            vis_span / (tf_sec * 1000LL), 500LL, 1000LL);
        const int64_t max_chunk = tf_sec * 1000LL * chunk_candles;
        int64_t req_start, req_end;

        if (missing_left) {
            req_end = available_min;
            req_start = req_end - max_chunk;
        } else {
            req_start = available_max;
            req_end = std::min(req_start + max_chunk, now_ms);
        }

        // Skip tiny/zero-length requests
        if (req_end - req_start < tf_sec * 1000LL * 5) return;

        ctx_.stream_mgr().request_historical_heatmap(
            pair_, heatmap_mode_, req_start, req_end, tf_sec);
        last_heatmap_rebuild_ = now;
    }
}

void ChartWidget::update_live_heatmap_from_orderbook() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heatmap_update_ms_
    ).count();
    if (elapsed_ms < HEATMAP_UPDATE_INTERVAL_MS) return;
    last_heatmap_update_ms_ = now;

    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor || !reconstructor->has_data()) return;

    // Skip expensive OB iteration when user has scrolled away from live.
    // The live column would be outside the viewport grid and silently dropped anyway.
    if (last_visible_range_.X.Max > 0) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const auto visible_right = static_cast<int64_t>(last_visible_range_.X.Max);
        const int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000LL;
        // If the right edge of viewport is more than 5 candles behind "now", skip
        if (now_ms - visible_right > tf_ms * 5) return;
    }

    const auto* orderbook = ctx_.ob_mgr().get_orderbook(pair_);
    if (!orderbook || !orderbook->is_synchronized()) return;

    std::unordered_map<double, float> price_qty;
    price_qty.reserve(orderbook->bids.size() + orderbook->asks.size());

    for (const auto& [price, qty] : orderbook->bids) {
        price_qty[price] += static_cast<float>(qty);
    }
    for (const auto& [price, qty] : orderbook->asks) {
        price_qty[price] += static_cast<float>(qty);
    }

    int64_t timestamp_ms = orderbook->timestamp_ms;
    if (timestamp_ms == 0) {
        timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    reconstructor->update_live_column(timestamp_ms, price_qty);
}

void ChartWidget::render_heatmap_tooltip() {
    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor || !ImPlot::IsPlotHovered()) return;

    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const double bucket_size = reconstructor->get_display_bucket_size();
    const int64_t timeframe_ms = tf_sec * 1000;

    const double center_price = std::floor(mouse.y / bucket_size) * bucket_size;
    // +half a step before flooring: heatmap columns are center-anchored on their bucket
    // time (see shader_heatmap_renderer data_time_start), so the cell under the cursor is
    // the bucket whose CENTER is nearest x, not the one whose left edge is left of x.
    const int64_t center_time = (static_cast<int64_t>(mouse.x + timeframe_ms * 0.5) / timeframe_ms) * timeframe_ms;

    const float center_value = reconstructor->get_value_at_price_and_time(center_price, center_time);
    if (center_value < 0.001f) return;

    Theme::begin_tooltip();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 4));

    ImGui::Text("Time: %s", format_time_hms(center_time).c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("HeatmapGrid", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("T-1", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("T", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("T+1", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        const float max_qty = reconstructor->get_max_qty();

        for (int row = 0; row < 3; row++) {
            const double price = center_price + (1 - row) * bucket_size;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text(fmt_.price_fmt, price);

            for (int col = 0; col < 3; col++) {
                ImGui::TableSetColumnIndex(col + 1);
                const int64_t time = center_time + (col - 1) * timeframe_ms;
                const float qty = reconstructor->get_value_at_price_and_time(price, time);

                if (qty > 0.001f) {
                    float normalized = std::clamp(qty / max_qty, 0.0f, 1.0f);
                    ImVec4 bg_color = get_tooltip_cell_color(normalized);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                           ImGui::ColorConvertFloat4ToU32(bg_color));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        normalized > 0.5f ? ImVec4(0,0,0,1) : ImVec4(1,1,1,1));
                    ImGui::Text("%.2f", qty);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("-");
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    Theme::end_tooltip();
}

void ChartWidget::calculate_heatmap_price_range(double& min_price, double& max_price) {
    if (ctx_.candle_mgr().empty()) {
        min_price = ctx_.candle_mgr().last_close_price() * 0.90;
        max_price = ctx_.candle_mgr().last_close_price() * 1.10;
        return;
    }

    double data_min = std::numeric_limits<double>::max();
    double data_max = std::numeric_limits<double>::lowest();
    for (const auto& candle : ctx_.candle_mgr().candles()) {
        data_min = std::min(data_min, candle.low);
        data_max = std::max(data_max, candle.high);
    }

    double range = data_max - data_min;
    min_price = data_min - (range * 0.05);
    max_price = data_max + (range * 0.05);

    auto* recon = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (recon) {
        const double bucket_size = recon->get_display_bucket_size();
        min_price = std::floor(min_price / bucket_size) * bucket_size;
        max_price = std::ceil(max_price / bucket_size) * bucket_size;
    }
}

ImPlotColormap ChartWidget::get_colormap_for_type(HeatmapType type) const {
    switch (type) {
        case HeatmapType::OrderbookDepth:  return ImPlotColormap_Viridis;
        case HeatmapType::VolumeDelta:     return ImPlotColormap_RdBu;
        case HeatmapType::TradeIntensity:  return ImPlotColormap_Hot;
        case HeatmapType::Liquidations:    return ImPlotColormap_Plasma;
        case HeatmapType::VWAPDeviation:   return ImPlotColormap_RdBu;
        default:                           return ImPlotColormap_Viridis;
    }
}

std::string ChartWidget::format_time_hms(int64_t timestamp_ms) {
    char buffer[40]{};
    DisplayTimeZone::instance().format(timestamp_ms, TimeZoneFormat::DateTimeSeconds,
                                        buffer, sizeof(buffer));
    return std::string(buffer);
}

ImVec4 ChartWidget::get_tooltip_cell_color(float normalized) const {
    if (normalized < 0.01f) {
        return {0.06f, 0.10f, 0.18f, 1.0f};
    } else if (normalized < 0.15f) {
        const float s = (normalized - 0.01f) / 0.14f;
        return {
            0.06f + s * 0.04f,
            0.10f + s * 0.37f,
            0.18f + s * 0.41f,
            1.0f
        };
    } else if (normalized < 0.35f) {
        const float s = (normalized - 0.15f) / 0.20f;
        return {
            0.10f + s * 0.14f,
            0.47f + s * 0.16f,
            0.59f + s * 0.21f,
            1.0f
        };
    } else if (normalized < 0.55f) {
        const float s = (normalized - 0.35f) / 0.20f;
        return {
            0.24f + s * 0.47f,
            0.63f - s * 0.31f,
            0.80f + s * 0.12f,
            1.0f
        };
    } else if (normalized < 0.75f) {
        const float s = (normalized - 0.55f) / 0.20f;
        return {
            0.71f + s * 0.25f,
            0.32f + s * 0.39f,
            0.92f - s * 0.61f,
            1.0f
        };
    } else {
        const float s = (normalized - 0.75f) / 0.25f;
        return {
            0.96f + s * 0.04f,
            0.71f + s * 0.29f,
            0.31f + s * 0.69f,
            1.0f
        };
    }
}

void ChartWidget::toggle_heatmap() {
    heatmap_enabled_ = !heatmap_enabled_;
    if (heatmap_enabled_ && !heatmap_data_requested_) {
        request_heatmap_data();
    }
}

// =============================================================================
// Liquidation Heatmap Overlay
// =============================================================================

void ChartWidget::request_liq_heatmap_data() {
    if (liq_timeline_requested_) return;
    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    // During replay, use the replay start time as the anchor instead of wall clock.
    // The DataContext factory already loaded liq heatmap up to replay start,
    // so this re-request on timeframe change must use the same time anchor.
    int64_t anchor_ms;
    if (ctx_.candle_mgr().replay_start_time_ms() > 0) {
        anchor_ms = ctx_.candle_mgr().replay_start_time_ms();
    } else {
        const auto now = std::chrono::system_clock::now();
        anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }
    // Request ~500 candles worth of liq heatmap history
    const int64_t lookback_ms = std::min(
        static_cast<int64_t>(tf_sec) * 1000LL * 500,
        7200LL * 60 * 1000  // Cap at 5 days
    );
    const int64_t start_time_ms = anchor_ms - lookback_ms;
    ctx_.stream_mgr().request_historical_liq_heatmap(pair_, start_time_ms, anchor_ms, tf_sec);
    liq_timeline_requested_ = true;
}

void ChartWidget::update_liq_heatmap_scroll() {
    if (!liq_heatmap_enabled_ || !liq_timeline_requested_ || ctx_.candle_mgr().empty()) return;

    const ImPlotRect limits = last_visible_range_;
    if (limits.X.Size() <= 0) return;

    auto* reconstructor = ctx_.liq_heatmap_mgr().get_timeline_reconstructor(pair_);
    if (!reconstructor || !reconstructor->has_data()) return;

    const int64_t available_min = reconstructor->get_min_time();
    const int64_t available_max = reconstructor->get_max_time();
    if (available_min == 0 || available_max == 0) return;

    const auto display_time_start = static_cast<int64_t>(limits.X.Min);
    const auto display_time_end = static_cast<int64_t>(limits.X.Max);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // Trigger when viewport is within threshold of available data boundary.
    // Zoom-aware: scale threshold to 20% of visible range or 30 min, whichever larger.
    constexpr int64_t threshold_ms = 30LL * 60 * 1000;
    const int64_t vis_span = display_time_end - display_time_start;
    const int64_t dynamic_threshold = std::max(threshold_ms, vis_span / 5);

    // Liq heatmap: max 14 days lookback
    const int64_t earliest_liq = now_ms - 14LL * 24 * 60 * 60 * 1000;

    bool missing_left = (display_time_start < available_min + dynamic_threshold)
                        && (available_min > earliest_liq)
                        && (liq_heatmap_data_boundary_ms_ == 0
                            || available_min > liq_heatmap_data_boundary_ms_);
    bool missing_right = (display_time_end > available_max - dynamic_threshold)
                         && (available_max < now_ms - 60000);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_liq_heatmap_scroll_load_time_).count();

    // Prevent duplicate requests - use 2s cooldown to wait for in-flight data
    if ((missing_left || missing_right) && elapsed > 2000) {
        // Check if previous left-scroll request produced no meaningful data
        // (data boundary reached - compressed chunks or no data)
        if (missing_left && liq_heatmap_prev_available_min_ > 0) {
            const int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000LL;
            const int64_t advance = liq_heatmap_prev_available_min_ - available_min;
            if (advance < tf_ms * 10) {  // Less than 10 columns advanced
                liq_heatmap_data_boundary_ms_ = available_min;
                liq_heatmap_prev_available_min_ = 0;
                return;
            }
        }
        const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
        // Cap chunk at 1000 candles to stay within memory budget.
        const int64_t chunk_candles = std::clamp(
            vis_span / (tf_sec * 1000LL), 500LL, 1000LL);
        const int64_t max_chunk = tf_sec * 1000LL * chunk_candles;
        int64_t req_start, req_end;

        if (missing_left) {
            req_end = available_min;
            req_start = req_end - max_chunk;
        } else {
            req_start = available_max;
            req_end = std::min(req_start + max_chunk, now_ms);
        }

        // Skip tiny/zero-length requests (available_max ≈ now)
        if (req_end - req_start < tf_sec * 1000LL * 5) return;  // less than 5 candles

        ctx_.stream_mgr().request_historical_liq_heatmap(
            pair_, req_start, req_end, tf_sec);
        last_liq_heatmap_scroll_load_time_ = now;
        if (missing_left) liq_heatmap_prev_available_min_ = available_min;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// VPVR (Volume Profile Visible Range) - sidebar histogram
// ═══════════════════════════════════════════════════════════════════════════════

double ChartWidget::compute_vpvr_tick_per_row(double price_range, double plot_height) const {
    if (vpvr_ticks_per_row_ > 0) {
        // Manual override
        return vpvr_ticks_per_row_ * tick_size_;
    }
    // Auto: target ~150-250 visible rows for good density.
    // This gives a reasonable bar height (2-4px per row at typical chart heights).
    constexpr double kTargetRows = 200.0;
    if (price_range <= 0 || plot_height <= 0 || tick_size_ <= 0) return tick_size_;

    const double ideal_row_price = price_range / kTargetRows;
    // Round to nearest multiple of tick_size
    const double ticks = std::max(1.0, std::round(ideal_row_price / tick_size_));
    return ticks * tick_size_;
}

void ChartWidget::render_vpvr_profile() {
    const auto* profile = ctx_.vpvr_mgr().get_profile(pair_.symbol);
    if (!profile || profile->levels.empty()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();

    const auto mode = ctx_.vpvr_mgr().mode();

    // Find max volume for normalization
    double max_vol = 0.001;
    for (const auto& lvl : profile->levels) {
        double v = 0;
        switch (mode) {
            case VolumeProfileManager::Mode::Standard:
                v = lvl.total_volume; break;
            case VolumeProfileManager::Mode::TotalVolume:
                v = lvl.total_volume; break;
            case VolumeProfileManager::Mode::TotalDelta:
                v = lvl.total_volume; break;
        }
        if (v > max_vol) max_vol = v;
    }

    // Compute band width from adjacent levels (or use tick_per_row)
    double band_width = tick_size_;
    if (profile->levels.size() >= 2) {
        band_width = std::abs(profile->levels[1].price - profile->levels[0].price);
        if (band_width < tick_size_) band_width = tick_size_;
    }

    // Token → renderer color helper (design system: UP/DOWN split, WARN POC)
    auto tok = [](const ImVec4& c) {
        return Profile::Color{static_cast<uint8_t>(c.x * 255.0f),
                              static_cast<uint8_t>(c.y * 255.0f),
                              static_cast<uint8_t>(c.z * 255.0f)};
    };

    // Build ProfileBand vector
    std::vector<Profile::ProfileBand> bands;
    bands.reserve(profile->levels.size());

    for (int i = 0; i < static_cast<int>(profile->levels.size()); ++i) {
        const auto& lvl = profile->levels[i];

        // Skip levels outside visible Y range (with small margin)
        if (lvl.price < limits.Y.Min - band_width || lvl.price > limits.Y.Max + band_width)
            continue;

        const float norm = static_cast<float>(lvl.total_volume / max_vol);
        if (norm < 0.001f) continue;

        // Design rule: value-area bins at full alpha, outside dimmed (~0.42)
        const float va_alpha = lvl.in_value_area ? 0.90f : 0.42f;

        Profile::ProfileBand pb{};
        pb.price_mid = lvl.price;
        pb.band_width = band_width;

        switch (mode) {
        case VolumeProfileManager::Mode::Standard: {
            // Primary = total bar (sell base, DOWN), secondary = buy overlay (UP)
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::DOWN);
            pb.primary_alpha = va_alpha;

            const float buy_ratio = (lvl.total_volume > 0)
                ? static_cast<float>(lvl.buy_volume / lvl.total_volume) : 0.5f;
            pb.secondary_value = norm * buy_ratio;
            pb.secondary_color = tok(Theme::Tokens::UP);
            pb.secondary_alpha = va_alpha;
            break;
        }
        case VolumeProfileManager::Mode::TotalVolume: {
            // Single neutral bar - brand-tinted, VA-dimmed
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::BRAND);
            pb.primary_alpha = va_alpha * 0.75f;
            pb.secondary_value = 0;
            break;
        }
        case VolumeProfileManager::Mode::TotalDelta: {
            // 3-layer: faint neutral total → dominant side in UP/DOWN
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::TX4);
            pb.primary_alpha = va_alpha * 0.45f;

            const float buy_ratio = (lvl.total_volume > 0)
                ? static_cast<float>(lvl.buy_volume / lvl.total_volume) : 0.5f;
            const float dominant_ratio = std::max(buy_ratio, 1.0f - buy_ratio);
            pb.secondary_value = norm * dominant_ratio;
            pb.secondary_color = (buy_ratio >= 0.5f)
                ? tok(Theme::Tokens::UP)
                : tok(Theme::Tokens::DOWN);
            pb.secondary_alpha = va_alpha;
            break;
        }
        }

        // POC highlight - WARN amber edge
        if (lvl.is_poc) {
            pb.show_edge = true;
            pb.edge_color = tok(Theme::Tokens::WARN);
            pb.edge_alpha = 0.85f;
        }

        // Value area wash
        if (lvl.in_value_area) {
            pb.wash_alpha = 0.04f;
        }

        // Labels (show volume on significant levels)
        if (ctx_.vpvr_mgr().show_values() && norm > 0.15f) {
            const double vol = lvl.total_volume;
            if (vol >= 1e9)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.1fB", vol / 1e9);
            else if (vol >= 1e6)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.1fM", vol / 1e6);
            else if (vol >= 1e3)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.0fK", vol / 1e3);
            else
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.0f", vol);
            pb.label = pb.label_buf;
        }

        pb.has_tooltip = true;
        pb.source_index = i;
        bands.push_back(pb);
    }

    if (bands.empty()) return;

    // Price markers: POC (WARN solid), VAH/VAL (TX3 dashed)
    std::vector<Profile::PriceMarker> markers;
    if (ctx_.vpvr_mgr().show_poc()) {
        Profile::PriceMarker poc{};
        poc.price = profile->poc;
        poc.color = Theme::u32(Theme::Tokens::WARN, 0.75f);
        poc.dashed = false;
        snprintf(poc.label_buf, sizeof(poc.label_buf), "POC");
        poc.label = poc.label_buf;
        markers.push_back(poc);
    }
    if (ctx_.vpvr_mgr().show_vah_val()) {
        Profile::PriceMarker vah{};
        vah.price = profile->vah;
        vah.color = Theme::u32(Theme::Tokens::TX3, 0.80f);
        vah.dashed = true;
        snprintf(vah.label_buf, sizeof(vah.label_buf), "VAH");
        vah.label = vah.label_buf;
        markers.push_back(vah);

        Profile::PriceMarker val{};
        val.price = profile->val;
        val.color = Theme::u32(Theme::Tokens::TX3, 0.80f);
        val.dashed = true;
        snprintf(val.label_buf, sizeof(val.label_buf), "VAL");
        val.label = val.label_buf;
        markers.push_back(val);
    }

    // Render config
    Profile::ProfileConfig cfg;
    cfg.max_bar_width_pct = ctx_.vpvr_mgr().width_pct();
    cfg.right_aligned = true;
    cfg.draw_wash = true;
    cfg.label_min_band_height = 10.0f;

    // Tooltip
    Profile::render_profile_with_tooltip(
        draw_list, limits, plot_pos, plot_size,
        bands, markers, cfg,
        [](const Profile::ProfileBand& band, int idx, void* ud) {
            auto* prof = static_cast<const VolumeProfileManager::ProfileData*>(ud);
            const int si = band.source_index;
            if (si < 0 || si >= static_cast<int>(prof->levels.size())) return;
            const auto& lvl = prof->levels[si];
            Theme::begin_tooltip();
            ImGui::Text("Price: %g", lvl.price);
            ImGui::Separator();
            const double buy_pct = (lvl.total_volume > 0.001) ? (lvl.buy_volume / lvl.total_volume * 100.0) : 0.0;
            const double sell_pct = (lvl.total_volume > 0.001) ? (lvl.sell_volume / lvl.total_volume * 100.0) : 0.0;
            ImGui::TextColored(Theme::Tokens::UP,
                "Buy:   %.2f (%.1f%%)", lvl.buy_volume, buy_pct);
            ImGui::TextColored(Theme::Tokens::DOWN,
                "Sell:  %.2f (%.1f%%)", lvl.sell_volume, sell_pct);
            ImGui::Text("Total: %.2f (%.2f%% of profile)", lvl.total_volume, lvl.volume_pct);
            ImGui::Text("Delta: %+.2f", lvl.delta);
            if (lvl.is_poc) {
                ImGui::TextColored(Theme::Tokens::WARN, "** POC **");
            }
            if (lvl.in_value_area) {
                ImGui::TextDisabled("Inside Value Area");
            }
            Theme::end_tooltip();
        },
        const_cast<void*>(static_cast<const void*>(profile))
    );
}

void ChartWidget::render_liq_timeline() {
    auto* reconstructor = ctx_.liq_heatmap_mgr().get_timeline_reconstructor(pair_);
    if (!reconstructor || !reconstructor->has_data()) return;

    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const int64_t candle_ms = tf_sec * 1000;

    // Adaptive tick-per-row: auto-adjust bucket_multiplier so the visible
    // price range has a reasonable number of rows (~80-200).
    // Prevents micro-cap coins with tiny tick_size from generating
    // thousands of sub-pixel rows that look like static noise.
    const double native_bucket = reconstructor->get_native_bucket_size();
    if (native_bucket > 0) {
        int target_multiplier;
        if (liq_ticks_per_row_ > 0) {
            // Manual override: user-specified ticks per row
            // Convert ticks to price range, then to bucket multiplier
            // tick_size_ is from symbol metadata (e.g. $0.10 for BTC)
            if (tick_size_ > 0) {
                const double row_price_range = liq_ticks_per_row_ * tick_size_;
                target_multiplier = std::max(1, static_cast<int>(std::round(row_price_range / native_bucket)));
            } else {
                target_multiplier = liq_ticks_per_row_;
            }
        } else {
            // Auto: pixel-space adaptive - ensure each liq band is at least
            // ~3px tall. This works for both BTC (tight range, many rows fit)
            // and LABUSDT on 4h (300%+ range, needs heavy aggregation).
            // The old fixed target of 200 rows broke on volatile alts where
            // 200 rows × tiny cells = sub-pixel noise with no visible bands.
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            const double visible_range = limits.Y.Max - limits.Y.Min;
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            const float plot_height_px = plot_size.y;
            constexpr float min_cell_px = 2.5f;  // Minimum pixels per liq band
            const int native_rows = static_cast<int>(visible_range / native_bucket);
            const int max_visible_rows = std::max(1, static_cast<int>(plot_height_px / min_cell_px));
            target_multiplier = std::max(1, native_rows / max_visible_rows);
        }
        target_multiplier = std::clamp(target_multiplier, 1, 100);
        const int current = reconstructor->get_bucket_multiplier();
        // Only update if significantly different to avoid constant grid rebuilds
        if (target_multiplier != current &&
            (target_multiplier > current * 1.3 || target_multiplier < current * 0.7 || current <= 1)) {
            reconstructor->set_bucket_multiplier(target_multiplier);
        }
    }

    // Push opacity and color scale to the reconstructor
    reconstructor->set_opacity(liq_opacity_);
    // Historical field: never cone-fade by the CURRENT mark (that erases past bands far from
    // price - preserving them is the whole point). GL_NEAREST + extend-levels give MMT's crisp
    // horizontal streaks rather than a smeared gaussian cloud.
    reconstructor->set_reach_modulation(false);
    reconstructor->set_extend_levels(true);
    reconstructor->set_linear_filtering(false);

    // Auto-compute Low/Peak from viewport data distribution.
    // The Bayesian estimator spreads liquidation estimates via Gaussians,
    // producing smooth non-zero values across all visible bands. Fixed
    // low/peak can't create visual contrast because the values cluster
    // in a narrow range (e.g., all between 1.2 and 2.0). Viewport-adaptive
    // normalization uses the actual mean ± stddev of visible data to stretch
    // that narrow range across the full colormap.
    if (liq_color_auto_) {
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        auto stats = reconstructor->get_viewport_stats(limits.Y.Min, limits.Y.Max);
        if (stats.count > 10 && stats.stddev > 0.001f) {
            // V7d: Final tuning - show more of the purple/magenta mid-range so
            // clusters feel substantial with visible halos, not just isolated dots.
            // Low = mean - 0.2σ: slightly below mean so the upper ~60% of data shows.
            // Peak = mean + 2.5σ: tighter peak brings more values into orange/yellow range.
            // Combined with pow(t, 1.3) in shader for gentler contrast curve.
            // V8 render spike: PEAKY floor. The field is dense/low-contrast, so a floor just
            // below the mean lets ~half the bands clear the shader discard → green fog. Push
            // the floor well above the mean so only the strong tail (the real levels) survives;
            // extend-levels then carries those few bands across columns as clean horizontal
            // lines. (Diagnostic: if survivors form lines, the data has persistent levels and
            // this was a render problem; if they scatter, the levels jitter → estimator work.)
            // V9: dense floor (was the V8 "peaky" mean+1σ diagnostic, which discarded
            // everything but the strong tail → rails-only). That diagnostic passed -
            // survivors form clean horizontal lines = render problem, data good - so
            // restore the dense setting: low just below the mean renders the field as
            // the purple/magenta background; peak at +2.5σ keeps dominant levels bright.
            liq_color_low_ = std::max(0.0f, stats.mean - 0.5f * stats.stddev);
            liq_color_peak_ = stats.mean + 2.5f * stats.stddev;
        } else {
            // Fallback: not enough data yet
            liq_color_low_ = 0.0f;
            liq_color_peak_ = 2.0f;
        }
    }
    reconstructor->set_color_low(liq_color_low_);
    reconstructor->set_color_peak(liq_color_peak_);

    reconstructor->render_cells(candle_ms, 1.0f, false);
}

void ChartWidget::toggle_liquidation_heatmap() {
    liq_heatmap_enabled_ = !liq_heatmap_enabled_;
    if (liq_heatmap_enabled_ && !liq_heatmap_subscribed_) {
        const StreamKey key{pair_, Terminal::Stream::LiquidationHeatmap, 0};
        ctx_.stream_mgr().send_subscribe(key);
        liq_heatmap_subscribed_ = true;
        // Reset data boundary so scroll-load can try again (e.g., after migration)
        liq_heatmap_data_boundary_ms_ = 0;
        liq_heatmap_prev_available_min_ = 0;
    } else if (!liq_heatmap_enabled_ && liq_heatmap_subscribed_) {
        const StreamKey key{pair_, Terminal::Stream::LiquidationHeatmap, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
        liq_heatmap_subscribed_ = false;
    }
}

// "Liq Levels HL" (P2e): the census layer's subscribe follows the pill, on the
// UNDERLYING-keyed HL pair (not pair_ - cross-venue overlay). Same live-only
// semantics as toggle_liquidation_heatmap: during replay the StreamManager is
// subscribe-inert and reset_overlay_subscriptions re-arms the flag on exit.
void ChartWidget::toggle_liq_census() {
    liq_census_enabled_ = !liq_census_enabled_;
    if (liq_census_enabled_ && !liq_census_subscribed_) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_subscribe(key);
        liq_census_subscribed_ = true;
    } else if (!liq_census_enabled_ && liq_census_subscribed_) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
        liq_census_subscribed_ = false;
    }
}

void ChartWidget::reset_overlay_subscriptions() {
    // Reset subscription flags so overlays re-subscribe on the new context.
    // Called when replay starts/stops (context swap).
    liq_heatmap_subscribed_ = false;
    liq_census_subscribed_ = false;
    liq_heatmap_data_boundary_ms_ = 0;
    liq_heatmap_prev_available_min_ = 0;
    heatmap_data_requested_ = false;
    // Force VPVR re-request
    vpvr_last_request_start_ = 0;
    vpvr_last_request_end_ = 0;
    // VPIN pane: fresh SeriesCache on the new context (replay ctx starts
    // empty and is seed-fed; live ctx refills via re-request) - force
    // re-subscribe + re-request + repopulate.
    vpin_subscribed_ = false;
    vpin_history_requested_ = false;
    vpin_populated_revision_ = 0;
    vpin_hist_last_oldest_ = 0;
    vpin_hist_exhausted_ = false;
    // Admin patterns are live context only. Never carry a pre-replay candidate
    // into historical playback or restore it stale on replay exit; the next
    // backend forming update repopulates the fixed slots.
    pattern_overlay_active_.fill(false);
}

// ── Shared liquidation-level SELECTION - used by BOTH the rail render (replay) and the standing-
// shelf fallback (live). Floors at max*min_frac, sorts by USD desc, merges near-duplicate prices,
// then caps: per SIDE when per_side_cap>0 (rails), else a single TOTAL cap (shelves). Pure
// selection - the caller keeps its OWN styling (rails draw LINEAR brightness, shelves SQRT - a
// deliberate difference, see the LIQMAP handoff). lx/rx (the resolved pixel x-span) ride through
// untouched. out_max_usd returns the pre-floor max for the caller's brightness normalization.
namespace {
struct LiqDrawLine { double price; double usd; bool is_long; float lx; float rx; bool swept = false; };
static std::vector<LiqDrawLine> select_liq_lines(
        const std::vector<LiqDrawLine>& in, float min_frac, double merge_abs,
        int per_side_cap, int total_cap, double& out_max_usd) {
    out_max_usd = 0.0;
    for (const auto& l : in) if (l.usd > out_max_usd) out_max_usd = l.usd;
    std::vector<LiqDrawLine> kept;
    if (out_max_usd <= 0.0) return kept;
    const double floor_usd = out_max_usd * static_cast<double>(min_frac);
    std::vector<LiqDrawLine> floored;
    floored.reserve(in.size());
    for (const auto& l : in) if (l.price > 0.0 && l.usd >= floor_usd) floored.push_back(l);
    std::sort(floored.begin(), floored.end(),
        [](const LiqDrawLine& a, const LiqDrawLine& b) { return a.usd > b.usd; });
    kept.reserve(floored.size());
    int n_long = 0, n_short = 0, n_total = 0;
    for (const auto& l : floored) {
        bool dup = false;
        if (merge_abs > 0.0)
            for (const auto& r : kept)
                if (std::abs(r.price - l.price) < merge_abs) { dup = true; break; }
        if (dup) continue;
        if (per_side_cap > 0) {
            if (l.is_long) { if (n_long >= per_side_cap) continue; ++n_long; }
            else           { if (n_short >= per_side_cap) continue; ++n_short; }
        } else if (total_cap > 0 && n_total >= total_cap) {
            continue;
        }
        ++n_total;
        kept.push_back(l);
    }
    return kept;
}
} // namespace

// The stateful per-level discharge tracker (update_liq_discharge_tracker) and liq_level_taken were
// REMOVED with the rails pivot (2026-06-29). "Taken" is no longer inferred from candle [low,high] on
// the client - it is the backend RailTracker's consume_ms (the first LAST-PRICE wick-through),
// delivered per rail on the wire and surfaced via BandTrack.consume_ms. No client candle-scan
// inference.
// ── Liq Heatmap projection Field (V1) ────────────────────────────────────────────────────
// MMT-style dense liquidation heatmap, computed CLIENT-SIDE from candles (universal +
// deterministic - renders on every symbol/timeframe, unlike the OI estimator).
//
// Sentinel end time for a segment that's still PENDING (never consumed) → extends to the live/replay edge.
static constexpr int64_t LIQ_SEG_PENDING = std::numeric_limits<int64_t>::max();

// (Re)build the 2D time×price Field cache as horizontal SEGMENTS. A single left→right walk over ALL
// loaded candles maintains the fuel currently standing at each price bucket: each candle CONSUMES the
// fuel its [low,high] trades through (emitting a finished segment, then going dark) and DEPOSITS fresh
// fuel at its per-tier liquidation prices. A level therefore emits a NEW segment each time it's swept
// and re-lit - giving the time-varying intensity + dense fill MMT shows (vs the old single-block carve).
// Purely candle-derived → identical live and in replay; rebuilt only when the closed-candle set / mask /
// timeframe changes. §5b(b).
void ChartWidget::rebuild_liq_field_cache(uint8_t lmask, int64_t tf_ms) {
    // Rebuild spikes attribute to this section in the perf overlay / spike log
    // (fires on candle-set/mask/TF signature change - frequent during replay).
    ProfileScope _ps("LiqRebuild");
    (void)tf_ms;
    liq_field_segs_.clear();
    liq_field_max_mag_ = 0.0f;
    liq_field_norm_lo_ = 0.0f;
    liq_field_norm_hi_ = 0.0f;
    liq_field_bw_ = 0.0;
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();
    if (candles.empty() && !has_bld) return;

    // LOG-PRICE buckets (2026-07-02d): bucket k = llround(ln(price)/lbw), so a bucket is liq_field_bps_
    // of its OWN price at every level - uniform relative row thickness across the whole loaded range.
    // A LINEAR grid cannot serve a symbol whose price spans several × (TAIKO 8×: one shared bucket was
    // sub-pixel at the top of the range AND rendered as giant solid blocks at the bottom). Log levels
    // are still absolute in price (static across zoom/scroll) and candle-derived (replayable); bucket
    // count is inherently bounded (ln(range-ratio)/lbw). lbw is the width in log-price ≈ the relative
    // width (ln(1+x) ≈ x).
    const double lbw = static_cast<double>(std::clamp(liq_field_bps_, 1.0f, 50.0f)) * 1.0e-4;
    const double inv_lbw = 1.0 / lbw;

    // ROBUST relative-volume (anomaly) weight - MEDIAN-relative, log-soft-clipped in step() below.
    // The old GLOBAL-mean reference let a few mega-volume pump candles inflate the mean and dim every
    // other deposit (TAIKO: the whole consolidation went sub-floor). Median is viewport-independent
    // and insensitive to those outliers, so consolidation fuel and pump fuel stay on comparable scales.
    std::vector<double> vols;
    vols.reserve(candles.size() + 1);
    for (const auto& c : candles) if (c.volume > 0.0) vols.push_back(c.volume);
    if (has_bld && cm.building_candle().volume > 0.0) vols.push_back(cm.building_candle().volume);
    if (vols.empty()) return;
    const size_t vmid = vols.size() / 2;
    std::nth_element(vols.begin(), vols.begin() + vmid, vols.end());
    const double vmed = vols[vmid];
    if (vmed <= 0.0) return;
    const double wcap = std::max(1.0, static_cast<double>(liq_field_wcap_));
    // AGE-DECAY half-life (task-1 belt fix, round 6). Standing fuel decays exponentially with CANDLE
    // age - deposits from days ago fade unless the bucket keeps being refed. Bounds every bucket's
    // steady state (no unbounded standing accumulation → no belts at any load length) while keeping
    // the SUM model's wide, structured dynamic range (rounds 4/5 showed the percentile map just
    // re-normalizes any distribution reshaping into soup/all-bright). Purely candle-timestamp-driven:
    // static across zoom/scroll, identical in replay; consumed segments decay to their CONSUME time,
    // so history stays a fixed record. ≤0 disables.
    const double hl_ms = static_cast<double>(liq_field_halflife_h_) * 3600.0e3;

    // Mass-conserving gaussian deposit kernel (±K buckets; K=0 default - bucket coarseness supplies
    // thickness). A fast/vertical move leaves ~1 candle per price level, so point deposits can render
    // as 1-bucket threads - the kernel spreads each deposit into a legible band. Σkw = 1 keeps total
    // deposited mass identical to a point deposit, so stacked zones don't inflate the normalization.
    const int K = std::clamp(liq_field_kernel_, 0, 8);
    double kw[9];
    {
        const double ksig = std::max(0.5, static_cast<double>(K) / 1.6659);  // edge bucket ≈ 0.25×center
        double ksum = 0.0;
        for (int d = 0; d <= K; ++d) {
            kw[d] = std::exp(-(static_cast<double>(d) * d) / (2.0 * ksig * ksig));
            ksum += (d == 0) ? kw[d] : 2.0 * kw[d];
        }
        for (int d = 0; d <= K; ++d) kw[d] /= ksum;
    }

    static constexpr double  kLev[6] = {5.0, 10.0, 25.0, 50.0, 75.0, 100.0};
    static constexpr uint8_t kBit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};

    struct State { double f; int64_t run_start; int64_t last_ms; };  // decayed fuel sum + run start +
    std::map<long long, State> live;                 // last deposit time (decay is applied lazily);
                                                     // key = llround(ln(price)/lbw) → ordered consume
    auto decayed = [&](const State& st, int64_t now_ms) -> double {
        return (hl_ms > 0.0 && now_ms > st.last_ms)
            ? st.f * std::exp2(-static_cast<double>(now_ms - st.last_ms) / hl_ms) : st.f;
    };

    auto emit = [&](long long k, int64_t start_ms, int64_t end_ms, double f) {
        if (f <= 0.0) return;
        const float fi = static_cast<float>(f);
        const float pr = static_cast<float>(std::exp(static_cast<double>(k) * lbw));
        liq_field_segs_.push_back({ pr, pr, fi, start_ms, end_ms });
        if (fi > liq_field_max_mag_) liq_field_max_mag_ = fi;
    };

    auto step = [&](const Terminal::Candle& c) {
        if (c.high < c.low || c.low <= 0.0) return;
        // CONSUME: standing fuel this candle's [low,high] trades through ends here (emit + go dark).
        const long long klo = std::llround(std::log(c.low)  * inv_lbw);
        const long long khi = std::llround(std::log(c.high) * inv_lbw);
        auto it   = live.lower_bound(klo);
        auto stop = live.upper_bound(khi);
        while (it != stop) {
            emit(it->first, it->second.run_start, c.timestamp_ms, decayed(it->second, c.timestamp_ms));
            it = live.erase(it);
        }
        // DEPOSIT: candle opens leveraged size → project to its per-tier liquidation prices.
        const double p = (c.high + c.low + c.close) / 3.0;
        if (p <= 0.0 || c.volume <= 0.0) return;
        // ANOMALY weight (MMT's flow-deviation crux) with a NEAR-TIER floor - the round-3e statistic,
        // RESTORED. Brightness comes from the EXCESS over baseline volume (log-compressed + capped);
        // the dim per-candle floor trace applies ONLY to the near tiers (50/75/100×, ≤2% offsets),
        // drawing MMT's short in-channel fragments; far tiers (5/10/25×) are excess-only.
        // Rounds 4/5 post-mortem (belts → soup → all-yellow): gating far deposits harder, then
        // replacing the run SUM with a per-plot MAX, only RESHAPED the intensity distribution - and
        // the dual-percentile map ADAPTS to whatever distribution it gets, so both times it re-
        // normalized the whole field bright (and a 93%-mover's pump projections dominated every level
        // under MAX). The sum keeps the wide, structured dynamic range the map needs; the age-decay
        // above fixes the belts at their actual root: UNBOUNDED STANDING ACCUMULATION over long loads.
        const double ex  = c.volume / vmed - static_cast<double>(liq_field_wbase_);
        const double wex = (ex <= 0.0) ? 0.0 : ((ex <= 1.0) ? ex : 1.0 + std::log(ex));
        const double wfl = static_cast<double>(liq_field_wfloor_);
        if (wex <= 0.0 && wfl <= 0.0) return;
        auto add = [&](double price, double w) {
            if (price <= 0.0) return;
            const long long k0 = std::llround(std::log(price) * inv_lbw);
            for (int d = -K; d <= K; ++d) {
                auto [jt, fresh] = live.try_emplace(
                    k0 + d, State{0.0, c.timestamp_ms, c.timestamp_ms});
                (void)fresh;
                State& st = jt->second;
                if (!fresh) st.f = decayed(st, c.timestamp_ms);   // lazy decay, then deposit
                st.last_ms = c.timestamp_ms;
                st.f += w * kw[d < 0 ? -d : d];
            }
        };
        for (int t = 0; t < 6; ++t) {
            if (!(lmask & kBit[t])) continue;
            const double invL = 1.0 / kLev[t];
            const double w = std::min(wcap, wex + ((invL <= 0.021) ? wfl : 0.0));
            if (w <= 0.0) continue;
            add(p * (1.0 - invL), w);   // long liq (below entry)
            add(p * (1.0 + invL), w);   // short liq (above entry)
        }
    };

    for (const auto& c : candles) step(c);
    if (has_bld) step(cm.building_candle());

    // Flush fuel still standing → PENDING segments that run to the live/replay edge. Pending fuel is
    // decayed to the latest loaded candle's open (a candle-set timestamp, NOT wall clock - the field
    // stays deterministic between rebuilds and identical in replay).
    int64_t t_ref = candles.empty() ? 0 : candles.back().timestamp_ms;
    if (has_bld) t_ref = std::max(t_ref, cm.building_candle().timestamp_ms);
    for (const auto& [k, st] : live) emit(k, st.run_start, LIQ_SEG_PENDING, decayed(st, t_ref));
    if (liq_field_segs_.empty()) { liq_field_max_mag_ = 0.0f; return; }

    liq_field_bw_ = lbw;   // stored: LOG bucket width (grid = exp(k·lbw))

    // DUAL-PERCENTILE LOG normalization (on the RAW per-bucket segments, pre-merge) - MMT's Intensity
    // Low/Peak semantics. Segment mass spans orders of magnitude; a single divisor + power curve either
    // crushed the tail (max-norm, the TAIKO vanish) or bunched everything into flat saturated slabs
    // (p98.5 clip, the BTC "colour walls"). t = ln(f/lo)/ln(hi/lo): below p(lo_pct) → dark, p(hi_pct)+
    // → ramp top, and the decades in between spread purple→cyan→yellow like MMT.
    {
        std::vector<float> mags;
        mags.reserve(liq_field_segs_.size());
        for (const auto& s : liq_field_segs_) mags.push_back(s.intensity);
        auto pick = [&](float pct) -> float {
            const size_t idx = static_cast<size_t>(
                static_cast<double>(mags.size() - 1) * std::clamp(pct, 0.0f, 1.0f));
            std::nth_element(mags.begin(), mags.begin() + idx, mags.end());
            return mags[idx];
        };
        float hi = pick(std::clamp(liq_field_hi_pct_, 0.50f, 1.00f));
        float lo = pick(std::clamp(liq_field_lo_pct_, 0.00f, 0.95f));
        if (hi <= 0.0f) hi = liq_field_max_mag_;
        if (hi <= 0.0f) { liq_field_segs_.clear(); return; }
        // Degenerate spread (young symbol / near-uniform mass) → fall back to a wide fixed range so
        // the map stays defined; everything then reads bright, which is the honest degenerate render.
        if (lo <= 0.0f || lo >= hi * 0.5f) lo = hi / 256.0f;
        liq_field_norm_lo_ = lo;
        liq_field_norm_hi_ = hi;
    }

    // MERGE pass (perf): fuse vertically-adjacent buckets with the SAME time interval and the same
    // QUANTIZED normalized intensity into one taller run - the deposit kernel + dense consolidations
    // emit long identical bucket runs, so this collapses the per-frame AddRectFilled count with no
    // visual change (1/96 of the normalized ramp is under one visible colormap step; the running mean
    // of values inside a shared quantization bin stays inside that bin).
    if (liq_field_segs_.size() > 1) {
        std::sort(liq_field_segs_.begin(), liq_field_segs_.end(),
                  [](const LiqFieldSeg& a, const LiqFieldSeg& b) {
                      if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
                      if (a.end_ms   != b.end_ms)   return a.end_ms   < b.end_ms;
                      return a.price_lo < b.price_lo;
                  });
        const float qlo = liq_field_norm_lo_;
        const float qinv_lr = 1.0f / std::log(liq_field_norm_hi_ / qlo);
        auto quant = [&](float f) -> int {   // quantize in FINAL (log-mapped) t-space
            const float t = (f <= qlo) ? 0.0f
                          : std::min(std::log(f / qlo) * qinv_lr, 1.0f);
            return static_cast<int>(std::lround(t * 96.0f));
        };
        std::vector<LiqFieldSeg> merged;
        merged.reserve(liq_field_segs_.size());
        merged.push_back(liq_field_segs_[0]);
        int run = 1;
        // Adjacency on the LOG grid is a price RATIO of exp(lbw) between neighbouring bucket centers.
        const float r_lo = static_cast<float>(std::exp(0.5 * lbw));
        const float r_hi = static_cast<float>(std::exp(1.5 * lbw));
        for (size_t i = 1; i < liq_field_segs_.size(); ++i) {
            const LiqFieldSeg& s = liq_field_segs_[i];
            LiqFieldSeg& cur = merged.back();
            const float ratio = (cur.price_hi > 0.0f) ? s.price_lo / cur.price_hi : 0.0f;
            if (s.start_ms == cur.start_ms && s.end_ms == cur.end_ms &&
                run < kLiqFieldMaxMergeRun && ratio > r_lo && ratio < r_hi &&
                quant(s.intensity) == quant(cur.intensity)) {
                cur.intensity = (cur.intensity * static_cast<float>(run) + s.intensity) /
                                static_cast<float>(run + 1);
                cur.price_hi = s.price_hi;
                ++run;
            } else {
                merged.push_back(s);
                run = 1;
            }
        }
        liq_field_segs_.swap(merged);
    }

    // Sort by price so the renderer can binary-search the visible Y-band (then X-cull within).
    std::sort(liq_field_segs_.begin(), liq_field_segs_.end(),
              [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
}

// Rebuild the Field cache when its inputs changed (closed-candle set / leverage mask / timeframe).
// Shared by the Field render AND the Liq Profile marginal, so the profile works even when the Field
// layer itself is toggled off. Cheap no-op when nothing changed.
void ChartWidget::ensure_liq_field_cache() {
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    if (candles.empty() && !cm.has_building_candle()) return;
    const uint8_t lmask = ctx_.liq_heatmap_mgr().get_leverage_mask();
    const int64_t tf_ms = cm.timeframe_seconds() * 1000;
    const int64_t sig_ts = candles.empty() ? 0 : candles.back().timestamp_ms;
    if (sig_ts != liq_field_sig_ts_ || candles.size() != liq_field_sig_n_ ||
        lmask != liq_field_sig_mask_ || tf_ms != liq_field_sig_tf_) {
        liq_field_sig_ts_   = sig_ts;
        liq_field_sig_n_    = candles.size();
        liq_field_sig_mask_ = lmask;
        liq_field_sig_tf_   = tf_ms;
        rebuild_liq_field_cache(lmask, tf_ms);
        ++liq_field_rebuild_gen_;   // WS2: keys the texture re-raster (renderer compares)
    }
}

// Liq Heatmap Field - dense candle×leverage liquidation projection (MMT-style backdrop), 2D time×price.
// The field is built as cached horizontal SEGMENTS (rebuild_liq_field_cache) over ALL loaded candles in
// absolute price buckets, so it's STRICTLY STATIC across zoom/scroll and 100% replayable. This function
// only maps the visible segments to pixels: each runs from its deposit time to its consume time (or to
// the live/replay edge if still pending), colored by its own fuel intensity → the traded region stays
// dense and the same level shows different colors on either side of a sweep.
void ChartWidget::render_liq_dense_field() {
    if (!ct_allows_time_overlays(chart_type_)) return;  // TPO / Renko have no time overlays
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();
    if (candles.empty() && !has_bld) return;

    const int64_t tf_ms = cm.timeframe_seconds() * 1000;
    ensure_liq_field_cache();   // shared cache - also feeds the Liq Profile marginal (§8)
    if (liq_field_segs_.empty() || liq_field_norm_hi_ <= 0.0f || liq_field_bw_ <= 0.0) return;
    // WS2: texture-quad path - ONE LUT-shaded GPU quad per frame. Returns false
    // on any unsupported case (rows > 4096, shader init failure) → the rect
    // path below stays the A/B fallback (Tweaks → LIQ FIELD RENDER).
    if (liq_field_use_texture_ && render_liq_field_textured(tf_ms)) return;
    const double bw = liq_field_bw_;

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    const double y_min = lims.Y.Min, y_max = lims.Y.Max;
    const double vx_min = lims.X.Min, vx_max = lims.X.Max;   // visible time window (ms)
    if (y_max <= y_min) return;
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double x_ref = lims.X.Min;   // any X - price→pixel-y is X-independent

    const float x_left  = plot_pos.x;
    const float x_right = plot_pos.x + plot_size.x;
    // Pending-fuel right edge = latest built candle (playback head in replay, wall-current live) + a
    // small forward magnet, clamped to the visible right → never paints across the empty future.
    int64_t latest_ms = 0;
    if (has_bld)                latest_ms = cm.building_candle().timestamp_ms;
    else if (!candles.empty())  latest_ms = candles.back().timestamp_ms;
    float x_live = x_right;
    if (latest_ms > 0) {
        const float cx = ImPlot::PlotToPixels(
            ImPlotPoint(static_cast<double>(latest_ms) + tf_ms * 0.5, y_min)).x;
        x_live = std::clamp(cx, x_left, x_right);
    }
    constexpr double kLiqCascadeMaxCandles =
        static_cast<double>(LiqFieldTextureRenderer::kMaxProjectionCols);
    const float x_projection_end = latest_ms > 0
        ? std::clamp(
            ImPlot::PlotToPixels(ImPlotPoint(
                static_cast<double>(latest_ms) +
                    tf_ms * (0.5 + kLiqCascadeMaxCandles), y_min)).x,
            x_live, x_right)
        : x_right;
    // Base pending right edge = live edge + a small forward magnet. With liq_field_extend_ on, each
    // pending band instead projects right of the live edge ∝ its own strength (x_pend in the loop) -
    // the MMT future cascade, where the projection length doubles as the histogram bar.
    const float x_edge = std::min(x_projection_end, x_live + plot_size.x * 0.06f);

    // Live building-candle carve: while the current candle trades through a price, pending fuel there
    // stops at the candle NOW (no candle-close lag) - the overlap fix, applied per-frame at render.
    double bld_lo = 1.0, bld_hi = 0.0; int64_t bld_ms = 0;
    if (has_bld) { const auto& b = cm.building_candle(); bld_lo = b.low; bld_hi = b.high; bld_ms = b.timestamp_ms; }

    auto time_to_x = [&](int64_t ms, double off) -> float {
        return std::clamp(
            ImPlot::PlotToPixels(ImPlotPoint(static_cast<double>(ms) + off, y_min)).x, x_left, x_right);
    };

    const float gamma    = std::max(0.1f, liq_field_gamma_);
    const int   alpha    = static_cast<int>(std::clamp(liq_field_opacity_, 0.0f, 1.0f) * 255.0f);
    // LOG map between the Low/Peak percentile clips (MMT Intensity semantics; set at build).
    const float nlo = liq_field_norm_lo_;
    if (nlo <= 0.0f || liq_field_norm_hi_ <= nlo) return;
    const float inv_lr  = 1.0f / std::log(liq_field_norm_hi_ / nlo);
    const float floor_t = std::max(0.0f, liq_field_floor_);
    // LOG grid: bw is the bucket width in log-price → neighbours differ by a price RATIO of exp(bw),
    // a bucket's edges sit at center·exp(±bw/2), and "± eps" tests become relative (× (1 ∓ bw/4)).
    const double e_half  = std::exp(bw * 0.5);
    const float  e_lbw   = static_cast<float>(std::exp(bw));
    const float  r_eps_m = 1.0f - static_cast<float>(bw) * 0.25f;
    const float  r_eps_p = 1.0f + static_cast<float>(bw) * 0.25f;

    // Live-carve band in BUCKET-GRID coordinates (centers at exp(k·bw)): the first and last bucket
    // centers the per-bucket carve test would catch (± one bucket around the building candle's range).
    // Splitting merged runs on these bounds reproduces the pre-merge per-bucket carve exactly.
    double carve_glo = 0.0, carve_ghi = -1.0;
    if (bld_hi >= bld_lo && bld_lo > 0.0) {
        const double inv_lbw = 1.0 / bw;
        carve_glo = std::exp(std::ceil (std::log(bld_lo) * inv_lbw - 1.0) * bw);  // first carved center
        carve_ghi = std::exp(std::floor(std::log(bld_hi) * inv_lbw + 1.0) * bw);  // last carved center
    }
    const float glo_f = static_cast<float>(carve_glo), ghi_f = static_cast<float>(carve_ghi);

    // One rect per (sub-)run: [p_lo..p_hi] are bucket CENTERS, drawn ± half a bucket (ratio) tall.
    // Rows are clamped to liq_field_min_row_px_ tall (expanded symmetrically about their center) so
    // alt-tier rows on wide-span movers stay legible bands instead of sub-pixel hairlines (design
    // 2026-07-02 - replaces the ±1 deposit kernel on the render side).
    const float min_row_px = std::max(1.0f, liq_field_min_row_px_);
    auto emit_rect = [&](float p_lo, float p_hi, float xa, float xb, ImU32 col) {
        if (p_hi < p_lo * r_eps_m || xb - xa < 1.0f) return;
        const float ya = ImPlot::PlotToPixels(ImPlotPoint(x_ref, static_cast<double>(p_hi) * e_half)).y;
        const float yb = ImPlot::PlotToPixels(ImPlotPoint(x_ref, static_cast<double>(p_lo) / e_half)).y;
        float y0 = std::min(ya, yb), y1 = std::max(ya, yb) + 1.0f;
        if (y1 - y0 < min_row_px) {
            const float yc = 0.5f * (y0 + y1);
            y0 = yc - 0.5f * min_row_px;
            y1 = yc + 0.5f * min_row_px;
        }
        dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), col);
    };

    // Pending fuel = its history run + the forward CASCADE right of the live edge (design `cascade`
    // spec): the projection drops to liq_cascade_alpha_ × the history alpha, and its tail steps down
    // at kLiqCascadeTailFrac/Alpha - 88/95/100% of the projection length at 100/55/28% alpha (interim;
    // the texture-quad pass will replace the steps with a linear fade over the final 12%).
    auto emit_pending = [&](float p_lo, float p_hi, float pxa, float pxb,
                            uint8_t cr, uint8_t cg, uint8_t cb, int ra) {
        emit_rect(p_lo, p_hi, pxa, std::min(pxb, x_live), IM_COL32(cr, cg, cb, ra));
        const float cs = std::max(pxa, x_live);   // cascade start
        if (pxb <= cs + 0.5f) return;
        const float len = pxb - cs;
        const float ca  = static_cast<float>(ra) * std::clamp(liq_cascade_alpha_, 0.0f, 1.0f);
        float f0 = 0.0f;
        for (int s = 0; s < 3; ++s) {
            const float f1 = kLiqCascadeTailFrac[s];
            emit_rect(p_lo, p_hi, cs + len * f0, cs + len * f1,
                      IM_COL32(cr, cg, cb, static_cast<int>(ca * kLiqCascadeTailAlpha[s])));
            f0 = f1;
        }
    };

    // Clip to the plot area: partially-visible rows are kept by the Y-cull below and must be CLIPPED
    // at the plot edges, not painted over the time axis / readouts (the draw list alone doesn't clip).
    ImPlot::PushPlotClipRect();

    // Y-cull: segments sorted by price_lo → binary-search the bottom of the visible band, iterate up.
    // Merged runs span up to kLiqFieldMaxMergeRun buckets, so the scan starts that many RATIOS lower;
    // runs still entirely below the view are skipped in the loop.
    const double y_lo_margin = (y_min > 0.0)
        ? y_min * std::exp(-bw * (kLiqFieldMaxMergeRun + 1)) : y_min;
    const double y_hi_cut = (y_max > 0.0) ? y_max * std::exp(bw) : y_max;
    const double y_lo_cut = (y_min > 0.0) ? y_min * std::exp(-bw) : y_min;
    const LiqFieldSeg key{ static_cast<float>(y_lo_margin), 0.0f, 0.0f, 0, 0 };
    auto sit = std::lower_bound(liq_field_segs_.begin(), liq_field_segs_.end(), key,
                   [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
    for (; sit != liq_field_segs_.end(); ++sit) {
        const LiqFieldSeg& sg = *sit;
        if (sg.price_lo > y_hi_cut) break;                      // above view → done
        if (sg.price_hi < y_lo_cut) continue;                   // merged run entirely below view
        const bool pending = (sg.end_ms == LIQ_SEG_PENDING);
        if (sg.start_ms > vx_max) continue;                     // X-cull: entirely right of view
        if (!pending && sg.end_ms < vx_min) continue;           // entirely left of view
        if (sg.intensity <= nlo) continue;                      // Intensity-Low clip → dark
        const float tl = std::min(std::log(sg.intensity / nlo) * inv_lr, 1.0f);
        const float tv = std::pow(tl, gamma);
        if (tv < floor_t) continue;                             // noise floor → keep background dark
        uint8_t cr, cg, cb;
        HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, tv, cr, cg, cb);
        // Design alpha curve (export JSON alphaCurve): alpha = opacity·(floor + (1−floor)·t^pow).
        // Dim fuel melts much further into the background than the old (0.55+0.45·t) linear blend
        // (t=0 → 0.072 vs 0.468) while magnets keep near-full presence.
        const int ra = static_cast<int>(static_cast<float>(alpha) *
            (liq_field_alpha_floor_ +
             (1.0f - liq_field_alpha_floor_) * std::pow(tv, liq_field_alpha_pow_)));
        const ImU32 col = IM_COL32(cr, cg, cb, ra);
        // NO-OVERLAP edges (2026-07-02b): a band starts at the DEPOSITING candle's RIGHT edge and (if
        // consumed) ends at the CONSUMING candle's LEFT edge. A level price traded through during
        // candle T must be dark across column T, so bands can never paint over the candles that
        // created or took them; churn consumed on the very next candle collapses into the inter-candle
        // gap (the faint stubs MMT also shows). Old edges (start-left/end-right) overlapped both.
        const float xa = time_to_x(sg.start_ms, tf_ms * 0.35);
        if (!pending) {
            emit_rect(sg.price_lo, sg.price_hi, xa, time_to_x(sg.end_ms, -tf_ms * 0.35), col);
            continue;
        }
        // Standing fuel: history runs [xa .. live edge]; beyond the live edge it projects forward by a
        // length ∝ its strength (MMT future cascade - strong magnets reach the right edge, weak fuel
        // is a stub, so the projection doubles as a price-anchored histogram). Carve where the
        // building candle is trading NOW; merged runs are split so the carve stays per-bucket exact.
        float x_pend = x_edge;
        if (liq_field_extend_) {
            const float span = x_projection_end - x_live;
            if (span > 1.0f)
                x_pend = std::min(
                    x_projection_end, x_live + span * (0.15f + 0.85f * tv));
        }
        if (carve_ghi >= carve_glo && sg.price_hi >= glo_f * r_eps_m && sg.price_lo <= ghi_f * r_eps_p) {
            const float carve_xb = std::min(x_pend, time_to_x(bld_ms, -tf_ms * 0.35));
            if (sg.price_lo < glo_f * r_eps_m)
                emit_pending(sg.price_lo, std::min(sg.price_hi, glo_f / e_lbw), xa, x_pend, cr, cg, cb, ra);
            emit_rect(std::max(sg.price_lo, glo_f), std::min(sg.price_hi, ghi_f), xa, carve_xb, col);
            if (sg.price_hi > ghi_f * r_eps_p)
                emit_pending(std::max(sg.price_lo, ghi_f * e_lbw), sg.price_hi, xa, x_pend, cr, cg, cb, ra);
        } else {
            emit_pending(sg.price_lo, sg.price_hi, xa, x_pend, cr, cg, cb, ra);
        }
    }
    // 1px live-edge seam (design `cascade.seamPx/seamColor`) - the visual boundary between the
    // static history and the forward cascade projection.
    if (latest_ms > 0 && x_live > x_left + 1.0f && x_live < x_right - 1.0f)
        dl->AddLine(ImVec2(x_live, plot_pos.y), ImVec2(x_live, plot_pos.y + plot_size.y),
                    kLiqCascadeSeam, 1.0f);
    ImPlot::PopPlotClipRect();
}

// WS2 - the Field as ONE LUT-shaded textured quad.
// RENDER-ONLY: the segment cache stays the source of truth. The cache is
// rasterized into an R8 log-grid texture on rebuild (keyed by
// liq_field_rebuild_gen_), the live column (building-candle carve) + the
// standing-fuel 1xH texture upload per frame, and the fragment shader applies
// gamma → noise floor → Ember-K LUT → the design alpha curve, plus the forward
// CASCADE with the design's LINEAR fade over the final 12% (replacing the rect
// path's interim stepped tail). Returns false on any unsupported case so
// render_liq_dense_field falls through to the rect path (the A/B fallback).
bool ChartWidget::render_liq_field_textured(int64_t tf_ms) {
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();

    int64_t first_ms = 0;
    if (!candles.empty())  first_ms = candles.front().timestamp_ms;
    else if (has_bld)      first_ms = cm.building_candle().timestamp_ms;
    int64_t latest_ms = 0;
    if (has_bld)                latest_ms = cm.building_candle().timestamp_ms;
    else if (!candles.empty())  latest_ms = candles.back().timestamp_ms;
    if (first_ms <= 0 || latest_ms <= 0 || tf_ms <= 0) return false;

    LiqFieldTextureRenderer::RasterParams rp;
    rp.rebuild_gen = liq_field_rebuild_gen_;
    rp.lbw       = liq_field_bw_;
    rp.norm_lo   = liq_field_norm_lo_;
    rp.norm_hi   = liq_field_norm_hi_;
    rp.tf_ms     = tf_ms;
    rp.first_ms  = first_ms;
    rp.latest_ms = latest_ms;
    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    rp.view_min_ms = plot_limits.X.Min;
    rp.view_max_ms = plot_limits.X.Max;
    if (!liq_field_tex_.ensure(liq_field_segs_, rp)) return false;

    LiqFieldTextureRenderer::FrameParams fp;
    fp.latest_ms = latest_ms;
    if (has_bld) {
        // Live building-candle carve band - the same bucket-grid math as the
        // rect path + the Profile (first/last carved bucket ±1 around range).
        const auto& b = cm.building_candle();
        if (b.high >= b.low && b.low > 0.0) {
            const double inv_lbw = 1.0 / liq_field_bw_;
            fp.carve_klo = static_cast<long long>(
                std::ceil (std::log(b.low)  * inv_lbw - 1.0));
            fp.carve_khi = static_cast<long long>(
                std::floor(std::log(b.high) * inv_lbw + 1.0));
        }
    }
    fp.extend        = liq_field_extend_ ? 1 : 0;
    fp.magnet_frac   = 0.06f;   // the rect path's x_edge forward magnet
    fp.max_projection_cols = LiqFieldTextureRenderer::kMaxProjectionCols;
    fp.gamma         = liq_field_gamma_;
    fp.floor_t       = liq_field_floor_;
    fp.alpha_floor   = liq_field_alpha_floor_;
    fp.alpha_pow     = liq_field_alpha_pow_;
    fp.opacity       = liq_field_opacity_;
    fp.cascade_alpha = liq_cascade_alpha_;
    fp.fade_frac     = 0.12f;   // design: linear fade over the final 12%
    fp.min_row_px    = liq_field_min_row_px_;   // rect-path row clamp parity (§4)

    liq_field_tex_.update_live(fp);
    const float x_live = liq_field_tex_.render(fp);
    if (x_live < 0.0f) return true;   // valid raster; nothing visible this frame

    // 1px live-edge seam (design `cascade.seamPx/seamColor`) - same as the rect path.
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (x_live > plot_pos.x + 1.0f && x_live < plot_pos.x + plot_size.x - 1.0f) {
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddLine(
            ImVec2(x_live, plot_pos.y), ImVec2(x_live, plot_pos.y + plot_size.y),
            kLiqCascadeSeam, 1.0f);
        ImPlot::PopPlotClipRect();
    }
    return true;
}

// WS4 - Observed layer (§7): discrete REAL @forceOrder liquidations as side-tinted dots,
// area ∝ USD. The fact layer against the estimated Field - deliberately a different visual
// grammar (glyphs, not bands). Forced BUY = a SHORT was liquidated (tint Tokens::UP, the
// buy-side force); forced SELL = a LONG was liquidated (Tokens::DOWN). Events come from
// LiquidationHeatmapManager::observed_events - live stream, archive bundle, or the replay
// liquidation_events DB seed - always time-sorted, so the visible window binary-searches.
// Replay correctness needs no special casing here: events are delivered in playback order
// and trimmed on rewind, so nothing past the playback head can exist in the store.
void ChartWidget::render_liq_observed() {
    const auto& evs = ctx_.liq_heatmap_mgr().observed_events(pair_);
    if (evs.empty()) return;

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    if (lims.Y.Max <= lims.Y.Min) return;

    // Visible time window (sorted store → binary search the index span).
    const auto ts_less = [](const Terminal::Liquidation& e, int64_t ts) { return e.timestamp_ms < ts; };
    const auto first = std::lower_bound(evs.begin(), evs.end(),
                                        static_cast<int64_t>(lims.X.Min), ts_less);
    const auto last = std::lower_bound(evs.begin(), evs.end(),
                                       static_cast<int64_t>(lims.X.Max) + 1, ts_less);
    if (first == last) return;

    const float min_usd = std::max(0.0f, liq_obs_min_usd_);
    const float ref_usd = std::max(1.0f, liq_obs_ref_usd_);

    // Draw cap: beyond kLiqObsMaxDraw visible markers, keep the LARGEST by USD
    // (nth_element cutoff). Deterministic per visible set - no frame flicker.
    // Scratch is function-static: reused across frames, no per-frame heap churn.
    float usd_cutoff = min_usd;
    const size_t n_vis = static_cast<size_t>(last - first);
    if (n_vis > static_cast<size_t>(kLiqObsMaxDraw)) {
        static std::vector<float> usd_scratch;
        usd_scratch.clear();
        usd_scratch.reserve(n_vis);
        for (auto it = first; it != last; ++it) {
            const double px = it->avg_price > 0.0 ? it->avg_price : it->price;
            const float usd = static_cast<float>(it->qty * px);
            if (usd >= min_usd) usd_scratch.push_back(usd);
        }
        if (usd_scratch.size() > static_cast<size_t>(kLiqObsMaxDraw)) {
            auto nth = usd_scratch.begin() +
                       (usd_scratch.size() - static_cast<size_t>(kLiqObsMaxDraw));
            std::nth_element(usd_scratch.begin(), nth, usd_scratch.end());
            usd_cutoff = std::max(usd_cutoff, *nth);
        }
    }

    const ImU32 col_up = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Theme::Tokens::UP.x, Theme::Tokens::UP.y, Theme::Tokens::UP.z, 0.85f));
    const ImU32 col_dn = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Theme::Tokens::DOWN.x, Theme::Tokens::DOWN.y, Theme::Tokens::DOWN.z, 0.85f));
    const ImU32 ring = IM_COL32(10, 12, 16, 110);   // dark contrast ring over the bright field

    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    for (auto it = first; it != last; ++it) {
        const double px_price = it->avg_price > 0.0 ? it->avg_price : it->price;
        if (px_price < lims.Y.Min || px_price > lims.Y.Max) continue;
        const float usd = static_cast<float>(it->qty * px_price);
        if (usd < usd_cutoff) continue;
        const ImVec2 c = ImPlot::PlotToPixels(
            ImPlotPoint(static_cast<double>(it->timestamp_ms), px_price));
        // Area ∝ USD (sqrt radius), clamped: dust stays a 1.5px tick, whales cap at 9px.
        const float r = std::clamp(1.5f + 1.1f * std::sqrt(usd / ref_usd), 1.5f, 9.0f);
        const ImU32 col = it->is_buy ? col_up : col_dn;
        if (r >= 2.5f) dl->AddCircle(c, r + 0.8f, ring, 0, 1.5f);
        dl->AddCircleFilled(c, r, col, 0);
    }
    ImPlot::PopPlotClipRect();
}

// ONE tier-mask fuel sum per liq band (handoff §12 DRY - was copy-pasted in the shelf and the
// fuel-ratio pill). mask bits: 0=5x,1=10x,2=25x,3=50x,4=75x,5=100x; observed liqs (obs_*) are not
// tier-specific, so they always count. 0x3F = the mask-independent all-tier total.
static double tiered_band_usd(const Terminal::LiquidationBand& band, uint8_t mask) {
    double u = 0.0;
    if (mask & 0x01) u += band.est_5x_usd;
    if (mask & 0x02) u += band.est_10x_usd;
    if (mask & 0x04) u += band.est_25x_usd;
    if (mask & 0x08) u += band.est_50x_usd;
    if (mask & 0x10) u += band.est_75x_usd;
    if (mask & 0x20) u += band.est_100x_usd;
    return u + band.obs_long_usd + band.obs_short_usd;
}

void ChartWidget::render_liquidation_heatmap(double visible_x_min, double visible_x_max) {
    (void)visible_x_min;
    (void)visible_x_max;
    const auto* snapshot = ctx_.liq_heatmap_mgr().get_snapshot(pair_);
    if (!snapshot || snapshot->bands.empty()) {
        static int no_snap_count = 0;
        if (++no_snap_count % 300 == 1) {  // ~once every 5s at 60fps
        }
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const double mark_price = snapshot->mark_price;

    // Leverage-tier mask guard (25/50/75/100x toggles). 0 = show nothing.
    const uint8_t lmask = ctx_.liq_heatmap_mgr().get_leverage_mask();
    if (lmask == 0) return;

    // latest_ms = the live price bar's time - rails stop there (at consume_ms or the live edge),
    // never across the empty future. tf_ms = the timeframe in ms (for bar-center x mapping).
    int64_t latest_ms = 0;
    if (ctx_.candle_mgr().has_building_candle())
        latest_ms = ctx_.candle_mgr().building_candle().timestamp_ms;
    else if (!ctx_.candle_mgr().candles().empty())
        latest_ms = ctx_.candle_mgr().candles().back().timestamp_ms;
    const int64_t tf_ms = ctx_.candle_mgr().timeframe_seconds() * 1000;

    // ---------------------------------------------------------------------
    // Discrete HISTORICAL liquidation bands (MMT) - lines only, NO cloud.
    // One horizontal line per price level the per-minute timeline has seen
    // (manager get_band_history, leverage-mask aware), drawn from where it first
    // appeared to either the candle that consumed it (price traded through → dim
    // remnant) or the live edge (still standing → bright). first_ms comes from the
    // timeline (stable), so lines can't collapse to ticks. The GPU field is OFF.
    // ---------------------------------------------------------------------
    bool drew_rails = false;   // set when backend rails are available → suppress the shelf fallback
    if (liq_hist_bands_) {
        const auto& tracks = ctx_.liq_heatmap_mgr().get_band_history(pair_, lmask);
        auto& cmh = ctx_.candle_mgr();
        // Consume is BACKEND-BAKED now: each rail carries consume_ms (the first LAST-PRICE
        // wick-through, from the RailTracker) - no client candle scan, correct on deep scroll-back,
        // and captures stop-hunt wicks. We read BandTrack.consume_ms directly below. Gated on candles
        // so the time→x axis is populated before we map rail timestamps to pixels.
        if (!tracks.empty() && !cmh.candles().empty()) {
            drew_rails = true;   // rails available (replay) → the standing-shelf fallback stays hidden
            const float x0 = plot_pos.x;
            const float x_right = plot_pos.x + plot_size.x;
            float x_live = x_right;
            if (latest_ms > 0) {
                const float cx = ImPlot::PlotToPixels(
                    ImPlotPoint(static_cast<double>(latest_ms) + tf_ms * 0.5, mark_price)).x;
                x_live = std::clamp(cx, plot_pos.x, x_right);
            }
            // Standing levels extend to the live bar + a short forward projection (the magnet).
            const float x_anchor = std::min(x_right, x_live + (x_right - plot_pos.x) * 0.06f);
            auto time_to_x = [&](int64_t ms) -> float {
                const float cx = ImPlot::PlotToPixels(
                    ImPlotPoint(static_cast<double>(ms) + tf_ms * 0.5, mark_price)).x;
                return std::clamp(cx, x0, x_right);
            };
            // LAST TOUCH = the most recent candle whose [low,high] spans the rail price (price's last
            // visit to that level). Each rail draws from there into OPEN AIR toward the live edge, so it
            // NEVER paints across the candles where price actually traded - the MMT "no overlap" look.
            // This replaces the FORMATION-time left edge, which painted a standing rail straight across
            // the whole candle history (the rails-over-candles bug James flagged). A level price has not
            // touched in the loaded candles → x0 / full width (it floats in open air anyway).
            auto last_touch = [&](double price) -> int64_t {
                if (cmh.has_building_candle()) {
                    const auto& b = cmh.building_candle();
                    if (price >= b.low && price <= b.high) return b.timestamp_ms;
                }
                for (auto it = cmh.candles().rbegin(); it != cmh.candles().rend(); ++it)
                    if (price >= it->low && price <= it->high) return it->timestamp_ms;
                return 0; // never touched in the loaded window → full-width (open air)
            };
            // 2026-07-01 fix (round 2): the swept-remnant left edge used to anchor at t.first_ms
            // (the backend RailTracker's formed_ms). That's the SAME "formation-time left edge"
            // bug this file's own history already fixed once for the standing case (see the
            // comment above last_touch) - formed_ms/consumed_ms come from the backend's own price
            // series, which doesn't line up with whatever candle timeframe is on screen, so
            // candles between formation and consumption can (and did) trade through the level
            // without the backend calling it "consumed" yet → the line painted over real wicks.
            // Fix: scan backward from the sweep, same touch/no-touch test as last_touch(), and
            // stop at the first candle that DOESN'T span the price - that's the last genuine
            // open-air moment before the sweep. No safe gap found → don't draw a remnant at all
            // (collapses lx==rx below) rather than guess and risk another overlap.
            auto last_departure_before = [&](double price, int64_t before_ms) -> int64_t {
                if (cmh.has_building_candle()) {
                    const auto& b = cmh.building_candle();
                    if (b.timestamp_ms <= before_ms && !(price >= b.low && price <= b.high))
                        return b.timestamp_ms;
                }
                for (auto it = cmh.candles().rbegin(); it != cmh.candles().rend(); ++it) {
                    if (it->timestamp_ms > before_ms) continue;
                    if (!(price >= it->low && price <= it->high)) return it->timestamp_ms;
                }
                return 0; // price touched this level in every loaded candle up to the sweep → no gap
            };
            // Left edge of the candle AT candle_ts (mirrors plot_candles' own half_width=0.35*tf_ms
            // body sizing), used below to stop a swept remnant BEFORE the candle that took it.
            auto candle_left_edge_x = [&](int64_t candle_ts) -> float {
                const float cx = ImPlot::PlotToPixels(
                    ImPlotPoint(static_cast<double>(candle_ts) - tf_ms * 0.35, mark_price)).x;
                return std::clamp(cx, x0, x_right);
            };
            std::vector<LiqDrawLine> cand;
            cand.reserve(tracks.size());
            for (const auto& t : tracks) {
                if (t.price <= 0.0) continue;
                const bool swept = (t.consume_ms > 0);
                float lx, rx;
                if (swept) {
                    const int64_t departure_ms = last_departure_before(t.price, t.consume_ms);
                    // 2026-07-01 fix (round 3): rx used to be time_to_x(t.consume_ms) - but
                    // consume_ms is the backend's raw wall-clock wick-through moment, NOT a candle
                    // boundary, so the "+tf_ms*0.5" offset time_to_x applies (designed for candle-
                    // timestamp inputs, to land past that candle's edge) could instead land the
                    // right edge INSIDE the sweeping candle's own body, depending on where within
                    // that candle's span the sweep happened - still visually "overlapping a wick".
                    // Fix: land rx at the LEFT edge of the candle immediately after the departure
                    // gap (the one that actually swept it, per last_departure_before's definition),
                    // computed with the same candle-boundary math plot_candles itself uses.
                    rx = (departure_ms > 0) ? candle_left_edge_x(departure_ms + tf_ms)
                                             : candle_left_edge_x(t.consume_ms);
                    lx = (departure_ms > 0) ? time_to_x(departure_ms) : rx;   // no gap → no remnant
                } else {
                    // Standing (not yet swept): runs from its last touch to the live-edge magnet.
                    const int64_t touch_ms = last_touch(t.price);
                    lx = (touch_ms > 0) ? time_to_x(touch_ms) : x0;
                    rx = x_anchor;
                }
                cand.push_back({ t.price, t.peak_usd, t.price < mark_price, lx, rx, swept });
            }
            // Rails always use the plain floor/cap now. The dense look moved to the projection
            // Field (render_liq_dense_field); liq_dense_field_ no longer re-tunes the rails.
            const float rail_min_frac    = liq_hist_min_frac_;
            const int   rail_per_side_cap = liq_hist_per_side_cap_;
            double max_usd = 0.0;
            const std::vector<LiqDrawLine> lines = select_liq_lines(
                cand, rail_min_frac,
                mark_price * (static_cast<double>(liq_hist_merge_pct_) / 100.0),
                rail_per_side_cap, 0, max_usd);
            for (const auto& r : lines) {
                const ImVec2 px = ImPlot::PlotToPixels(ImPlotPoint(0, r.price));
                if (px.y < plot_pos.y || px.y > plot_pos.y + plot_size.y) continue;
                // LINEAR magnitude weight (NOT sqrt - sqrt flattened everything to the "wall of
                // lines" look). Dominant rail ≈ full brightness/thickness; weak rails fade thin/dim.
                // (The dense look now lives in the projection Field, not here.)
                const float lin = (max_usd > 0.0)
                    ? static_cast<float>(std::clamp(r.usd / max_usd, 0.0, 1.0)) : 0.0f;
                const float s = lin;
                float cr, cg, cb;
                if (r.is_long) { cr = 200.0f + s * 50.0f; cg = 120.0f + s * 110.0f; cb = 45.0f + s * 25.0f; }
                else           { cr = 60.0f + s * 150.0f; cg = 160.0f + s * 80.0f; cb = 175.0f - s * 95.0f; }
                // Swept remnant: same hue, dimmed + thinned toward a "taken" look (build -> persist
                // -> discharge) instead of the disappearing act this had before the 2026-07-01 fix.
                // Floored (not just multiplied) so weak-but-swept levels stay legible rather than
                // fading to a near-invisible sliver - a remnant should read as "was here, dimmer",
                // not "barely rendered".
                const float alpha = r.swept ? std::max(70.0f, (95.0f + s * 160.0f) * 0.55f)
                                             : (95.0f + s * 160.0f);          // 95 (faint) → 255 (dominant)
                const float thickness = r.swept ? std::max(1.6f, (1.3f + s * 4.2f) * 0.55f)
                                                 : (1.3f + s * 4.2f);
                const ImU32 col = IM_COL32(
                    static_cast<int>(std::clamp(cr, 0.0f, 255.0f)),
                    static_cast<int>(std::clamp(cg, 0.0f, 255.0f)),
                    static_cast<int>(std::clamp(cb, 0.0f, 255.0f)),
                    static_cast<int>(std::clamp(alpha, 0.0f, 255.0f)));
                if (r.rx - r.lx >= 1.0f)
                    draw_list->AddLine(ImVec2(r.lx, px.y), ImVec2(r.rx, px.y), col, thickness);
            }
        }
    }

    // Standing shelves = the LIVE fallback (+ rollback): drawn ONLY when no backend rails are
    // available (live has no rails yet), so the chart is never bare; suppressed in replay where the
    // de-cluttered rails render instead.
    if (liq_show_lines_ && !drew_rails) {
        // ── Standing liquidation shelves (current levels, optional overlay on the field) ──
        // Each CURRENT liquidity level (from the live snapshot - always present, live AND replay)
        // draws as a bright horizontal band at its price, anchored to the live edge (+ a short
        // forward projection = the standing magnet) and extending LEFT only as far as the most
        // recent candle that traded through that price (its "last touch"). So bands span the
        // chart as standing shelves, never cross candles, and a re-approached level always
        // reappears near price. Brightness + thickness scale with size; warm = below price
        // (long fuel), cool = above (short fuel). No "origin" timestamp, so it can't collapse to ticks.
        const double bw = std::max(snapshot->band_width_pct / 100.0 * mark_price, mark_price * 0.0002);
        auto& cmh = ctx_.candle_mgr();
        if (bw > 0.0 && !snapshot->bands.empty()) {
            // PERF: the agg-over-ALL-bands + sort/merge/cap selection only changes when the snapshot
            // (~1/min) or the leverage mask changes - NOT every frame. Cache the selected levels
            // (price/usd/side); the per-frame path below just resolves pixel-x for the ~N kept lines
            // and draws. (This block previously rebuilt over ~800 bands EVERY frame - the FPS hit.)
            const int64_t snap_ts = snapshot->timestamp_ms;
            if (snap_ts != liq_shelf_cache_ts_ || lmask != liq_shelf_cache_mask_
                || liq_dense_field_ != liq_shelf_cache_dense_) {
                liq_shelf_cache_ts_ = snap_ts;
                liq_shelf_cache_mask_ = lmask;
                liq_shelf_cache_dense_ = liq_dense_field_;
                liq_shelf_cache_.clear();
                liq_shelf_cache_max_usd_ = 0.0;
                // Aggregate per price bucket. `agg` holds the ENABLED-tier fuel (what the toggle
                // shows); `full_max` is the ALL-tier fuel max (mask-INDEPENDENT). We floor + scale
                // brightness against full_max - NOT the per-mask max - so the LIQ LEV toggle actually
                // thins the map: 100x-only keeps ~15% of the mass, so most bands fall under the
                // absolute floor and only the genuine near-price 100x levels survive. (Before this,
                // both the floor and brightness renormalized to the per-mask max, so every mask
                // looked identical - the toggle was a visual no-op. See LIQMAP V1.)
                std::unordered_map<long long, double> agg;   // price bucket → enabled-tier fuel (USD)
                double full_max = 0.0;
                for (const auto& band : snapshot->bands) {
                    // Respect the leverage-tier toggles; all-tier total = the stable floor/brightness ref.
                    const double u    = tiered_band_usd(band, lmask);
                    const double full = tiered_band_usd(band, 0x3F);
                    if (full > full_max) full_max = full;
                    if (u <= 0.0) continue;
                    const long long k = llround(band.price_mid / bw);
                    double& slot = agg[k];
                    slot = std::max(slot, u);
                }
                // Absolute floor = a fraction of the ALL-tier max. Scale-invariant across symbols
                // (it's still the symbol's own all-tier max) but FIXED across mask changes, so a
                // masked-out band that can't clear it simply disappears.
                // (§12 cleanup 2026-07-03: the kLiqDense* constants that let liq_dense_field_ flip
                // the shelf floor/contrast are DELETED - leftover coupling from before the Field
                // existed. The shelf reads its own knobs regardless of the Field toggle.)
                const double abs_floor = full_max * static_cast<double>(liq_hist_min_frac_);
                if (!agg.empty() && full_max > 0.0) {
                    // Select on price/usd/side only (lx/rx are pixel positions → resolved per-frame).
                    std::vector<LiqDrawLine> cand;
                    cand.reserve(agg.size());
                    for (const auto& a : agg) {
                        const double price = static_cast<double>(a.first) * bw;
                        if (price <= 0.0 || a.second < abs_floor) continue;  // absolute floor → mask-aware thinning
                        cand.push_back({ price, a.second, price < mark_price, 0.0f, 0.0f });
                    }
                    const int cap = (liq_hist_max_lines_ > 0)
                        ? std::min(liq_hist_max_lines_, liq_hist_max_lines_cap_)
                        : liq_hist_max_lines_cap_;
                    double sel_max = 0.0;
                    // min_frac = 0: the floor is already applied above against the all-tier max;
                    // select_liq_lines only merges nearby levels + applies the top-N cap here.
                    const std::vector<LiqDrawLine> sel = select_liq_lines(
                        cand, 0.0f,
                        mark_price * (static_cast<double>(liq_hist_merge_pct_) / 100.0),
                        0, cap, sel_max);
                    // Brightness/thickness normalize to the all-tier max (see draw loop), so a masked
                    // view renders DIMMER + sparser instead of renormalizing back to full brightness.
                    liq_shelf_cache_max_usd_ = full_max;
                    liq_shelf_cache_.reserve(sel.size());
                    for (const auto& s2 : sel) liq_shelf_cache_.push_back({ s2.price, s2.usd, s2.is_long });
                }
            }
            if (!liq_shelf_cache_.empty()) {
                const float x0 = plot_pos.x;
                const float x_right = plot_pos.x + plot_size.x;
                // Right anchor: the live bar + a short forward projection (the standing magnet).
                float x_live = x_right;
                if (latest_ms > 0) {
                    const float cx = ImPlot::PlotToPixels(
                        ImPlotPoint(static_cast<double>(latest_ms) + tf_ms * 0.5, mark_price)).x;
                    x_live = std::clamp(cx, plot_pos.x, x_right);
                }
                const float x_anchor = std::min(x_right, x_live + (x_right - plot_pos.x) * 0.06f);
                auto time_to_x = [&](int64_t ms) -> float {
                    const float cx = ImPlot::PlotToPixels(
                        ImPlotPoint(static_cast<double>(ms) + tf_ms * 0.5, mark_price)).x;
                    return std::clamp(cx, x0, x_right);
                };
                // Most recent candle (scanning back) whose [low,high] spans price = last touch. Runs
                // only on the ~N cached shelves now, not every agg bucket.
                auto last_touch = [&](double price) -> int64_t {
                    if (cmh.has_building_candle()) {
                        const auto& b = cmh.building_candle();
                        if (price >= b.low && price <= b.high) return b.timestamp_ms;
                    }
                    for (auto it = cmh.candles().rbegin(); it != cmh.candles().rend(); ++it)
                        if (price >= it->low && price <= it->high) return it->timestamp_ms;
                    return 0;   // never touched in history → full-width shelf
                };
                const double max_usd = liq_shelf_cache_max_usd_;
                for (const auto& sh : liq_shelf_cache_) {
                    const ImVec2 px = ImPlot::PlotToPixels(ImPlotPoint(0, sh.price));
                    if (px.y < plot_pos.y || px.y > plot_pos.y + plot_size.y) continue;
                    // Default: sqrt (pow 0.5) contrast - gentler than linear. Dense mode softens
                    // further to pow(0.35) so more of the ~100 active bands stay legible (see
                    // liq_dense_field_ above; matches the rail path's curve for a consistent look).
                    const float shelf_lin = (max_usd > 0.0)
                        ? static_cast<float>(std::clamp(sh.usd / max_usd, 0.0, 1.0)) : 0.0f;
                    const float s = std::pow(shelf_lin, 0.5f);
                    float cr, cg, cb;
                    if (sh.is_long) { cr = 210.0f + s * 40.0f; cg = 120.0f + s * 120.0f; cb = 55.0f + s * 30.0f; }
                    else            { cr = 70.0f + s * 150.0f; cg = 170.0f + s * 70.0f;  cb = 170.0f - s * 90.0f; }
                    const float alpha = 190.0f + s * 65.0f;     // bright: 190 -> 255
                    const float thickness = 2.0f + s * 3.3f;    // 2.0 -> 5.3 px
                    const ImU32 line_color = IM_COL32(
                        static_cast<int>(std::clamp(cr, 0.0f, 255.0f)),
                        static_cast<int>(std::clamp(cg, 0.0f, 255.0f)),
                        static_cast<int>(std::clamp(cb, 0.0f, 255.0f)),
                        static_cast<int>(std::clamp(alpha, 0.0f, 255.0f)));
                    const int64_t lt = last_touch(sh.price);
                    const float lx = (lt > 0) ? time_to_x(lt) : x0;
                    if (x_anchor - lx >= 1.0f)
                        draw_list->AddLine(ImVec2(lx, px.y), ImVec2(x_anchor, px.y), line_color, thickness);
                }
            }
        }
    }

    // (Removed the dashed "mark" guide line - redundant with the price-axis mark
    //  tag and the candles; rails + extend-to-candle already anchor to the live bar.)

    // -- Hawkes cascade indicator (BR pill) -- top-right of the plot.
    if (snapshot->hawkes_branching_ratio > 0.01) {
        const float br = static_cast<float>(snapshot->hawkes_branching_ratio);
        const float sev = static_cast<float>(snapshot->hawkes_severity);
        char hawkes_buf[64];
        if (sev > 0.1f && br > 0.3f)
            snprintf(hawkes_buf, sizeof(hawkes_buf), "BR %.2f  Sev %.0f%%", br, sev * 100);
        else
            snprintf(hawkes_buf, sizeof(hawkes_buf), "BR %.2f", br);
        ImU32 br_color;
        if (br >= 0.9f)
            br_color = IM_COL32(255, 50, 30, 220);
        else if (br >= 0.7f)
            br_color = IM_COL32(255, 150, 30, 200);
        else if (br >= 0.3f)
            br_color = IM_COL32(255, 220, 50, 180);
        else
            br_color = IM_COL32(80, 200, 120, 160);
        const ImVec2 text_size = ImGui::CalcTextSize(hawkes_buf);
        const float x = plot_pos.x + plot_size.x - text_size.x - 8.0f;
        const float y = plot_pos.y + 4.0f;
        ImU32 pill_bg = IM_COL32(15, 15, 20, 180);
        if (br >= 0.7f) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f * 3.14159f);
            const uint8_t pulse_r = static_cast<uint8_t>(40 + pulse * 60);
            const uint8_t pulse_a = static_cast<uint8_t>(160 + pulse * 60);
            pill_bg = IM_COL32(pulse_r, 10, 10, pulse_a);
        }
        draw_list->AddRectFilled(
            ImVec2(x - 4.0f, y - 1.0f),
            ImVec2(x + text_size.x + 4.0f, y + text_size.y + 1.0f),
            pill_bg, 3.0f);
        draw_list->AddText(ImVec2(x, y), br_color, hawkes_buf);
    }

    // -- Liq Fuel Ratio pill -- estimated USD above vs below mark price.
    // Tells the trader which side has more liquidation fuel at a glance.
    {
        const uint8_t fuel_mask = ctx_.liq_heatmap_mgr().get_leverage_mask();
        double fuel_below = 0.0; // Long liquidation fuel (longs get rekt)
        double fuel_above = 0.0; // Short liquidation fuel (shorts get rekt)
        for (const auto& band : snapshot->bands) {
            const double est = tiered_band_usd(band, fuel_mask);
            if (band.price_mid < mark_price)
                fuel_below += est;
            else
                fuel_above += est;
        }
        if (fuel_below > 100.0 || fuel_above > 100.0) {
            char fuel_buf[80];
            auto fmt_usd = [](char* buf, size_t sz, double v) {
                if (v >= 1e6)      snprintf(buf, sz, "$%.1fM", v / 1e6);
                else if (v >= 1e3) snprintf(buf, sz, "$%.0fK", v / 1e3);
                else               snprintf(buf, sz, "$%.0f", v);
            };
            char below_buf[16], above_buf[16];
            fmt_usd(below_buf, sizeof(below_buf), fuel_below);
            fmt_usd(above_buf, sizeof(above_buf), fuel_above);
            snprintf(fuel_buf, sizeof(fuel_buf), "L:%s / S:%s", below_buf, above_buf);

            double ratio = (fuel_below > fuel_above && fuel_above > 1.0)
                ? fuel_below / fuel_above
                : (fuel_above > fuel_below && fuel_below > 1.0)
                    ? fuel_above / fuel_below : 1.0;
            bool long_dominant = fuel_below > fuel_above;

            ImU32 fuel_color;
            if (ratio < 1.5)
                fuel_color = IM_COL32(160, 160, 170, 200); // neutral
            else if (long_dominant)
                fuel_color = IM_COL32(230, 170, 60, 220);  // amber -- more long liqs
            else
                fuel_color = IM_COL32(60, 190, 220, 220);  // cyan -- more short liqs

            const ImVec2 fuel_text_size = ImGui::CalcTextSize(fuel_buf);
            // Stack below the BR pill when it is showing (BR -> Fuel).
            float fuel_y_offset = 0.0f;
            if (snapshot->hawkes_branching_ratio > 0.01)
                fuel_y_offset += ImGui::CalcTextSize("BR").y + 6.0f;
            const float fuel_x = plot_pos.x + plot_size.x - fuel_text_size.x - 8.0f;
            const float fuel_y = plot_pos.y + 4.0f + fuel_y_offset;
            draw_list->AddRectFilled(
                ImVec2(fuel_x - 4.0f, fuel_y - 1.0f),
                ImVec2(fuel_x + fuel_text_size.x + 4.0f, fuel_y + fuel_text_size.y + 1.0f),
                IM_COL32(15, 15, 20, 180), 3.0f);
            draw_list->AddText(ImVec2(fuel_x, fuel_y), fuel_color, fuel_buf);
        }
    }
}

// ── "Liq Levels HL" (P2e) - REAL predictive liq levels from the HL census ───────────────
// Renders the census snapshot (LiquidationHeatmapManager census store, UNDERLYING-keyed)
// in the standing-shelf grammar of the modelled layer - same render reuse as the backend's
// message reuse - but with a DISTINCT colormap so real-vs-modelled reads at a glance:
// longs (real positions liquidating below mark) = violet, shorts (above) = mint; the
// modelled layer keeps its amber/cyan. No rails/timeline/remnants: the census is a ~60s
// snapshot of standing real positions, not a history. The badge (top-right, stacked under
// the modelled BR/Fuel pills when those show) carries coverage_frac - the census covers
// the at-risk leveraged tail only (~1-10% of OI notional) and must NEVER read as a
// complete liq map. The LIQ LEV chips filter this layer by REAL leverage tier (est_Nx =
// actual position leverage; the 5x/10x tiers exist on the wire but, as for the modelled
// layer, have no UI chip - their far-from-mark levels are rarely cascade-relevant).
void ChartWidget::render_liq_census() {
    // Live-only layer: replay has no census data - stay silent, no badge.
    if (ctx_.candle_mgr().replay_start_time_ms() > 0) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();

    // Badge stacking: sit under the modelled layer's BR/Fuel pills when that layer
    // is rendering (approximate - the fuel pill hides only when fuel < $100).
    float badge_y_off = 0.0f;
    if (liq_heatmap_enabled_) {
        if (const auto* m = ctx_.liq_heatmap_mgr().get_snapshot(pair_); m && !m->bands.empty()) {
            const float row_h = ImGui::CalcTextSize("BR").y + 6.0f;
            if (m->hawkes_branching_ratio > 0.01) badge_y_off += row_h;
            badge_y_off += row_h;
        }
    }
    auto draw_badge = [&](const char* text, ImU32 color) {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float x = plot_pos.x + plot_size.x - ts.x - 8.0f;
        const float y = plot_pos.y + 4.0f + badge_y_off;
        draw_list->AddRectFilled(ImVec2(x - 4.0f, y - 1.0f),
                                 ImVec2(x + ts.x + 4.0f, y + ts.y + 1.0f),
                                 IM_COL32(15, 15, 20, 180), 3.0f);
        draw_list->AddText(ImVec2(x, y), color, text);
    };

    const auto* snap = ctx_.liq_heatmap_mgr().get_census_snapshot(liq_census_pair_);
    if (!snap || snap->bands.empty()) {
        // Layer on but nothing arrived (feed warming up, or no HL market for this
        // underlying) - say so instead of silently rendering nothing.
        draw_badge("HL \xc2\xb7 NO DATA", IM_COL32(160, 160, 170, 170));
        return;
    }

    const uint8_t lmask = ctx_.liq_heatmap_mgr().get_leverage_mask();
    if (lmask == 0) return;
    // REAL-leverage filter, venue-aware: HL caps leverage PER ASSET (HYPE ≤10x,
    // WLFI ≤5x - live probe 2026-07-24: 100% of their census notional sits in
    // the 5x/10x tiers; even BTC holds ~44% there). The LIQ LEV chips only go
    // down to 25x, so with ALL FOUR on (the default = "no filter") we show ALL
    // real tiers (0x3F) rather than silently hiding venue-capped positions; a
    // narrowed chip selection is respected exactly.
    const uint8_t cmask = (lmask == 0x3C) ? 0x3F : lmask;
    const double mark_price = snap->mark_price;

    // Selection cache: rebuilt only when the snapshot ts or the leverage mask
    // changes (mirrors liq_shelf_cache_ - never per frame).
    if (snap->timestamp_ms != liq_census_cache_ts_ || lmask != liq_census_cache_mask_) {
        liq_census_cache_ts_ = snap->timestamp_ms;
        liq_census_cache_mask_ = lmask;
        liq_census_cache_.clear();
        liq_census_cache_max_usd_ = 0.0;
        // Bands ARE the discrete census buckets (0.25% of mark) - no re-bucketing.
        // Floor against the ALL-tier max, same mask-thinning rule as the shelves:
        // a masked view goes sparser + dimmer, never renormalizes back to bright.
        //
        // SCOPED floor (2026-07-28): the max is taken PER SIDE over bands within
        // liq_census_rel_window_ of mark, not globally. The census is REAL positions, so on
        // a thin alt one whale's far-away liq can sit orders of magnitude above every level
        // near price - a global floor then erases the whole near-price layer while the whale
        // itself is culled off-screen anyway, leaving a COV badge over an empty chart.
        // Measured on KAITO: a single $305k position at +81% floored out all 12 levels.
        // Bands outside the window are floored against the same per-side number, so they are
        // still decluttered and never promoted above their own side's near-price scale.
        const double rel_win = mark_price * static_cast<double>(liq_census_rel_window_);
        double win_max[2] = {0.0, 0.0};   // [0] = short (above mark), [1] = long (below)
        double all_max[2] = {0.0, 0.0};
        double full_max = 0.0;
        for (const auto& band : snap->bands) {
            const double u = tiered_band_usd(band, 0x3F);
            full_max = std::max(full_max, u);
            if (band.price_mid <= 0.0 || u <= 0.0) continue;
            const int side = (band.price_mid < mark_price) ? 1 : 0;
            all_max[side] = std::max(all_max[side], u);
            if (rel_win > 0.0 && std::abs(band.price_mid - mark_price) <= rel_win)
                win_max[side] = std::max(win_max[side], u);
        }
        // An empty window (every level on that side sits far from price) falls back to that
        // side's own max, reproducing the old behaviour for it rather than dropping the floor
        // to zero and flooding the chart with dust.
        double side_floor[2];
        for (int s = 0; s < 2; ++s) {
            const double base = (win_max[s] > 0.0) ? win_max[s] : all_max[s];
            side_floor[s] = base * static_cast<double>(liq_hist_min_frac_);
        }
        std::vector<LiqDrawLine> cand;
        cand.reserve(snap->bands.size());
        for (const auto& band : snap->bands) {
            const double u = tiered_band_usd(band, cmask);   // REAL leverage tiers
            if (band.price_mid <= 0.0 || u <= 0.0) continue;
            const bool is_long = band.price_mid < mark_price;
            if (u < side_floor[is_long ? 1 : 0]) continue;
            cand.push_back({ band.price_mid, u, is_long, 0.0f, 0.0f });
        }
        double sel_max = 0.0;
        const std::vector<LiqDrawLine> sel = select_liq_lines(
            cand, 0.0f,
            mark_price * (static_cast<double>(liq_hist_merge_pct_) / 100.0),
            liq_hist_per_side_cap_, 0, sel_max);
        // Brightness still normalizes against the GLOBAL max on purpose: the floor decides
        // what is worth drawing, the ramp encodes how big it actually is. A near-price level
        // that is small next to the venue's biggest real position should read as small - and
        // it stays legible regardless, since the census alpha ramp floors at 190/255.
        liq_census_cache_max_usd_ = full_max;
        liq_census_cache_.reserve(sel.size());
        for (const auto& s2 : sel) liq_census_cache_.push_back({ s2.price, s2.usd, s2.is_long });
    }

    // Coverage badge - always on while the layer has data (coverage honesty).
    {
        const double cov = std::clamp(snap->flow_intensity, 0.0, 1.0);
        char cov_buf[48];
        if (cov >= 0.095) snprintf(cov_buf, sizeof(cov_buf), "HL \xc2\xb7 COV %.0f%%", cov * 100.0);
        else              snprintf(cov_buf, sizeof(cov_buf), "HL \xc2\xb7 COV %.1f%%", cov * 100.0);
        draw_badge(cov_buf, IM_COL32(110, 245, 200, 230));
    }
    if (liq_census_cache_.empty()) return;

    // Shelf geometry, same grammar as the modelled standing shelves: each level runs
    // from its last touch (never across candles that traded through it) to the live
    // edge + the short forward magnet.
    auto& cmh = ctx_.candle_mgr();
    int64_t latest_ms = 0;
    if (cmh.has_building_candle()) latest_ms = cmh.building_candle().timestamp_ms;
    else if (!cmh.candles().empty()) latest_ms = cmh.candles().back().timestamp_ms;
    const int64_t tf_ms = cmh.timeframe_seconds() * 1000;

    const float x0 = plot_pos.x;
    const float x_right = plot_pos.x + plot_size.x;
    float x_live = x_right;
    if (latest_ms > 0) {
        const float cx = ImPlot::PlotToPixels(
            ImPlotPoint(static_cast<double>(latest_ms) + tf_ms * 0.5, mark_price)).x;
        x_live = std::clamp(cx, plot_pos.x, x_right);
    }
    const float x_anchor = std::min(x_right, x_live + (x_right - plot_pos.x) * 0.06f);
    auto time_to_x = [&](int64_t ms) -> float {
        const float cx = ImPlot::PlotToPixels(
            ImPlotPoint(static_cast<double>(ms) + tf_ms * 0.5, mark_price)).x;
        return std::clamp(cx, x0, x_right);
    };
    auto last_touch = [&](double price) -> int64_t {
        if (cmh.has_building_candle()) {
            const auto& b = cmh.building_candle();
            if (price >= b.low && price <= b.high) return b.timestamp_ms;
        }
        for (auto it = cmh.candles().rbegin(); it != cmh.candles().rend(); ++it)
            if (price >= it->low && price <= it->high) return it->timestamp_ms;
        return 0;   // never touched in the loaded window → full-width shelf
    };

    const double max_usd = liq_census_cache_max_usd_;
    for (const auto& lv : liq_census_cache_) {
        const ImVec2 px = ImPlot::PlotToPixels(ImPlotPoint(0, lv.price));
        if (px.y < plot_pos.y || px.y > plot_pos.y + plot_size.y) continue;
        const float lin = (max_usd > 0.0)
            ? static_cast<float>(std::clamp(lv.usd / max_usd, 0.0, 1.0)) : 0.0f;
        const float s = std::pow(lin, 0.5f);
        // DISTINCT census ramp: violet (real long liqs, below mark) / mint (real
        // short liqs, above) - never the modelled amber/cyan.
        float cr, cg, cb;
        if (lv.is_long) { cr = 150.0f + s * 60.0f; cg = 85.0f + s * 55.0f;  cb = 235.0f + s * 20.0f; }
        else            { cr = 45.0f + s * 65.0f;  cg = 190.0f + s * 65.0f; cb = 150.0f + s * 60.0f; }
        const float alpha = 190.0f + s * 65.0f;
        const float thickness = 2.0f + s * 3.3f;
        const ImU32 col = IM_COL32(
            static_cast<int>(std::clamp(cr, 0.0f, 255.0f)),
            static_cast<int>(std::clamp(cg, 0.0f, 255.0f)),
            static_cast<int>(std::clamp(cb, 0.0f, 255.0f)),
            static_cast<int>(std::clamp(alpha, 0.0f, 255.0f)));
        const int64_t lt = last_touch(lv.price);
        const float lx = (lt > 0) ? time_to_x(lt) : x0;
        if (x_anchor - lx >= 1.0f)
            draw_list->AddLine(ImVec2(lx, px.y), ImVec2(x_anchor, px.y), col, thickness);
    }
}

// Liquidation Profile - the price-marginal of STANDING fuel (WS1 Option A, 2026-07-03; replaces the
// time-averaged rendered-brightness marginal, which was window-length-dependent and needed an
// arbitrary width gain to be visible at all). Each visible price row takes the MAX of the field's
// own rendered brightness (fixed dual-percentile log map + gamma - the exact tv the forward cascade
// projects with) across the PENDING segments covering it: the sidebar answers "which prices hold the
// most fuel RIGHT NOW", agrees with the cascade by construction (a zone's cascade length IS its tv),
// and is zoom/scroll/window-stable. NO adaptive re-normalization (the round-3/4 "orange wall"), NO
// width gain. Consumed history doesn't count - the field itself already shows it. Buckets the live
// building candle is trading through are carved (same grid band as the field's live carve). Style =
// design `profile` spec: slim right-edge silhouette, LUT tint at FLAT alpha, gaussian sigma = 3
// field rows, 1px outline, baseline hairline. Candle-derived → universal + replay-identical;
// independent toggle (liq_profile_enabled_). §8.
void ChartWidget::render_liq_profile() {
    ensure_liq_field_cache();
    if (liq_field_segs_.empty() || liq_field_norm_hi_ <= 0.0f || liq_field_bw_ <= 0.0) return;

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    const double y_min = lims.Y.Min, y_max = lims.Y.Max;
    if (y_max <= y_min || y_min <= 0.0) return;
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double bw = liq_field_bw_;

    // The field's FIXED log map (set at cache build) - the profile inherits Low/Peak/gamma directly
    // and never re-normalizes.
    const float nlo = liq_field_norm_lo_;
    if (nlo <= 0.0f || liq_field_norm_hi_ <= nlo) return;
    const float inv_lr = 1.0f / std::log(liq_field_norm_hi_ / nlo);
    const float pgamma = std::max(0.1f, liq_field_gamma_);

    // Live building-candle carve - the same bucket-grid band as render_liq_dense_field: fuel the
    // current candle is trading through is being consumed NOW, so it no longer stands.
    double carve_glo = 0.0, carve_ghi = -1.0;
    {
        auto& cm = ctx_.candle_mgr();
        if (cm.has_building_candle()) {
            const auto& b = cm.building_candle();
            if (b.high >= b.low && b.low > 0.0) {
                const double inv_lbw = 1.0 / bw;
                carve_glo = std::exp(std::ceil (std::log(b.low)  * inv_lbw - 1.0) * bw);
                carve_ghi = std::exp(std::floor(std::log(b.high) * inv_lbw + 1.0) * bw);
            }
        }
    }
    const double r_eps_m = 1.0 - bw * 0.25;
    const double r_eps_p = 1.0 + bw * 0.25;

    // 1) MAX-raster the standing fuel into N price rows. Each bucket paints its full ± half-bucket
    //    span (clamped to liq_field_min_row_px_, mirroring the field's emit_rect row clamp) so a
    //    lone shelf enters the smoothing pass as a small plateau, not a 1px spike it would flatten.
    //    MAX (not sum) across buckets sharing a row keeps the reading zoom-stable: zooming out can
    //    never inflate a row past the strongest shelf actually standing there.
    const int N = std::clamp(static_cast<int>(plot_size.y), 64, 1200);
    liq_profile_scratch_.assign(static_cast<size_t>(N), 0.0f);
    const double rows_per_price = static_cast<double>(N) / (y_max - y_min);
    const double e_half = std::exp(bw * 0.5);
    const double half_row_min = 0.5 * static_cast<double>(std::max(1.0f, liq_field_min_row_px_))
                                * (static_cast<double>(N) / plot_size.y);   // px → rows
    // LOG grid: bw is the bucket width in log-price (see rebuild_liq_field_cache).
    const double y_lo_margin = y_min * std::exp(-bw * (kLiqFieldMaxMergeRun + 1));
    const double y_hi_cut = y_max * std::exp(bw);
    const double y_lo_cut = y_min * std::exp(-bw);
    const LiqFieldSeg key{ static_cast<float>(y_lo_margin), 0.0f, 0.0f, 0, 0 };
    auto sit = std::lower_bound(liq_field_segs_.begin(), liq_field_segs_.end(), key,
                   [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
    for (; sit != liq_field_segs_.end(); ++sit) {
        const LiqFieldSeg& sg = *sit;
        if (sg.price_lo > y_hi_cut) break;
        if (sg.price_hi < y_lo_cut) continue;                   // merged run entirely below view
        if (sg.end_ms != LIQ_SEG_PENDING) continue;             // STANDING fuel only (Option A)
        if (sg.intensity <= nlo) continue;                      // same Intensity-Low clip as the field
        const float tl = std::min(std::log(sg.intensity / nlo) * inv_lr, 1.0f);
        const float tv = std::pow(tl, pgamma);                  // the field's rendered brightness
        if (tv <= 0.0f) continue;
        // A merged segment spans several buckets - test/deposit per bucket, exactly like pre-merge.
        const int nb = (sg.price_hi > sg.price_lo && sg.price_lo > 0.0f)
            ? 1 + static_cast<int>(std::lround(std::log(static_cast<double>(sg.price_hi) /
                                                        static_cast<double>(sg.price_lo)) / bw))
            : 1;
        for (int bi = 0; bi < nb; ++bi) {
            const double pr = static_cast<double>(sg.price_lo) * std::exp(bi * bw);
            if (pr < y_lo_cut || pr > y_hi_cut) continue;
            if (carve_ghi >= carve_glo && pr >= carve_glo * r_eps_m && pr <= carve_ghi * r_eps_p)
                continue;                                       // being traded through right now
            const double rc = (pr - y_min) * rows_per_price;
            const double half = std::max(half_row_min,
                                         pr * (e_half - 1.0 / e_half) * rows_per_price * 0.5);
            int r0 = std::max(0,     static_cast<int>(std::floor(rc - half)));
            int r1 = std::min(N - 1, static_cast<int>(std::ceil (rc + half)));
            for (int r = r0; r <= r1; ++r) {
                float& cell = liq_profile_scratch_[static_cast<size_t>(r)];
                if (tv > cell) cell = tv;
            }
        }
    }

    // 2) Gaussian smooth - design `profile.smoothingGaussianSigmaRows` = 3 FIELD price rows.
    //    The marginal is sampled at ~1 row/pixel, so the sigma is converted from field rows to
    //    samples via the current zoom (field row height in px = plot_h · bw / ln(y_span)) - the
    //    profile's smoothness tracks the field's visible row size. True gaussian, kernel on the stack.
    {
        const double span_l = (y_min > 0.0 && y_max > y_min) ? std::log(y_max / y_min) : 0.0;
        float sigma = (span_l > 0.0)
            ? static_cast<float>(static_cast<double>(liq_profile_sigma_rows_) * bw / span_l
                                 * static_cast<double>(N))
            : 3.0f;
        // Min clamp 2.5px: on very wide loaded ranges (a 40% mover fully zoomed out) 3 field rows
        // shrink sub-pixel and the silhouette went spiky/jagged (2026-07-03 MAGMA screenshots).
        sigma = std::clamp(sigma, 2.5f, 40.0f);
        const int R = std::min(96, static_cast<int>(std::ceil(sigma * 2.5f)));
        float kw[97];
        float ksum = 0.0f;
        for (int i = 0; i <= R; ++i) {
            kw[i] = std::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
            ksum += (i ? 2.0f : 1.0f) * kw[i];
        }
        for (int i = 0; i <= R; ++i) kw[i] /= ksum;
        liq_profile_scratch2_.assign(liq_profile_scratch_.begin(), liq_profile_scratch_.end());
        for (int i = 0; i < N; ++i) {
            float acc = liq_profile_scratch2_[static_cast<size_t>(i)] * kw[0];
            for (int j = 1; j <= R; ++j) {
                if (i - j >= 0) acc += liq_profile_scratch2_[static_cast<size_t>(i - j)] * kw[j];
                if (i + j < N)  acc += liq_profile_scratch2_[static_cast<size_t>(i + j)] * kw[j];
            }
            liq_profile_scratch_[static_cast<size_t>(i)] = acc;
        }
    }

    // 3) Design render (export `profile` spec): a slim silhouette growing left from the right plot
    //    edge - LUT-tinted per row at a FLAT liq_profile_alpha_, one 1px outline at
    //    kLiqProfileOutline, and a 1px baseline hairline at the plot edge. Width AND tint are the
    //    SAME bounded standing-fuel brightness (no re-normalization, no gain) → the silhouette can
    //    never read hotter or wider than the fuel the field/cascade actually draw: quiet prices hug
    //    the baseline, magnet shelves bulge and glow through.
    ImPlot::PushPlotClipRect();
    const float x_right = plot_pos.x + plot_size.x;
    const float max_w   = plot_size.x * std::clamp(liq_profile_width_, 0.05f, 0.45f);
    const float row_h   = plot_size.y / static_cast<float>(N);
    const int   fill_a  = static_cast<int>(std::clamp(liq_profile_alpha_, 0.0f, 1.0f) * 255.0f);
    const float v_floor = std::max(0.004f, liq_field_floor_);   // same noise floor as the field
    liq_profile_pts_.clear();
    liq_profile_pts_.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const float v = std::clamp(liq_profile_scratch_[static_cast<size_t>(i)], 0.0f, 1.0f);
        const float py = plot_pos.y + plot_size.y - (static_cast<float>(i) + 0.5f) * row_h;
        const float xc = x_right - v * max_w;
        if (v > v_floor) {
            uint8_t cr, cg, cb;
            HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, v, cr, cg, cb);
            dl->AddRectFilled(ImVec2(xc, py - row_h * 0.5f - 0.5f),
                              ImVec2(x_right, py + row_h * 0.5f + 0.5f),
                              IM_COL32(cr, cg, cb, fill_a));
        }
        liq_profile_pts_.push_back(ImVec2(xc, py));
    }
    if (liq_profile_pts_.size() >= 2)
        dl->AddPolyline(liq_profile_pts_.data(), static_cast<int>(liq_profile_pts_.size()),
                        kLiqProfileOutline, 0, 1.0f);
    // 1px baseline hairline at the plot edge (design `profile.baseline`).
    dl->AddLine(ImVec2(x_right - 0.5f, plot_pos.y),
                ImVec2(x_right - 0.5f, plot_pos.y + plot_size.y), kLiqProfileBaseline, 1.0f);
    ImPlot::PopPlotClipRect();
}


// ═══════════════════════════════════════════════════════════════════════════════
// Research pattern overlay
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::handle_pattern_overlay(const Terminal::PatternOverlay& pattern) {
    size_t slot = pattern_overlays_.size();
    for (size_t i = 0; i < pattern_overlays_.size(); ++i) {
        if (pattern_overlay_active_[i] &&
            pattern_overlays_[i].timeframe_ms == pattern.timeframe_ms) {
            slot = i;
            break;
        }
    }

    // This surface is for live candidates only. Expired is the ordinary death
    // state; treating every other terminal state as removal also prevents a
    // breakout update from leaving a stale line behind indefinitely.
    if (pattern.state != "forming") {
        if (slot < pattern_overlay_active_.size() &&
            (pattern.pattern_id.empty() ||
             pattern_overlays_[slot].pattern_id == pattern.pattern_id)) {
            pattern_overlay_active_[slot] = false;
        }
        return;
    }

    if (slot == pattern_overlays_.size()) {
        for (size_t i = 0; i < pattern_overlay_active_.size(); ++i) {
            if (!pattern_overlay_active_[i]) {
                slot = i;
                break;
            }
        }
    }
    if (slot == pattern_overlays_.size()) return;

    // One current candidate per timeframe: a new pattern id replaces the old
    // slot, so a newly-added terminal swing cannot strand the prior segment.
    pattern_overlays_[slot] = pattern;
    pattern_overlay_active_[slot] = true;
}

void ChartWidget::render_pattern_overlay(double visible_x_min, double visible_x_max) {
    if (!Entitlements::is_research()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const ImVec2 plot_max{plot_min.x + plot_size.x, plot_min.y + plot_size.y};
    const ImU32 resistance_color = Theme::u32(Theme::Tokens::DOWN, 0.78f);
    const ImU32 support_color = Theme::u32(Theme::Tokens::UP, 0.78f);
    const ImU32 label_bg = Theme::u32(Theme::Tokens::PANEL, 0.94f);
    const ImU32 label_border = Theme::u32(Theme::Tokens::BD2, 0.92f);
    const ImU32 label_text = Theme::u32(Theme::Tokens::TX2);

    int rendered = 0;
    ImPlot::PushPlotClipRect();
    for (size_t i = 0; i < pattern_overlays_.size(); ++i) {
        if (!pattern_overlay_active_[i]) continue;
        const auto& pattern = pattern_overlays_[i];
        if (pattern.state != "forming" || pattern.end_ms <= pattern.start_ms) continue;
        if (static_cast<double>(pattern.end_ms) < visible_x_min ||
            static_cast<double>(pattern.start_ms) > visible_x_max) {
            continue;
        }

        const double x0 = std::max(visible_x_min, static_cast<double>(pattern.start_ms));
        const double x1 = std::min(visible_x_max, static_cast<double>(pattern.end_ms));
        if (x1 <= x0) continue;

        const double resistance_y0 = pattern.resistance_at(static_cast<int64_t>(x0));
        const double resistance_y1 = pattern.resistance_at(static_cast<int64_t>(x1));
        const double support_y0 = pattern.support_at(static_cast<int64_t>(x0));
        const double support_y1 = pattern.support_at(static_cast<int64_t>(x1));
        if (!std::isfinite(resistance_y0) || !std::isfinite(resistance_y1) ||
            !std::isfinite(support_y0) || !std::isfinite(support_y1)) {
            continue;
        }

        const ImVec2 resistance_px0 = ImPlot::PlotToPixels(x0, resistance_y0);
        const ImVec2 resistance_px1 = ImPlot::PlotToPixels(x1, resistance_y1);
        const ImVec2 support_px0 = ImPlot::PlotToPixels(x0, support_y0);
        const ImVec2 support_px1 = ImPlot::PlotToPixels(x1, support_y1);
        draw_list->AddLine(resistance_px0, resistance_px1, resistance_color, 1.8f);
        draw_list->AddLine(support_px0, support_px1, support_color, 1.5f);

        const double support_start = pattern.support_at(pattern.start_ms);
        const double support_end = pattern.support_at(pattern.end_ms);
        const double support_delta = support_end - support_start;
        const double support_scale = std::max(std::abs(support_start), std::abs(support_end));
        const double flat_epsilon = std::max(tick_size_, support_scale * 1e-8);
        const char* support_regime = pattern.convergence < 0.0 ? "none"
            : std::abs(support_delta) <= flat_epsilon ? "flat"
            : support_delta > 0.0 ? "rising" : "falling";

        char timeframe[12];
        if (pattern.timeframe_ms % 86400000LL == 0) {
            snprintf(timeframe, sizeof(timeframe), "%lldD",
                     static_cast<long long>(pattern.timeframe_ms / 86400000LL));
        } else if (pattern.timeframe_ms % 3600000LL == 0) {
            snprintf(timeframe, sizeof(timeframe), "%lldH",
                     static_cast<long long>(pattern.timeframe_ms / 3600000LL));
        } else {
            snprintf(timeframe, sizeof(timeframe), "%lldM",
                     static_cast<long long>(pattern.timeframe_ms / 60000LL));
        }

        char label[96];
        snprintf(label, sizeof(label), "%s  %dT  %.0fD  SUP %s",
                 timeframe, pattern.touch_count, pattern.span_days, support_regime);
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        const ImVec2 pad{5.0f, 3.0f};
        float label_x = resistance_px1.x + 5.0f;
        if (label_x + text_size.x + pad.x * 2.0f > plot_max.x - 3.0f) {
            label_x = resistance_px1.x - text_size.x - pad.x * 2.0f - 5.0f;
        }
        label_x = std::clamp(label_x, plot_min.x + 3.0f,
                             plot_max.x - text_size.x - pad.x * 2.0f - 3.0f);
        float label_y = resistance_px1.y -
            static_cast<float>(rendered + 1) * (text_size.y + pad.y * 2.0f + 3.0f);
        label_y = std::clamp(label_y, plot_min.y + 3.0f,
                             plot_max.y - text_size.y - pad.y * 2.0f - 3.0f);
        const ImVec2 label_min{label_x, label_y};
        const ImVec2 label_max{label_x + text_size.x + pad.x * 2.0f,
                               label_y + text_size.y + pad.y * 2.0f};
        draw_list->AddRectFilled(label_min, label_max, label_bg, 3.0f);
        draw_list->AddRect(label_min, label_max, label_border, 3.0f, 0, 1.0f);
        draw_list->AddText(ImVec2(label_x + pad.x, label_y + pad.y), label_text, label);
        ++rendered;
    }
    ImPlot::PopPlotClipRect();
}



// ═══════════════════════════════════════════════════════════════════════════════
// TPO / Market Profile Renderer
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::render_tpo(double visible_x_min, double visible_x_max) {
    auto& tpo = ctx_.tpo_mgr();

    // Build sessions from current candle data
    const auto& timestamps = ctx_.candle_mgr().timestamps();
    const auto& highs = ctx_.candle_mgr().highs();
    const auto& lows = ctx_.candle_mgr().lows();
    if (timestamps.empty()) return;

    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    double tick_per_row = tpo.ticks_per_row_setting > 0
        ? tpo.ticks_per_row_setting * tick_size_ : 0.0;

    tpo.build_sessions(pair_.symbol,
                       timestamps.data(), highs.data(), lows.data(),
                       timestamps.size(), tf_sec, tick_per_row);

    const auto* sessions = tpo.get_sessions(pair_.symbol);
    if (!sessions || sessions->empty()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.75f;

    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double vis_y_min = plot_limits.Y.Min;
    const double vis_y_max = plot_limits.Y.Max;

    for (size_t si = 0; si < sessions->size(); ++si) {
        const auto& sess = (*sessions)[si];
        double sess_start = static_cast<double>(sess.session_start_ms);
        double sess_end = static_cast<double>(sess.session_end_ms);

        // Skip sessions outside visible range
        if (sess_end < visible_x_min || sess_start > visible_x_max) continue;
        if (sess.rows.empty() || sess.max_block_count == 0) continue;

        // Session pixel bounds on x-axis
        const ImVec2 px_sess_start = ImPlot::PlotToPixels(sess_start, 0.0);
        const ImVec2 px_sess_end = ImPlot::PlotToPixels(sess_end, 0.0);
        const float sess_px_width = px_sess_end.x - px_sess_start.x;
        if (sess_px_width < 2.0f) continue;

        // Block dimensions: make blocks roughly square
        // Row height in pixels determines block width for square appearance
        double sample_row_mid = (sess.session_high + sess.session_low) * 0.5;
        const ImVec2 px_row_top = ImPlot::PlotToPixels(sess_start, sample_row_mid + sess.tick_per_row * 0.5);
        const ImVec2 px_row_bot = ImPlot::PlotToPixels(sess_start, sample_row_mid - sess.tick_per_row * 0.5);
        float row_px_height = std::abs(px_row_bot.y - px_row_top.y);
        if (row_px_height < 1.0f) row_px_height = 1.0f;

        // Block width: scale so widest row fills ~82% of session (18% gap for labels/lines)
        const float spacing = static_cast<float>(tpo.profile_spacing);
        float usable_width = sess_px_width * 0.82f - spacing;
        if (usable_width < 2.0f) usable_width = 2.0f;
        float block_px_width = 1.0f;
        if (sess.max_block_count > 0) {
            block_px_width = usable_width / static_cast<float>(sess.max_block_count);
        }
        // Cap: never wider than 3x row height (reasonable max when zoomed way in)
        float max_block_w = row_px_height * 3.0f;
        if (max_block_w < 4.0f) max_block_w = 4.0f;
        if (block_px_width > max_block_w) block_px_width = max_block_w;
        if (block_px_width < 1.0f) block_px_width = 1.0f;
        float profile_left_x = px_sess_start.x;

        // Draw blocks per row
        for (const auto& row : sess.rows) {
            if (row.blocks.empty()) continue;
            if (row.price_hi < vis_y_min || row.price_lo > vis_y_max) continue;

            const ImVec2 px_top = ImPlot::PlotToPixels(sess_start, row.price_hi);
            const ImVec2 px_bot = ImPlot::PlotToPixels(sess_start, row.price_lo);
            float row_top = px_top.y;
            float row_bot = px_bot.y;
            float row_height = row_bot - row_top;
            if (row_height < 1.0f) row_height = 1.0f;

            int block_count = static_cast<int>(row.blocks.size());

            for (int b = 0; b < block_count; ++b) {
                float bx_left = profile_left_x + b * block_px_width;
                float bx_right = bx_left + block_px_width - 1.0f;
                if (bx_right < bx_left + 1.0f) bx_right = bx_left + 1.0f;

                // Color based on value area / period
                ImU32 color;
                int period_idx = row.blocks[b].period_idx;

                if (row.is_poc) {
                    // POC row - white (MMT style)
                    color = IM_COL32(220, 220, 230, 230);
                } else if (row.is_value_area) {
                    // Value area - blue with brightness by period
                    float brightness = 0.4f + 0.5f *
                        (static_cast<float>(period_idx) / std::max(1, sess.total_periods - 1));
                    color = IM_COL32(
                        static_cast<int>(30 + 80 * brightness),
                        static_cast<int>(60 + 90 * brightness),
                        static_cast<int>(140 + 80 * brightness), 220);
                } else {
                    // Outside value area - grey with brightness by period
                    float brightness = 0.25f + 0.5f *
                        (static_cast<float>(period_idx) / std::max(1, sess.total_periods - 1));
                    int grey = static_cast<int>(60 + 120 * brightness);
                    color = IM_COL32(grey, grey, grey + 5, 200);
                }

                // Highlight start/end periods (overrides above)
                if (tpo.highlight_start_end) {
                    if (period_idx == 0) {
                        color = IM_COL32(50, 90, 200, 230); // blue for first period
                    } else if (period_idx == sess.total_periods - 1) {
                        color = IM_COL32(200, 70, 110, 230); // pink for last period
                    }
                }

                // Single print highlight (overrides above)
                if (row.is_single_print && tpo.show_single_prints) {
                    color = IM_COL32(80, 160, 210, 170); // light blue
                }

                // Poor High/Low row - white blocks (MMT style)
                if (tpo.show_poor_high_low) {
                    if (sess.has_poor_high && &row == &sess.rows.back()) {
                        color = IM_COL32(220, 220, 230, 230);
                    }
                    if (sess.has_poor_low && &row == &sess.rows.front()) {
                        color = IM_COL32(220, 220, 230, 230);
                    }
                }

                draw_list->AddRectFilled(
                    ImVec2(bx_left, row_top + 0.5f),
                    ImVec2(bx_right, row_bot - 0.5f), color);
            }
        }

        // ── Profile right edge (for line endpoints) ─────────────
        float profile_right_x = profile_left_x +
            sess.max_block_count * block_px_width;
        float line_extend = block_px_width * 2.0f; // extend a bit past profile
        float line_end_x = profile_right_x + line_extend;
        // Clamp lines to session boundary
        float max_line_x = px_sess_end.x - 2.0f;
        if (line_end_x > max_line_x) line_end_x = max_line_x;
        if (line_end_x < profile_right_x) line_end_x = profile_right_x;

        // Labels positioned just past line end (always shown, may overlap next session like MMT)
        float label_x = line_end_x + 3.0f;

        // ── POC line (solid white, short - from profile edge) ────
        if (tpo.show_poc_ray && sess.poc_row_idx >= 0) {
            ImVec2 poc_px = ImPlot::PlotToPixels(sess_start, sess.poc_price);
            draw_list->AddLine(
                ImVec2(profile_right_x, poc_px.y),
                ImVec2(line_end_x, poc_px.y),
                IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddText(font, font_size,
                ImVec2(label_x, poc_px.y - font_size * 0.5f),
                IM_COL32(255, 255, 255, 255), "POC");
        }

        // ── VAH / VAL lines (dashed) ─────────────────────────────
        if (tpo.show_vah_val_rays) {
            ImVec2 vah_px = ImPlot::PlotToPixels(sess_start, sess.vah);
            for (float x = profile_right_x; x < line_end_x; x += 6.0f) {
                float x2 = std::min(x + 3.0f, line_end_x);
                draw_list->AddLine(ImVec2(x, vah_px.y), ImVec2(x2, vah_px.y),
                                   IM_COL32(100, 150, 210, 180), 1.0f);
            }
            draw_list->AddText(font, font_size,
                ImVec2(label_x, vah_px.y - font_size * 0.5f),
                IM_COL32(100, 150, 210, 200), "VAH");

            ImVec2 val_px = ImPlot::PlotToPixels(sess_start, sess.val);
            for (float x = profile_right_x; x < line_end_x; x += 6.0f) {
                float x2 = std::min(x + 3.0f, line_end_x);
                draw_list->AddLine(ImVec2(x, val_px.y), ImVec2(x2, val_px.y),
                                   IM_COL32(100, 150, 210, 180), 1.0f);
            }
            draw_list->AddText(font, font_size,
                ImVec2(label_x, val_px.y - font_size * 0.5f),
                IM_COL32(100, 150, 210, 200), "VAL");
        }

        // ── Poor High / Low (solid white line + label) ───────────
        if (tpo.show_poor_high_low) {
            if (sess.has_poor_high) {
                ImVec2 ph = ImPlot::PlotToPixels(sess_start, sess.session_high);
                draw_list->AddLine(
                    ImVec2(profile_right_x, ph.y),
                    ImVec2(line_end_x, ph.y),
                    IM_COL32(255, 255, 255, 220), 1.0f);
                draw_list->AddText(font, font_size,
                    ImVec2(label_x, ph.y - font_size * 0.5f),
                    IM_COL32(255, 255, 255, 255), "PH");
            }
            if (sess.has_poor_low) {
                ImVec2 pl = ImPlot::PlotToPixels(sess_start, sess.session_low);
                draw_list->AddLine(
                    ImVec2(profile_right_x, pl.y),
                    ImVec2(line_end_x, pl.y),
                    IM_COL32(255, 255, 255, 220), 1.0f);
                draw_list->AddText(font, font_size,
                    ImVec2(label_x, pl.y - font_size * 0.5f),
                    IM_COL32(255, 255, 255, 255), "PL");
            }
        }

        // ── Session boundary line ─────────────────────────────────
        {
            const ImVec2 plot_pos = ImPlot::GetPlotPos();
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            draw_list->AddLine(
                ImVec2(px_sess_start.x, plot_pos.y),
                ImVec2(px_sess_start.x, plot_pos.y + plot_size.y),
                IM_COL32(80, 80, 100, 100), 1.0f);
        }

        // ── Hover detection ──────────────────────────────────────
        // Profile bounding box: from session start to rightmost block edge
        {
            ImVec2 box_tl = ImPlot::PlotToPixels(sess_start, sess.session_high);
            box_tl.x = profile_left_x;
            ImVec2 box_br = ImPlot::PlotToPixels(sess_start, sess.session_low);
            box_br.x = profile_right_x;

            ImVec2 mouse = ImGui::GetMousePos();
            if (mouse.x >= box_tl.x && mouse.x <= box_br.x &&
                mouse.y >= box_tl.y && mouse.y <= box_br.y) {
                tpo_hovered_session_ = static_cast<int>(si);

                // White outline around hovered session
                draw_list->AddRect(
                    ImVec2(box_tl.x - 1.0f, box_tl.y - 1.0f),
                    ImVec2(box_br.x + 1.0f, box_br.y + 1.0f),
                    IM_COL32(220, 220, 220, 160), 0.0f, 0, 1.5f);

                // Right-click opens context menu
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    ImPlot::IsPlotHovered()) {
                    tpo_context_session_ = static_cast<int>(si);
                    ImGui::OpenPopup("tpo_session_context");
                }
            }
        }
    }

    ImPlot::PopPlotClipRect();

    // ── TPO Session Context Menu (must be outside clip rect) ─────
    if (ImGui::BeginPopup("tpo_session_context")) {
        int ctx_si = tpo_context_session_;
        if (ctx_si >= 0 && ctx_si < static_cast<int>(sessions->size())) {
            const auto& ctx_sess = (*sessions)[ctx_si];

            if (ImGui::Selectable("Expand")) {
                tpo.toggle_expand(pair_.symbol, ctx_si);
            }

            // Merge right - greyed out if rightmost session
            bool can_merge_right = (ctx_si + 1 < static_cast<int>(sessions->size()));
            if (!can_merge_right) ImGui::BeginDisabled();
            if (ImGui::Selectable("Merge right \xe2\x86\x92|")) {
                // TODO: Phase 2 - merge sessions
            }
            if (!can_merge_right) ImGui::EndDisabled();

            // Merge left - greyed out if leftmost session
            bool can_merge_left = (ctx_si > 0);
            if (!can_merge_left) ImGui::BeginDisabled();
            if (ImGui::Selectable("Merge left |\xe2\x86\x90")) {
                // TODO: Phase 2 - merge sessions
            }
            if (!can_merge_left) ImGui::EndDisabled();

            if (ImGui::BeginMenu("Prices")) {
                char buf[64];
                snprintf(buf, sizeof(buf), "POC: %.1f", ctx_sess.poc_price);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "VAH: %.1f", ctx_sess.vah);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "VAL: %.1f", ctx_sess.val);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "High: %.1f", ctx_sess.session_high);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "Low: %.1f", ctx_sess.session_low);
                ImGui::TextDisabled("%s", buf);
                ImGui::EndMenu();
            }

            if (ImGui::Selectable("Reset chart")) {
                tpo.clear(pair_.symbol);
            }
        }
        ImGui::EndPopup();
    }

    // Reset hover if mouse not over any session this frame
    if (tpo_hovered_session_ >= 0) {
        // Will be set again next frame if still hovering
        tpo_hovered_session_ = -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Footprint Chart Overlay
// ═══════════════════════════════════════════════════════════════════════════════

// Format volume for footprint cells: compact K/M notation
static void format_fp_volume(char* buf, size_t buf_size, double vol) {
    double abs_vol = std::abs(vol);
    if (abs_vol >= 1000000.0) {
        snprintf(buf, buf_size, "%.1fM", vol / 1000000.0);
    } else if (abs_vol >= 1000.0) {
        snprintf(buf, buf_size, "%.0fK", vol / 1000.0);
    } else if (abs_vol >= 1.0) {
        snprintf(buf, buf_size, "%.0f", vol);
    } else {
        snprintf(buf, buf_size, "%.1f", vol);
    }
}

void ChartWidget::render_footprint_overlay(double visible_x_min, double visible_x_max) {
    auto& fp_mgr = ctx_.footprint_mgr();
    const auto& timestamps = ctx_.candle_mgr().timestamps();
    if (timestamps.empty()) return;

    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const double tf_ms = static_cast<double>(tf_sec) * 1000.0;
    const double visible_candles = (visible_x_max - visible_x_min) / tf_ms;
    const int64_t tf_ms_i = static_cast<int64_t>(tf_sec) * 1000;

    // Hard cutoff - too many candles, skip entirely
    if (visible_candles > 500.0) return;

    const double pixels_per_candle = ImPlot::GetPlotSize().x / visible_candles;
    if (pixels_per_candle < 3.0) return;

    // Zoom modes:
    //   zoomed_out_block: single colored rect per candle (no per-level detail)
    //                     Kicks in when candles are too narrow for text - matches MMT behavior
    //   show_text:        draw text labels inside cells
    const bool zoomed_out_block = (pixels_per_candle < 55.0);
    const bool show_text = !zoomed_out_block;

    // Get visible Y range for vertical culling
    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double visible_y_min = plot_limits.Y.Min;
    const double visible_y_max = plot_limits.Y.Max;

    // Request historical data if needed (deduplication handled by FootprintManager)
    {
        const int64_t start_ms = static_cast<int64_t>(visible_x_min) - tf_ms_i;
        const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms_i;
        fp_mgr.request_history(pair_, start_ms, end_ms, &ctx_.stream_mgr());
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const auto& opens = ctx_.candle_mgr().opens();
    const auto& closes = ctx_.candle_mgr().closes();
    const auto& lows = ctx_.candle_mgr().lows();
    const auto& highs = ctx_.candle_mgr().highs();

    // Font setup (only used when show_text)
    float font_scale = 1.0f;
    if (pixels_per_candle < 80.0f) font_scale = 0.7f;
    else if (pixels_per_candle < 120.0f) font_scale = 0.8f;
    ImFont* font = ImGui::GetFont();
    const float fp_font_size = ImGui::GetFontSize() * font_scale;

    // Tick per row - with minimum pixel height guarantee
    double tick_per_row = fp_mgr.ticks_per_row > 0
        ? fp_mgr.ticks_per_row * tick_size_ : 0.0;

    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min - tf_ms);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    for (size_t i = start_idx; i < timestamps.size(); ++i) {
        const double ts = static_cast<double>(timestamps[i]);
        if (ts > visible_x_max + tf_ms) break;

        // Early cull: skip candles entirely off-screen vertically
        if (highs[i] < visible_y_min || lows[i] > visible_y_max) continue;

        const double candle_center = ts + tf_ms * 0.5;

        // ── ZOOMED OUT BLOCK MODE ──────────────────────────────────────
        // Single colored rectangle per candle, delta-colored (blue/pink)
        if (zoomed_out_block) {
            // Use cached merge - no per-frame allocation
            const auto* mc = fp_mgr.get_merged_grouped(
                pair_.symbol, timestamps[i], tf_sec, 0.0);
            if (!mc) continue;

            double delta = mc->delta;
            float dn = (mc->total_buy + mc->total_sell > 0)
                ? static_cast<float>(std::abs(delta) / (mc->total_buy + mc->total_sell))
                : 0.0f;
            dn = std::sqrt(dn);

            const double body_top_price = std::max(opens[i], closes[i]);
            const double body_bot_price = std::min(opens[i], closes[i]);
            const double half_w = tf_ms * 0.35;
            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, body_top_price);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, body_bot_price);
            const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
            const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);

            float top_y = std::min(px_top.y, px_bot.y);
            float bot_y = std::max(px_top.y, px_bot.y);
            if (bot_y - top_y < 2.0f) {
                float cy = (top_y + bot_y) * 0.5f;
                top_y = cy - 1.0f; bot_y = cy + 1.0f;
            }

            ImU32 block_color;
            if (delta >= 0.0) {
                block_color = IM_COL32(
                    static_cast<int>(20 + 30 * dn),
                    static_cast<int>(30 + 70 * dn),
                    static_cast<int>(50 + 140 * dn), 230);
            } else {
                block_color = IM_COL32(
                    static_cast<int>(40 + 140 * dn),
                    static_cast<int>(20 + 25 * dn),
                    static_cast<int>(40 + 70 * dn), 230);
            }
            draw_list->AddRectFilled(ImVec2(px_left.x, top_y), ImVec2(px_right.x, bot_y), block_color);
            continue;
        }

        // ── DETAILED CELL MODE (zoomed in) ─────────────────────────────
        // Auto tick_per_row with minimum pixel height guarantee
        double effective_tpr = tick_per_row;
        if (effective_tpr <= 0.0) {
            double range = highs[i] - lows[i];
            if (range <= 0.0) range = 1.0;
            effective_tpr = range / 18.0;
            const ImVec2 px_hi = ImPlot::PlotToPixels(candle_center, highs[i]);
            const ImVec2 px_lo = ImPlot::PlotToPixels(candle_center, lows[i]);
            float candle_px_height = std::abs(px_lo.y - px_hi.y);
            if (candle_px_height > 0.0f) {
                int max_rows = std::max(3, static_cast<int>(candle_px_height / 12.0f));
                double min_tpr = range / static_cast<double>(max_rows);
                if (effective_tpr < min_tpr) effective_tpr = min_tpr;
            }
        }

        // Use cached merge+group - no per-frame vector allocations
        const auto* mc = fp_mgr.get_merged_grouped(
            pair_.symbol, timestamps[i], tf_sec, effective_tpr);
        if (!mc) continue;
        const auto& grouped = mc->levels;

        // Find max volume for color scaling
        double max_vol = 0.0;
        double max_abs_delta = 0.0;
        for (const auto& gl : grouped) {
            if (gl.total_volume > max_vol) max_vol = gl.total_volume;
            double ad = std::abs(gl.delta);
            if (ad > max_abs_delta) max_abs_delta = ad;
        }
        if (max_vol < 1e-8) continue;

        // Candle pixel bounds - footprint box IS the candle body
        const double half_w = tf_ms * 0.35;
        const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
        const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);
        const float cell_width = px_right.x - px_left.x;
        if (cell_width < 2.0f) continue;

        for (const auto& gl : grouped) {
            // Vertical culling - skip levels off-screen
            if (gl.price_hi < visible_y_min || gl.price_lo > visible_y_max) continue;

            const ImVec2 px_top    = ImPlot::PlotToPixels(candle_center, gl.price_hi);
            const ImVec2 px_bottom = ImPlot::PlotToPixels(candle_center, gl.price_lo);
            const float row_top    = px_top.y;
            const float row_bottom = px_bottom.y;
            const float row_height = row_bottom - row_top;
            if (row_height < 1.0f) continue;

            const float left  = px_left.x;
            const float right = px_right.x;

            if (fp_mgr.mode == FootprintManager::Mode::SellsBuys) {
                float mid_x = left + cell_width * 0.5f;

                // MMT-style: dark base, pink-red for sells, blue-teal for buys
                float si = max_vol > 0 ? static_cast<float>(gl.sell_volume / max_vol) : 0.0f;
                float bi = max_vol > 0 ? static_cast<float>(gl.buy_volume / max_vol) : 0.0f;
                si = std::sqrt(si); bi = std::sqrt(bi);

                ImU32 sell_bg = IM_COL32(
                    static_cast<int>(25 + 130 * si),
                    static_cast<int>(14 + 20 * si),
                    static_cast<int>(30 + 60 * si),
                    static_cast<int>(230 + 25 * si));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(mid_x, row_bottom), sell_bg);

                ImU32 buy_bg = IM_COL32(
                    static_cast<int>(14 + 20 * bi),
                    static_cast<int>(20 + 55 * bi),
                    static_cast<int>(35 + 120 * bi),
                    static_cast<int>(230 + 25 * bi));
                draw_list->AddRectFilled(ImVec2(mid_x, row_top), ImVec2(right, row_bottom), buy_bg);

                // Subtle vertical divider between sell/buy columns
                draw_list->AddLine(ImVec2(mid_x, row_top), ImVec2(mid_x, row_bottom),
                                   IM_COL32(100, 100, 120, 80), 1.0f);

                // Issue 4 fix: lowered threshold from 0.7f to 0.5f
                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char sell_buf[16], buy_buf[16];
                    format_fp_volume(sell_buf, sizeof(sell_buf), gl.sell_volume);
                    format_fp_volume(buy_buf, sizeof(buy_buf), gl.buy_volume);
                    float text_y = row_top + (row_height - fp_font_size) * 0.5f;
                    float half_cell = cell_width * 0.5f;

                    // Center sell text in left half
                    ImVec2 sell_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, sell_buf);
                    float sell_x = left + (half_cell - sell_size.x) * 0.5f;
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(sell_x, text_y),
                        IM_COL32(230, 215, 215, 255), sell_buf);

                    // Center buy text in right half
                    ImVec2 buy_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, buy_buf);
                    float buy_x = mid_x + (half_cell - buy_size.x) * 0.5f;
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(buy_x, text_y),
                        IM_COL32(215, 225, 240, 255), buy_buf);
                }

            } else if (fp_mgr.mode == FootprintManager::Mode::Delta) {
                float dn = max_abs_delta > 0
                    ? static_cast<float>(std::sqrt(std::abs(gl.delta) / max_abs_delta)) : 0.0f;
                ImU32 bg = (gl.delta >= 0.0)
                    ? IM_COL32(14+int(20*dn), 20+int(60*dn), 35+int(110*dn), 230+int(25*dn))
                    : IM_COL32(25+int(125*dn), 14+int(20*dn), 30+int(55*dn), 230+int(25*dn));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(right, row_bottom), bg);

                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char delta_buf[16];
                    format_fp_volume(delta_buf, sizeof(delta_buf), gl.delta);
                    ImVec2 sz = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, delta_buf);
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(left + (cell_width - sz.x) * 0.5f,
                               row_top + (row_height - fp_font_size) * 0.5f),
                        IM_COL32(230, 230, 230, 255), delta_buf);
                }

            } else { // Volume mode
                float vi = std::sqrt(static_cast<float>(gl.total_volume / max_vol));
                ImU32 bg = IM_COL32(14+int(20*vi), 20+int(50*vi), 40+int(115*vi), 230+int(25*vi));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(right, row_bottom), bg);

                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char vol_buf[16];
                    format_fp_volume(vol_buf, sizeof(vol_buf), gl.total_volume);
                    ImVec2 sz = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, vol_buf);
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(left + (cell_width - sz.x) * 0.5f,
                               row_top + (row_height - fp_font_size) * 0.5f),
                        IM_COL32(230, 230, 230, 255), vol_buf);
                }
            }

            // Imbalance highlight - subtle border
            if (fp_mgr.show_imbalances && gl.is_imbalance) {
                ImU32 imb_color = gl.buy_dominant
                    ? IM_COL32(80, 180, 120, 160)
                    : IM_COL32(180, 80, 80, 160);
                draw_list->AddRect(ImVec2(left, row_top), ImVec2(right, row_bottom),
                                   imb_color, 0.0f, 0, 1.0f);
            }

            // POC - subtle horizontal line
            if (fp_mgr.show_poc && gl.is_poc) {
                float poc_y = row_top + row_height * 0.5f;
                draw_list->AddLine(ImVec2(left, poc_y), ImVec2(right, poc_y),
                                   IM_COL32(200, 180, 100, 140), 1.0f);
            }
        }

        // Outer border around the entire footprint box
        {
            const ImVec2 box_top = ImPlot::PlotToPixels(candle_center, mc->high_price);
            const ImVec2 box_bot = ImPlot::PlotToPixels(candle_center, mc->low_price);
            draw_list->AddRect(ImVec2(px_left.x, box_top.y), ImVec2(px_right.x, box_bot.y),
                               IM_COL32(120, 130, 150, 60), 0.0f, 0, 1.0f);
        }

        // V:/D: footer
        if (fp_mgr.show_summary && show_text) {
            const ImVec2 px_low = ImPlot::PlotToPixels(candle_center, lows[i]);
            float footer_y = px_low.y + 3.0f;
            float ff = fp_font_size * 0.8f;

            char vf[24], df[24];
            snprintf(vf, sizeof(vf), "V:"); format_fp_volume(vf+2, sizeof(vf)-2, mc->total_volume);
            snprintf(df, sizeof(df), "D:"); format_fp_volume(df+2, sizeof(df)-2, mc->delta);

            ImVec2 vs = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, vf);
            ImVec2 ds = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, df);
            float tw = vs.x + 3.0f + ds.x;
            float sx = px_left.x + (cell_width - tw) * 0.5f;

            draw_list->AddText(font, ff, ImVec2(sx, footer_y),
                               IM_COL32(120, 170, 200, 180), vf);
            draw_list->AddText(font, ff, ImVec2(sx + vs.x + 3.0f, footer_y),
                mc->delta >= 0 ? IM_COL32(100, 180, 130, 180) : IM_COL32(180, 100, 100, 180), df);
        }
    }

    ImPlot::PopPlotClipRect();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Footprint Profile - MMT-style per-candle volume profile bars
//
// Like Cluster mode but with proportional-width bar backgrounds behind the
// sell|buy text. Each row has two columns (sell left, buy right) filling the
// full candle width. Bar width within each half is proportional to that side's
// volume relative to the candle's max. POC row gets a white outline box.
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::render_footprint_profile(double visible_x_min, double visible_x_max) {
    auto& fp_mgr = ctx_.footprint_mgr();
    const auto& timestamps = ctx_.candle_mgr().timestamps();
    if (timestamps.empty()) return;

    const int64_t tf_sec = ctx_.candle_mgr().timeframe_seconds();
    const double tf_ms = static_cast<double>(tf_sec) * 1000.0;
    const int64_t tf_ms_i = static_cast<int64_t>(tf_sec) * 1000;
    const double visible_candles = (visible_x_max - visible_x_min) / tf_ms;

    if (visible_candles > 500.0) return;

    const double pixels_per_candle = ImPlot::GetPlotSize().x / visible_candles;
    if (pixels_per_candle < 3.0) return;

    const bool zoomed_out_block = (pixels_per_candle < 55.0);
    const bool show_text = !zoomed_out_block;

    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double visible_y_min = plot_limits.Y.Min;
    const double visible_y_max = plot_limits.Y.Max;

    // Request historical data
    {
        const int64_t start_ms = static_cast<int64_t>(visible_x_min) - tf_ms_i;
        const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms_i;
        fp_mgr.request_history(pair_, start_ms, end_ms, &ctx_.stream_mgr());
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const auto& opens = ctx_.candle_mgr().opens();
    const auto& closes = ctx_.candle_mgr().closes();
    const auto& lows = ctx_.candle_mgr().lows();
    const auto& highs = ctx_.candle_mgr().highs();

    ImFont* font = ImGui::GetFont();
    float font_scale = 1.0f;
    if (pixels_per_candle < 80.0f) font_scale = 0.7f;
    else if (pixels_per_candle < 120.0f) font_scale = 0.8f;
    const float fp_font_size = ImGui::GetFontSize() * font_scale;

    double tick_per_row = fp_mgr.ticks_per_row > 0
        ? fp_mgr.ticks_per_row * tick_size_ : 0.0;

    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min - tf_ms);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    for (size_t i = start_idx; i < timestamps.size(); ++i) {
        const double ts = static_cast<double>(timestamps[i]);
        if (ts > visible_x_max + tf_ms) break;
        if (highs[i] < visible_y_min || lows[i] > visible_y_max) continue;

        const double candle_center = ts + tf_ms * 0.5;

        // ── ZOOMED OUT BLOCK MODE ──────────────────────────────────────
        // Same as cluster: single delta-colored rectangle per candle
        if (zoomed_out_block) {
            const auto* mc = fp_mgr.get_merged_grouped(
                pair_.symbol, timestamps[i], tf_sec, 0.0);
            if (!mc) continue;

            double delta = mc->delta;
            float dn = (mc->total_buy + mc->total_sell > 0)
                ? static_cast<float>(std::abs(delta) / (mc->total_buy + mc->total_sell))
                : 0.0f;
            dn = std::sqrt(dn);

            const double body_top_price = std::max(opens[i], closes[i]);
            const double body_bot_price = std::min(opens[i], closes[i]);
            const double half_w = tf_ms * 0.35;
            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, body_top_price);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, body_bot_price);
            const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
            const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);

            float top_y = std::min(px_top.y, px_bot.y);
            float bot_y = std::max(px_top.y, px_bot.y);
            if (bot_y - top_y < 2.0f) {
                float cy = (top_y + bot_y) * 0.5f;
                top_y = cy - 1.0f; bot_y = cy + 1.0f;
            }

            ImU32 block_color;
            if (delta >= 0.0) {
                block_color = IM_COL32(
                    static_cast<int>(20 + 30 * dn),
                    static_cast<int>(30 + 70 * dn),
                    static_cast<int>(50 + 140 * dn), 230);
            } else {
                block_color = IM_COL32(
                    static_cast<int>(40 + 140 * dn),
                    static_cast<int>(20 + 25 * dn),
                    static_cast<int>(40 + 70 * dn), 230);
            }
            draw_list->AddRectFilled(ImVec2(px_left.x, top_y), ImVec2(px_right.x, bot_y), block_color);
            continue;
        }

        // ── DETAILED PROFILE MODE (zoomed in) ─────────────────────────
        // Auto tick_per_row (same logic as cluster)
        double effective_tpr = tick_per_row;
        if (effective_tpr <= 0.0) {
            double range = highs[i] - lows[i];
            if (range <= 0.0) range = 1.0;
            effective_tpr = range / 18.0;
            const ImVec2 px_hi = ImPlot::PlotToPixels(candle_center, highs[i]);
            const ImVec2 px_lo = ImPlot::PlotToPixels(candle_center, lows[i]);
            float candle_px_height = std::abs(px_lo.y - px_hi.y);
            if (candle_px_height > 0.0f) {
                int max_rows = std::max(3, static_cast<int>(candle_px_height / 12.0f));
                double min_tpr = range / static_cast<double>(max_rows);
                if (effective_tpr < min_tpr) effective_tpr = min_tpr;
            }
        }

        const auto* mc = fp_mgr.get_merged_grouped(
            pair_.symbol, timestamps[i], tf_sec, effective_tpr);
        if (!mc) continue;
        const auto& grouped = mc->levels;

        // Find max volume per side (for proportional bar widths)
        double max_side_vol = 0.0;
        for (const auto& gl : grouped) {
            if (gl.buy_volume > max_side_vol) max_side_vol = gl.buy_volume;
            if (gl.sell_volume > max_side_vol) max_side_vol = gl.sell_volume;
        }
        if (max_side_vol < 1e-8) continue;

        // Candle pixel bounds - same width as cluster mode
        const double half_w = tf_ms * 0.35;
        const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
        const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);
        const float cell_width = px_right.x - px_left.x;
        if (cell_width < 2.0f) continue;

        const float left = px_left.x;
        const float right = px_right.x;
        const float mid_x = left + cell_width * 0.5f;
        const float half_cell = cell_width * 0.5f;

        for (const auto& gl : grouped) {
            if (gl.price_hi < visible_y_min || gl.price_lo > visible_y_max) continue;

            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, gl.price_hi);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, gl.price_lo);
            const float row_top = px_top.y;
            const float row_bot = px_bot.y;
            const float row_height = row_bot - row_top;
            if (row_height < 1.0f) continue;

            // Volume fractions for proportional bar widths
            float sell_frac = static_cast<float>(gl.sell_volume / max_side_vol);
            float buy_frac  = static_cast<float>(gl.buy_volume / max_side_vol);

            // Bar widths within each half-cell
            float sell_bar_w = sell_frac * half_cell;
            float buy_bar_w  = buy_frac * half_cell;

            // Sell bar - fills from LEFT edge rightward within left half
            if (sell_bar_w > 0.5f) {
                float si = std::sqrt(sell_frac);
                ImU32 sell_color = IM_COL32(
                    static_cast<int>(40 + 150 * si),
                    static_cast<int>(15 + 25 * si),
                    static_cast<int>(35 + 70 * si), 200);
                draw_list->AddRectFilled(
                    ImVec2(mid_x - sell_bar_w, row_top),
                    ImVec2(mid_x, row_bot), sell_color);
            }

            // Buy bar - fills from mid_x rightward within right half
            if (buy_bar_w > 0.5f) {
                float bi = std::sqrt(buy_frac);
                ImU32 buy_color = IM_COL32(
                    static_cast<int>(15 + 25 * bi),
                    static_cast<int>(25 + 65 * bi),
                    static_cast<int>(45 + 140 * bi), 200);
                draw_list->AddRectFilled(
                    ImVec2(mid_x, row_top),
                    ImVec2(mid_x + buy_bar_w, row_bot), buy_color);
            }

            // Text labels: sell vol on left, buy vol on right (always both shown)
            if (show_text && row_height >= fp_font_size * 0.5f) {
                char sell_buf[16], buy_buf[16];
                format_fp_volume(sell_buf, sizeof(sell_buf), gl.sell_volume);
                format_fp_volume(buy_buf, sizeof(buy_buf), gl.buy_volume);
                float text_y = row_top + (row_height - fp_font_size) * 0.5f;

                // Sell text - centered in left half
                ImVec2 sell_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, sell_buf);
                float sell_x = left + (half_cell - sell_size.x) * 0.5f;
                draw_list->AddText(font, fp_font_size,
                    ImVec2(sell_x, text_y),
                    IM_COL32(230, 215, 215, 255), sell_buf);

                // Buy text - centered in right half
                ImVec2 buy_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, buy_buf);
                float buy_x = mid_x + (half_cell - buy_size.x) * 0.5f;
                draw_list->AddText(font, fp_font_size,
                    ImVec2(buy_x, text_y),
                    IM_COL32(215, 225, 240, 255), buy_buf);
            }

            // Subtle vertical divider between sell/buy columns
            draw_list->AddLine(ImVec2(mid_x, row_top), ImVec2(mid_x, row_bot),
                               IM_COL32(100, 100, 120, 60), 1.0f);

            // POC - white outline box around the highest-volume row (MMT style)
            if (fp_mgr.show_poc && gl.is_poc) {
                draw_list->AddRect(ImVec2(left, row_top), ImVec2(right, row_bot),
                                   IM_COL32(220, 220, 220, 200), 0.0f, 0, 1.5f);
            }

            // Imbalance highlight
            if (fp_mgr.show_imbalances && gl.is_imbalance) {
                ImU32 imb_color = gl.buy_dominant
                    ? IM_COL32(80, 180, 120, 160)
                    : IM_COL32(180, 80, 80, 160);
                draw_list->AddRect(ImVec2(left, row_top), ImVec2(right, row_bot),
                                   imb_color, 0.0f, 0, 1.0f);
            }
        }

        // Outer border around entire footprint box
        {
            const ImVec2 box_top = ImPlot::PlotToPixels(candle_center, mc->high_price);
            const ImVec2 box_bot = ImPlot::PlotToPixels(candle_center, mc->low_price);
            draw_list->AddRect(ImVec2(left, box_top.y), ImVec2(right, box_bot.y),
                               IM_COL32(120, 130, 150, 60), 0.0f, 0, 1.0f);
        }

        // V:/D: footer
        if (fp_mgr.show_summary && show_text) {
            const ImVec2 px_low = ImPlot::PlotToPixels(candle_center, lows[i]);
            float footer_y = px_low.y + 3.0f;
            float ff = fp_font_size * 0.8f;

            char vf[24], df[24];
            snprintf(vf, sizeof(vf), "V:"); format_fp_volume(vf+2, sizeof(vf)-2, mc->total_volume);
            snprintf(df, sizeof(df), "D:"); format_fp_volume(df+2, sizeof(df)-2, mc->delta);

            ImVec2 vs = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, vf);
            ImVec2 ds = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, df);
            float tw = vs.x + 3.0f + ds.x;
            float sx = left + (cell_width - tw) * 0.5f;

            draw_list->AddText(font, ff, ImVec2(sx, footer_y),
                               IM_COL32(120, 170, 200, 180), vf);
            draw_list->AddText(font, ff, ImVec2(sx + vs.x + 3.0f, footer_y),
                mc->delta >= 0 ? IM_COL32(100, 180, 130, 180) : IM_COL32(180, 100, 100, 180), df);
        }
    }

    ImPlot::PopPlotClipRect();
}

void ChartWidget::render_footprint_settings_popup() {
    footprint_settings_panel_.set_panel_height(280.0f);
    if (footprint_settings_panel_.begin()) {
        auto& fp = ctx_.footprint_mgr();

        if (footprint_settings_panel_.tab("Mode")) {
            const char* fp_modes[] = { "Cluster", "Delta", "Volume", "Profile" };
            int mode_idx = static_cast<int>(fp.mode);
            ImGui::SetNextItemWidth(130);
            if (ImGui::Combo("Mode##fp", &mode_idx, fp_modes, 4)) {
                fp.mode = static_cast<FootprintManager::Mode>(mode_idx);
            }
            ImGui::Spacing();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderFloat("Imbalance Ratio##fp", &fp.imbalance_ratio, 1.5f, 10.0f, "%.1f:1");
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Minimum buy:sell or sell:buy ratio\nfor imbalance highlighting");
            }
        }
        if (footprint_settings_panel_.tab("Display")) {
            ImGui::Checkbox("Imbalance Highlights##fp", &fp.show_imbalances);
            ImGui::Checkbox("POC Marker##fp", &fp.show_poc);
            ImGui::Checkbox("V:/D: Footer##fp", &fp.show_summary);
        }
        if (footprint_settings_panel_.tab("Sizing")) {
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (fp.ticks_per_row == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.2f)", fp.ticks_per_row * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##fptpr") && fp.ticks_per_row > 0) {
                fp.ticks_per_row = std::max(0, fp.ticks_per_row - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##fptpr")) {
                fp.ticks_per_row += 1;
            }
            ImGui::SetNextItemWidth(60);
            if (ImGui::InputInt("##fptpr_input", &fp.ticks_per_row, 0, 0)) {
                fp.ticks_per_row = std::max(0, fp.ticks_per_row);
            }
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Price ticks per row (0 = auto ~18 rows)");
            }
            ImGui::Spacing();
            ImGui::Separator();
            int cached = fp.candle_count(pair_.symbol);
            ImGui::TextDisabled("Cached candles: %d", cached);
            if (fp.is_loading()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(loading...)");
            }
            if (ImGui::SmallButton("Clear Cache##fp")) {
                fp.clear(pair_.symbol);
            }
        }
        footprint_settings_panel_.end();
    }
}
