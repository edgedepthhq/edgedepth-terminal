// ═══════════════════════════════════════════════════════════════════════════════
// watchlist_widget.cpp - dense-grid watchlist (edgedepth v2, treatment 1b)
//
// Header stack (non-scrolling): title bar · filter row · category + venue
// selectors/popovers · SYMBOL/LAST/24H% sort chips. Then a scrolling
// list of dense rows: star · sparkline · symbol (ellipsized) · last · 24h% in
// fixed tabular columns, plus a dim 24h-volume figure beneath. Colours resolve to
// Theme::Tokens, numerals to PriceFormatter, rows are virtualised with
// ImGuiListClipper; no per-row heap allocations in the hot path.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/watchlist_widget.h"
#include "rendering/theme.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "core/url_router.h"
#include "imgui.h"
#include "imgui_internal.h"          // ImGuiDockNodeFlags_NoTabBar (window class)
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

struct WatchlistVenue {
    const char* id;
    const char* label;
    const char* compact_label;
};

constexpr WatchlistVenue kWatchlistVenues[] = {
    {"binancef", "BINANCE",     "BINANCE"},
    {"hl",       "HYPERLIQUID", "HYPERLIQ"},
};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// case-insensitive substring (ASCII symbols / categories only)
bool contains_ci(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const size_t nlen = strlen(needle);
    if (nlen > hay.size()) return false;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        size_t j = 0;
        while (j < nlen &&
               std::tolower(static_cast<unsigned char>(hay[i + j])) ==
               std::tolower(static_cast<unsigned char>(needle[j]))) j++;
        if (j == nlen) return true;
    }
    return false;
}

// 5-point star, drawn (fonts carry no reliable star glyph). Filled = triangle
// fan from centre (a 5-point star is star-convex from its centroid).
void draw_star(ImDrawList* dl, ImVec2 ctr, float r, bool filled, ImU32 col) {
    ImVec2 p[10];
    const float ri = r * 0.42f;
    for (int i = 0; i < 10; ++i) {
        const float a = (-90.0f + i * 36.0f) * 3.14159265f / 180.0f;
        const float rr = (i & 1) ? ri : r;
        p[i] = ImVec2(ctr.x + std::cos(a) * rr, ctr.y + std::sin(a) * rr);
    }
    if (filled) {
        for (int i = 0; i < 10; ++i)
            dl->AddTriangleFilled(ctr, p[i], p[(i + 1) % 10], col);
    } else {
        dl->AddPolyline(p, 10, col, ImDrawFlags_Closed, 1.0f);
    }
}

// compact 24h quote-volume label: "$1.2B" / "$340M" / "$12M" / "$120K"
void fmt_volume(char* buf, size_t n, double v) {
    if (v >= 1e9)      snprintf(buf, n, "$%.2fB", v / 1e9);
    else if (v >= 1e6) snprintf(buf, n, "$%.0fM", v / 1e6);
    else if (v >= 1e3) snprintf(buf, n, "$%.0fK", v / 1e3);
    else               snprintf(buf, n, "$%.0f", v);
}

} // namespace

WatchlistWidget::WatchlistWidget(const Terminal::Pair& active_pair, const AppContext& ctx)
    : active_pair_(active_pair)
    , ctx_(ctx)
{}

// ═══════════════════════════════════════════════════════════════════════════════
// Update - sample sparkline ring buffers off the always-on ticker stream
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::update() {
    const int64_t now = now_ms();
    if (now - last_sample_ms_ < static_cast<int64_t>(SAMPLE_MS)) return;
    last_sample_ms_ = now;

    auto& reg = SymbolRegistry::instance();
    if (!reg.is_loaded()) return;
    auto& tickers = TickerManager::instance();
    if (!tickers.has_data()) return;

    for (const auto& [key, meta] : reg.all()) {
        const auto* t = tickers.get(meta.exchange, meta.symbol);
        if (!t || t->last_price <= 0.0) continue;
        sparks_[meta.pair_key].push(static_cast<float>(t->last_price));
    }
}

