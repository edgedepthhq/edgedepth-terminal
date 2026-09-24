#include "ui/realtime_dom_ladder.h"
#include "core/trade_at_price.h"
#include "rendering/theme.h"
#include "types/frame_profiler.h"
#include "ui/drawing/drawing_icons.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
void ladder_quantity(double value, bool signed_value, char* out, size_t capacity, float width) {
    const double v = std::abs(value);
    const double scale = v >= 1e9 ? 1e9 : v >= 1e6 ? 1e6 : v >= 1e3 ? 1e3 : 1;
    const char* suffix = scale == 1e9 ? "B" : scale == 1e6 ? "M" : scale == 1e3 ? "K" : "";
    const char* sign = signed_value ? (value >= 0 ? "+" : "-") : "";
    if (scale == 1 && v < 1) snprintf(out, capacity, "%s%.2g", sign, v);
    else for (int decimals = 1; decimals >= 0; --decimals) {
        snprintf(out, capacity, "%s%.*f%s", sign, decimals, v / scale, suffix);
        if (ImGui::CalcTextSize(out).x <= width) break;
    }
}

bool close_button(const char* id, const char* tip) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                                   Theme::u32(Theme::Tokens::ELEV), Theme::Radius::R1);
    drawing::draw_ui_icon(dl, drawing::UiIcon::Close, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f),
        5.0f, Theme::u32(hovered ? Theme::Tokens::TX1 : Theme::Tokens::TX2), 1.4f);
    if (hovered) Theme::tooltip("%s", tip);
    return clicked;
}
}

float RealtimeDomLadder::preferred_width() {
    return ImGui::CalcTextSize("000000.000").x + 5 * ImGui::CalcTextSize("+0000").x + 52;
}

