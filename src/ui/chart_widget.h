#pragma once

#include <implot.h>
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <array>
#include <utility>
class ReplayManager;

#include "indicators/indicator_manager.h"
#include "indicators/volume_indicator.h"
#include "indicators/cvd_indicator.h"
#include "indicators/rsi_indicator.h"
#include "indicators/macd_indicator.h"
#include "indicators/funding_rate_indicator.h"
#include "indicators/oi_indicator.h"
#include "indicators/vpin_indicator.h"
#include "core/app_context.h"
#include "core/heatmap_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/orderbook_manager.h"
#include "core/candle_manager.h"
#include "core/renko_builder.h"
#include "core/volume_profile_manager.h"
#include "core/tpo_manager.h"
#include "core/symbol_metadata.h"
#include "rendering/liq_field_renderer.h"
#include "ui/drawing/drawing_layer.h"
#include "ui/settings_panel.h"

// ═══════════════════════════════════════════════════════════════════════════════
// ChartWidget - RENDERING + UI ONLY
//
// Does NOT own candle data. Reads from CandleManager via const accessors.
// Responsible for: ImPlot chart, candle drawing, heatmap overlays,
// crosshair, indicators, toolbar.
//
// CandleManager owns: candle deque, SoA cache, trade→candle building,
// server candle merging, historical batch loading, scroll-load.
// ═══════════════════════════════════════════════════════════════════════════════

enum class HeatmapType {
    OrderbookDepth,
    VolumeDelta,
    TradeIntensity,
    Liquidations,
    VWAPDeviation
};

// ── Chart type (was a bare int; the underlying values are frozen so the
//    chart-type popup rows and the tick-cache key can round-trip via int) ──
//    0=Candles, 1=FP Cluster, 2=FP Profile, 3=Heikin Ashi, 4=Line, 5=TPO, 6=Renko.
enum class ChartType : int {
    Candles          = 0,
    FootprintCluster = 1,
    FootprintProfile = 2,
    HeikinAshi       = 3,
    Line             = 4,
    TPO              = 5,
    Renko            = 6,
};

// True when the chart's X-axis is time in ms (everything except Renko).
constexpr bool ct_is_time_axis(ChartType t) { return t != ChartType::Renko; }
// True when the type draws from the real OHLC candle geometry (Candles, FP, TPO).
constexpr bool ct_uses_real_candles(ChartType t) {
    return t == ChartType::Candles || t == ChartType::FootprintCluster
        || t == ChartType::FootprintProfile || t == ChartType::TPO;
}
// True when time-keyed overlays (heatmap, liq, VPVR, footprint, patterns) may draw.
constexpr bool ct_allows_time_overlays(ChartType t) {
    return t != ChartType::TPO && t != ChartType::Renko;
}

class ChartWidget : public Widget {
public:
    ChartWidget(const Terminal::Pair& pair,
                const AppContext& ctx,
                double tick_size);
    ~ChartWidget() override;

    void render() override;
    void update() override;

    WidgetType type() const override { return WidgetType::Chart; }
    const char* title() const override { return title_.c_str(); }

    void change_timeframe(int new_tf_seconds);

    // Current chart timeframe in seconds - live source of truth is the
    // CandleManager (the title's tf field lags a frame). Lets the topbar TF
    // segment both highlight the active TF and drive timeframe changes.
    int64_t timeframe_seconds() const { return ctx_.candle_mgr().timeframe_seconds(); }

    // Real-time view (RT). Applies regardless of chart type: the chart follows
    // the live edge (follow-live streaming) and, for the Line, draws the live-
    // edge dot. Toggled from the TIMEFRAME dropdown (see app_shell render_tf_menu).
    bool rt_mode() const { return rt_mode_; }
    void set_rt_mode(bool v) { rt_mode_ = v; }
    void toggle_rt_mode() { rt_mode_ = !rt_mode_; }

    float liq_opacity() const { return liq_opacity_; }   // liq-heatmap opacity (Tweaks panel)
    void  set_liq_opacity(float v);                      // set + push to the reconstructor
    // WS2 A/B: Field render path - texture quad (GPU) vs per-rect (CPU). The
    // rect path stays selectable until the texture pass is screenshot-approved.
    bool liq_field_use_texture() const { return liq_field_.knobs().use_texture; }
    void set_liq_field_use_texture(bool v) { liq_field_.knobs().use_texture = v; }

