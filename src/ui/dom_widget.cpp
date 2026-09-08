#include "ui/dom_widget.h"
#include "rendering/theme.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"
#include "replayer/replay_manager.h"

// ═══════════════════════════════════════════════════════════════════════════════
// File-local helpers
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

// Ocean ramp lerp - t∈[0,1] across Theme::Tokens::OCEAN stops
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

// Relative luminance - decides light vs dark ink over a depth bar
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

// Text over a depth bar - dark ink when the bar is bright AND covers the text,
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
        // shadow pass (skipped on bright bars - dark ink needs no shadow)
        dl->AddText(ImVec2(cur.x + x_off + 1.0f, cur.y + 1.0f),
                    IM_COL32(0, 0, 0, 170), txt);
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_off);
    ImGui::PushStyleColor(ImGuiCol_Text,
        (covered && bright) ? Theme::Tokens::BRAND_INK : Theme::Tokens::TX1);
    ImGui::TextUnformatted(txt);
    ImGui::PopStyleColor();
}

// Segmented pill button - active = brand-soft fill, brand text
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
    , title_(std::string("DOM · ") + pair.symbol + " · " + widget_venue_label(pair.exchange) + "###dom_" + pair.exchange + "_" + pair.symbol)
    , tick_size_(tick_size)
    , levels_per_side_(levels_per_side)
    , fmt_(SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol))
{
    subscribed_streams_ = &ctx_.stream_mgr();
    subscribed_streams_->subscribe_orderbook(stream_key_);
    trade_accumulator_.init(pair, ctx_.stream_mgr(), tick_size);
}

DOMWidget::~DOMWidget() {
    if (subscribed_streams_) subscribed_streams_->unsubscribe_orderbook(stream_key_, this);
}

// Late-bind the instrument. A DOM can outlive the moment its tick was unknown:
// the metadata XHR is ~500 KB and in a /demo session it races the pack's own
// range fetches, so the widget is routinely built first. Everything the ladder
// derives from the tick (row grid, trade buckets, price text) is rebuilt here.
void DOMWidget::refresh_instrument() {
    const auto& reg = SymbolRegistry::instance();
    const double tick = reg.tick_or_zero(pair_.exchange, pair_.symbol);
    if (tick <= 0.0 || tick == tick_size_) return;

    tick_size_ = tick;
    fmt_       = reg.get_formatter(pair_.exchange, pair_.symbol);
    trade_accumulator_.set_tick_size(tick);

    // Grid-dependent view state: the old center/scroll were expressed in the
    // placeholder grid's units, and every cached row string was formatted at
    // placeholder precision.
    ladder_center_ = 0.0;
    scroll_offset_ = 0;
    group_mult_    = 1;
    max_bid_size_  = 0.0;
    max_ask_size_  = 0.0;
    cache_ob_ts_   = -1;
    cache_ob_uid_  = -1;
    cache_center_  = -1.0;
}