// ── Per-category pair counts (built once with the category list) ─────────────
void WatchlistWidget::build_category_counts() {
    auto& reg = SymbolRegistry::instance();
    category_counts_.assign(categories_.size(), 0);
    exchange_counts_.fill(0);
    all_count_ = 0;
    scoped_count_ = 0;
    for (const auto& [key, meta] : reg.all()) {
        if (!meta.is_active) continue;
        ++all_count_;
        for (size_t i = 0; i < exchange_counts_.size(); ++i) {
            if (meta.exchange == kWatchlistVenues[i].id) {
                ++exchange_counts_[i];
                break;
            }
        }
        if (!selected_exchange_.empty() && meta.exchange != selected_exchange_) continue;
        ++scoped_count_;
        for (const auto& c : meta.categories) {
            for (size_t i = 0; i < categories_.size(); ++i) {
                if (categories_[i] == c) { ++category_counts_[i]; break; }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Visible-list cache - filter (search + category) then sort.
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::rebuild_visible() {
    auto& reg = SymbolRegistry::instance();
    auto& tickers = TickerManager::instance();

    visible_.clear();
    const char* needle = search_buf_;
    const std::string* cat = (selected_category_ >= 0 &&
                              selected_category_ < static_cast<int>(categories_.size()))
                             ? &categories_[selected_category_] : nullptr;

    for (const auto& [key, meta] : reg.all()) {
        if (!meta.is_active) continue;
        if (!selected_exchange_.empty() && meta.exchange != selected_exchange_) continue;
        if (cat) {
            bool has = false;
            for (const auto& c : meta.categories) if (c == *cat) { has = true; break; }
            if (!has) continue;
        }
        if (needle[0] && !contains_ci(meta.symbol, needle) &&
            !contains_ci(meta.base_asset, needle)) continue;
        visible_.push_back(&meta);
    }

    auto& tm = tickers;
    const bool desc = sort_desc_;
    auto sort_by = [&](auto valfn) {
        std::sort(visible_.begin(), visible_.end(),
            [&](const SymbolMetadata* a, const SymbolMetadata* b) {
                const double va = valfn(a), vb = valfn(b);
                return desc ? (va > vb) : (va < vb);
            });
    };
    switch (sort_) {
        case Sort::Symbol:
            std::sort(visible_.begin(), visible_.end(),
                [desc](const SymbolMetadata* a, const SymbolMetadata* b) {
                    return desc ? (a->symbol > b->symbol) : (a->symbol < b->symbol);
                });
            break;
        case Sort::Change:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->change_pct_24h:-1e9; });
            break;
        case Sort::Volume:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->volume_quote:0.0; });
            break;
        case Sort::Price:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->last_price:0.0; });
            break;
    }

    strncpy(prev_search_, search_buf_, sizeof(prev_search_));
    prev_category_ = selected_category_;
    prev_exchange_ = selected_exchange_;
    prev_sort_ = sort_;
    prev_sort_desc_ = sort_desc_;
    prev_ticker_ms_ = tickers.last_update_ms();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render - header stack (title · filter · category · sort) then the scroll list
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::render() {
    if (!is_open) return;

    // One clean panel: suppress the single-tab dock tab bar so our custom title
    // bar (live dot · WATCHLIST · count · close) reads as the panel header.
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin("Watchlist", &is_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    was_visible_last_frame_ = visible;
    if (!visible) { ImGui::End(); return; }

    auto& reg = SymbolRegistry::instance();
    if (!reg.is_loaded()) {
        ImGui::TextColored(Theme::Tokens::TX3, "  Loading symbols\xE2\x80\xA6");
        ImGui::End();
        return;
    }
    if (!categories_loaded_) {
        categories_ = reg.unique_categories();
        build_category_counts();
        categories_loaded_ = true;
    }

    render_title_bar();
    render_filter_bar();
    render_category_selector();
    render_sort_header();

    // Rebuild on any input change immediately; live ticker drift re-sorts at most
    // every 5s (re-sorting ~680 symbols is the costly part, not the filter).
    const int64_t now = now_ms();
    const bool inputs_changed =
        strcmp(prev_search_, search_buf_) != 0 ||
        prev_category_ != selected_category_ ||
        prev_exchange_ != selected_exchange_ ||
        prev_sort_ != sort_ || prev_sort_desc_ != sort_desc_;
    const bool ticker_drift =
        (sort_ != Sort::Symbol) && (now - last_resort_ms_ > 5000) &&
        (prev_ticker_ms_ != TickerManager::instance().last_update_ms());
    if (inputs_changed || ticker_drift || visible_.empty()) {
        rebuild_visible();
        last_resort_ms_ = now;
    }

    render_rows();
    ImGui::End();
}

// ── Title bar (h28) - live dot · WATCHLIST · [compact][count][close] ─────────

void WatchlistWidget::render_title_bar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 28.0f;
    const float cy = p0.y + h * 0.5f;

    dl->AddLine(ImVec2(p0.x, p0.y + h - 1.0f), ImVec2(p0.x + w, p0.y + h - 1.0f),
                Theme::u32(Theme::Tokens::BD1));                 // bottom hairline

    dl->AddRectFilled(ImVec2(p0.x + 10.0f, cy - 3.0f),
                      ImVec2(p0.x + 16.0f, cy + 3.0f),
                      Theme::u32(Theme::Tokens::UP));            // live dot 6x6

    ImGui::PushFont(Theme::Fonts::label());
    dl->AddText(ImVec2(p0.x + 23.0f, cy - ImGui::GetFontSize() * 0.5f),
                Theme::u32(Theme::Tokens::TX2), "WATCHLIST");

    const ImU32 tx3 = Theme::u32(Theme::Tokens::TX3);
    float x = p0.x + w - 10.0f;

    // close glyph (x)
    const float k = 4.5f;
    dl->AddLine(ImVec2(x - 9.0f, cy - k), ImVec2(x, cy + k), tx3, 1.3f);
    dl->AddLine(ImVec2(x - 9.0f, cy + k), ImVec2(x, cy - k), tx3, 1.3f);
    const float close_x0 = x - 9.0f;
    x = close_x0 - 9.0f;

    // pair count "N PAIRS"
    char cnt[24]; snprintf(cnt, sizeof(cnt), "%d PAIRS", all_count_);
    const float cnt_w = ImGui::CalcTextSize(cnt).x;
    dl->AddText(ImVec2(x - cnt_w, cy - ImGui::GetFontSize() * 0.5f), tx3, cnt);
    x -= cnt_w + 9.0f;
    ImGui::PopFont();

    // compact toggle - 3 stacked ticks (accent when on)
    const float gx0 = x - 12.0f, gy = cy - 4.0f;
    const ImU32 gc = Theme::u32(compact_ ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX3);
    for (int i = 0; i < 3; ++i)
        dl->AddLine(ImVec2(gx0, gy + i * 4.0f), ImVec2(gx0 + 12.0f, gy + i * 4.0f), gc, 1.3f);

    // hit targets
    ImGui::SetCursorScreenPos(ImVec2(close_x0 - 3.0f, cy - 8.0f));
    if (ImGui::InvisibleButton("##wl_close", ImVec2(15.0f, 16.0f))) is_open = false;
    ImGui::SetCursorScreenPos(ImVec2(gx0 - 2.0f, cy - 8.0f));
    if (ImGui::InvisibleButton("##wl_compact", ImVec2(16.0f, 16.0f))) compact_ = !compact_;
    if (ImGui::IsItemHovered()) Theme::tooltip(compact_ ? "Dense rows: on" : "Compact rows");

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
}

