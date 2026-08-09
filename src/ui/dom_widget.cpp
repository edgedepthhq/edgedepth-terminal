#include "ui/dom_widget.h"
#include "rendering/theme.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"

// ═══════════════════════════════════════════════════════════════════════════════
// File-local helpers
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

// Ocean ramp lerp — t∈[0,1] across Theme::Tokens::OCEAN stops
ImVec4 ocean_vec4(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float f = t * 5.0f;
    int i = static_cast<int>(f);
    if (i > 4) i = 4;
    const float fr = f - static_cast<float>(i);
    const ImVec4& a = Theme::Tokens::OCEAN[i];
    const ImVec4& b = Theme::Tokens::OCEAN[i + 1];
    return ImVec4(a.x + (b.x - a.x) * fr,
                  a.y + (b.y - a.y) * fr,
                  a.z + (b.z - a.z) * fr, 1.0f);
}

ImU32 ocean_u32(float t) {
    const ImVec4 c = ocean_vec4(t);
    return IM_COL32(static_cast<int>(c.x * 255.0f),
                    static_cast<int>(c.y * 255.0f),
                    static_cast<int>(c.z * 255.0f), 255);
}

// Relative luminance — decides light vs dark ink over a depth bar
float luminance(const ImVec4& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// Right/center-aligned text within the current table cell
void cell_text_right(const char* txt) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
    ImGui::TextUnformatted(txt);
}

void cell_text_center(const char* txt) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? (w - tw) * 0.5f : 0.0f));
    ImGui::TextUnformatted(txt);
}

// Text over a depth bar — dark ink when the bar is bright AND covers the text,
// otherwise TX1 with a 1px drop shadow so it reads over partial/dark bars.
void depth_cell_text(ImDrawList* dl, const char* txt, bool right_align,
                     float bar_w, float bar_t) {
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const float x_off = right_align ? (w > ts.x ? w - ts.x : 0.0f) : 0.0f;
    const bool covered = bar_w >= ts.x + 4.0f;
    const bool bright = luminance(ocean_vec4(bar_t)) > 0.45f;

    if (!(covered && bright)) {
        // shadow pass (skipped on bright bars — dark ink needs no shadow)
        dl->AddText(ImVec2(cur.x + x_off + 1.0f, cur.y + 1.0f),
                    IM_COL32(0, 0, 0, 170), txt);
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_off);
    ImGui::PushStyleColor(ImGuiCol_Text,
        (covered && bright) ? Theme::Tokens::BRAND_INK : Theme::Tokens::TX1);
    ImGui::TextUnformatted(txt);
    ImGui::PopStyleColor();
}

// Segmented pill button — active = brand-soft fill, brand text
bool seg_button(const char* label, bool active) {
    // De-cyaned (2026-07-17): selected = monochrome raised (bg-2 + text-1),
    // slim ~24px. Teal is reserved for CTAs / links / live data, never on-states.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,        active ? Theme::Tokens::ELEV : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_Text,          active ? Theme::Tokens::TX1 : Theme::Tokens::TX2);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return clicked;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Lifecycle / data
// ═══════════════════════════════════════════════════════════════════════════════

DOMWidget::DOMWidget(const Terminal::Pair& pair, const AppContext& ctx,
                     const double tick_size,
                     const size_t levels_per_side)
    : pair_(pair)
    , ctx_(ctx)
    , stream_key_{pair, Terminal::Stream::Orderbook, 0}
    , title_("DOM " + pair.exchange + " " + pair.symbol)
    , tick_size_(tick_size)
    , levels_per_side_(levels_per_side)
    , fmt_(SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol))
{
    ctx_.stream_mgr().subscribe_orderbook(stream_key_);
    trade_accumulator_.init(pair, ctx_.stream_mgr(), tick_size);
}

DOMWidget::~DOMWidget() {
    ctx_.stream_mgr().unsubscribe_orderbook(stream_key_, this);
}