    void add_volume_indicator();
    void add_cvd_indicator();
    void add_rsi_indicator(int period = 14);
    void add_macd_indicator(int fast = 12, int slow = 26, int signal = 9);
    void add_funding_rate_indicator();
    void add_oi_indicator();
    void add_vpin_indicator();

    // Reset overlay subscription state (called on replay context swap)
    void reset_overlay_subscriptions();

    template<typename IndicatorType, typename... Args>
    void add_indicator(Args&&... args) {
        auto indicator = std::make_unique<IndicatorType>(std::forward<Args>(args)...);
        if (auto* vol_ind = dynamic_cast<Indicators::VolumeIndicator*>(indicator.get())) {
            populate_volume_data(vol_ind);
        }
        indicator->update();
        indicator_mgr_.add_indicator(std::move(indicator));
    }

    Indicators::IndicatorManager& get_indicator_manager() { return indicator_mgr_; }

    static std::string timeframe_to_string(int64_t seconds);
    bool is_loading() const;

    struct CrosshairState {
        bool is_active = false;
        ImPlotPoint plot_pos;
        ImVec2 first_plot_min;
        ImVec2 first_plot_max;
        ImVec2 last_plot_max;
        ImVec2 hovered_plot_min;
        ImVec2 hovered_plot_max;
        double hovered_y_min = 0.0;
        double hovered_y_max = 0.0;
        bool chart_hovered = false;
        bool indicator_hovered = false;
        ImPlotFormatter indicator_y_formatter = nullptr;  // Y-axis formatter from hovered indicator
    };

    CrosshairState crosshair_state_;
    float chart_allocated_height_ = 0.0f;
    float indicator_allocated_height_ = 0.0f;
    // Replay time-range selection (Shift+drag on chart)
    struct ReplaySelection {
        bool active = false;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
    };
    ReplaySelection replay_selection_;
    int64_t context_menu_time_ms_ = 0;  // Captured at right-click, used by popup
    // The right-clicked MINUTE (60000-floored), captured in the same block as
    // context_menu_time_ms_. NEVER reuse context_menu_time_ms_ for research:
    // it floors to the chart timeframe (a 4H chart floors to a 4h boundary),
    // which is right for replay and silently wrong by up to hours for a
    // minute-addressed research read (the spec's named trap).
    int64_t context_menu_minute_ms_ = 0;
    double  context_menu_price_ = 0.0;  // Price under cursor, captured at right-click

    // Measure tool (right-click "Measure from here"): anchor + live readout that
    // follows the cursor (dPrice, d%, elapsed, candle count) until click/Esc.
    struct MeasureState { bool active = false; int64_t t0_ms = 0; double p0 = 0.0; };
    MeasureState measure_;

    // Client-side price alerts (live chart only, session-scoped): a level drawn
    // on the chart that fires a browser notification + in-app toast + beep when
    // the live price crosses it, then clears (one-shot). Not persisted.
    struct PriceAlert { double price = 0.0; bool arm_above = false; };
    std::vector<PriceAlert> alerts_;
    double      alert_last_price_ = 0.0;   // last live price seen (crossing edge)
    double      alert_toast_until_ = 0.0;  // ImGui::GetTime() deadline for the toast
    std::string alert_toast_msg_;

    struct DateBoundary {
        double timestamp;
        bool show_date;
    };
    mutable std::vector<DateBoundary> date_boundaries_;

    void setup_time_axis_ticks(double visible_x_min, double visible_x_max) const;

private:
    // ─── External Dependencies (not owned) ───────────────────────────────
    Terminal::Pair pair_;
    const AppContext& ctx_;

    // ─── Chart Identity ──────────────────────────────────────────────────
    // title_ = "Chart <ex> <sym> <tf>###chart_<ex>_<sym>". The visible prefix
    // carries the live timeframe; the part after "###" is the TF-independent
    // docking identity (must match LayoutManager::setup_default_layout), so the
    // chart stays docked when the TF changes. title_tf_seconds_ tracks the TF the
    // title was last built for, so render() can refresh it lazily.
    std::string title_;
    std::string timeframe_label_;
    int64_t title_tf_seconds_ = -1;
    double tick_size_;

