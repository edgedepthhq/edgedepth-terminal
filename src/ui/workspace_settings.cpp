#include "ui/chart_widget.h"
#include "ui/dom_widget.h"
#include "ui/trades_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/stats_widget.h"
#include "ui/watchlist_widget.h"
#include "ui/indicators/rsi_indicator.h"
#include "ui/indicators/macd_indicator.h"
#include "ui/indicators/volume_indicator.h"
#include "ui/indicators/cvd_indicator.h"
#include "ui/indicators/oi_indicator.h"
#include "ui/indicators/funding_rate_indicator.h"
#include "ui/indicators/vpin_indicator.h"
#include "core/footprint_manager.h"
#include "core/tpo_manager.h"
#include "core/volume_profile_manager.h"
#include "core/candle_manager.h"
using workspace::Json;

Json DOMWidget::save_settings() const {
    Json j = Json::object();
    j["group_mult"] = group_mult_;
    j["levels_per_side"] = levels_per_side_;
    j["link_rt"] = link_rt_;
    j["auto_center"] = auto_center_;
    j["show_trade_columns"] = show_trade_columns_;
    j["display_usd"] = display_usd_;
    j["reset_mode"] = trade_accumulator_.reset_mode();
    j["reset_seconds"] = trade_accumulator_.reset_interval_sec();
    return j;
}
void DOMWidget::load_settings(const Json& j) {
    workspace::read(j, "group_mult", group_mult_, 1, 1000);
    workspace::read(j, "levels_per_side", levels_per_side_, 5, 200);
    workspace::read(j, "link_rt", link_rt_);
    workspace::read(j, "auto_center", auto_center_);
    workspace::read(j, "show_trade_columns", show_trade_columns_);
    workspace::read(j, "display_usd", display_usd_);
    auto mode = trade_accumulator_.reset_mode();
    auto seconds = trade_accumulator_.reset_interval_sec();
    workspace::read(j, "reset_mode", mode, 0, 2);
    workspace::read(j, "reset_seconds", seconds, 1, 86400);
    trade_accumulator_.set_reset_mode(mode, seconds);
    cache_ob_ts_ = -1;
}

Json TradesWidget::save_settings() const {
    Json j = Json::object();
    j["auto_scroll"] = auto_scroll_;
    j["show_stats"] = show_stats_;
    return j;
}
void TradesWidget::load_settings(const Json& j) {
    workspace::read(j, "auto_scroll", auto_scroll_);
    workspace::read(j, "show_stats", show_stats_);
}

Json OrderbookWidget::save_settings() const {
    Json j = Json::object();
    j["show_cumulative"] = show_cumulative_;
    j["show_depth_bars"] = show_depth_bars_;
    j["bar_opacity"] = bar_opacity_;
    return j;
}
void OrderbookWidget::load_settings(const Json& j) {
    workspace::read(j, "show_cumulative", show_cumulative_);
    workspace::read(j, "show_depth_bars", show_depth_bars_);
    workspace::read(j, "bar_opacity", bar_opacity_, 0, 1);
}

Json StatsWidget::save_settings() const {
    Json j = Json::object();
    j["sections"] = Json::array({section_open_[0], section_open_[1], section_open_[2]});
    return j;
}
void StatsWidget::load_settings(const Json& j) {
    auto it = j.find("sections");
    if (it != j.end() && it->is_array() && it->size() == 3)
        for (size_t i = 0; i < 3; ++i)
            if ((*it)[i].is_boolean()) section_open_[i] = (*it)[i].get<bool>();
}

Json WatchlistWidget::save_settings() const {
    Json j = Json::object();
    j["selected_category"] = selected_category_;
    j["sort"] = sort_;
    j["sort_desc"] = sort_desc_;
    j["compact"] = compact_;
    return j;
}
void WatchlistWidget::load_settings(const Json& j) {
    workspace::read(j, "selected_category", selected_category_, -1, 1000);
    workspace::read(j, "sort", sort_, 0, 3);
    workspace::read(j, "sort_desc", sort_desc_);
    workspace::read(j, "compact", compact_);
}