void DOMWidget::update() {
    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot) return;

    if (auto_center_) {
        // Boot transient: the (seeded) book lands before the first trade, so
        // last_price is still 0 — centering there rendered a placeholder
        // 0.000-price ladder until playback's first trade. Fall back to the
        // book mid so the seeded depth is on-screen from frame one.
        double center = ob->last_price;
        if (center <= 0.0 && !ob->bids.empty() && !ob->asks.empty()) {
            center = (ob->bids.begin()->first + ob->asks.begin()->first) * 0.5;
        }
        if (center > 0.0) {
            ladder_center_ = std::round(center / tick_size_) * tick_size_;
        }
    } else if (manual_center_price_ > 0.0) {
        ladder_center_ = std::round(manual_center_price_ / tick_size_) * tick_size_;
    }

    handle_keyboard_input();
    trade_accumulator_.check_auto_reset();

    // Rebuild the row models ONLY when an input actually changed — the book
    // read-buffer moves at server cadence, not at render FPS.
    if (ob->timestamp_ms != cache_ob_ts_ ||
        ob->last_update_id != cache_ob_uid_ ||
        trade_accumulator_.revision() != cache_acc_rev_ ||
        ladder_center_ != cache_center_ ||
        scroll_offset_ != cache_scroll_ ||
        group_mult_ != cache_group_ ||
        display_usd_ != cache_usd_ ||
        show_trade_columns_ != cache_trade_cols_) {
        cache_ob_ts_      = ob->timestamp_ms;
        cache_ob_uid_     = ob->last_update_id;
        cache_acc_rev_    = trade_accumulator_.revision();
        cache_center_     = ladder_center_;
        cache_scroll_     = scroll_offset_;
        cache_group_      = group_mult_;
        cache_usd_        = display_usd_;
        cache_trade_cols_ = show_trade_columns_;
        build_row_models(*ob);
    }
}

// Sum book depth across a grouped band of `mult` sub-ticks. With mult == 1 this is
// exactly the single-tick lookup, so the default (ungrouped) ladder is unchanged.
static double dom_band_size(const Terminal::Orderbook& ob, double base, bool ask,
                            double tick, int mult) {
    double s = 0.0;
    for (int k = 0; k < mult; ++k) {
        const double p = std::round((base + (ask ? 1.0 : -1.0) * k * tick) / tick) * tick;
        if (ask) {
            const auto it = ob.asks.find(p);
            if (it != ob.asks.end()) s += it->second;
        } else {
            const auto it = ob.bids.find(p);
            if (it != ob.bids.end()) s += it->second;
        }
    }
    return s;
}