    // ─── Viewport ────────────────────────────────────────────────────────
    ImPlotRect last_visible_range_{};
    double x_axis_min_ = 0.0;
    double x_axis_max_ = 0.0;
    double stored_x_min_ = 0.0;
    double stored_x_max_ = 0.0;

    // ─── Chart Type ───────────────────────────────────────────────────────
    // See ChartType above. Stored as the enum; the popup rows / tick-cache key
    // round-trip through static_cast<int> where an int is genuinely needed.
    ChartType chart_type_ = ChartType::Candles;

    // ─── Renko (ChartType::Renko) ─────────────────────────────────────────
    // Price-driven bricks on a brick-index X-axis (not time). The builder is a
    // pure, deterministic transform over the candle close series (replay-safe);
    // see core/renko_builder.h.
    RenkoBuilder renko_;
    // Brick size in ticks: 0 = auto (resolved once from price into
    // renko_resolved_ticks_ and then frozen so the sequence does not repaint as
    // price moves); >0 = manual override (config popup). Resolved price size =
    // renko_resolved_ticks_ * tick_size_.
    int  renko_brick_ticks_    = 0;   // user setting (0 = auto)
    int  renko_resolved_ticks_ = 0;   // frozen resolved tick count (0 = unresolved)
    // Committed-cache signature: rebuild the committed bricks only when the
    // closed-candle set / brick size / timeframe change (the building candle
    // folds in per frame, cheaply).
    mutable size_t  renko_sig_count_ = 0;
    mutable int64_t renko_sig_time_  = 0;   // last closed candle's TIMESTAMP (not close:
                                            // a close mutation must NOT force a rebuild)
    mutable double  renko_sig_size_  = 0.0;
    mutable int64_t renko_sig_tf_    = 0;
    // Time window of the currently visible bricks, published by
    // render_chart_renko so update() can feed CandleManager a TIME range (never
    // brick indices) for scroll-load. 0 until the first Renko frame renders.
    int64_t renko_view_t0_ms_ = 0;
    int64_t renko_view_t1_ms_ = 0;
    // On-chart brick-size affordance: a gear next to the "RENKO …" readout opens
    // the config popup (deferred one frame so OpenPopup runs at window scope, not
    // inside BeginPlot). renko_gear_pos_ anchors the popup under the gear.
    bool   open_renko_settings_ = false;
    ImVec2 renko_gear_pos_{};
    // ─── Real-time view (RT) ──────────────────────────────────────────────
    // RT mode (any chart type): engage follow-live so the chart streams and
    // keeps the live edge current, while normal ImPlot zoom/pan stay fully
    // interactive (the follow-live latch clears on the first user pan/scroll -
    // handle_plot_interaction). No per-frame axis pin. For the Line, plot_line
    // also draws the live-edge dot. Toggled from the TIMEFRAME dropdown
    // (app_shell render_tf_menu). rt_was_on_ edge-detects the toggle so RT
    // re-arms follow-live exactly ONCE on the rising edge (not every frame).
    bool    rt_mode_    = false;
    bool    rt_was_on_  = false;
    // Deferred popup opens (set inside Indicators popup, opened next frame)
    bool open_liq_settings_ = false;
    bool open_vpvr_settings_ = false;

    // ─── Heatmap ─────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point last_heatmap_update_ms_;
    static constexpr int64_t HEATMAP_UPDATE_INTERVAL_MS = 100;
    bool heatmap_enabled_ = false;
    std::string heatmap_mode_ = "UHD";
    HeatmapType heatmap_type_ = HeatmapType::OrderbookDepth;
    bool heatmap_data_requested_ = false;
    int64_t heatmap_time_offset_ms_ = 0;
    int heatmap_bucket_multiplier_ = 1;
    int64_t heatmap_loaded_timeframe_ = 0;
    float heatmap_sensitivity_ = 1.0f;
    std::chrono::steady_clock::time_point last_heatmap_rebuild_;
    std::chrono::steady_clock::time_point last_liq_heatmap_scroll_load_time_;
    int64_t liq_heatmap_data_boundary_ms_ = 0;  // Stop requesting older than this
    int64_t liq_heatmap_prev_available_min_ = 0; // Track if data boundary was reached
    ImPlotRect last_rebuilt_range_{0, 0, 0, 0};
    bool viewport_initialized_ = false;

