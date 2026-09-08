#pragma once
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include "imgui.h"
#include <string>

#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"
#include "core/app_context.h"

class OrderbookWidget : public Widget {
public:
    workspace::Json save_settings() const override;
    void load_settings(const workspace::Json& settings) override;
    OrderbookWidget(const Terminal::Pair &pair, const AppContext& ctx, const PriceFormatter& fmt, size_t depth = 25);
    ~OrderbookWidget() override;

    // Price precision can land after the widget is built. See Widget::refresh_instrument.
    void refresh_instrument() override {
        const PriceFormatter f =
            SymbolRegistry::instance().get_formatter(pair_.exchange, pair_.symbol);
        if (f.resolved) fmt_ = f;
    }

    void render() override;
    void update() override;

    void update_price_color();

    WidgetType type() const override { return WidgetType::Orderbook; }
    const char* title() const override { return title_.c_str(); }

private:
    // Configuration
    Terminal::Pair pair_;
    std::string title_;
    StreamKey stream_key_;
    const AppContext& ctx_;
    size_t depth_;
    PriceFormatter fmt_;
    // Terminal::Orderbook orderbook_;
    // Visual effects
    ImVec4 center_color_;
    bool price_went_up_;
    double previous_price_;
    // UI state
    bool show_cumulative_ = true;
    bool show_depth_bars_ = true;
    float bar_opacity_ = 0.4f;

    void render_asks_table(
        const std::vector<Terminal::Orderbook::Level>& asks,
        double max_cumulative,
        float table_width,
        ImDrawList* draw_list
    ) const;

    void render_bids_table(
        const std::vector<Terminal::Orderbook::Level>& bids,
        double max_cumulative,
        float table_width,
        ImDrawList* draw_list
    ) const;

    void render_center_price(const Terminal::Orderbook& ob) const;
    void render_settings();
};