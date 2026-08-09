#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// dom_widget.h — Order ladder (DOM), edgedepth design-system rework
//
// 6-column grid: buys · bids · PRICE · asks · sells · Δ
//   - bid/ask depth cells colored on the OCEAN ramp (small=deep blue →
//     large=green/yellow), bar anchored to the price-side edge
//   - buys/sells = locally-accumulated traded volume (TradeAtPriceAccumulator),
//     faint UP/DOWN; Δ = buys − sells, direction-colored
//   - current-price row: ELEV fill, inset hairlines, solid BRAND price chip
//   - USD/COIN display toggle, reset-mode pills, density-aware row_h()
// ═══════════════════════════════════════════════════════════════════════════════
#include "widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include <vector>
#include <string>
#include <climits>
#include <cstdint>

#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"
#include "core/trade_at_price.h"
#include "core/app_context.h"

class DOMWidget : public Widget {
public:
    DOMWidget(const Terminal::Pair& pair, const AppContext& ctx,
              double tick_size, size_t levels_per_side = 25);
    ~DOMWidget() override;

    void update() override;
    void render() override;
    WidgetType type() const override { return WidgetType::DOM; }
    const char* title() const override { return title_.c_str(); }

    void on_rewind(int64_t /*cutoff_ms*/) override {
        trade_accumulator_.reset();
        max_bid_size_ = 0.0;
        max_ask_size_ = 0.0;
        imbalance_ema_ = -1.0f;
    }

private:
    // Core data
    Terminal::Pair pair_;
    const AppContext& ctx_;
    StreamKey stream_key_;
    std::string title_;

    // Trade accumulator for BUYS/SELLS/Δ columns
    TradeAtPriceAccumulator trade_accumulator_;

    // Display settings
    double tick_size_;
    size_t levels_per_side_;
    PriceFormatter fmt_;
    double ladder_center_ = 0.0;
    double manual_center_price_ = 0.0;
    int scroll_offset_ = 0;
    int group_mult_ = 1;                 // price grouping: x1 / x10 / x100 of base tick

    // UI state
    bool auto_center_ = true;
    bool show_trade_columns_ = true;
    bool display_usd_ = false;

    // Cached max orderbook size for bar normalization
    double max_bid_size_ = 0.0;
    double max_ask_size_ = 0.0;
    // Shown-window resting totals (bid vs ask) for the book-imbalance meter.
    double shown_bid_total_ = 0.0;
    double shown_ask_total_ = 0.0;
    float  imbalance_ema_ = -1.0f;   // EMA-smoothed bid-share (-1 = uninitialised)

    // ── Row-model cache (FPS item, 2026-07-05) ──────────────────────────────
    // The ladder used to re-run the full format cascade every frame: per row,
    // grouped FlatMap binary searches + up to 6 snprintf — at 160fps that's
    // thousands of redundant formats/sec, because the book/tape only change at
    // server cadence. build_row_models() re-formats ONLY when an input changed
    // (book timestamp, accumulator revision, center/scroll/group/display);
    // render_ladder() just draws the cached strings/fractions each frame.
    struct RowModel {
        double price = 0.0;
        float  depth_frac = 0.0f;          // size / max(bid,ask max) — bar width
        bool   has_size = false;
        bool   has_buy = false, has_sell = false, has_delta = false;
        bool   delta_pos = true;
        char   price_txt[24] = {};
        char   size_txt[16]  = {};
        char   buy_txt[16]   = {};
        char   sell_txt[16]  = {};
        char   delta_txt[16] = {};
    };
    std::vector<RowModel> rows_ask_;       // index 0 = closest-to-center ask
    std::vector<RowModel> rows_bid_;       // index 0 = closest-to-center bid
    struct CurrentRowModel {
        bool  has_buy = false, has_sell = false;
        bool  delta_pos = true;
        char  price_txt[24] = {};
        char  buy_txt[16]   = {};
        char  sell_txt[16]  = {};
        char  delta_txt[16] = {};
    } row_current_;
    // Cache key — rebuild when any of these move. ob timestamp alone is not
    // enough (multiple deltas can land within one ms) → pair it with
    // last_update_id, which is strictly increasing on every delta/snapshot.
    int64_t  cache_ob_ts_ = -1;
    int64_t  cache_ob_uid_ = -1;
    uint64_t cache_acc_rev_ = ~0ull;
    double   cache_center_ = 0.0;
    int      cache_scroll_ = INT_MIN;
    int      cache_group_ = 0;
    bool     cache_usd_ = false;
    bool     cache_trade_cols_ = false;
    void build_row_models(const Terminal::Orderbook& ob);

    // Input / data
    void handle_keyboard_input();
    void handle_mouse_input();
    void update_max_sizes(const Terminal::Orderbook& ob);

    // Value formatting (COIN qty vs compact USD notional)
    void fmt_value(double qty, double price, char* buf, size_t n) const;
    void fmt_signed(double v, double price, char* buf, size_t n) const;

    // Rendering
    void render_controls();
    void render_ladder(const Terminal::Orderbook& ob);
    void render_imbalance(const Terminal::Orderbook& ob);
    void render_level_row(const RowModel& rm, bool is_ask, ImDrawList* dl, float pad_y);
    void render_current_row(const Terminal::Orderbook& ob, ImDrawList* dl, float pad_y);
};