Json ChartWidget::save_settings() const {
    Json j = Json::object();
    j["session_vwap"] = session_vwap_;
    j["previous_day"] = previous_day_;
    j["previous_week"] = previous_week_;
    j["vwap_anchor"] = {{"time",vwap_anchor_ms_},{"exchange",pair_.exchange},{"symbol",pair_.symbol}};
    j["rt_mode"] = rt_mode_;
    j["rt_candles"] = rt_candles_;
    j["rt_bubbles"] = rt_bubbles_;
    j["rt_trade_line"] = rt_trade_line_;
    j["rt_extend_depth"] = rt_extend_depth_;
    j["rt_auto_price"] = rt_auto_price_;
    j["rt_auto_bubbles"] = rt_auto_bubbles_;
    j["rt_min_notional"] = rt_min_notional_;
    j["rt_span_ms"] = rt_span_ms_;
    j["rt_bucket_multiplier"] = rt_bucket_multiplier_;
    j["heatmap_enabled"] = heatmap_enabled_;
    j["heatmap_adapt_to_zoom"] = heatmap_adapt_to_zoom_;
    j["heatmap_bucket_multiplier"] = heatmap_bucket_multiplier_;
    j["heatmap_sensitivity"] = heatmap_sensitivity_;
    j["liq_extend_levels"] = liq_extend_levels_;
    j["liq_opacity"] = liq_opacity_;
    j["liq_color_low"] = liq_color_low_;
    j["liq_color_peak"] = liq_color_peak_;
    j["liq_color_auto"] = liq_color_auto_;
    j["liq_ticks_per_row"] = liq_ticks_per_row_;
    j["liq_lev_5x"] = liq_lev_5x_;
    j["liq_lev_10x"] = liq_lev_10x_;
    j["liq_lev_25x"] = liq_lev_25x_;
    j["liq_lev_50x"] = liq_lev_50x_;
    j["liq_lev_75x"] = liq_lev_75x_;
    j["liq_lev_100x"] = liq_lev_100x_;
    j["liq_dense_field"] = liq_dense_field_;
    j["liq_profile_enabled"] = liq_profile_enabled_;
    j["liq_observed_enabled"] = liq_observed_enabled_;
    j["liq_census_enabled"] = liq_census_enabled_;
    j["liq_obs_min_usd"] = liq_obs_min_usd_;
    j["liq_obs_ref_usd"] = liq_obs_ref_usd_;
    j["vpvr_enabled"] = vpvr_enabled_;
    j["vpvr_ticks_per_row"] = vpvr_ticks_per_row_;
    j["renko_brick_ticks"] = renko_brick_ticks_;
    { const auto& k = liq_field_.knobs();
      j["liq_field"] = {{"opacity",k.opacity},{"gamma",k.gamma},{"low",k.lo_pct},
        {"peak",k.hi_pct},{"noise",k.floor_t},{"bps",k.bps},{"half_life",k.halflife_h}}; }
    { const auto& v = ctx_.vpvr_mgr();
      j["volume_profile"] = {{"mode",v.mode()},{"poc",v.show_poc()},
        {"value_area",v.show_vah_val()},{"values",v.show_values()},{"width",v.width_pct()}}; }
    j["chart_type"] = chart_type_;
    j["timeframe"] = timeframe_seconds();
    j["indicators"] = Json::array();
    for (size_t i = 0; i < indicator_mgr_.count(); ++i) {
        const auto* ind = indicator_mgr_.get_indicator(i);
        auto row = ind->save_settings();
        row["name"] = ind->get_name();
        row["height"] = ind->get_height_pixels();
        row["visible"] = ind->is_visible();
        j["indicators"].push_back(std::move(row));
    }
    j["indicator_layout"] = indicator_mgr_.save_layout();
    { auto& m = ctx_.footprint_mgr(); Json s;
    s["mode"] = m.mode;
    s["comparison"] = m.comparison;
    s["ticks_per_row"] = m.ticks_per_row;
    s["show_imbalances"] = m.show_imbalances;
    s["show_poc"] = m.show_poc;
    s["show_summary"] = m.show_summary;
    s["imbalance_min_volume"] = m.imbalance_min_volume;
    s["stacked_levels"] = m.stacked_levels;
    s["imbalance_ratio"] = m.imbalance_ratio;
    j["footprint"] = std::move(s); }
    { auto& m = ctx_.tpo_mgr(); Json s;
    s["session_period_hours"] = m.session_period_hours;
    s["ticks_per_row_setting"] = m.ticks_per_row_setting;
    s["value_area_pct"] = m.value_area_pct;
    s["show_poc_ray"] = m.show_poc_ray;
    s["show_vah_val_rays"] = m.show_vah_val_rays;
    s["show_single_prints"] = m.show_single_prints;
    s["show_poor_high_low"] = m.show_poor_high_low;
    s["show_initial_balance"] = m.show_initial_balance;
    s["show_session_header"] = m.show_session_header;
    s["highlight_start_end"] = m.highlight_start_end;
    s["profile_spacing"] = m.profile_spacing;
    j["tpo"] = std::move(s); }

    return j;
}
void ChartWidget::load_settings(const Json& j) {
    workspace::read(j, "session_vwap", session_vwap_);
    workspace::read(j, "previous_day", previous_day_);
    workspace::read(j, "previous_week", previous_week_);
    const auto& anchor = workspace::object(j, "vwap_anchor");
    if (anchor.contains("exchange") && anchor["exchange"] == pair_.exchange &&
        anchor.contains("symbol") && anchor["symbol"] == pair_.symbol)
        workspace::read(anchor, "time", vwap_anchor_ms_, 0, 4102444800000LL);
    reference_update_time_ = -1;

    { auto& k = liq_field_.knobs(); const auto& s = workspace::object(j,"liq_field");
      workspace::read(s,"opacity",k.opacity,0,1); workspace::read(s,"gamma",k.gamma,0.5,4);
      workspace::read(s,"low",k.lo_pct,0,0.9); workspace::read(s,"peak",k.hi_pct,0.95,1);
      workspace::read(s,"noise",k.floor_t,0,0.1); workspace::read(s,"bps",k.bps,2,20);
      workspace::read(s,"half_life",k.halflife_h,0,72); liq_field_.invalidate(); }
    { auto& v = ctx_.vpvr_mgr(); const auto& s = workspace::object(j,"volume_profile");
      auto mode=v.mode(); bool poc=v.show_poc(), va=v.show_vah_val(), values=v.show_values();
      float width=v.width_pct(); workspace::read(s,"mode",mode,0,2);
      workspace::read(s,"poc",poc); workspace::read(s,"value_area",va); workspace::read(s,"values",values);
      workspace::read(s,"width",width,0.01,0.5);
      v.set_mode(mode); v.set_show_poc(poc); v.set_show_vah_val(va); v.set_show_values(values); v.set_width_pct(width); }
    int tf = static_cast<int>(timeframe_seconds());
    workspace::read(j, "timeframe", tf, 1, 604800);
    if (tf != timeframe_seconds()) change_timeframe(tf);
    auto ct = chart_type_;
    workspace::read(j, "chart_type", ct, 0, 6);
    set_chart_type(ct);
    workspace::read(j, "rt_mode", rt_mode_);
    workspace::read(j, "rt_candles", rt_candles_);
    workspace::read(j, "rt_bubbles", rt_bubbles_);
    workspace::read(j, "rt_trade_line", rt_trade_line_);
    workspace::read(j, "rt_extend_depth", rt_extend_depth_);
    workspace::read(j, "rt_auto_price", rt_auto_price_);
    workspace::read(j, "rt_auto_bubbles", rt_auto_bubbles_);
    workspace::read(j, "rt_min_notional", rt_min_notional_, 0, 1000000000000.0);
    workspace::read(j, "rt_span_ms", rt_span_ms_, 1000, 1800000);
    workspace::read(j, "rt_bucket_multiplier", rt_bucket_multiplier_, 1, 20);
    workspace::read(j, "heatmap_enabled", heatmap_enabled_);
    workspace::read(j, "heatmap_adapt_to_zoom", heatmap_adapt_to_zoom_);
    workspace::read(j, "heatmap_bucket_multiplier", heatmap_bucket_multiplier_, 1, 100);
    workspace::read(j, "heatmap_sensitivity", heatmap_sensitivity_, 0.01, 100);
    workspace::read(j, "liq_extend_levels", liq_extend_levels_);
    workspace::read(j, "liq_opacity", liq_opacity_, 0, 1);
    workspace::read(j, "liq_color_low", liq_color_low_, 0, 1000000000000000.0);
    workspace::read(j, "liq_color_peak", liq_color_peak_, 1, 1000000000000000.0);
    workspace::read(j, "liq_color_auto", liq_color_auto_);
    workspace::read(j, "liq_ticks_per_row", liq_ticks_per_row_, 0, 100000);
    workspace::read(j, "liq_lev_5x", liq_lev_5x_);
    workspace::read(j, "liq_lev_10x", liq_lev_10x_);
    workspace::read(j, "liq_lev_25x", liq_lev_25x_);
    workspace::read(j, "liq_lev_50x", liq_lev_50x_);
    workspace::read(j, "liq_lev_75x", liq_lev_75x_);
    workspace::read(j, "liq_lev_100x", liq_lev_100x_);
    workspace::read(j, "liq_dense_field", liq_dense_field_);
    workspace::read(j, "liq_profile_enabled", liq_profile_enabled_);
    workspace::read(j, "liq_observed_enabled", liq_observed_enabled_);
    workspace::read(j, "liq_census_enabled", liq_census_enabled_);
    workspace::read(j, "liq_obs_min_usd", liq_obs_min_usd_, 0, 1000000000000000.0);
    workspace::read(j, "liq_obs_ref_usd", liq_obs_ref_usd_, 1, 1000000000000000.0);
    workspace::read(j, "vpvr_enabled", vpvr_enabled_);
    workspace::read(j, "vpvr_ticks_per_row", vpvr_ticks_per_row_, 0, 100000);
    workspace::read(j, "renko_brick_ticks", renko_brick_ticks_, 0, 100000);
    if (rt_bucket_multiplier_ != 1 && rt_bucket_multiplier_ != 2 &&
        rt_bucket_multiplier_ != 5 && rt_bucket_multiplier_ != 10 &&
        rt_bucket_multiplier_ != 20) rt_bucket_multiplier_ = 5;
    auto inds = j.find("indicators");
    if (inds != j.end() && inds->is_array() && inds->size() <= 16) {
        while (indicator_mgr_.count()) indicator_mgr_.remove_indicator(0);
        for (const auto& row : *inds) {
            if (!row.is_object()) continue;
            auto n = row.find("name");
            if (n == row.end() || !n->is_string()) continue;
            const auto& name = n->get_ref<const std::string&>();
            const auto before = indicator_mgr_.count();
            if (name == "Vol (USDT)") add_volume_indicator();
            else if (name == "CVD") add_cvd_indicator();
            else if (name == "Open Interest") add_oi_indicator();
            else if (name == "Funding Rate") add_funding_rate_indicator();
            else if (name == "TOXICITY") add_vpin_indicator();
            else if (name == "RSI") {
                int period = 14; workspace::read(row, "period", period, 2, 500);
                add_rsi_indicator(period);
            } else if (name == "MACD") {
                int fast = 12, slow = 26, signal = 9;
                workspace::read(row, "fast", fast, 2, 500);
                workspace::read(row, "slow", slow, 3, 1000);
                workspace::read(row, "signal", signal, 2, 500);
                if (fast < slow) add_macd_indicator(fast, slow, signal);
            }
            if (indicator_mgr_.count() == before) continue;
            auto* ind = indicator_mgr_.get_indicator(before);
            float height = ind->get_height_pixels(); bool visible = true;
            workspace::read(row, "height", height, 120, 360);
            workspace::read(row, "visible", visible);
            ind->set_height_pixels(height); ind->set_visible(visible);
            ind->load_settings(row);
        }
        indicator_mgr_.load_layout(workspace::object(j, "indicator_layout"));
    }
    { auto& m = ctx_.footprint_mgr(); const auto& s = workspace::object(j, "footprint");
    workspace::read(s, "mode", m.mode, 0, 3);
    workspace::read(s, "comparison", m.comparison, 0, 1);
    workspace::read(s, "ticks_per_row", m.ticks_per_row, 0, 100000);
    workspace::read(s, "show_imbalances", m.show_imbalances);
    workspace::read(s, "show_poc", m.show_poc);
    workspace::read(s, "show_summary", m.show_summary);
    workspace::read(s, "imbalance_min_volume", m.imbalance_min_volume, 0, 1000000000000000.0);
    workspace::read(s, "stacked_levels", m.stacked_levels, 0, 10);
    workspace::read(s, "imbalance_ratio", m.imbalance_ratio, 1.5, 10);
    }
    { auto& m = ctx_.tpo_mgr(); const auto& s = workspace::object(j, "tpo");
    workspace::read(s, "session_period_hours", m.session_period_hours, 24, 168);
    workspace::read(s, "ticks_per_row_setting", m.ticks_per_row_setting, 0, 100000);
    workspace::read(s, "value_area_pct", m.value_area_pct, 0.01, 1);
    workspace::read(s, "show_poc_ray", m.show_poc_ray);
    workspace::read(s, "show_vah_val_rays", m.show_vah_val_rays);
    workspace::read(s, "show_single_prints", m.show_single_prints);
    workspace::read(s, "show_poor_high_low", m.show_poor_high_low);
    workspace::read(s, "show_initial_balance", m.show_initial_balance);
    workspace::read(s, "show_session_header", m.show_session_header);
    workspace::read(s, "highlight_start_end", m.highlight_start_end);
    workspace::read(s, "profile_spacing", m.profile_spacing, 0, 30);
    }
    if (ctx_.footprint_mgr().stacked_levels == 1) ctx_.footprint_mgr().stacked_levels = 2;
    if (ctx_.tpo_mgr().session_period_hours != 168) ctx_.tpo_mgr().session_period_hours = 24;

}