void DOMWidget::update_max_sizes(const Terminal::Orderbook& ob) {
    max_bid_size_ = 0.0;
    max_ask_size_ = 0.0;
    const double et = tick_size_ * static_cast<double>(group_mult_);
    const double adjusted_center = ladder_center_ + (scroll_offset_ * et);

    for (int i = 0; i < static_cast<int>(levels_per_side_); i++) {
        const double ask_price = std::round((adjusted_center + (i + 1) * et) / et) * et;
        max_ask_size_ = std::max(max_ask_size_,
                                 dom_band_size(ob, ask_price, true, tick_size_, group_mult_));
        const double bid_price = std::round((adjusted_center - (i + 1) * et) / et) * et;
        max_bid_size_ = std::max(max_bid_size_,
                                 dom_band_size(ob, bid_price, false, tick_size_, group_mult_));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Row-model cache build — ALL formatting + book/tape lookups happen here, on
// input change only. render_ladder() just draws these each frame.
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::build_row_models(const Terminal::Orderbook& ob) {
    update_max_sizes(ob);
    const double max_size = std::max(max_bid_size_, max_ask_size_);
    const double et = tick_size_ * static_cast<double>(group_mult_);
    const double adjusted_center = ladder_center_ + (scroll_offset_ * et);

    rows_ask_.resize(levels_per_side_);
    rows_bid_.resize(levels_per_side_);
    shown_bid_total_ = 0.0; shown_ask_total_ = 0.0;   // book-imbalance meter totals

    auto fill = [&](RowModel& rm, double price, bool is_ask) {
        rm = RowModel{};
        rm.price = price;
        snprintf(rm.price_txt, sizeof(rm.price_txt), fmt_.price_fmt, price);

        const double size = dom_band_size(ob, price, is_ask, tick_size_, group_mult_);
        if (size > 0.0 && max_size > 0.0) {
            rm.has_size = true;
            rm.depth_frac = static_cast<float>(size / max_size);
            fmt_value(size, price, rm.size_txt, sizeof(rm.size_txt));
            if (is_ask) shown_ask_total_ += size; else shown_bid_total_ += size;
        }

        if (show_trade_columns_) {
            double agg_buy = 0.0, agg_sell = 0.0;
            bool agg_any = false;
            for (int k = 0; k < group_mult_; ++k) {
                const double p = std::round((price + (is_ask ? 1.0 : -1.0) * k * tick_size_) / tick_size_) * tick_size_;
                if (const TradeAtPriceLevel* t = trade_accumulator_.get(p)) {
                    agg_buy += t->buy_volume; agg_sell += t->sell_volume; agg_any = true;
                }
            }
            if (agg_any && agg_buy > 0.0) {
                rm.has_buy = true;
                fmt_value(agg_buy, price, rm.buy_txt, sizeof(rm.buy_txt));
            }
            if (agg_any && agg_sell > 0.0) {
                rm.has_sell = true;
                fmt_value(agg_sell, price, rm.sell_txt, sizeof(rm.sell_txt));
            }
            if (agg_any && (agg_buy + agg_sell) > 0.0) {
                rm.has_delta = true;
                const double delta = agg_buy - agg_sell;
                rm.delta_pos = delta >= 0.0;
                fmt_signed(delta, price, rm.delta_txt, sizeof(rm.delta_txt));
            }
        }
    };

    for (int i = 0; i < static_cast<int>(levels_per_side_); i++) {
        const double ask_price = std::round((adjusted_center + (i + 1) * et) / et) * et;
        fill(rows_ask_[static_cast<size_t>(i)], ask_price, true);
        const double bid_price = std::round((adjusted_center - (i + 1) * et) / et) * et;
        fill(rows_bid_[static_cast<size_t>(i)], bid_price, false);
    }

    // Current-price row texts
    row_current_ = CurrentRowModel{};
    snprintf(row_current_.price_txt, sizeof(row_current_.price_txt), fmt_.price_fmt, ob.last_price);
    if (show_trade_columns_) {
        if (trade_accumulator_.total_buy_volume() > 0.0) {
            row_current_.has_buy = true;
            fmt_value(trade_accumulator_.total_buy_volume(), ob.last_price,
                      row_current_.buy_txt, sizeof(row_current_.buy_txt));
        }
        if (trade_accumulator_.total_sell_volume() > 0.0) {
            row_current_.has_sell = true;
            fmt_value(trade_accumulator_.total_sell_volume(), ob.last_price,
                      row_current_.sell_txt, sizeof(row_current_.sell_txt));
        }
        const double total_delta = trade_accumulator_.total_delta();
        row_current_.delta_pos = total_delta >= 0.0;
        fmt_signed(total_delta, ob.last_price,
                   row_current_.delta_txt, sizeof(row_current_.delta_txt));
    }
}

void DOMWidget::handle_keyboard_input() {
    if (!ImGui::IsWindowFocused()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        scroll_offset_++;
        auto_center_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        scroll_offset_--;
        auto_center_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        auto_center_ = true;
        scroll_offset_ = 0;
    }
}

void DOMWidget::handle_mouse_input() {
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        scroll_offset_ += static_cast<int>(ImGui::GetIO().MouseWheel);
        auto_center_ = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Value formatting — COIN qty vs compact USD notional
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::fmt_value(double qty, double price, char* buf, size_t n) const {
    if (display_usd_) {
        const double usd = qty * price;
        const double a = std::abs(usd);
        if (a >= 1e9)      snprintf(buf, n, "%.2fB", usd / 1e9);
        else if (a >= 1e6) snprintf(buf, n, "%.2fM", usd / 1e6);
        else if (a >= 1e3) snprintf(buf, n, "%.1fK", usd / 1e3);
        else               snprintf(buf, n, "%.0f", usd);
    } else {
        // Compact large COIN quantities too (else 5–6 digit sizes for low-priced
        // coins overflow the slim trade/Δ columns and clip past the panel edge).
        const double a = std::abs(qty);
        if (a >= 1e9)      snprintf(buf, n, "%.2fB", qty / 1e9);
        else if (a >= 1e6) snprintf(buf, n, "%.2fM", qty / 1e6);
        else if (a >= 1e3) snprintf(buf, n, "%.1fK", qty / 1e3);
        else               snprintf(buf, n, fmt_.qty_fmt, qty);
    }
}

void DOMWidget::fmt_signed(double v, double price, char* buf, size_t n) const {
    char tmp[24];
    fmt_value(std::abs(v), price, tmp, sizeof(tmp));
    snprintf(buf, n, "%s%s", v >= 0.0 ? "+" : "-", tmp);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main render
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render() {
    if (!is_open) return;

    std::string window_title = title_suffix_.empty() ? title_ : title_ + title_suffix_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin(window_title.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot || ob->asks.empty() || ob->bids.empty()) {
        ImGui::PushFont(Theme::Fonts::ui());
        ImGui::TextColored(Theme::Tokens::TX3, "  No DOM data for %s %s",
                           pair_.symbol.c_str(), pair_.exchange.c_str());
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    render_controls();
    render_ladder(*ob);
    render_imbalance(*ob);
    handle_mouse_input();

    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Controls bar — grouping · Auto/Reset · USD/COIN · flow hint (window via r-click)
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_controls() {
    using namespace Theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 14.0f, TIER = 30.0f, GAPV = 9.0f;
    const float block_h = 12.0f + TIER + GAPV + TIER + 12.0f;   // pad 12 : two 30 tiers : gap 9

    dl->AddRectFilled(org, ImVec2(org.x + ww, org.y + block_h), u32(Tokens::PANEL));
    dl->AddLine(ImVec2(org.x, org.y + block_h - 0.5f), ImVec2(org.x + ww, org.y + block_h - 0.5f),
                u32(Tokens::BD1), 1.0f);

    // v2 segmented group: optional leading label cell (bg-2) + option cells split
    // by 1px dividers; selected option = accent-soft fill + accent-text. Returns the
    // group's total width; out_clk = clicked option index, or -1.
    auto seg_group = [&](const char* id, const char* label, const char* const* opts, int n,
                         int active, float x, float y, int& out_clk) -> float {
        const float H = TIER;
        out_clk = -1;
        float lbl_w = 0.0f;
        if (label && label[0]) {
            ImGui::PushFont(Fonts::label());
            lbl_w = ImGui::CalcTextSize(label).x + 18.0f;
            ImGui::PopFont();
        }
        float opt_w[8]; float total = lbl_w;
        ImGui::PushFont(Fonts::mono_sm());
        for (int i = 0; i < n && i < 8; ++i) { opt_w[i] = ImGui::CalcTextSize(opts[i]).x + 16.0f; total += opt_w[i]; }
        ImGui::PopFont();

        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + total, y + H), u32(Tokens::PANEL), 0.0f);
        if (lbl_w > 0.0f) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + lbl_w, y + H), u32(Tokens::ELEV), Radius::R2);
            ImGui::PushFont(Fonts::label());
            dl->AddText(ImVec2(x + 9.0f, y + (H - ImGui::GetFontSize()) * 0.5f), u32(Tokens::TX3), label);
            ImGui::PopFont();
        }
        float ox = x + lbl_w;
        for (int i = 0; i < n && i < 8; ++i) {
            char cid[24]; snprintf(cid, sizeof(cid), "##%s%d", id, i);
            ImGui::SetCursorScreenPos(ImVec2(ox, y));
            const bool clk = ImGui::InvisibleButton(cid, ImVec2(opt_w[i], H));
            const bool hov = ImGui::IsItemHovered();
            const bool on  = (i == active);
            if (on) dl->AddRectFilled(ImVec2(ox + 1.0f, y + 1.0f), ImVec2(ox + opt_w[i] - 1.0f, y + H - 1.0f),
                                      u32(Tokens::ELEV));
            else if (i > 0) dl->AddLine(ImVec2(ox, y + 6.0f), ImVec2(ox, y + H - 6.0f), u32(Tokens::BD1), 1.0f);
            ImGui::PushFont(Fonts::mono_sm());
            const float tw = ImGui::CalcTextSize(opts[i]).x;
            dl->AddText(ImVec2(ox + (opt_w[i] - tw) * 0.5f, y + (H - ImGui::GetFontSize()) * 0.5f),
                        u32((on || hov) ? Tokens::TX1 : Tokens::TX2), opts[i]);
            ImGui::PopFont();
            if (clk) out_clk = i;
            ox += opt_w[i];
        }
        dl->AddRect(ImVec2(x, y), ImVec2(x + total, y + H), u32(Tokens::BD2), 0.0f, 0, 1.0f);
        return total;
    };

    int clk = -1;

    // Tier 1: TICK aggregation (labels = the coin's tick x 1/10/100) + UNIT (USD/COIN).
    const float y1 = org.y + 12.0f;
    float x = org.x + PADX;
    const int kMults[3] = {1, 10, 100};
    char t0[24], t1[24], t2[24];
    snprintf(t0, sizeof(t0), fmt_.price_fmt, tick_size_ * 1.0);
    snprintf(t1, sizeof(t1), fmt_.price_fmt, tick_size_ * 10.0);
    snprintf(t2, sizeof(t2), fmt_.price_fmt, tick_size_ * 100.0);
    const char* tick_opts[3] = { t0, t1, t2 };
    const int tick_active = (group_mult_ == 1) ? 0 : (group_mult_ == 10 ? 1 : 2);
    x += seg_group("tk", "", tick_opts, 3, tick_active, x, y1, clk) + 9.0f;
    if (clk >= 0 && group_mult_ != kMults[clk]) { group_mult_ = kMults[clk]; auto_center_ = true; scroll_offset_ = 0; }

    const char* unit_opts[2] = { "USD", "COIN" };
    seg_group("un", "", unit_opts, 2, display_usd_ ? 0 : 1, x, y1, clk);
    if (clk == 0) display_usd_ = true; else if (clk == 1) display_usd_ = false;

    // Tier 2: Auto (auto-center toggle) / Reset (clear cumulative), + a right-aligned
    // CUMULATIVE chip and the flow-window readout (click to change the window).
    const float y2 = y1 + TIER + GAPV;
    x = org.x + PADX;
    const char* ar_opts[2] = { "Auto", "Reset" };
    seg_group("ar", "", ar_opts, 2, auto_center_ ? 0 : -1, x, y2, clk);
    if (clk == 0) { auto_center_ = !auto_center_; if (auto_center_) scroll_offset_ = 0; }
    else if (clk == 1) trade_accumulator_.reset();

    const auto mode = trade_accumulator_.reset_mode();
    int mode_idx = 0;
    if (mode == AccumulatorResetMode::Manual)       mode_idx = 0;
    else if (mode == AccumulatorResetMode::Session) mode_idx = 4;
    else { const int64_t sec = trade_accumulator_.reset_interval_sec();
           mode_idx = (sec <= 300) ? 1 : (sec <= 900 ? 2 : 3); }
    static const char* kWin[5] = { "MANUAL", "5M", "15M", "1H", "SESSION" };
    char flow_buf[40]; snprintf(flow_buf, sizeof(flow_buf), "%s FLOW", kWin[mode_idx]);
    ImGui::PushFont(Fonts::label());
    const float flow_w = ImGui::CalcTextSize(flow_buf).x;
    const float cum_tw = ImGui::CalcTextSize("CUMULATIVE").x;
    ImGui::PopFont();
    const float cum_w = cum_tw + 16.0f;
    const float chip_y = y2 + (TIER - 22.0f) * 0.5f;

    float rx = org.x + ww - PADX - flow_w;
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(rx, y2 + (TIER - ImGui::GetFontSize()) * 0.5f), u32(Tokens::TX3), flow_buf);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(rx, y2));
    ImGui::InvisibleButton("##dom_flow", ImVec2(flow_w, TIER));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("dom_flow_window");
    if (ImGui::IsItemHovered()) Theme::tooltip("Click to change the cumulative-flow window");

    rx -= 8.0f + cum_w;
    dl->AddRect(ImVec2(rx, chip_y), ImVec2(rx + cum_w, chip_y + 22.0f), u32(Tokens::BD2), Radius::R1, 0, 1.0f);
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(rx + (cum_w - cum_tw) * 0.5f, chip_y + (22.0f - ImGui::GetFontSize()) * 0.5f),
                u32(Tokens::TX3), "CUMULATIVE");
    ImGui::PopFont();

    if (ImGui::BeginPopup("dom_flow_window")) {
        if (ImGui::MenuItem("Manual",  nullptr, mode_idx == 0))
            trade_accumulator_.set_reset_mode(AccumulatorResetMode::Manual);
        if (ImGui::MenuItem("5m",      nullptr, mode_idx == 1))
            trade_accumulator_.set_reset_mode(AccumulatorResetMode::Periodic, ResetPresets::FIVE_MIN);
        if (ImGui::MenuItem("15m",     nullptr, mode_idx == 2))
            trade_accumulator_.set_reset_mode(AccumulatorResetMode::Periodic, ResetPresets::FIFTEEN_MIN);
        if (ImGui::MenuItem("1h",      nullptr, mode_idx == 3))
            trade_accumulator_.set_reset_mode(AccumulatorResetMode::Periodic, ResetPresets::ONE_HOUR);
        if (ImGui::MenuItem("Session", nullptr, mode_idx == 4))
            trade_accumulator_.set_reset_mode(AccumulatorResetMode::Session);
        ImGui::EndPopup();
    }

    // Consume the fixed block so the ladder table begins directly below it.
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(ImVec2(ww, block_h));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Ladder table — buys · bids · PRICE · asks · sells · Δ
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_ladder(const Terminal::Orderbook& ob) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float row_h = Theme::row_h_dense();   // DOM/tape density (18px @ default dial)
    const float pad_y = std::max(0.0f, (row_h - ImGui::GetFontSize()) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, pad_y));

    const int num_columns = show_trade_columns_ ? 6 : 3;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoHostExtendX;

    // Reserve a fixed band at the bottom for the book-imbalance meter (drawn after).
    const ImVec2 tbl_size(0.0f, std::max(60.0f, ImGui::GetContentRegionAvail().y - 34.0f));
    if (!ImGui::BeginTable("DOMTable", num_columns, flags, tbl_size)) {
        ImGui::PopStyleVar();
        ImGui::PopFont();
        return;
    }

    // Column grid (design: 44px · 1fr · 58px · 1fr · 44px · 42px, scaled to font)
    if (show_trade_columns_)
        ImGui::TableSetupColumn("BUYS", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("BIDS",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("PRICE", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("ASKS",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (show_trade_columns_) {
        ImGui::TableSetupColumn("SELLS", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableSetupColumn("DELTA", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    }
    ImGui::TableSetupScrollFreeze(0, 1);

    // Custom header row — uppercase micro-labels, TX4, ELEV bg
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 22.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
        int c = 0;
        auto head = [&](const char* t, int align, bool mono) {
            ImGui::TableSetColumnIndex(c++);
            if (!mono) ImGui::PushFont(Theme::Fonts::label());
            const float w = ImGui::GetContentRegionAvail().x;
            const float tw = ImGui::CalcTextSize(t).x;
            if (align == 2)      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
            else if (align == 1) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? (w - tw) * 0.5f : 0.0f));
            ImGui::TextUnformatted(t);
            if (!mono) ImGui::PopFont();
        };
        if (show_trade_columns_) head("BUYS", 2, false);
        head("BIDS", 2, false);
        head("PRICE", 1, false);
        head("ASKS", 0, false);
        if (show_trade_columns_) {
            head("SELLS", 2, false);
            head("\u0394", 2, true);  // Δ — JetBrains Mono carries Greek
        }
        ImGui::PopStyleColor();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Rows come from the row-model cache (rebuilt in update() on change only).
    // Guard: first frames can render before update() built the models.
    if (rows_ask_.size() == levels_per_side_ && rows_bid_.size() == levels_per_side_) {
        // ── Ask levels (top, highest price first) ──
        for (int i = static_cast<int>(levels_per_side_) - 1; i >= 0; i--) {
            render_level_row(rows_ask_[static_cast<size_t>(i)], /*is_ask=*/true, dl, pad_y);
        }

        // ── Current-price row ──
        render_current_row(ob, dl, pad_y);

        // ── Bid levels (bottom, highest bid first) ──
        for (int i = 0; i < static_cast<int>(levels_per_side_); i++) {
            render_level_row(rows_bid_[static_cast<size_t>(i)], /*is_ask=*/false, dl, pad_y);
        }
    }

    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Book-imbalance meter — pinned under the ladder (shown-window bid vs ask)
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_imbalance(const Terminal::Orderbook& ob) {
    using namespace Theme;
    (void)ob;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 14.0f, band_h = 34.0f;

    dl->AddLine(org, ImVec2(org.x + ww, org.y), u32(Tokens::BD1), 1.0f);   // top border

    const double tot = shown_bid_total_ + shown_ask_total_;
    const float target = tot > 0.0 ? static_cast<float>(shown_bid_total_ / tot) : 0.5f;
    // EMA smoothing (tau ~4s, framerate-independent) so the meter reads as a trend
    // instead of strobing every book tick. TODO(next): configurable rolling window.
    const float dt = ImGui::GetIO().DeltaTime;
    if (imbalance_ema_ < 0.0f) imbalance_ema_ = target;
    else imbalance_ema_ += (target - imbalance_ema_) * (1.0f - std::exp(-dt / 4.0f));
    const float bid_pct = imbalance_ema_;

    // Label row (padding 8 14): "BOOK IMBALANCE" left; "NN% BID \xc2\xb7 NN% ASK" right.
    const float ly = org.y + 8.0f;
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(org.x + PADX, ly), u32(Tokens::TX3), "BOOK IMBALANCE");
    ImGui::PopFont();

    char rb[32], ra[32];
    snprintf(rb, sizeof(rb), "%d%% BID", static_cast<int>(std::lround(bid_pct * 100.0f)));
    snprintf(ra, sizeof(ra), "%d%% ASK", static_cast<int>(std::lround((1.0f - bid_pct) * 100.0f)));
    ImGui::PushFont(Fonts::mono_sm());
    const float aw = ImGui::CalcTextSize(ra).x;
    const float dw = ImGui::CalcTextSize(" \xc2\xb7 ").x;
    const float bw = ImGui::CalcTextSize(rb).x;
    float rxx = org.x + ww - PADX - aw;
    dl->AddText(ImVec2(rxx, ly), u32(Tokens::DOWN), ra);
    rxx -= dw; dl->AddText(ImVec2(rxx, ly), u32(Tokens::TX3), " \xc2\xb7 ");
    rxx -= bw; dl->AddText(ImVec2(rxx, ly), u32(Tokens::UP), rb);
    ImGui::PopFont();

    // Track (height 5): down-soft bg, up fill from left (bid share), center tick.
    const float ty = org.y + band_h - 8.0f - 5.0f;
    const float tx0 = org.x + PADX, tx1 = org.x + ww - PADX, tw = tx1 - tx0;
    dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + 5.0f), u32(Tokens::DOWN_SOFT));
    dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx0 + tw * bid_pct, ty + 5.0f), u32(Tokens::UP));
    const float cxp = tx0 + tw * 0.5f;
    dl->AddLine(ImVec2(cxp, ty - 2.0f), ImVec2(cxp, ty + 7.0f), u32(Tokens::BD2), 1.0f);

    ImGui::Dummy(ImVec2(ww, band_h));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Level row — one price tick (ask or bid side)
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_level_row(const RowModel& rm, bool is_ask,
                                 ImDrawList* dl, float pad_y) {
    const float row_h = Theme::row_h_dense();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);

    int col = 0;

    // ── BUYS — faint UP, right-aligned ──
    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        if (rm.has_buy) {
            ImVec4 c = Theme::Tokens::UP; c.w = 0.55f;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            cell_text_right(rm.buy_txt);
            ImGui::PopStyleColor();
        }
    }

    // ── BIDS depth cell — ocean bar anchored to the price side (grows left) ──
    ImGui::TableSetColumnIndex(col++);
    if (!is_ask && rm.has_size) {
        const float frac = rm.depth_frac;
        const float bar_t = 0.12f + frac * 0.88f;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float cell_w = ImGui::GetContentRegionAvail().x;
        const float bar_w = std::max(2.0f, frac * cell_w);
        const float y0 = cur.y - pad_y + 1.0f;
        dl->AddRectFilled(ImVec2(cur.x + cell_w - bar_w, y0),
                          ImVec2(cur.x + cell_w, y0 + row_h - 2.0f),
                          ocean_u32(bar_t), 1.0f);
        depth_cell_text(dl, rm.size_txt, /*right_align=*/true, bar_w, bar_t);
    }

    // ── PRICE — TX2, centered ──
    ImGui::TableSetColumnIndex(col++);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
        cell_text_center(rm.price_txt);
        ImGui::PopStyleColor();
    }

    // ── ASKS depth cell — ocean bar anchored to the price side (grows right) ──
    ImGui::TableSetColumnIndex(col++);
    if (is_ask && rm.has_size) {
        const float frac = rm.depth_frac;
        const float bar_t = 0.12f + frac * 0.88f;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float bar_w = std::max(2.0f, frac * ImGui::GetContentRegionAvail().x);
        const float y0 = cur.y - pad_y + 1.0f;
        dl->AddRectFilled(ImVec2(cur.x, y0),
                          ImVec2(cur.x + bar_w, y0 + row_h - 2.0f),
                          ocean_u32(bar_t), 1.0f);
        depth_cell_text(dl, rm.size_txt, /*right_align=*/false, bar_w, bar_t);
    }

    if (!show_trade_columns_) return;

    // ── SELLS — faint DOWN, right-aligned ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_sell) {
        ImVec4 c = Theme::Tokens::DOWN; c.w = 0.55f;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        cell_text_right(rm.sell_txt);
        ImGui::PopStyleColor();
    }

    // ── Δ — signed, direction-colored, right-aligned ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_delta) {
        const ImVec4& c = rm.delta_pos ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        cell_text_right(rm.delta_txt);
        ImGui::PopStyleColor();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Current-price row — ELEV fill, inset hairlines, solid BRAND price chip,