    // ─── Admin Pattern Overlay ───────────────────────────────────────────
    // Fixed slots avoid allocations in the render loop. The backend currently
    // publishes one live candidate per detector timeframe (4h and 1d); extra
    // capacity keeps the client tolerant of additive admin timeframes.
    std::array<Terminal::PatternOverlay, 4> pattern_overlays_{};
    std::array<bool, 4> pattern_overlay_active_{};
    StreamManager* pattern_stream_mgr_ = nullptr;
    bool pattern_subscribed_ = false;

    // ─── Liquidation Heatmap Overlay ─────────────────────────────────────
    bool recorder_view_applied_ = false;  // CLIP_FACTORY: script view overrides, one-shot
    bool liq_heatmap_enabled_ = true;
    bool liq_heatmap_subscribed_ = false;
    bool liq_timeline_requested_ = false;
    bool liq_extend_levels_ = false;     // V3: OFF - backend EMA provides persistence, forward-fill smears bands

    // ─── Liq Heatmap Visual Controls (MMT-style) ────────────────────────
    float liq_opacity_ = 0.85f;          // Global opacity multiplier (0.0-1.0)
    float liq_color_low_ = 0.0f;         // Low cutoff: values below → transparent (noise filter)
    float liq_color_peak_ = 100000.0f;   // Peak: values at/above → max intensity
    bool liq_color_auto_ = true;         // Auto-compute Low/Peak from data distribution
    int liq_ticks_per_row_ = 0;          // 0 = auto, >0 = manual override
    // Per-leverage toggles (6 tiers matching backend)
    bool liq_lev_5x_ = false;    // Off by default (too far for short-term BTC/ETH)
    bool liq_lev_10x_ = false;   // Off by default
    bool liq_lev_25x_ = true;
    bool liq_lev_50x_ = true;
    bool liq_lev_75x_ = true;
    bool liq_lev_100x_ = true;

    // PERF: cached live standing-shelf selection (price/usd/side), recomputed ONLY when the snapshot
    // ts or the leverage mask changes - the agg-over-~800-bands + sort/merge/cap was running EVERY
    // frame (the liq-map FPS hit). Per-frame now only resolves pixel x for the ~N kept shelves + draws.
    struct ShelfSel { double price = 0.0; double usd = 0.0; bool is_long = false; };
    std::vector<ShelfSel> liq_shelf_cache_;
    int64_t liq_shelf_cache_ts_      = -1;
    uint8_t liq_shelf_cache_mask_    = 0xFF;
    double  liq_shelf_cache_max_usd_ = 0.0;
    bool    liq_shelf_cache_dense_   = false;  // rebuild the cache when liq_dense_field_ is toggled

    // (MMT rail-behavior flags - extend_to_candle / truncate_at_take / min_frac / show_labels - were
    //  removed with the rails pivot 2026-06-29; consume + truncation are backend-baked now.)
    // (The stateful per-level discharge tracker - LiqLevelState, liq_levels_, liq_track_*,
    //  liq_discharge_* - was REMOVED with the rails pivot 2026-06-29. "Taken" is now the backend
    //  RailTracker's consume_ms, delivered per rail on the wire; no client candle-scan inference.)
    // ── Historical line rendering (MMT "rich chart": show past levels, not just the live ones) ──
    // Every tracked level draws as a horizontal line from where it first appeared. A consumed
    // level stops at the take candle and dims to a remnant (KEPT, not deleted) so the chart reads
    // as a cohesive record of where liquidity built and where price took it.
    bool    liq_hist_bands_ = true;             // historical rails ON - backend-baked consume (rails list, RailTracker); LIQMAP_RAILS_DECISION_2026-06-29
    bool    liq_show_lines_ = true;             // standing-shelf overlay - now the LIVE fallback (drawn only when no backend rails) + rollback
    // (liq_consume_cache_ + its candle-set keys were removed with the rails pivot 2026-06-29 - the
    //  per-rail consume time is now BandTrack.consume_ms, baked by the backend RailTracker.)
    int     liq_hist_max_lines_ = 0;            // max drawn lines (0 = unlimited; strongest kept)
    int     liq_hist_max_lines_cap_ = 400;      // hard safety cap (perf), applied even when unlimited
    float   liq_hist_min_frac_ = 0.06f;         // a rail must be >= this fraction of the dominant rail to show (declutter)
    int     liq_hist_per_side_cap_ = 12;        // keep only the strongest N rails per side (long/short) so the dominant magnet reads
    float   liq_hist_merge_pct_ = 0.10f;        // merge lines within this % of a stronger one (0 = off)
    // Census declutter is scoped, not global: the floor is taken per SIDE over bands within
    // this fraction of mark, so a lone whale parked far from price cannot erase every level
    // near it. Measured 2026-07-28 on KAITO: 12 real census levels, all suppressed because a
    // single $305k position at +81% set an $18k floor. See render_liquidation_census().
    float   liq_census_rel_window_ = 0.30f;     // +/- fraction of mark that defines "near price" for the floor