// ── Filter row (h28 controls) - "Filter pairs" input + Vol volume-sort toggle ─

void WatchlistWidget::render_filter_bar() {
    const float pad = 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    // Full-width filter input; volume sorting lives in the sort-header chips.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pad);
    ImGui::InputTextWithHint("##wl_search", "Filter pairs", search_buf_, sizeof(search_buf_));

    ImGui::PopStyleVar(3);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    const float hy = ImGui::GetCursorScreenPos().y;
    dl->AddLine(ImVec2(bp.x, hy), ImVec2(bp.x + ww, hy), Theme::u32(Theme::Tokens::BD1));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
}

// ── Category + venue selectors (h28) with compact popovers ───────────────────────

void WatchlistWidget::render_category_selector() {
    const float pad = 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float h = 28.0f;

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 8.0f));
    const float w = ImGui::GetContentRegionAvail().x - pad;   // inset both sides
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float selector_gap = 6.0f;
    const float venue_w = std::clamp(w * 0.39f, 92.0f, 108.0f);
    const float category_w = w - selector_gap - venue_w;
    const ImVec2 venue_p(p0.x + category_w + selector_gap, p0.y);

    ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_Border,        Theme::Tokens::BD2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    if (ImGui::Button("##wl_cat", ImVec2(category_w, h))) ImGui::OpenPopup("##wl_cat_pop");
    ImGui::SetCursorScreenPos(venue_p);
    if (ImGui::Button("##wl_venue", ImVec2(venue_w, h))) ImGui::OpenPopup("##wl_venue_pop");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // Category overlay: icon, clipped label, right-aligned count, and caret.
    // The reserved count column prevents narrow-rail label collisions.
    const float ix = p0.x + 10.0f, iy = p0.y + h * 0.5f - 6.0f;
    const ImU32 ac = Theme::u32(Theme::Tokens::BRAND_TX);
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 2; ++c)
        dl->AddRectFilled(ImVec2(ix + c*7.0f, iy + r*7.0f),
                          ImVec2(ix + c*7.0f + 5.0f, iy + r*7.0f + 5.0f), ac);

    const bool category_valid = selected_category_ >= 0 &&
                                selected_category_ < static_cast<int>(categories_.size());
    const char* name = category_valid ? categories_[selected_category_].c_str() : "All";
    const int ct = category_valid && selected_category_ < static_cast<int>(category_counts_.size())
                   ? category_counts_[selected_category_] : scoped_count_;
    char cbuf[16]; snprintf(cbuf, sizeof(cbuf), "%d", ct);
    ImGui::PushFont(Theme::Fonts::label());
    const float count_w = ImGui::CalcTextSize(cbuf).x;
    const float count_x = p0.x + category_w - 27.0f - count_w;
    ImGui::PopFont();
    ImGui::PushFont(Theme::Fonts::ui_semibold());
    dl->PushClipRect(ImVec2(p0.x + 31.0f, p0.y),
                     ImVec2(std::max(p0.x + 32.0f, count_x - 6.0f), p0.y + h), true);
    dl->AddText(ImVec2(p0.x + 31.0f, p0.y + h*0.5f - ImGui::GetFontSize()*0.5f),
                Theme::u32(Theme::Tokens::TX1), name);
    dl->PopClipRect();
    ImGui::PopFont();
    ImGui::PushFont(Theme::Fonts::label());
    dl->AddText(ImVec2(count_x, p0.y + h*0.5f - ImGui::GetFontSize()*0.5f),
                Theme::u32(Theme::Tokens::TX3), cbuf);
    ImGui::PopFont();

    const float qx = p0.x + category_w - 15.0f, qy = p0.y + h*0.5f;
    dl->AddTriangleFilled(ImVec2(qx, qy-2.0f), ImVec2(qx+7.0f, qy-2.0f),
                          ImVec2(qx+3.5f, qy+3.0f), Theme::u32(Theme::Tokens::TX3));

    // Venue overlay. A compact exchange mark is useful here; the top-level
    // symbol selector remains coin-only as requested.
    const WatchlistVenue* selected_venue = nullptr;
    for (const auto& venue : kWatchlistVenues) {
        if (selected_exchange_ == venue.id) { selected_venue = &venue; break; }
    }
    const char* venue_label = selected_venue ? selected_venue->compact_label : "ALL VENUES";
    float venue_text_x = venue_p.x + 8.0f;
    if (selected_venue) {
        constexpr float logo_size = 13.0f;
        LogoManager::instance().draw_exchange(dl, selected_venue->id,
            ImVec2(venue_p.x + 8.0f, venue_p.y + (h - logo_size) * 0.5f), logo_size);
        venue_text_x += logo_size + 5.0f;
    } else {
        const float gx = venue_p.x + 9.0f, gy = venue_p.y + h * 0.5f - 4.0f;
        for (int i = 0; i < 3; ++i)
            dl->AddLine(ImVec2(gx, gy + i * 4.0f), ImVec2(gx + 10.0f, gy + i * 4.0f),
                        Theme::u32(Theme::Tokens::TX3), 1.0f);
        venue_text_x += 16.0f;
    }
    ImGui::PushFont(Theme::Fonts::label());
    dl->PushClipRect(ImVec2(venue_text_x, venue_p.y),
                     ImVec2(venue_p.x + venue_w - 17.0f, venue_p.y + h), true);
    dl->AddText(ImVec2(venue_text_x, venue_p.y + h*0.5f - ImGui::GetFontSize()*0.5f),
                Theme::u32(Theme::Tokens::TX2), venue_label);
    dl->PopClipRect();
    ImGui::PopFont();
    const float vqx = venue_p.x + venue_w - 14.0f;
    dl->AddTriangleFilled(ImVec2(vqx, qy-2.0f), ImVec2(vqx+6.0f, qy-2.0f),
                          ImVec2(vqx+3.0f, qy+2.5f), Theme::u32(Theme::Tokens::TX3));

    // taxonomy popover - searchable 2-column grid of name+count chips
    // It spans the full rail and is opaque so the selector underneath cannot
    // leak into the first All / AI row at the popup margins.
    ImGui::SetNextWindowPos(ImVec2(bp.x, p0.y + h + 4.0f));
    ImGui::SetNextWindowSize(ImVec2(ww, 0.0f));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border,  Theme::Tokens::BD2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::BeginPopup("##wl_cat_pop")) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##wl_cat_search", "Search categories", cat_search_, sizeof(cat_search_));
        ImGui::PopStyleVar(2);
        ImGui::Dummy(ImVec2(0, 3.0f));

        ImGui::BeginChild("##wl_cat_grid", ImVec2(0, 224.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 5.0f));
        // Reserve the always-present vertical scrollbar (~19 categories overflow the
        // 224px child) BEFORE splitting into two columns. Without this, cw is sized
        // to the full width and the right chip collides with the scrollbar / its
        // neighbour on the frame the scrollbar first appears.
        constexpr float chip_gap = 7.0f;
        const float cw = (ImGui::GetContentRegionAvail().x
                          - ImGui::GetStyle().ScrollbarSize - chip_gap) * 0.5f;
        int col = 0;
        auto chip = [&](const char* label, int count, bool active, int catidx) {
            if (col == 1) ImGui::SameLine(0.0f, chip_gap);
            ImGui::PushID(catidx + 2);
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            const float ch = 28.0f;
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const bool clicked = ImGui::InvisibleButton("##c", ImVec2(cw, ch));
            const bool hov = ImGui::IsItemHovered();
            cdl->AddRectFilled(cp, ImVec2(cp.x+cw, cp.y+ch),
                Theme::u32(active ? Theme::Tokens::BRAND_SOFT
                                  : (hov ? Theme::Tokens::ELEV : Theme::Tokens::PANEL)), Theme::Radius::R2);
            cdl->AddRect(cp, ImVec2(cp.x+cw, cp.y+ch),
                Theme::u32(active ? Theme::Tokens::BRAND : Theme::Tokens::BD2), Theme::Radius::R2);
            ImGui::PushFont(Theme::Fonts::label());
            char nb[16]; snprintf(nb, sizeof(nb), "%d", count);
            const float nbw = ImGui::CalcTextSize(nb).x;
            cdl->PushClipRect(ImVec2(cp.x + 9.0f, cp.y),
                              ImVec2(cp.x + cw - 17.0f - nbw, cp.y + ch), true);
            cdl->AddText(ImVec2(cp.x+9.0f, cp.y+ch*0.5f-ImGui::GetFontSize()*0.5f),
                Theme::u32(active ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2), label);
            cdl->PopClipRect();
            cdl->AddText(ImVec2(cp.x+cw-9.0f-nbw, cp.y+ch*0.5f-ImGui::GetFontSize()*0.5f),
                Theme::u32(Theme::Tokens::TX3), nb);
            ImGui::PopFont();
            ImGui::PopID();
            if (clicked) { selected_category_ = catidx; ImGui::CloseCurrentPopup(); }
            col ^= 1;
        };
        if (!cat_search_[0] || contains_ci(std::string("All"), cat_search_))
            chip("All", scoped_count_, selected_category_ < 0, -1);
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            if (cat_search_[0] && !contains_ci(categories_[i], cat_search_)) continue;
            chip(categories_[i].c_str(), category_counts_[i], selected_category_ == i, i);
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Venue popover - a compact three-row filter, defaulting to All venues.
    const float venue_popup_w = std::min(160.0f, ww);
    ImGui::SetNextWindowPos(ImVec2(bp.x + ww - venue_popup_w, p0.y + h + 4.0f));
    ImGui::SetNextWindowSize(ImVec2(venue_popup_w, 0.0f));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border,  Theme::Tokens::BD2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::BeginPopup("##wl_venue_pop")) {
        ImDrawList* venue_dl = ImGui::GetWindowDrawList();
        auto venue_row = [&](const char* id, const char* label, int count, int row_id) {
            const bool active = selected_exchange_ == id;
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const float rw = ImGui::GetContentRegionAvail().x;
            const float rh = 27.0f;
            ImGui::PushID(row_id);
            const bool clicked = ImGui::InvisibleButton("##venue_row", ImVec2(rw, rh));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            venue_dl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rh),
                Theme::u32(active ? Theme::Tokens::BRAND_SOFT
                                  : (hovered ? Theme::Tokens::ELEV : Theme::Tokens::PANEL)),
                Theme::Radius::R2);
            if (active)
                venue_dl->AddRectFilled(rp, ImVec2(rp.x + 2.0f, rp.y + rh),
                                        Theme::u32(Theme::Tokens::BRAND), Theme::Radius::R2);

            float tx = rp.x + 8.0f;
            if (id[0]) {
                constexpr float logo_size = 13.0f;
                LogoManager::instance().draw_exchange(venue_dl, id,
                    ImVec2(tx, rp.y + (rh - logo_size) * 0.5f), logo_size);
                tx += logo_size + 6.0f;
            }
            ImGui::PushFont(Theme::Fonts::label());
            char nb[16]; snprintf(nb, sizeof(nb), "%d", count);
            const float nbw = ImGui::CalcTextSize(nb).x;
            venue_dl->PushClipRect(ImVec2(tx, rp.y), ImVec2(rp.x + rw - nbw - 15.0f, rp.y + rh), true);
            venue_dl->AddText(ImVec2(tx, rp.y + (rh - ImGui::GetFontSize()) * 0.5f),
                              Theme::u32(active ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2), label);
            venue_dl->PopClipRect();
            venue_dl->AddText(ImVec2(rp.x + rw - 8.0f - nbw, rp.y + (rh - ImGui::GetFontSize()) * 0.5f),
                              Theme::u32(Theme::Tokens::TX3), nb);
            ImGui::PopFont();

            if (clicked) {
                selected_exchange_ = id;
                selected_category_ = -1;
                cat_search_[0] = '\0';
                build_category_counts();
                ImGui::CloseCurrentPopup();
            }
        };
        venue_row("", "ALL VENUES", all_count_, 0);
        for (size_t i = 0; i < exchange_counts_.size(); ++i)
            venue_row(kWatchlistVenues[i].id, kWatchlistVenues[i].label,
                      exchange_counts_[i], static_cast<int>(i) + 1);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // advance band (8 top + 28 + 8 bottom) + bottom hairline
    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(ww, 8.0f + h + 8.0f));
    const float hy = bp.y + 8.0f + h + 8.0f - 1.0f;
    dl->AddLine(ImVec2(bp.x, hy), ImVec2(bp.x + ww, hy), Theme::u32(Theme::Tokens::BD1));
}