void DOMWidget::update() {
    // No tick means no ladder: every row price is center + n * tick, so an
    // invented tick invents the whole grid. render() draws the pending state.
    if (tick_size_ <= 0.0) return;

    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot) return;

    if (auto_center_) {
        // Boot transient: the (seeded) book lands before the first trade, so
        // last_price is still 0 - centering there rendered a placeholder
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

    // Rebuild the row models ONLY when an input actually changed - the book
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
// Row-model cache build - ALL formatting + book/tape lookups happen here, on
// input change only. render_ladder() just draws these each frame.
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::build_row_models(const Terminal::Orderbook& ob) {
    update_max_sizes(ob);
    const double max_size = std::max(max_bid_size_, max_ask_size_);
    const double et = tick_size_ * static_cast<double>(group_mult_);
    const double adjusted_center = ladder_center_ + (scroll_offset_ * et);

    rows_ask_.resize(levels_per_side_);
    rows_bid_.resize(levels_per_side_);

    auto fill = [&](RowModel& rm, double price, bool is_ask) {
        rm = RowModel{};
        rm.price = price;
        snprintf(rm.price_txt, sizeof(rm.price_txt), fmt_.price_fmt, price);

        const double size = dom_band_size(ob, price, is_ask, tick_size_, group_mult_);
        if (size > 0.0 && max_size > 0.0) {
            rm.has_size = true;
            rm.depth_frac = static_cast<float>(size / max_size);
            fmt_value(size, price, rm.size_txt, sizeof(rm.size_txt));
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
// Value formatting - COIN qty vs compact USD notional
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
        // Compact large COIN quantities too (else 5-6 digit sizes for low-priced
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

    if (tick_size_ <= 0.0) {
        // Deliberately NOT a ladder. A grid drawn on a guessed tick is a lie
        // that looks like data: at 0.10 on a $0.24 perp the entire book folds
        // into one row and the DOM reads as empty. refresh_instrument() swaps
        // this out for the real grid as soon as the tick lands.
        const auto& reg = SymbolRegistry::instance();
        // Distinguish "not answered yet" from "answered, and this instrument is
        // not in it": the second is permanent and a spinner would read as a hang.
        const bool absent = reg.is_loaded() && !reg.has(pair_.exchange, pair_.symbol);
        ImGui::PushFont(Theme::Fonts::ui());
        if (absent) {
            ImGui::TextColored(Theme::Tokens::TX3, "  No tick size for %s %s",
                               pair_.symbol.c_str(), pair_.exchange.c_str());
        } else {
            ImGui::TextColored(Theme::Tokens::TX3, "  Loading %s tick size...",
                               pair_.symbol.c_str());
        }
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Link RT", &link_rt_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Shares the matching RT chart's depth, trade-flow clock and price positions. Readable tick groups on zoom out; best quotes keep their exact prices. Turn off for independent centering and reset controls.");
    if (link_rt_ && rt_frame_) {
        render_linked_ladder(*rt_frame_);
        ImGui::End();
        return;
    }
    ImGui::TextColored(Theme::Tokens::TX2, "%s / independent%s",
        ctx_.replay_mgr().is_active() ? (ctx_.replay_mgr().is_paused() ? "Replay paused" : "Replay") : "Live",
        link_rt_ ? " (no matching RT chart)" : "");
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
    handle_mouse_input();

    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Controls bar - grouping · Auto/Reset · USD/COIN · flow hint (window via r-click)
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
// Ladder table - buys · bids · PRICE · asks · sells · Δ
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_ladder(const Terminal::Orderbook& ob) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float row_h = Theme::row_h_dense();   // DOM/tape density (18px @ default dial)
    const float pad_y = std::max(0.0f, (row_h - ImGui::GetFontSize()) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, pad_y));

    const int num_columns = show_trade_columns_ ? 6 : 3;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoHostExtendX;

    // Reserve a fixed band at the bottom for the book-imbalance meter (drawn after).
    const ImVec2 tbl_size(0.0f, std::max(60.0f, ImGui::GetContentRegionAvail().y));
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

    // Custom header row - uppercase micro-labels, TX4, ELEV bg
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
            head("\u0394", 2, true);  // Δ - JetBrains Mono carries Greek
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
// Level row - one price tick (ask or bid side)
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_level_row(const RowModel& rm, bool is_ask,
                                 ImDrawList* dl, float pad_y) {
    const float row_h = Theme::row_h_dense();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);

    int col = 0;

    // ── BUYS - faint UP, right-aligned ──
    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        if (rm.has_buy) {
            ImVec4 c = Theme::Tokens::UP; c.w = 0.55f;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            cell_text_right(rm.buy_txt);
            ImGui::PopStyleColor();
        }
    }

    // ── BIDS depth cell - ocean bar anchored to the price side (grows left) ──
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

    // ── PRICE - TX2, centered ──
    ImGui::TableSetColumnIndex(col++);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
        cell_text_center(rm.price_txt);
        ImGui::PopStyleColor();
    }

    // ── ASKS depth cell - ocean bar anchored to the price side (grows right) ──
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

    // ── SELLS - faint DOWN, right-aligned ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_sell) {
        ImVec4 c = Theme::Tokens::DOWN; c.w = 0.55f;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        cell_text_right(rm.sell_txt);
        ImGui::PopStyleColor();
    }

    // ── Δ - signed, direction-colored, right-aligned ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_delta) {
        const ImVec4& c = rm.delta_pos ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        cell_text_right(rm.delta_txt);
        ImGui::PopStyleColor();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Current-price row - ELEV fill, inset hairlines, solid BRAND price chip,
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

    // ── BIDS - empty ──
    ImGui::TableSetColumnIndex(col++);
    if (!show_trade_columns_) {
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        row_y0 = c0.y - pad_y;
        x_left = c0.x - 6.0f;
    }

    // ── PRICE - solid BRAND chip, BRAND_INK text ──
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

    // ── ASKS - empty ──
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