    // liq_dense_field_ toggles the dense projection Field ("Liq Heatmap" pill). §12 cleanup
    // 2026-07-03: the old kLiqDense* constants (pre-Field "dense mode" that flipped the SHELF
    // floor/contrast) are deleted - the Field toggle no longer re-tunes the Levels shelf.
    // Default ON: the dense projection Field ("Liq Heatmap" pill) is a headline layer, so the
    // bare terminal boots with it active alongside Levels + Profile.
    bool liq_dense_field_ = true;

    // ── Liq Heatmap projection Field ──────────────────────────────────────
    // The dense candle×leverage projection Field (the "Liq Heatmap" pill).
    // Owns its own segment cache, rebuild signature, tuning knobs and both
    // render paths; see rendering/liq_field_renderer.h. liq_dense_field_ above
    // toggles whether it draws. The Liq Profile below is the price-marginal of
    // this same cache and reads it through the renderer's accessors.
    LiqFieldRenderer liq_field_;
    // §8 Liq Profile - price-marginal of STANDING fuel (WS1 Option A, 2026-07-03: per visible
    // price row, the MAX of the field's rendered brightness across PENDING segments - the same tv
    // the forward cascade projects with. No time-averaging → no window-length dependence; no
    // adaptive re-norm; no width gain - the old liq_profile_gain_ 2.75 hack is deleted).
    // Style = the design system's `profile` spec (liq-heatmap-colormap.json):
    // width 0.12 of plot, LUT-tinted fill at FLAT 0.26 alpha, gaussian sigma = 3 field rows,
    // 1px outline rgba(154,164,178,.50), 1px baseline hairline @ 8% white at the plot edge.
    std::vector<float>  liq_profile_scratch_;
    std::vector<float>  liq_profile_scratch2_;
    std::vector<ImVec2> liq_profile_pts_;
    float liq_profile_width_      = 0.12f;  // sidebar width as a fraction of plot width
    float liq_profile_alpha_      = 0.26f;  // FLAT fill opacity (LUT tint carries the intensity)
    float liq_profile_sigma_rows_ = 3.0f;   // gaussian sigma in FIELD price rows (converted to
                                            // marginal samples via the current zoom, so smoothing
                                            // tracks the field's visible row size)
    static constexpr ImU32 kLiqProfileOutline  = IM_COL32(154, 164, 178, 128);  // 1px outline @ .50
    static constexpr ImU32 kLiqProfileBaseline = IM_COL32(255, 255, 255, 20);   // 1px hairline @ 8%


    // Legend ignition notch - the ramp position where the rare orange→yellow
    // "ignition" top begins (the Ember-K 0.945 stop; see the annotated mockup).
    static constexpr float kLiqLegendNotchT = 0.945f;

    // ─── VPVR Overlay ─────────────────────────────────────────────────────
    bool vpvr_enabled_ = false;
    int  vpvr_ticks_per_row_ = 0;          // 0 = auto
    int64_t vpvr_last_request_start_ = 0;  // track range for re-request
    int64_t vpvr_last_request_end_ = 0;

    // ─── Liquidation Profile Overlay (right-edge sidebar histogram, mirrors VPVR) ────────
    // Reuses the shared Profile:: renderer (price_profile_renderer.h) - same one VPVR uses -
    // fed from the current liq heatmap snapshot instead of volume levels. Default ON: it ships
    // as an active headline layer ("Liq Profile" pill) on the bare terminal.
    bool liq_profile_enabled_ = true;