// ── Sort header - SYMBOL(+total) left, LAST / 24H% right; active chip arrow ───

void WatchlistWidget::render_sort_header() {
    const float pad = 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float ph = 18.0f;
    const float cy = bp.y + 7.0f + ph * 0.5f;

    ImGui::PushFont(Theme::Fonts::label());

    auto pill_w = [&](const char* label, int count, bool /*active*/) {
        float lw = ImGui::CalcTextSize(label).x, nbw = 0.0f;
        if (count >= 0) { char nb[12]; snprintf(nb,sizeof(nb),"%d",count); nbw = 4.0f + ImGui::CalcTextSize(nb).x; }
        return 9.0f + lw + nbw + 12.0f + 9.0f;   // always reserve the sort chevron
    };
    auto draw_chip = [&](const char* label, int count, Sort col, float x_left) {
        const bool active = (sort_ == col);
        const float pw = pill_w(label, count, active);
        const ImVec2 cp(x_left, cy - ph * 0.5f);
        // Active sort is shown by the teal arrow only (SPEC); the chip body stays
        // neutral like the rest - active just brightens the label to text-1.
        dl->AddRectFilled(cp, ImVec2(cp.x+pw, cp.y+ph), Theme::u32(Theme::Tokens::PANEL), ph * 0.5f);
        dl->AddRect(cp, ImVec2(cp.x+pw, cp.y+ph), Theme::u32(Theme::Tokens::BD2), ph * 0.5f);
        const float lw = ImGui::CalcTextSize(label).x;
        dl->AddText(ImVec2(cp.x+9.0f, cy - ImGui::GetFontSize()*0.5f),
            Theme::u32(active ? Theme::Tokens::TX1 : Theme::Tokens::TX2), label);
        if (count >= 0) {
            char nb[12]; snprintf(nb,sizeof(nb),"%d",count);
            dl->AddText(ImVec2(cp.x+9.0f+lw+4.0f, cy - ImGui::GetFontSize()*0.5f),
                Theme::u32(Theme::Tokens::TX3), nb);
        }
        // Binance-style sort chevron on every chip: up + down triangles. The
        // active direction is teal (accent), the other dim; both dim when this
        // column is not the active sort.
        const float chx = cp.x + pw - 9.0f - 7.0f;
        const ImU32 hot = Theme::u32(Theme::Tokens::TX1);   // active direction: white
        const ImU32 dim = Theme::u32(Theme::Tokens::TX3);
        const ImU32 up_c = (active && !sort_desc_) ? hot : dim;
        const ImU32 dn_c = (active &&  sort_desc_) ? hot : dim;
        dl->AddTriangleFilled(ImVec2(chx, cy - 1.5f), ImVec2(chx + 7.0f, cy - 1.5f),
                              ImVec2(chx + 3.5f, cy - 5.5f), up_c);
        dl->AddTriangleFilled(ImVec2(chx, cy + 1.5f), ImVec2(chx + 7.0f, cy + 1.5f),
                              ImVec2(chx + 3.5f, cy + 5.5f), dn_c);
        ImGui::SetCursorScreenPos(cp);
        ImGui::PushID(static_cast<int>(col) + 40);
        if (ImGui::InvisibleButton("##sc", ImVec2(pw, ph))) {
            if (sort_ == col) sort_desc_ = !sort_desc_;
            else { sort_ = col; sort_desc_ = (col != Sort::Symbol); }
        }
        ImGui::PopID();
    };

    // Left group: SYMBOL (+ total) then VOL, same chip style as the rest.
    const int   sym_ct = static_cast<int>(visible_.size());
    const float w_sym  = pill_w("SYMBOL", sym_ct, sort_ == Sort::Symbol);
    draw_chip("SYMBOL", sym_ct, Sort::Symbol, bp.x + pad);
    draw_chip("VOL",    -1,     Sort::Volume, bp.x + pad + w_sym + 6.0f);

    // Right group: LAST then 24H%, pinned to the right edge.
    const float w_chg  = pill_w("24H%", -1, sort_ == Sort::Change);
    const float w_last = pill_w("LAST", -1, sort_ == Sort::Price);
    const float x_chg  = bp.x + ww - pad - w_chg;
    draw_chip("LAST", -1, Sort::Price,  x_chg - 8.0f - w_last);
    draw_chip("24H%", -1, Sort::Change, x_chg);

    ImGui::PopFont();

    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(ww, 7.0f + ph + 7.0f));
    const float hy = bp.y + 7.0f + ph + 7.0f - 1.0f;
    dl->AddLine(ImVec2(bp.x, hy), ImVec2(bp.x + ww, hy), Theme::u32(Theme::Tokens::BD1));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Rows - star · sparkline · symbol (ellipsized) · last · 24h% + a dim 24h-volume
