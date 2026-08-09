#include "ui/trades_widget.h"
#include "core/display_time_zone.h"
#include "rendering/theme.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

TradesWidget::TradesWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt)
    : pair_(pair)
    , title_("T " + pair.exchange + " " + pair.symbol)
    , stream_key_{pair, Terminal::Stream::Trades, 0}
    , ctx_(ctx)
    , fmt_(fmt)
{
    trade_flash_times_.fill(0.0f);
    trade_alphas_.fill(0.0f);
    StreamHandler<Terminal::Trade> handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Trade& t) {
            static_cast<TradesWidget*>(ptr)->handle_trade(t);
        }
    };
    ctx_.stream_mgr().subscribe_trades(stream_key_, handler);
}

TradesWidget::~TradesWidget() {
    ctx_.stream_mgr().unsubscribe_trades(stream_key_, this);
}

// For data-driven events.
void TradesWidget::handle_trade(const Terminal::Trade& trade) {
    const size_t slot = trade_count_ % MAX_TRADES;
    trades_[slot] = trade;
    // Format ONCE at insert - trades are immutable (see RowText).
    RowText& txt = row_text_[slot];
    snprintf(txt.price, sizeof(txt.price), fmt_.price_fmt, trade.price);
    snprintf(txt.qty, sizeof(txt.qty), fmt_.qty_fmt, trade.qty);
    format_trade_time(slot);
    formatted_time_zone_generation_ = DisplayTimeZone::instance().generation();
    trade_count_++;
    // Rolling qty EMA for "big print" detection (slow alpha - stable baseline)
    qty_ema_ = (qty_ema_ <= 0.0) ? trade.qty : qty_ema_ * 0.98 + trade.qty * 0.02;
}

void TradesWidget::update() {  // Calculate statistics if enabled
    auto& time_zone = DisplayTimeZone::instance();
    if (formatted_time_zone_generation_ != time_zone.generation()) {
        const size_t count = std::min(trade_count_, MAX_TRADES);
        for (size_t i = 0; i < count; ++i) {
            const size_t slot = (trade_count_ - 1 - i) % MAX_TRADES;
            format_trade_time(slot);
        }
        formatted_time_zone_generation_ = time_zone.generation();
    }
    // Track new trades (newest rendered at top, no scroll needed)
    if (trade_count_ > last_rendered_count_) {
        last_rendered_count_ = trade_count_;
    }
}

void TradesWidget::format_trade_time(size_t slot) {
    if (!DisplayTimeZone::instance().format(trades_[slot].timestamp_ms,
                                             TimeZoneFormat::TimeSeconds,
                                             row_text_[slot].time,
                                             sizeof(row_text_[slot].time))) {
        snprintf(row_text_[slot].time, sizeof(row_text_[slot].time), "--:--:--");
    }
}

void TradesWidget::calculate_statistics() {
    float current_time = ImGui::GetTime();
    constexpr float WINDOW_SECONDS = 60.0f;
    volume_buy_1m_ = 0.0;
    volume_sell_1m_ = 0.0;
    size_t recent_trades = 0;
    size_t count_to_check = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < count_to_check; ++i) {
        size_t idx = (trade_count_ - 1 - i) % MAX_TRADES;
        const auto& trade = trades_[idx];
        // Check if trade is within time window
        float trade_time = static_cast<float>(trade.timestamp_ms) / 1000.0f;
        float age = current_time - trade_time;
        if (age > WINDOW_SECONDS) break;
        recent_trades++;
        if (trade.is_buy) {
            volume_buy_1m_ += trade.qty;
        } else {
            volume_sell_1m_ += trade.qty;
        }
    }
    // Calculate buy pressure (0.0 = all sells, 1.0 = all buys)
    double total_volume = volume_buy_1m_ + volume_sell_1m_;
    if (total_volume > 0.0) {
        buy_pressure_ = static_cast<float>(volume_buy_1m_ / total_volume);
    } else {
        buy_pressure_ = 0.5f;
    }
    // Trades per second
    trades_per_second_ = static_cast<size_t>(recent_trades / WINDOW_SECONDS);
}

void TradesWidget::render_header() {
    // Minimal header - just trade count
    ImGui::Text("%zu trades", std::min(trade_count_, MAX_TRADES));
}

void TradesWidget::render_table() {
    // .tape-* - mono numerics, hairline-free dense rows, micro-label header
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float row_h = 18.0f;
    const float pad_y = std::max(0.0f, (row_h - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, pad_y));

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("TradesTable", 3, flags)) {
        ImGui::PopStyleVar();
        ImGui::PopFont();
        return;
    }

    ImGui::TableSetupColumn("PRICE", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("QTY",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("TIME",  ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupScrollFreeze(0, 1);

    // Custom header - uppercase micro-labels, TX4 (PRICE left, QTY/TIME right)
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 20.0f);
        ImGui::PushFont(Theme::Fonts::label());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX4);
        auto head = [](int col, const char* t, bool right) {
            ImGui::TableSetColumnIndex(col);
            if (right) {
                const float w = ImGui::GetContentRegionAvail().x;
                const float tw = ImGui::CalcTextSize(t).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
            }
            ImGui::TextUnformatted(t);
        };
        head(0, "PRICE", false);
        head(1, "QTY", true);
        head(2, "TIME", true);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Render trades (most recent first)
    const size_t display_count = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < display_count; ++i) {
        const size_t idx = (trade_count_ - 1 - i) % MAX_TRADES;
        render_trade_row(trades_[idx], row_text_[idx]);
    }

    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void TradesWidget::render_trade_row(const Terminal::Trade& trade, const RowText& txt) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 18.0f);

    // Outsized print - brand-soft wash (design: .tape-row.big)
    if (qty_ema_ > 0.0 && trade.qty > qty_ema_ * 8.0) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               Theme::u32(Theme::Tokens::BRAND_SOFT));
    }

    const ImVec4& side = trade.is_buy ? Theme::Tokens::UP : Theme::Tokens::DOWN;

    // PRICE - sign-colored, left
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(side, "%s", txt.price);

    // QTY - sign-colored, right
    ImGui::TableSetColumnIndex(1);
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(txt.qty).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
        ImGui::TextColored(side, "%s", txt.qty);
    }

    // TIME - TX4, right
    ImGui::TableSetColumnIndex(2);
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(txt.time).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
        ImGui::TextColored(Theme::Tokens::TX4, "%s", txt.time);
    }
}

const char* TradesWidget::format_timestamp(int64_t unix_ms) const {
    static char buf[16];
    DisplayTimeZone::instance().format(unix_ms, TimeZoneFormat::TimeSeconds,
                                       buf, sizeof(buf));
    return buf;
}

void TradesWidget::render() {
    if (!is_open) return;
    // Use title_suffix_ for ImGui ID uniqueness (e.g. "##replay" during replay)
    std::string window_title = title_suffix_.empty() ? title_ : title_ + title_suffix_;
    if (!ImGui::Begin(window_title.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    render_table();
    ImGui::End();
}