// cumulative buy/sell/Δ totals in the trade columns
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_current_row(const Terminal::Orderbook& ob, ImDrawList* dl, float pad_y) {
    (void)ob;
    const float row_h = Theme::row_h_dense();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Theme::u32(Theme::Tokens::ELEV));

    const CurrentRowModel& cr = row_current_;
    int col = 0;
    float row_y0 = 0.0f, x_left = 0.0f;

    // ── BUYS total ──
    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        row_y0 = c0.y - pad_y;
        x_left = c0.x - 6.0f;
        if (cr.has_buy) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::UP);
            cell_text_right(cr.buy_txt);
            ImGui::PopStyleColor();
        }
    }

    // ── BIDS — empty ──
    ImGui::TableSetColumnIndex(col++);
    if (!show_trade_columns_) {
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        row_y0 = c0.y - pad_y;
        x_left = c0.x - 6.0f;
    }

    // ── PRICE — solid BRAND chip, BRAND_INK text ──
    ImGui::TableSetColumnIndex(col++);
    {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float cell_w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(cr.price_txt).x;
        const float chip_w = std::min(cell_w + 8.0f, tw + 14.0f);
        const float cx0 = cur.x + (cell_w - chip_w) * 0.5f;
        const float cy0 = cur.y - pad_y + 1.5f;
        dl->AddRectFilled(ImVec2(cx0, cy0),
                          ImVec2(cx0 + chip_w, cy0 + row_h - 3.0f),
                          Theme::u32(Theme::Tokens::BRAND), 2.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_w > tw ? (cell_w - tw) * 0.5f : 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
        ImGui::TextUnformatted(cr.price_txt);
        ImGui::PopStyleColor();
    }

    // ── ASKS — empty ──
    ImGui::TableSetColumnIndex(col++);
    float x_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + 6.0f;

    if (show_trade_columns_) {
        // ── SELLS total ──
        ImGui::TableSetColumnIndex(col++);
        if (cr.has_sell) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::DOWN);
            cell_text_right(cr.sell_txt);
            ImGui::PopStyleColor();
        }

        // ── Δ total ──
        ImGui::TableSetColumnIndex(col++);
        {
            x_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + 6.0f;
            const ImVec4& c = cr.delta_pos ? Theme::Tokens::UP : Theme::Tokens::DOWN;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            cell_text_right(cr.delta_txt);
            ImGui::PopStyleColor();
        }
    }

    // ── Inset hairlines top + bottom (design: inset 0 ±1px var(--bd-2)) ──
    const ImU32 hairline = Theme::u32(Theme::Tokens::BD2);
    dl->AddLine(ImVec2(x_left, row_y0 + 0.5f),         ImVec2(x_right, row_y0 + 0.5f),         hairline);
    dl->AddLine(ImVec2(x_left, row_y0 + row_h - 0.5f), ImVec2(x_right, row_y0 + row_h - 0.5f), hairline);
}