// figure. A compact toggle (or a narrow rail, < 300px) drops the sparkline.
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::render_rows() {
    auto& tickers = TickerManager::instance();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##wl_list", ImVec2(0, 0), false);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;

    const float pad_l = 8.0f, pad_r = 10.0f, gap = 8.0f;
    // These four move as one set. The non-symbol chrome below adds up to a
    // fixed ~215px regardless of panel width, so at the old default rail the
    // symbol column got about 30 to 41px and every row rendered nameless with
    // LAST and 24H% pushed off-panel. Widening the rail alone would not have
    // fixed it: LAST and 24H% were also too narrow for a real price and a
    // signed percentage, and the condense threshold sat below the new default,
    // so the rail would have opened already condensed.
    const float star_w = 15.0f, spark_w = 52.0f, last_w = 92.0f, chg_w = 88.0f;
    const bool  compact = compact_ || w < 410.0f;
    const bool  show_spark = !compact;
    const float content_h = compact ? 22.0f : 27.0f;
    const float vol_h = 12.0f;                      // dim 24h-volume figure line
    const float pitch = content_h + vol_h + 1.0f;   // content + volume + 1px hairline

    const float chg_right  = w - pad_r;
    const float last_right = chg_right - chg_w - gap;
    const float last_left  = last_right - last_w;
    const float sym_left   = show_spark ? (pad_l + star_w + gap + spark_w + gap)
                                        : (pad_l + star_w + gap);
    const float sym_right  = last_left - gap;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible_.size()), pitch);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto* meta = visible_[i];
            const auto* t = tickers.get(meta->exchange, meta->symbol);
            const bool selected = (meta->exchange == active_pair_.exchange &&
                                   meta->symbol == active_pair_.symbol);
            const bool fav = favorites_.find(meta->pair_key) != favorites_.end();
            ImGui::PushID(i);

            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, pitch));
            const bool hovered = ImGui::IsItemHovered();
            const float cy_line = row_min.y + content_h * 0.5f;

            if (selected) {
                dl->AddRectFilled(row_min, ImVec2(row_min.x + w, row_min.y + pitch - 1.0f),
                                  Theme::u32(Theme::Tokens::BRAND_SOFT));
                dl->AddRectFilled(row_min, ImVec2(row_min.x + 2.0f, row_min.y + pitch - 1.0f),
                                  Theme::u32(Theme::Tokens::BRAND));
            } else if (hovered) {
                dl->AddRectFilled(row_min, ImVec2(row_min.x + w, row_min.y + pitch - 1.0f),
                                  Theme::u32(Theme::Tokens::ELEV));
            }

            if (clicked) {
                const float mx = ImGui::GetIO().MousePos.x;
                if (mx <= row_min.x + pad_l + star_w + 4.0f) {   // star cell → toggle favourite
                    auto it = favorites_.find(meta->pair_key);
                    if (it == favorites_.end()) favorites_.insert(meta->pair_key);
                    else favorites_.erase(it);
                } else if (!selected) {
#ifdef __EMSCRIPTEN__
                    // url_navigate carries ?ws= (and any other user params)
                    // across the reload; a bare location.href would drop a
                    // self-hosted feed here.
                    url_navigate(build_terminal_path(meta->exchange, meta->symbol));
#endif
                }
            }

            const ImVec4 chg_col = (t && t->change_pct_24h < 0.0)
                                   ? Theme::Tokens::DOWN : Theme::Tokens::UP;

            draw_star(dl, ImVec2(row_min.x + pad_l + star_w * 0.5f, cy_line), 5.0f, fav,
                      Theme::u32(fav ? Theme::Tokens::TX1 : Theme::Tokens::TX3));

            if (show_spark) {
                auto sit = sparks_.find(meta->pair_key);
                if (sit != sparks_.end() && sit->second.count >= 2)
                    draw_sparkline(sit->second,
                        ImVec2(row_min.x + pad_l + star_w + gap, cy_line - 7.0f),
                        spark_w, 14.0f, chg_col);
            }

            // symbol - base (bright) + /quote (dim), ellipsized to its column so
            // long tickers (SKHYNIX/USDT, 1000PEPE/USDT) never reach the price cols
            {
                const char* base = meta->base_asset.empty() ? meta->symbol.c_str()
                                                             : meta->base_asset.c_str();
                char quote[16] = {};
                if (!meta->base_asset.empty())
                    snprintf(quote, sizeof(quote), "/%s", meta->quote_asset.c_str());
                // coin logo immediately left of the symbol (monogram fallback)
                const float logo_w = 16.0f;
                LogoManager::instance().draw_coin(dl, meta->base_asset,
                    ImVec2(row_min.x + sym_left, cy_line - logo_w * 0.5f), logo_w);
                if (selected_exchange_.empty()) {
                    // When venues are mixed, badge the coin rather than spending
                    // another crowded text column on exchange identity.
                    ImTextureID exchange_logo = LogoManager::instance().exchange(meta->exchange);
                    if (exchange_logo) {
                        constexpr float badge_w = 8.0f;
                        const ImVec2 badge(row_min.x + sym_left + logo_w - badge_w + 1.0f,
                                           cy_line + logo_w * 0.5f - badge_w + 1.0f);
                        dl->AddCircleFilled(ImVec2(badge.x + badge_w * 0.5f,
                                                   badge.y + badge_w * 0.5f),
                                            badge_w * 0.65f, Theme::u32(Theme::Tokens::PANEL));
                        dl->AddImage(exchange_logo, badge,
                                     ImVec2(badge.x + badge_w, badge.y + badge_w));
                    }
                }
                const float sxl   = row_min.x + sym_left + logo_w + 6.0f;
                const float avail = sym_right - sym_left - logo_w - 6.0f;
                const char* ell   = "\xE2\x80\xA6";   // horizontal ellipsis

                ImGui::PushFont(Theme::Fonts::ui_semibold());
                const float base_w = ImGui::CalcTextSize(base).x;
                const float ell_w  = ImGui::CalcTextSize(ell).x;
                const float by     = cy_line - ImGui::GetFontSize() * 0.5f;
                if (base_w <= avail) {
                    dl->AddText(ImVec2(sxl, by), Theme::u32(Theme::Tokens::TX1), base);
                    ImGui::PopFont();
                    if (quote[0]) {
                        ImGui::PushFont(Theme::Fonts::ui());
                        const float qy = cy_line - ImGui::GetFontSize() * 0.5f;
                        const float qw = ImGui::CalcTextSize(quote).x;
                        dl->AddText(ImVec2(sxl + base_w + 1.0f, qy), Theme::u32(Theme::Tokens::TX3),
                                    (base_w + qw <= avail) ? quote : ell);
                        ImGui::PopFont();
                    }
                } else {
                    int k = static_cast<int>(strlen(base));
                    while (k > 1 && ImGui::CalcTextSize(base, base + k).x + ell_w > avail) --k;
                    dl->AddText(ImVec2(sxl, by), Theme::u32(Theme::Tokens::TX1), base, base + k);
                    dl->AddText(ImVec2(sxl + ImGui::CalcTextSize(base, base + k).x, by),
                                Theme::u32(Theme::Tokens::TX1), ell);
                    ImGui::PopFont();
                }
            }

            // last · 24h% - mono, right-aligned in their fixed columns
            ImGui::PushFont(Theme::Fonts::mono());
            const float ly = cy_line - ImGui::GetFontSize() * 0.5f;
            if (t && t->last_price > 0.0) {
                char pbuf[32];
                meta->fmt.format_price(pbuf, sizeof(pbuf), t->last_price);
                dl->AddText(ImVec2(row_min.x + last_right - ImGui::CalcTextSize(pbuf).x, ly),
                            Theme::u32(Theme::Tokens::TX1), pbuf);
            }
            if (t) {
                char cbuf[16];
                snprintf(cbuf, sizeof(cbuf), "%+.2f%%", t->change_pct_24h);
                dl->AddText(ImVec2(row_min.x + chg_right - ImGui::CalcTextSize(cbuf).x, ly),
                            Theme::u32(chg_col), cbuf);
            }
            ImGui::PopFont();

            // 24h volume - compact grey figure left-aligned directly under the
            // symbol (Binance-style); never interrupts the 24h% read on the right.
            if (t && t->volume_quote > 0.0) {
                char vbuf[16];
                fmt_volume(vbuf, sizeof(vbuf), t->volume_quote);
                ImGui::PushFont(Theme::Fonts::mono_sm());
                const float vy = row_min.y + content_h + (vol_h - ImGui::GetFontSize()) * 0.5f;
                dl->AddText(ImVec2(row_min.x + sym_left, vy),
                            Theme::u32(Theme::Tokens::TX3), vbuf);
                ImGui::PopFont();
            }

            dl->AddLine(ImVec2(row_min.x, row_min.y + pitch - 1.0f),
                        ImVec2(row_min.x + w, row_min.y + pitch - 1.0f),
                        Theme::u32(Theme::Tokens::BD1));

            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::PopStyleVar();   // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleVar();   // WindowPadding
}

// ── Sparkline - polyline of ring-buffer samples, min/max normalized ──────────

void WatchlistWidget::draw_sparkline(const Spark& s, ImVec2 p0, float w, float h,
                                     const ImVec4& col) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < s.count; ++i) {
        const float v = s.v[(s.head - s.count + i + SPARK_N * 2) % SPARK_N];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float range = (hi - lo) > 1e-12f ? (hi - lo) : 1.0f;

    ImVec2 prev{};
    for (int i = 0; i < s.count; ++i) {
        const float v = s.v[(s.head - s.count + i + SPARK_N * 2) % SPARK_N];
        const float x = p0.x + (s.count > 1 ? (w * i / (s.count - 1)) : 0.0f);
        const float y = p0.y + h - ((v - lo) / range) * h;
        const ImVec2 pt(x, y);
        if (i > 0) dl->AddLine(prev, pt, Theme::u32(col, 0.9f), 1.25f);
        prev = pt;
    }
}
