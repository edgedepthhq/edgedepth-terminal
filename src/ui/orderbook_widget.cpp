#include "ui/orderbook_widget.h"
#include "rendering/theme.h"
#include "imgui.h"
#include <algorithm>

#include "core/orderbook_manager.h"

OrderbookWidget::OrderbookWidget(const Terminal::Pair &pair, const AppContext& ctx, const PriceFormatter& fmt, const size_t depth)
    : pair_(pair)
    , title_("Depth " + pair.exchange + " " + pair.symbol)
    , stream_key_{pair, Terminal::Stream::Orderbook, 0}
    , ctx_(ctx)
    , depth_(depth)
    , fmt_(fmt)
    , center_color_(Theme::Colors::NEUTRAL_GRAY)
    , price_went_up_(true)
    , previous_price_(0.0)
{
    ctx_.stream_mgr().subscribe_orderbook(stream_key_);
}

OrderbookWidget::~OrderbookWidget() {
    ctx_.stream_mgr().unsubscribe_orderbook(stream_key_, this);
}


void OrderbookWidget::update() {
    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot) return;
    if (previous_price_ > 0.0 && ob->last_price != previous_price_) {
        price_went_up_ = ob->last_price > previous_price_;
        center_color_ = price_went_up_
            ? Theme::get_buy_color()
            : Theme::get_sell_color();
    }
    previous_price_ = ob->last_price;
}

void OrderbookWidget::render() {
    if (!is_open) return;
    if (!ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot || ob->asks.empty() || ob->bids.empty()) {
        ImGui::Text(
            "No orderbook data for: %s %s",
            pair_.symbol.c_str(),
            pair_.exchange.c_str()
        );
        ImGui::End();
        return;
    }
    render_settings();
    ImGui::Separator();

    // Fit the ladder to the dock height: show as many levels PER SIDE as fit,
    // centered on the mid, so the bids below the price are ALWAYS visible. The old
    // fixed depth_ rendered depth_ ask rows (padding empties) + the mid + depth_ bid
    // rows, which overflowed a short dock and pushed the bids off the bottom.
    const float avail_h  = ImGui::GetContentRegionAvail().y;
    const float row_h    = ImGui::GetTextLineHeightWithSpacing();
    const float header_h = ImGui::GetFrameHeightWithSpacing();          // a table header row
    const float center_h = ImGui::GetTextLineHeight() + 32.0f;          // mid-price block + dummies
    const int   row_budget = static_cast<int>(
        (avail_h - center_h - 2.0f * header_h - row_h) / std::max(1.0f, row_h));  // -row_h = margin
    const size_t visible_levels = static_cast<size_t>(
        std::clamp(row_budget / 2, 1, static_cast<int>(depth_)));

    auto [bids, asks] = ob->top_levels(visible_levels);
    double max_cumulative = 0.0;
    for (const auto& b : bids) {
        max_cumulative = std::max(max_cumulative, b.cumulative);
    }
    for (const auto& a : asks) {
        max_cumulative = std::max(max_cumulative, a.cumulative);
    }
    const float table_width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImGui::PushFont(Theme::Fonts::mono());
    render_asks_table(asks, max_cumulative, table_width, draw_list);
    render_center_price(*ob);
    render_bids_table(bids, max_cumulative, table_width, draw_list);
    ImGui::PopFont();

    ImGui::End();
}

