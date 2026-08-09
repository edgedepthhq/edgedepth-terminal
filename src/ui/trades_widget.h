#pragma once
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include "core/symbol_metadata.h"
#include "core/app_context.h"
#include <array>
#include <string>

class TradesWidget : public Widget {
public:
    TradesWidget(const Terminal::Pair &pair, const AppContext& ctx, const PriceFormatter& fmt);
    ~TradesWidget() override;

    void render() override;
    void update() override;
    WidgetType type() const override { return WidgetType::Trades; }
    const char* title() const override { return title_.c_str(); }

    void on_rewind(int64_t /*cutoff_ms*/) override { clear(); }

    void handle_trade(const Terminal::Trade& trade);

    // Clear all trade data (used on replay rewind)
    void clear() {
        trade_count_ = 0;
        last_rendered_count_ = 0;
        volume_buy_1m_ = 0.0;
        volume_sell_1m_ = 0.0;
        buy_pressure_ = 0.5f;
        trades_per_second_ = 0;
    }

private:
    Terminal::Pair pair_;
    std::string title_;
    StreamKey stream_key_;
    const AppContext& ctx_;
    PriceFormatter fmt_;
    static constexpr size_t MAX_TRADES = 64;
    std::array<Terminal::Trade, MAX_TRADES> trades_{};
    size_t trade_count_ = 0;
    // Pre-formatted row text (FPS item, 2026-07-05): trades are immutable, so
    // price/qty/time strings are formatted once at insert. A display-time-zone
    // generation change reformats the bounded 64-row buffer outside render.
    struct RowText {
        char price[24];
        char qty[24];
        char time[12];
    };
    std::array<RowText, MAX_TRADES> row_text_{};
    uint64_t formatted_time_zone_generation_ = 0;
    // Visual effects
    std::array<float, MAX_TRADES> trade_flash_times_{};
    std::array<float, MAX_TRADES> trade_alphas_{};
    // Statistics
    double volume_buy_1m_ = 0.0;
    double volume_sell_1m_ = 0.0;
    float buy_pressure_ = 0.5f;
    size_t trades_per_second_ = 0;
    // Rolling qty EMA — flags outsized prints ("big" rows, brand-soft wash)
    double qty_ema_ = 0.0;
    // UI state
    bool auto_scroll_ = true;
    bool show_stats_ = true;
    size_t last_rendered_count_ = 0;
    bool scroll_to_bottom_ = false;
    // Helper methods
    void calculate_statistics();
    void render_header();
    void render_table();
    void render_trade_row(const Terminal::Trade& trade, const RowText& txt);
    void format_trade_time(size_t slot);
    const char* format_timestamp(int64_t unix_ms) const;
};