    // ─── Observed layer (WS4 §7) - REAL @forceOrder liquidation markers ──────────────────
    // Discrete dots at (time, price), area ∝ USD, side-tinted (forced BUY = short liquidated
    // → Tokens::UP; forced SELL = long liquidated → Tokens::DOWN). The fact layer next to
    // the estimated Field - deliberately a different visual grammar (glyphs, not bands).
    bool  liq_observed_enabled_ = false;
    float liq_obs_min_usd_ = 0.0f;       // dust filter (USD); 0 = show all
    float liq_obs_ref_usd_ = 25000.0f;   // USD at which a marker reads clearly (~2.5px radius)
    static constexpr int kLiqObsMaxDraw = 4000;  // per-frame draw cap → nth_element USD cutoff

    // ─── "Liq Levels HL" (P2e) - REAL predictive liq levels (HL census) ──────────────────
    // Ground-truth clusters from the HL clearinghouseState census (stream 34,
    // LiquidationHeatmapManager census store) - real open positions, NOT the modelled
    // heatmap. Keyed by UNDERLYING ({"hl","BTC"}), so it overlays cross-venue: a Binance
    // btcusdt chart subscribes the same HL BTC feed (headline feature - real liq levels
    // on a CEX chart). Distinct colormap (violet longs / mint shorts vs the modelled
    // amber/cyan) + an always-on coverage badge: the census is hot-set-only (the at-risk
    // leveraged tail, ~1-10% of OI notional) and must never read as a complete liq map.
    // The LIQ LEV chips filter this layer by REAL leverage tier (est_Nx = actual
    // position leverage) - same control as the modelled layer, venue-aware meaning.
    bool liq_census_enabled_ = false;      // ctor: defaults ON for HL-native pairs
    bool liq_census_subscribed_ = false;
    Terminal::Pair liq_census_pair_;       // {"hl", UNDERLYING} - resolved at construction
    // PERF: cached band selection (price/usd/side), recomputed only when the census
    // snapshot ts or the leverage mask changes (mirrors liq_shelf_cache_).
    struct CensusSel { double price = 0.0; double usd = 0.0; bool is_long = false; };
    std::vector<CensusSel> liq_census_cache_;
    int64_t liq_census_cache_ts_      = -1;
    uint8_t liq_census_cache_mask_    = 0xFF;
    double  liq_census_cache_max_usd_ = 0.0;

    // ─── Indicators ──────────────────────────────────────────────────────
    Indicators::IndicatorManager indicator_mgr_;

    // ─── Display Config ──────────────────────────────────────────────────
    PriceFormatter fmt_;

    // ─── Drawing tools ───────────────────────────────────────────────────
    // All logic lives in drawing::DrawingLayer (ui/drawing/) + DrawingManager
    // (ctx_.drawing_mgr()) - this widget only calls render_in_plot()/
    // end_frame() and gates its own handlers on captures_mouse().
    drawing::DrawingLayer drawing_layer_;

    // ─── Settings Panels (reusable tabbed popups) ────────────────────────
    // Liq settings = the design floating panel: Style (colormap, opacity) +
    // Intensity (gamma, Low/Peak, noise floor, tick-per-row, half-life), wired
    // to the liq_field_* members above.
    SettingsPanel liq_settings_panel_{"liq_heatmap_settings",
        {"Style", "Intensity"}};
    SettingsPanel vpvr_settings_panel_{"vpvr_settings",
        {"Display", "Sizing"}};
    SettingsPanel footprint_settings_panel_{"footprint_settings",
        {"Mode", "Display", "Sizing"}};
    SettingsPanel tpo_settings_panel_{"tpo_settings",
        {"General", "Session", "Value Area", "Single Prints"}};

    // ─── Rendering ───────────────────────────────────────────────────────
    void render_controls();
    void render_chart();
    void render_indicators();
    void render_crosshair(const ImPlotPoint& mouse_pos) const;

    // Candle drawing via ImDrawList (replaces custom_implot dependency).
    // The four OHLC source spans are passed in so Candles draws the REAL arrays
    // and Heikin Ashi draws the derived HA arrays through the exact same tiers.
    // The live building candle's OHLC is passed separately (bld_*) because HA
    // needs it transformed per frame; bld_valid=false skips the right-edge candle.
    struct BuildingOHLC { double open; double high; double low; double close; bool valid; };
    void plot_candles(double visible_x_min, double visible_x_max,
                      const std::vector<double>& src_opens,
                      const std::vector<double>& src_highs,
                      const std::vector<double>& src_lows,
                      const std::vector<double>& src_closes,
                      const BuildingOHLC& bld);
    // Close-price polyline (ChartType::Line). Uses the recent-tick ring buffer
    // where available and falls back to candle closes for older regions.
    void plot_line(double visible_x_min, double visible_x_max);
    // Preallocated pixel-space path for plot_line (reused each frame - no STL
    // churn in the render loop).
    std::vector<ImVec2> line_pts_;