// Absolute screen Y is intentional: the header takes the top of the panel,
// and rows are clipped to the part of the shared price grid below it. Never
// recenter or scale rows independently of the chart.
void RealtimeDomLadder::render(const RealtimeDOMFrame& frame, const PriceFormatter& fmt, bool* open) {
    ProfileScope profile("RT DOM");
    ImGui::TextColored(Theme::Tokens::TX2, "%s%s",
        frame.replay ? "Replay" : "Live", frame.paused ? " paused" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton(display_usd_ ? "Quote" : "Qty")) display_usd_ = !display_usd_;
    if (ImGui::IsItemHovered()) Theme::tooltip("Resting depth and received trade volume at the chart clock. CVD is buy quantity minus sell quantity, reset every 5 minutes of market time. Rows use the heatmap fidelity. The shared price scale keeps numbers readable at every fidelity.");
    if (open) {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                                 ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
        if (close_button("##rt_dom_close", "Hide the depth ladder. Real-time settings > Depth ladder shows it again."))
            *open = false;
    }
    if (!frame.projected(ImGui::GetFrameCount())) {
        ImGui::TextWrapped("Waiting for real-time depth...");
        return;
    }
    if (frame.flow) {
        char cvd[24];
        // CVD stays in base units: multiplying a session total by today's price
        // would misstate the actual traded quote value.
        const double delta = frame.flow->total_delta();
        ladder_quantity(delta, true, cvd, sizeof(cvd), 100);
        ImGui::TextColored(delta >= 0 ? Theme::Tokens::UP : Theme::Tokens::DOWN,
            "CVD %s", cvd);
        ImGui::SameLine();
        ImGui::TextDisabled("5m qty%s", frame.flow->reset_after_gap() ? " / reset after gap" : "");
    }
    if (!frame.fresh()) {
        ImGui::TextWrapped(frame.replay
            ? "Waiting for synchronized RT depth. Seeking reconstructs recorded depth."
            : "Recovering synchronized RT depth...");
        return;
    }
    const auto& book = *frame.book;
    char bid[24], ask[24];
    snprintf(bid, sizeof(bid), fmt.price_fmt, frame.bid());
    snprintf(ask, sizeof(ask), fmt.price_fmt, frame.ask());
    ImGui::TextColored(Theme::Tokens::UP, "Bid %s", bid);
    ImGui::SameLine();
    ImGui::TextColored(Theme::Tokens::DOWN, "Ask %s", ask);
    char spread[24];
    snprintf(spread, sizeof(spread), fmt.price_fmt, frame.ask() - frame.bid());
    ImGui::TextDisabled("Spread %s", spread);
    if (ImGui::IsItemHovered()) Theme::tooltip("The difference between the best ask and bid. PRICE rows show heatmap bucket centers; the bid and ask above show the exact quotes.");

    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float text_h = ImGui::GetFontSize();
    const float header_h = text_h * 2 + 8;
    float top = std::max(org.y + header_h, frame.top);
    float bottom = std::min(org.y + avail.y, frame.bottom);
    if (bottom <= top || avail.x <= 0) return;
    if (frame.native_tick <= 0 || frame.bucket_ticks < 1) return;
    const auto center_fmt = PriceFormatter::from_tick_and_step(frame.native_tick * 0.1, 1);
    char center_label[32];
    center_fmt.format_price(center_label, sizeof(center_label), frame.price_max);
    const float price_w = ImGui::CalcTextSize(center_label).x + 10;
    // Reserve a gutter for exact-price quote markers, clear of all numbers.
    const float quote_gutter = 8.0f;
    const float columns_x = org.x + quote_gutter;
    const float col_w = std::max(1.0f, (avail.x - quote_gutter - price_w) / 5);
    const float edges[] = {columns_x, columns_x + col_w, columns_x + col_w * 2,
        columns_x + col_w * 2 + price_w, columns_x + col_w * 3 + price_w,
        columns_x + col_w * 4 + price_w, org.x + avail.x};
    const double step = frame.bucket_size();
    // Keep the viewport edges on complete bands: no half-cut numeric rows
    // under the header or at the bottom of the panel.
    const auto price_at_y = [&](float y) {
        return frame.price_min + (frame.bottom - y) / (frame.bottom - frame.top) *
            (frame.price_max - frame.price_min);
    };
    top = frame.price_y(std::floor(price_at_y(top) / step + 1e-7) * step);
    bottom = frame.price_y(std::ceil(price_at_y(bottom) / step - 1e-7) * step);
    if (bottom <= top) return;
    const int64_t first_bucket = frame.bucket_index(frame.price_min);
    const int64_t last_bucket = frame.bucket_index(frame.price_max);
    if (last_bucket - first_bucket > 16384) {
        ImGui::TextWrapped("Zoom the price axis in to show the depth rows.");
        return;
    }
    const size_t count = size_t(last_bucket - first_bucket + 1);
    rows_.assign(count, {});
    const auto row_index = [&](double price) { return frame.bucket_index(price) - first_bucket; };
    const auto value = [&](double qty, double price) { return display_usd_ ? qty * price : qty; };
    for (const auto& level : book.levels) {
        const auto idx = row_index(level.price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = rows_[size_t(idx)];
        (level.price <= book.bid ? row.bid : row.ask) += value(level.size, level.price);
    }
    if (frame.flow) for (const auto& [price, level] : frame.flow->levels()) {
        const auto idx = row_index(price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = rows_[size_t(idx)];
        row.buy += value(level.buy_volume, price);
        row.sell += value(level.sell_volume, price);
    }
    double max_depth = 0, max_flow = 0;
    for (const auto& row : rows_) {
        max_depth = std::max({max_depth, row.bid, row.ask});
        max_flow = std::max({max_flow, row.buy, row.sell, std::abs(row.buy - row.sell)});
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[] = {"BUYS", "BIDS", "PRICE", "ASKS", "SELLS", "DELTA"};
    ImGui::PushFont(Theme::Fonts::label());
    for (int c = 0; c < 6; ++c) {
        dl->PushClipRect(ImVec2(edges[c], org.y), ImVec2(edges[c+1], bottom), true);
        dl->AddText(ImVec2((edges[c] + edges[c+1] - ImGui::CalcTextSize(names[c]).x) * 0.5f, org.y),
            Theme::u32(Theme::Tokens::TX2), names[c]);
        dl->PopClipRect();
    }
    ImGui::PopFont();
    const float row_h = float(step / (frame.price_max - frame.price_min) * (frame.bottom - frame.top));
    char grouping[96]; snprintf(grouping, sizeof(grouping), "%s%d ticks / row; prices are centers", frame.automatic_grouping ? "Auto: " : "", frame.bucket_ticks);
    dl->AddText(ImVec2(org.x, org.y + text_h + 3), Theme::u32(Theme::Tokens::TX2), grouping);
    dl->PushClipRect(ImVec2(org.x, top), ImVec2(org.x + avail.x, bottom), true);
    for (size_t i = 0; i < count; ++i) {
        const double price = frame.bucket_center(first_bucket + int64_t(i));
        const float y = frame.price_y(price);
        if (y + row_h * 0.5f < top || y - row_h * 0.5f > bottom) continue;
        if (row_index((book.bid + book.ask) * 0.5) == int64_t(i))
            dl->AddRectFilled(ImVec2(org.x, y - row_h * 0.5f), ImVec2(org.x + avail.x, y + row_h * 0.5f), Theme::u32(Theme::Tokens::ELEV));
        dl->AddLine(ImVec2(org.x, y + row_h * 0.5f), ImVec2(org.x + avail.x, y + row_h * 0.5f), Theme::u32(Theme::Tokens::BD1, 0.5f));
    }
    const float bid_y = frame.price_y(frame.bid()), ask_y = frame.price_y(frame.ask());
    dl->AddRectFilled(ImVec2(edges[2], ask_y), ImVec2(edges[3], bid_y), Theme::u32(Theme::Tokens::TX2, 0.07f));
    for (int c = 0; c < 6; ++c) {
        dl->PushClipRect(ImVec2(edges[c]+1, top), ImVec2(edges[c+1]-1, bottom), true);
        for (size_t i = 0; i < count; ++i) {
            const double price = frame.bucket_center(first_bucket + int64_t(i));
            const float y = frame.price_y(price);
            if (y + row_h * 0.5f < top || y - row_h * 0.5f > bottom) continue;
            const auto& row = rows_[i];
            const double values[] = {row.buy, row.bid, price, row.ask, row.sell, row.buy - row.sell};
            if (c != 2 && values[c] == 0) continue;
            const auto color = c < 2 || (c == 5 && values[c] >= 0) ? Theme::Tokens::UP : Theme::Tokens::DOWN;
            const double maximum = c == 1 || c == 3 ? max_depth : max_flow;
            char label[24];
            if (c == 2) center_fmt.format_price(label, sizeof(label), price);
            else {
                // Values have already been converted using each actual trade/level price.
                ladder_quantity(values[c], c == 5, label, sizeof(label), edges[c+1] - edges[c] - 6);
            }
            if (c != 2 && maximum > 0) {
                const float width = (edges[c+1] - edges[c] - 3) * float(std::abs(values[c]) / maximum);
                const bool leftward = c < 2;
                dl->AddRectFilled(ImVec2(leftward ? edges[c+1] - width : edges[c], y - row_h * 0.5f),
                    ImVec2(leftward ? edges[c+1] : edges[c] + width, y + row_h * 0.5f), Theme::u32(color, c == 1 || c == 3 ? 0.35f : 0.16f));
            }
            const float x = c == 2 ? (edges[c]+edges[c+1]-ImGui::CalcTextSize(label).x)*0.5f : edges[c+1]-ImGui::CalcTextSize(label).x-3;
            dl->AddText(ImVec2(x, y - text_h*0.5f), Theme::u32(c == 2 || c == 1 || c == 3 ? Theme::Tokens::TX1 : color), label);
        }
        dl->PopClipRect();
    }
    // Exact-price markers stay in the gutter, never crossing grouped-row text.
    for (int side = 0; side < 2; ++side) {
        const float y = frame.price_y(side ? frame.ask() : frame.bid());
        // PRICE column notches align with chart BBO without crossing its text.
        const auto color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
        const float notch_x = side ? edges[3] - 4 : edges[2];
        dl->AddLine(ImVec2(notch_x, y), ImVec2(notch_x + 3, y), color, 1.0f);
        // Separate horizontal halves preserve both colors at subpixel spreads.
        const float x = org.x + (side ? 4.5f : 0.5f);
        dl->AddLine(ImVec2(x, y), ImVec2(x + 3.0f, y),
            Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP), 1.25f);
    }
    dl->PopClipRect();
    ImGui::Dummy(avail);
}