// Absolute screen Y is intentional: the DOM may have different dock bounds or
// header height. Clip the shared price grid; never recenter to fit this panel.
namespace {
void linked_quantity(double value, bool signed_value, char* out, size_t capacity, float width) {
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
}

void DOMWidget::render_linked_ladder(const RealtimeDOMFrame& frame) {
    if (!frame.projected(ImGui::GetFrameCount())) {
        ImGui::TextWrapped("RT chart is not visible. Show it to align depth.");
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(Theme::Tokens::TX2, "%s%s",
        frame.replay ? "Replay" : "Live", frame.paused ? " paused" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton(display_usd_ ? "Quote" : "Qty")) display_usd_ = !display_usd_;
    if (ImGui::IsItemHovered()) Theme::tooltip("Resting depth and received trade volume at the chart clock. CVD is buy quantity minus sell quantity, reset every 5 minutes of market time. Zooming groups nearby ticks into readable rows.");
    if (frame.flow) {
        char cvd[24];
        // CVD stays in base units: multiplying a session total by today's price
        // would misstate the actual traded quote value.
        const double delta = frame.flow->total_delta();
        linked_quantity(delta, true, cvd, sizeof(cvd), 100);
        ImGui::TextColored(delta >= 0 ? Theme::Tokens::UP : Theme::Tokens::DOWN,
            "CVD %s", cvd);
        ImGui::SameLine();
        ImGui::TextDisabled("5m qty%s", frame.flow->reset_after_gap() ? " / reset after gap" : "");
    }
    if (!frame.fresh()) {
        ImGui::TextWrapped(frame.replay
            ? "Waiting for synchronized RT depth. For pack replay, restart from the beginning."
            : "Recovering synchronized RT depth...");
        return;
    }
    const auto& book = *frame.book;
    char bid[24], ask[24];
    snprintf(bid, sizeof(bid), fmt_.price_fmt, book.bid);
    snprintf(ask, sizeof(ask), fmt_.price_fmt, book.ask);
    ImGui::TextColored(Theme::Tokens::UP, "Bid %s", bid);
    ImGui::SameLine();
    ImGui::TextColored(Theme::Tokens::DOWN, "Ask %s", ask);
    char spread[24];
    snprintf(spread, sizeof(spread), fmt_.price_fmt, book.ask - book.bid);
    ImGui::Text("Spread %s / ticks %.0f", spread, (book.ask - book.bid) / tick_size_);
    if (ImGui::IsItemHovered()) Theme::tooltip("Ask minus bid from the same synchronized depth sample. Exact quote prices use the chart scale, even when the spread is smaller than one pixel. Grouped PRICE labels are row centers, not executable quotes.");
    ImGui::TextDisabled("Sample age %.1fs / 100ms bins",
        double(frame.clock_ms - book.timestamp_ms) / 1000.0);
    if (ImGui::IsItemHovered()) Theme::tooltip("Age is measured at the chart clock and freezes on pause. Sampling and the 512-level-per-side retained book are display limits, not a complete exchange event record. Independent ticker and trade arrivals do not replace this sampled book.");

    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float text_h = ImGui::GetFontSize();
    const float header_h = text_h * 2 + 8;
    const float top = std::max(org.y + header_h, frame.top);
    const float bottom = std::min(org.y + avail.y, frame.bottom);
    if (bottom <= top || avail.x <= 0) return;
    const float price_w = std::max(ImGui::CalcTextSize(bid).x, ImGui::CalcTextSize(ask).x) + 10;
    // Reserve a gutter for exact-price quote markers, clear of all numbers.
    const float quote_gutter = 8.0f;
    const float columns_x = org.x + quote_gutter;
    const float col_w = std::max(1.0f, (avail.x - quote_gutter - price_w) / 5);
    const float edges[] = {columns_x, columns_x + col_w, columns_x + col_w * 2,
        columns_x + col_w * 2 + price_w, columns_x + col_w * 3 + price_w,
        columns_x + col_w * 4 + price_w, org.x + avail.x};
    const float tick_h = float(tick_size_ / (frame.price_max - frame.price_min) * (frame.bottom - frame.top));
    const double ticks_per_row = std::max(1.0, std::ceil(double(text_h + 5) / tick_h));
    const double step = tick_size_ * ticks_per_row;
    const double first = std::floor(frame.price_min / step) * step;
    const size_t count = size_t(std::ceil((frame.price_max - first) / step)) + 1;
    linked_rows_.assign(count, {});
    const auto row_index = [&](double price) { return int64_t(std::llround((price - first) / step)); };
    const auto value = [&](double qty, double price) { return display_usd_ ? qty * price : qty; };
    for (const auto& level : book.levels) {
        const auto idx = row_index(level.price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = linked_rows_[size_t(idx)];
        (level.price <= book.bid ? row.bid : row.ask) += value(level.size, level.price);
    }
    if (frame.flow) for (const auto& [price, level] : frame.flow->levels()) {
        const auto idx = row_index(price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = linked_rows_[size_t(idx)];
        row.buy += value(level.buy_volume, price);
        row.sell += value(level.sell_volume, price);
    }
    double max_depth = 0, max_flow = 0;
    for (const auto& row : linked_rows_) {
        max_depth = std::max({max_depth, row.bid, row.ask});
        max_flow = std::max({max_flow, row.buy, row.sell, std::abs(row.buy - row.sell)});
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[] = {"BUYS", "BIDS", "PRICE", "ASKS", "SELLS", "DELTA"};
    for (int c = 0; c < 6; ++c) {
        dl->PushClipRect(ImVec2(edges[c], org.y), ImVec2(edges[c+1], bottom), true);
        dl->AddText(ImVec2((edges[c] + edges[c+1] - ImGui::CalcTextSize(names[c]).x) * 0.5f, org.y),
            Theme::u32(Theme::Tokens::TX2), names[c]);
        dl->PopClipRect();
    }
    char grouping[48]; snprintf(grouping, sizeof(grouping), "Rows %.0f ticks / centers", ticks_per_row);
    dl->AddText(ImVec2(org.x, org.y + text_h + 3), Theme::u32(Theme::Tokens::TX2), grouping);
    const float row_h = float(step / (frame.price_max - frame.price_min) * (frame.bottom - frame.top));
    dl->PushClipRect(ImVec2(org.x, top), ImVec2(org.x + avail.x, bottom), true);
    for (size_t i = 0; i < count; ++i) {
        const double price = first + double(i) * step;
        const float y = frame.price_y(price);
        if (y < top || y > bottom) continue;
        const auto& row = linked_rows_[i];
        const double values[] = {row.buy, row.bid, price, row.ask, row.sell, row.buy - row.sell};
        if (row_index((book.bid + book.ask) * 0.5) == int64_t(i))
            dl->AddRectFilled(ImVec2(org.x, y - row_h * 0.5f), ImVec2(org.x + avail.x, y + row_h * 0.5f), Theme::u32(Theme::Tokens::ELEV));
        dl->AddLine(ImVec2(org.x, y + row_h * 0.5f), ImVec2(org.x + avail.x, y + row_h * 0.5f), Theme::u32(Theme::Tokens::BD1, 0.5f));
        for (int c = 0; c < 6; ++c) {
            if (c != 2 && values[c] == 0) continue;
            const auto color = c < 2 || (c == 5 && values[c] >= 0) ? Theme::Tokens::UP : Theme::Tokens::DOWN;
            const double maximum = c == 1 || c == 3 ? max_depth : max_flow;
            char label[24];
            if (c == 2) snprintf(label, sizeof(label), fmt_.price_fmt, price);
            else {
                // Values have already been converted using each actual trade/level price.
                linked_quantity(values[c], c == 5, label, sizeof(label), edges[c+1] - edges[c] - 6);
            }
            dl->PushClipRect(ImVec2(edges[c]+1, top), ImVec2(edges[c+1]-1, bottom), true);
            if (c != 2 && maximum > 0) {
                const float width = (edges[c+1] - edges[c] - 3) * float(std::abs(values[c]) / maximum);
                const bool leftward = c < 2;
                dl->AddRectFilled(ImVec2(leftward ? edges[c+1] - width : edges[c], y - row_h * 0.5f + 1),
                    ImVec2(leftward ? edges[c+1] : edges[c] + width, y + row_h * 0.5f - 1), Theme::u32(color, c == 1 || c == 3 ? 0.35f : 0.16f));
            }
            const float x = c == 2 ? (edges[c]+edges[c+1]-ImGui::CalcTextSize(label).x)*0.5f : edges[c+1]-ImGui::CalcTextSize(label).x-3;
            dl->AddText(ImVec2(x, y - text_h*0.5f), Theme::u32(c == 2 || c == 1 || c == 3 ? Theme::Tokens::TX1 : color), label);
            dl->PopClipRect();
        }
    }
    // Exact-price markers stay in the gutter, never crossing grouped-row text.
    for (int side = 0; side < 2; ++side) {
        const float y = frame.price_y(side ? book.ask : book.bid);
        // Separate horizontal halves preserve both colors at subpixel spreads.
        const float x = org.x + (side ? 4.5f : 0.5f);
        dl->AddLine(ImVec2(x, y), ImVec2(x + 3.0f, y),
            Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP), 1.25f);
    }
    dl->PopClipRect();
    ImGui::Dummy(avail);
}