    // ─── Renko (ChartType::Renko) ─────────────────────────────────────────
    // Dedicated brick-index render path (self-contained BeginPlot/EndPlot),
    // delegated from render_chart() so the time-axis path is untouched.
    void render_chart_renko();
    // (Re)build the committed bricks if the closed-candle signature / brick size
    // changed, resolve the auto brick size on first use, and fold the building
    // candle in per frame. Cheap on the common path (signature unchanged).
    void ensure_renko();
    double renko_resolved_size();  // resolved brick size in price units (0 if unresolved)
    double renko_atr(int period) const;  // ATR(period) of loaded candles (price units), 0 if too few
    // Filled brick rectangles over the visible brick-index slice [i0, i1).
    void plot_renko(size_t i0, size_t i1);
    // Brick-index -> close-time X ticks (non-uniform, TradingView style).
    void setup_renko_axis_ticks(double vis_i_min, double vis_i_max) const;
    void render_renko_settings_popup();  // brick-size config (right-click the Renko row)
    mutable std::vector<double>      renko_tick_pos_;     // reused tick scratch
    mutable std::vector<std::string> renko_tick_lbls_;
    mutable std::vector<const char*> renko_tick_ptrs_;
    // Heikin Ashi extents over the visible window (drawn HA candles never clip);
    // out params stay untouched when there is no HA data in range.
    void ha_visible_extents(double visible_x_min, double visible_x_max,
                            double& y_min, double& y_max) const;
    void draw_ohlc_readout() const;  // OHLCV overlay, top-left of plot
    void draw_status_chip() const;   // LIVE/REPLAY status pill under the OHLC badge
    void draw_single_candle(ImDrawList* draw_list,
                            double x, double open, double close,
                            double low, double high,
                            double half_width,
                            const ImVec2& plot_min, const ImVec2& plot_max,
                            bool thin_body = false,
                            uint8_t alpha = 255);

    // Ghost scrub-preview pass (replay only): while the user aims a scrub on
    // the transport, renders the PreviewCandleStore's future candles ahead of
    // the playhead at reduced alpha (two tiers: playhead→aim stronger, beyond
    // the aim faint) plus a vertical line at the aimed-at time. Reads ONLY the
    // preview store - never CandleManager. Defined in
    // chart_widget_scrub_preview.cpp (keep it out of this God file).
    void render_scrub_preview(double visible_x_min, double visible_x_max);

    // Heatmap
    void request_heatmap_data();
    void update_heatmap();
    void update_live_heatmap_from_orderbook();
    void render_heatmap_tooltip();
    void toggle_heatmap();
    void calculate_heatmap_price_range(double& min_price, double& max_price);
    ImPlotColormap get_colormap_for_type(HeatmapType type) const;
    std::string format_time_hms(int64_t timestamp_ms);
    ImVec4 get_tooltip_cell_color(float normalized) const;

    // Admin-scoped live descending-resistance context.
    void handle_pattern_overlay(const Terminal::PatternOverlay& pattern);
    void render_pattern_overlay(double visible_x_min, double visible_x_max);

    // Liquidation heatmap overlay
    void render_liquidation_heatmap(double visible_x_min, double visible_x_max);
    void render_liq_timeline();
    // The Field itself lives in liq_field_ (rendering/liq_field_renderer.h).
    void request_liq_heatmap_data();
    void update_liq_heatmap_scroll();
    void toggle_liquidation_heatmap();
    void render_liq_profile();   // right-edge sidebar histogram (Profile:: renderer, mirrors VPVR)
    void render_liq_observed();  // WS4 Observed markers - real @forceOrder dots (drawn over candles)
    void toggle_liq_census();    // "Liq Levels HL" - subscribe/unsubscribe stream 34 (census)
    void render_liq_census();    // HL census shelves (REAL liq levels) + coverage badge