void OrderbookWidget::render_asks_table(
    const std::vector<Terminal::Orderbook::Level>& asks,
    double max_cumulative,
    float table_width,
    ImDrawList* draw_list) const
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 1.5f));
    if (!ImGui::BeginTable("AsksTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("PRICE", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("AMOUNT", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("TOTAL", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableHeadersRow();

    // Render highest price first. Loop over the actual ask count (the caller sizes it
    // to the dock height) - no fixed-depth_ padding, so it never overflows the dock.
    for (size_t i = 0; i < asks.size(); ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        {
            size_t idx = asks.size() - 1 - i;  // Reverse order (highest price first)
            const auto& ask = asks[idx];
            if (show_depth_bars_ && max_cumulative > 0.0) {
                float bar_width = static_cast<float>((ask.cumulative / max_cumulative) * table_width);
                ImVec2 row_pos = ImGui::GetCursorScreenPos();
                float row_height = ImGui::GetTextLineHeightWithSpacing();
                // right-aligned soft fill - keeps price text legible
                draw_list->AddRectFilled(
                    ImVec2(row_pos.x + table_width - bar_width, row_pos.y),
                    ImVec2(row_pos.x + table_width, row_pos.y + row_height),
                    Theme::get_sell_color_u32(
                        static_cast<uint8_t>(bar_opacity_ * 56)));
            }
            ImGui::TextColored(Theme::Colors::SELL_RED, fmt_.price_fmt, ask.price);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text(fmt_.qty_fmt, ask.size);
            if (show_cumulative_) {
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, fmt_.qty_fmt, ask.cumulative);
            }
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void OrderbookWidget::render_bids_table(
    const std::vector<Terminal::Orderbook::Level>& bids,
    double max_cumulative,
    float table_width,
    ImDrawList* draw_list) const {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 1.5f));
    if (!ImGui::BeginTable("BidsTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("PRICE", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("AMOUNT", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("TOTAL", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableHeadersRow();
    for (const auto& bid : bids) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (show_depth_bars_ && max_cumulative > 0.0) {
            float bar_width = static_cast<float>((bid.cumulative / max_cumulative) * table_width);
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            float row_height = ImGui::GetTextLineHeightWithSpacing();
            // right-aligned soft fill - keeps price text legible
            draw_list->AddRectFilled(
                ImVec2(row_pos.x + table_width - bar_width, row_pos.y),
                ImVec2(row_pos.x + table_width, row_pos.y + row_height),
                Theme::get_buy_color_u32(
                    static_cast<uint8_t>(bar_opacity_ * 56)));
        }
        ImGui::TextColored(Theme::Colors::BUY_GREEN, fmt_.price_fmt, bid.price);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text(fmt_.qty_fmt, bid.size);
        if (show_cumulative_) {
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, fmt_.qty_fmt, bid.cumulative);
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void OrderbookWidget::render_center_price(const Terminal::Orderbook& ob) const {
    constexpr float vertical_padding = 8.0f;
    ImGui::Dummy(ImVec2(0.0f, vertical_padding));
    ImGui::BeginChild(
        "CenterPrice",
        ImVec2(0.0f, ImGui::GetTextLineHeight() + vertical_padding * 2.0f),
        false,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration
    );
    // double best_ask = ob.asks.empty() ? 0.0 : ob.asks.begin()->first;
    // double best_bid = ob.bids.empty() ? 0.0 : ob.bids.begin()->first;
    // double spread = best_ask - best_bid;
    ImGui::SetCursorPosY(vertical_padding);
    char price_buf[32];
    snprintf(price_buf, sizeof(price_buf), fmt_.price_fmt, ob.last_price);
    ImGui::PushStyleColor(ImGuiCol_Text, center_color_);
    ImGui::PushFont(Theme::get_large_font());
    // ImGui::Text("%s", price_buf);
    ImGui::SetCursorPosX(ImGui::GetStyle().CellPadding.x);
    ImGui::TextUnformatted(price_buf);
    ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, vertical_padding));
}


void OrderbookWidget::render_settings() {
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_None)) {
        ImGui::Checkbox("Show Cumulative", &show_cumulative_);
        ImGui::Checkbox("Show Depth Bars", &show_depth_bars_);
        if (show_depth_bars_) {
            ImGui::SliderFloat("Bar Opacity", &bar_opacity_, 0.1f, 1.0f, "%.2f");
        }
    }
}