    // VPVR overlay
    void render_vpvr_profile();
    double compute_vpvr_tick_per_row(double price_range, double plot_height) const;

    // Footprint overlay
    void render_footprint_overlay(double visible_x_min, double visible_x_max);
    void render_footprint_profile(double visible_x_min, double visible_x_max);
    void render_footprint_settings_popup();

    // TPO / Market Profile
    void render_tpo(double visible_x_min, double visible_x_max);
    int  tpo_hovered_session_ = -1;  // index of session under mouse, -1 = none
    int  tpo_context_session_ = -1;  // session index for context menu
    bool tpo_zoom_pending_    = false; // zoom-out on TPO activation
    int  tpo_zoom_frames_     = 0;    // apply zoom for N frames to ensure ImPlot processes it

    // Interaction
    void handle_plot_interaction();

    // Time axis
    void generate_time_ticks(double visible_x_min, double visible_x_max) const;
    int64_t calculate_tick_interval(double visible_candles) const;
    int64_t align_to_interval(int64_t timestamp_s, int64_t interval_seconds) const;

    // Volume indicator helpers
    void populate_volume_data(Indicators::VolumeIndicator* vol_ind) const;
    void update_volume_indicators();

    // CVD indicator helpers
    void populate_cvd_data(Indicators::CVDIndicator* cvd_ind) const;
    void update_cvd_indicator();

    // Volume stream for CVD wicks (cvd_high/cvd_low from intra-candle tracking)
    void handle_volume(const Terminal::Volume& vol);
    struct CVDWickData { double cvd_high; double cvd_low; };
    std::unordered_map<int64_t, CVDWickData> cvd_wick_cache_;
    bool volume_subscribed_ = false;
    int64_t volume_sub_tf_ms_ = 0;  // Timeframe (ms) of current Volume subscription

    // Stats stream for funding rate indicator
    void handle_stat_for_chart(const Terminal::Stat& stat);
    std::unordered_map<int64_t, double> funding_cache_;  // timestamp → funding rate
    double latest_funding_rate_ = 0;
    bool stats_subscribed_ = false;
    int64_t stats_sub_tf_ms_ = 0;

    // Liquidation event stream (WS4 Observed markers) - TF-independent (timeframe 0),
    // subscribed once in the ctor, events stored in LiquidationHeatmapManager.
    bool liq_events_subscribed_ = false;
    bool funding_history_requested_ = false;
    bool funding_data_dirty_ = false;

    // Funding rate helpers
    void populate_funding_data(Indicators::FundingRateIndicator* ind) const;
    void update_funding_indicator();

    // VPIN / Toxicity indicator helpers (Indicators V1 S1b - pathfinder).
    // Data lives in the shared IndicatorSeriesManager (ctx_.series_mgr());
    // the indicator's render cache repopulates when the series revision
    // moves. Live sub = STATE_VPIN (tf 0), lazy on first add; history =
    // get_historical_vpin with an F3 absolute range from the loaded candle
    // window (replay end-clamp is server-side).
    void update_vpin_indicator();
    bool vpin_subscribed_ = false;
    bool vpin_history_requested_ = false;
    uint64_t vpin_populated_revision_ = 0;
    // F3 chunk-until-covered history loop: the server returns the MOST
    // RECENT `count` rows of a window, so one request never guarantees
    // coverage - chunk toward the candle window's start until the gap
    // closes or a chunk brings nothing older (series floor / retention).
    int64_t vpin_hist_last_oldest_ = 0;   // oldest at last chunk (progress check)
    bool vpin_hist_exhausted_ = false;    // no older data - stop chunking
    std::chrono::steady_clock::time_point vpin_hist_last_req_{};

    // OI indicator helpers
    void populate_oi_data(Indicators::OIIndicator* ind) const;
    void update_oi_indicator();
    void request_historical_oi();
    struct OI_OHLC { double open, high, low, close; };
    std::unordered_map<int64_t, OI_OHLC> oi_ohlc_cache_; // timestamp → OHLC (from historical query)
    std::unordered_map<int64_t, double> oi_cache_;  // timestamp → OI USD (from live stat stream)
    double latest_oi_usd_ = 0;
    bool oi_data_dirty_ = false; // Set when new OI data arrives, triggers repopulate
    bool oi_history_requested_ = false;

    static const char* format_timeframe(int64_t seconds);
};
