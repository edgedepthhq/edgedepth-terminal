#include "rendering/menu.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "ui/widget.h"
#include "ui/trades_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/dom_widget.h"
#include "ui/stats_widget.h"
#include "ui/debug_widget.h"
#include "ui/positions_panel.h"
#include "stream_handler.h"
#include "types/types.h"
#include "imgui.h"
#include "ui/chart_widget.h"
#include "core/app_context.h"
#include "core/symbol_metadata.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "core/scanner_manager.h"
#include <cctype>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Menu {

namespace {
    PriceFormatter fmt_for(const SymbolMetadata& meta) { return meta.fmt; }
    double tick_for(const SymbolMetadata& meta) { return meta.tick_size; }
    Terminal::Pair pair_for(const SymbolMetadata& meta) {
        return Terminal::Pair{meta.exchange, meta.symbol};
    }

    bool matches_search(const std::string& haystack, const char* needle) {
        if (needle[0] == '\0') return true;
        std::string lower_hay, lower_needle;
        for (char c : haystack) lower_hay += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const char* c = needle; *c; ++c)
            lower_needle += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
        return lower_hay.find(lower_needle) != std::string::npos;
    }

    bool matches_category(const SymbolMetadata& meta, int cat_idx, const std::vector<std::string>& cats) {
        if (cat_idx < 0) return true;
        if (cat_idx >= static_cast<int>(cats.size())) return true;
        const auto& target = cats[cat_idx];
        for (const auto& c : meta.categories) {
            if (c == target) return true;
        }
        return false;
    }

    void format_volume(char* buf, size_t buf_size, double vol) {
        if (vol >= 1e9)      snprintf(buf, buf_size, "%.2fB", vol / 1e9);
        else if (vol >= 1e6) snprintf(buf, buf_size, "%.2fM", vol / 1e6);
        else if (vol >= 1e3) snprintf(buf, buf_size, "%.1fK", vol / 1e3);
        else                 snprintf(buf, buf_size, "%.0f", vol);
    }

    // ── Recents / favorites persistence (comma-joined symbol lists in localStorage) ──
    constexpr const char* LS_RECENTS = "edgedepth.picker.recents";
    constexpr const char* LS_FAVS    = "edgedepth.picker.favorites";

    std::string ls_get(const char* key) {
#ifdef __EMSCRIPTEN__
        char buf[2048];
        buf[0] = '\0';
        EM_ASM({
            var v = localStorage.getItem(UTF8ToString($0)) || "";
            stringToUTF8(v, $1, $2);
        }, key, buf, (int)sizeof(buf));
        return std::string(buf);
#else
        (void)key;
        return std::string();
#endif
    }
    void ls_set(const char* key, const std::string& val) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ localStorage.setItem(UTF8ToString($0), UTF8ToString($1)); }, key, val.c_str());
#else
        (void)key; (void)val;
#endif
    }
    std::vector<std::string> split_csv(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    std::string join_csv(const std::vector<std::string>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ','; out += v[i]; }
        return out;
    }
    void persist_recents() { ls_set(LS_RECENTS, join_csv(g_symbol_picker.recents)); }
    void persist_favorites() {
        std::vector<std::string> v(g_symbol_picker.favorites.begin(), g_symbol_picker.favorites.end());
        ls_set(LS_FAVS, join_csv(v));
    }
    void load_persistence() {
        g_symbol_picker.recents = split_csv(ls_get(LS_RECENTS));
        if (g_symbol_picker.recents.size() > 8) g_symbol_picker.recents.resize(8);
        auto favs = split_csv(ls_get(LS_FAVS));
        g_symbol_picker.favorites.clear();
        g_symbol_picker.favorites.insert(favs.begin(), favs.end());
    }

}

// ═══════════════════════════════════════════════════════════════════════════════
// Symbol Picker Popup
// ═══════════════════════════════════════════════════════════════════════════════

void render_symbol_picker_popup(
    std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx)
{
    // ── Subscribe/unsubscribe to ticker24h on picker open/close ──────
    static bool was_open = false;
    static bool ticker_subscribed = false;
    static bool scanner_subscribed = false;
    if (g_symbol_picker.open && !was_open) {
        // Picker just opened - subscribe to scanner for BOTH venues (the feeds are
        // venue-parameterized: scanner.{binancef,hl}.global; ScannerManager keys by
        // exchange). ticker24h is always-on, owned by AppShell - don't manage it here.
        if (!scanner_subscribed) {
            for (const char* ex : {"binancef", "hl"}) {
                StreamKey scanner_key{
                    Terminal::Pair{ex, "global"},
                    Terminal::Stream::Scanner,
                    0
                };
                ctx.stream_mgr().send_subscribe(scanner_key);
            }
            scanner_subscribed = true;
        }
        g_symbol_picker.cached_categories = SymbolRegistry::instance().unique_categories();
        g_symbol_picker.selected_category = -1;
        g_symbol_picker.invalidate_cache();

        if (!g_symbol_picker.persistence_loaded) {
            load_persistence();
            g_symbol_picker.persistence_loaded = true;
        }
        // Per-category active-symbol counts, parallel to cached_categories.
        {
            auto& cc = g_symbol_picker.cached_category_counts;
            cc.assign(g_symbol_picker.cached_categories.size(), 0);
            for (const auto& [k, m] : SymbolRegistry::instance().all()) {
                if (!m.is_active) continue;
                for (const auto& c : m.categories) {
                    for (size_t ci = 0; ci < g_symbol_picker.cached_categories.size(); ++ci) {
                        if (g_symbol_picker.cached_categories[ci] == c) { cc[ci]++; break; }
                    }
                }
            }
        }
        g_symbol_picker.keyboard_sel = -1;
    }
    if (!g_symbol_picker.open && was_open) {
        // Picker just closed - unsubscribe from scanner only
        if (scanner_subscribed) {
            StreamKey scanner_key{
                Terminal::Pair{"binancef", "global"},
                Terminal::Stream::Scanner,
                0
            };
            ctx.stream_mgr().send_unsubscribe(scanner_key);
            scanner_subscribed = false;
        }
    }
    was_open = g_symbol_picker.open;

    if (!g_symbol_picker.open) return;
    auto& reg = SymbolRegistry::instance();
    if (!reg.is_loaded()) return;

    // ── 1i modal chrome: floating card, custom header/search/browse rows ──
    using namespace Theme;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1000, 640), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);

    ImGui::OpenPopup("Find a symbol");
    if (!ImGui::BeginPopupModal("Find a symbol", &g_symbol_picker.open,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_symbol_picker.open = false;
        ImGui::CloseCurrentPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 w0 = ImGui::GetWindowPos();
    const float  W  = ImGui::GetWindowWidth();
    const float  PADX = 18.0f;

    // Open a symbol: record it as recent, then add the pending widget (or, in
    // replace-mode, reload the terminal at that symbol). Shared by the row click,
    // the recent chips, and keyboard Enter.
    auto open_symbol = [&](const SymbolMetadata& m) {
        auto pair = pair_for(m); auto fmt = fmt_for(m); auto tick = tick_for(m);
        g_symbol_picker.record_recent(m.symbol);
        persist_recents();
        if (g_symbol_picker.replace_mode) {
            // Mirror url_router::build_terminal_path (not #included here - its EM_JS
            // defs must stay single-TU): ?exchange= only for non-binancef venues.
            std::string new_path = "/terminal/" + m.symbol;
            if (!m.exchange.empty() && m.exchange != "binancef") new_path += "?exchange=" + m.exchange;
#ifdef __EMSCRIPTEN__
            EM_ASM({ var path = UTF8ToString($0); window.location.href = path; }, new_path.c_str());
#endif
        } else {
            switch (g_symbol_picker.pending) {
                case SymbolPickerState::PendingWidget::Orderbook:
                    widgets.push_back(std::make_unique<OrderbookWidget>(pair, ctx, fmt, 25)); break;
                case SymbolPickerState::PendingWidget::DOM:
                    widgets.push_back(std::make_unique<DOMWidget>(pair, ctx, tick, 25)); break;
                case SymbolPickerState::PendingWidget::Trades:
                    widgets.push_back(std::make_unique<TradesWidget>(pair, ctx, fmt)); break;
                case SymbolPickerState::PendingWidget::Charts:
                    widgets.push_back(std::make_unique<ChartWidget>(pair, ctx, tick)); break;
                case SymbolPickerState::PendingWidget::Stats:
                    widgets.push_back(std::make_unique<StatsWidget>(pair, ctx, fmt)); break;
                case SymbolPickerState::PendingWidget::Debug:
                    widgets.push_back(std::make_unique<DebugWidget>(pair, ctx)); break;
                default: break;
            }
        }
        g_symbol_picker.open = false;
        g_symbol_picker.replace_mode = false;
        ImGui::CloseCurrentPopup();
    };

    // ── header: title · listed count · ESC chip · close ──────────────
    const float head_h = 44.0f;
    {
        ImGui::PushFont(Fonts::mono_md());
        dl->AddText(ImVec2(w0.x + PADX, w0.y + (head_h - ImGui::GetFontSize()) * 0.5f),
                    u32(Tokens::TX1), "Find a symbol");
        const float tw = ImGui::CalcTextSize("Find a symbol").x;
        ImGui::PopFont();
        char lc[64];
        snprintf(lc, sizeof(lc), "%d LISTED \xC2\xB7", g_symbol_picker.exchange_total);
        ImGui::PushFont(Fonts::mono_sm());
        const float lcw = ImGui::CalcTextSize(lc).x;
        dl->AddText(ImVec2(w0.x + PADX + tw + 12.0f, w0.y + (head_h - ImGui::GetFontSize()) * 0.5f),
                    u32(Tokens::TX3), lc);
        ImGui::PopFont();

        // ── venue toggle: BINANCE | HYPERLIQUID - filters the list by exchange ──
        {
            ImGui::PushFont(Fonts::label());
            const float chip_h = 20.0f;
            const float chip_y = w0.y + (head_h - chip_h) * 0.5f;
            float chip_x = w0.x + PADX + tw + 12.0f + lcw + 10.0f;
            struct Venue { const char* id; const char* label; };
            const Venue venues[2] = { {"binancef", "BINANCE"}, {"hl", "HYPERLIQUID"} };
            const float ven_logo = 14.0f;
            for (int i = 0; i < 2; ++i) {
                const bool active = (g_symbol_picker.selected_exchange == venues[i].id);
                const ImVec2 ts = ImGui::CalcTextSize(venues[i].label);
                // Exchange mark left of the label; only insets once its texture is
                // ready so a missing logo never leaves a permanent gap.
                ImTextureID ven_tex = LogoManager::instance().exchange(venues[i].id);
                const float logo_inset = ven_tex ? (ven_logo + 5.0f) : 0.0f;
                const float cw = ts.x + 16.0f + logo_inset;
                const ImVec2 cp(chip_x, chip_y);
                ImGui::SetCursorScreenPos(cp);
                ImGui::PushID(900 + i);
                const bool clk = ImGui::InvisibleButton("##venue", ImVec2(cw, chip_h));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + chip_h),
                    u32(active ? Tokens::BRAND_SOFT : (hov ? Tokens::ELEV : Tokens::PANEL)), 3.0f);
                dl->AddRect(cp, ImVec2(cp.x + cw, cp.y + chip_h),
                    u32(active ? Tokens::BRAND : Tokens::BD2), 3.0f, 0, 1.0f);
                if (ven_tex)
                    dl->AddImage(ven_tex, ImVec2(cp.x + 8.0f, cp.y + (chip_h - ven_logo) * 0.5f),
                                 ImVec2(cp.x + 8.0f + ven_logo, cp.y + (chip_h + ven_logo) * 0.5f));
                dl->AddText(ImVec2(cp.x + 8.0f + logo_inset, cp.y + (chip_h - ts.y) * 0.5f),
                    u32(active ? Tokens::BRAND_TX : Tokens::TX2), venues[i].label);
                if (clk && !active) {
                    g_symbol_picker.selected_exchange = venues[i].id;
                    g_symbol_picker.selected_category = -1; // categories are per-venue
                    g_symbol_picker.invalidate_cache();
                }
                chip_x += cw + 6.0f;
            }
            ImGui::PopFont();
        }

        // close (X) + ESC chip, right-aligned
        const float xs = 16.0f;
        const ImVec2 xp(w0.x + W - PADX - xs, w0.y + (head_h - xs) * 0.5f);
        ImGui::SetCursorScreenPos(xp);
        const bool xclk = ImGui::InvisibleButton("##pick_close", ImVec2(xs, xs));
        const bool xhov = ImGui::IsItemHovered();
        const ImU32 xc = u32(xhov ? Tokens::TX1 : Tokens::TX2);
        const float cx0 = xp.x + xs * 0.5f, cy0 = xp.y + xs * 0.5f, xr = 4.0f;
        dl->AddLine(ImVec2(cx0 - xr, cy0 - xr), ImVec2(cx0 + xr, cy0 + xr), xc, 1.4f);
        dl->AddLine(ImVec2(cx0 - xr, cy0 + xr), ImVec2(cx0 + xr, cy0 - xr), xc, 1.4f);
        if (xclk) { g_symbol_picker.open = false; ImGui::CloseCurrentPopup(); }

        ImGui::PushFont(Fonts::label());
        const char* esc = "ESC";
        const ImVec2 es = ImGui::CalcTextSize(esc);
        const ImVec2 ep0(xp.x - 10.0f - es.x - 12.0f, w0.y + (head_h - (es.y + 6.0f)) * 0.5f);
        dl->AddRect(ep0, ImVec2(ep0.x + es.x + 12.0f, ep0.y + es.y + 6.0f), u32(Tokens::BD2), 0.0f, 0, 1.0f);
        dl->AddText(ImVec2(ep0.x + 6.0f, ep0.y + 3.0f), u32(Tokens::TX3), esc);
        ImGui::PopFont();

        dl->AddLine(ImVec2(w0.x, w0.y + head_h), ImVec2(w0.x + W, w0.y + head_h), u32(Tokens::BD1));
    }

    // ── search row: accent magnifier · frameless input · CTRL K chip ─
    const float srch_h = 44.0f;
    const float srch_y = w0.y + head_h;
    {
        const ImVec2 mc(w0.x + PADX + 6.0f, srch_y + srch_h * 0.5f - 1.0f);
        dl->AddCircle(ImVec2(mc.x - 1.5f, mc.y - 1.5f), 4.2f, u32(Tokens::BRAND), 16, 1.3f);
        dl->AddLine(ImVec2(mc.x + 1.8f, mc.y + 1.8f), ImVec2(mc.x + 5.5f, mc.y + 5.5f),
                    u32(Tokens::BRAND), 1.3f);

        ImGui::PushFont(Fonts::mono());
        ImGui::SetCursorScreenPos(ImVec2(w0.x + PADX + 22.0f,
                                         srch_y + (srch_h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(W - PADX * 2.0f - 22.0f - 78.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##sym_search", "Search symbol or category...",
            g_symbol_picker.search_buf, sizeof(g_symbol_picker.search_buf));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::PushFont(Fonts::label());
        const char* kk = "CTRL K";
        const ImVec2 ks = ImGui::CalcTextSize(kk);
        const ImVec2 kp0(w0.x + W - PADX - ks.x - 12.0f, srch_y + (srch_h - (ks.y + 6.0f)) * 0.5f);
        dl->AddRect(kp0, ImVec2(kp0.x + ks.x + 12.0f, kp0.y + ks.y + 6.0f), u32(Tokens::BD2), 0.0f, 0, 1.0f);
        dl->AddText(ImVec2(kp0.x + 6.0f, kp0.y + 3.0f), u32(Tokens::TX3), kk);
        ImGui::PopFont();

        // accent inset underline (1i) over the row hairline
        dl->AddLine(ImVec2(w0.x, srch_y + srch_h - 1.0f), ImVec2(w0.x + W, srch_y + srch_h - 1.0f),
                    u32(Tokens::BD1));
        dl->AddRectFilled(ImVec2(w0.x, srch_y + srch_h - 2.0f), ImVec2(w0.x + W, srch_y + srch_h),
                          u32(Tokens::BRAND, 0.55f));
    }

    // ── RECENT row: last-opened symbols as chips (persisted) ─────────
    const float rec_y = srch_y + srch_h;
    float rec_h = 0.0f;
    if (!g_symbol_picker.recents.empty()) {
        rec_h = 42.0f;
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(w0.x + PADX, rec_y + (rec_h - ImGui::GetFontSize()) * 0.5f),
                    u32(Tokens::TX3), "RECENT");
        ImGui::PopFont();
        const float rch = 26.0f, rcy = rec_y + (rec_h - rch) * 0.5f, rxmax = w0.x + W - PADX;
        float rx = w0.x + PADX + 64.0f;
        int shown = 0;
        for (const auto& sym : g_symbol_picker.recents) {
            if (shown >= 8) break;
            std::string up = sym;
            std::transform(up.begin(), up.end(), up.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            // Recents are bare symbols (no venue stored); they are binancef-historical.
            const auto* tk = TickerManager::instance().get("binancef", sym);
            char pctbuf[16] = "";
            const bool has_pct = tk != nullptr;
            ImVec4 pctcol = Tokens::TX3;
            if (has_pct) {
                snprintf(pctbuf, sizeof(pctbuf), "%+.2f%%", tk->change_pct_24h);
                pctcol = tk->change_pct_24h >= 0.0 ? Tokens::UP : Tokens::DOWN;
            }
            ImGui::PushFont(Fonts::mono_sm());
            const float symw = ImGui::CalcTextSize(up.c_str()).x;
            const float pctw = has_pct ? ImGui::CalcTextSize(pctbuf).x + 8.0f : 0.0f;
            ImGui::PopFont();
            const float cw = symw + pctw + 20.0f;
            if (rx + cw > rxmax) break;
            ImGui::SetCursorScreenPos(ImVec2(rx, rcy));
            ImGui::PushID(3000 + shown);
            const bool clk = ImGui::InvisibleButton("##rec", ImVec2(cw, rch));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRect(ImVec2(rx, rcy), ImVec2(rx + cw, rcy + rch),
                        u32(hov ? Tokens::BRAND : Tokens::BD2), 0.0f, 0, 1.0f);
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImVec2(rx + 10.0f, rcy + (rch - ImGui::GetFontSize()) * 0.5f),
                        u32(Tokens::TX1), up.c_str());
            if (has_pct)
                dl->AddText(ImVec2(rx + 10.0f + symw + 8.0f, rcy + (rch - ImGui::GetFontSize()) * 0.5f),
                            u32(pctcol), pctbuf);
            ImGui::PopFont();
            if (clk) {
                const auto* m = SymbolRegistry::instance().get("binancef", sym);
                if (m) open_symbol(*m);
            }
            rx += cw + 8.0f;
            shown++;
        }
    }

    // ── BROWSE: category chips (square, accent on-state) - wraps + counts ─
    const float cat_y = rec_y + rec_h;
    const float cat_top = 10.0f, cat_bot = 12.0f, chh = 22.0f, chip_gap = 8.0f, row_gap = 8.0f;
    float cat_h = 0.0f;
    {
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(w0.x + PADX, cat_y + cat_top + (chh - ImGui::GetFontSize()) * 0.5f),
                    u32(Tokens::TX3), "BROWSE");
        ImGui::PopFont();

        const auto& cats = g_symbol_picker.cached_categories;
        const auto& counts = g_symbol_picker.cached_category_counts;
        const float x0 = w0.x + PADX + 64.0f, xmax = w0.x + W - PADX;
        float chx = x0, chy = cat_y + cat_top;
        auto cat_chip = [&](const char* label, int count, bool selected, int idx) -> bool {
            ImGui::PushFont(Fonts::mono_sm());
            const ImVec2 ts = ImGui::CalcTextSize(label);
            ImGui::PopFont();
            char nbuf[16] = "";
            float nw = 0.0f;
            if (count >= 0) {
                snprintf(nbuf, sizeof(nbuf), "%d", count);
                ImGui::PushFont(Fonts::label());
                nw = ImGui::CalcTextSize(nbuf).x + 6.0f;
                ImGui::PopFont();
            }
            const float cw2 = ts.x + nw + 20.0f;
            if (chx + cw2 > xmax) { chx = x0; chy += chh + row_gap; }   // wrap to next line
            ImGui::SetCursorScreenPos(ImVec2(chx, chy));
            ImGui::PushID(4000 + idx);
            const bool clk = ImGui::InvisibleButton("##cat", ImVec2(cw2, chh));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (selected)
                dl->AddRectFilled(ImVec2(chx, chy), ImVec2(chx + cw2, chy + chh), u32(Tokens::BRAND_SOFT));
            dl->AddRect(ImVec2(chx, chy), ImVec2(chx + cw2, chy + chh),
                        u32(selected ? Tokens::BRAND : (hov ? Tokens::BRAND_LINE : Tokens::BD2)),
                        0.0f, 0, 1.0f);
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImVec2(chx + 10.0f, chy + (chh - ts.y) * 0.5f),
                        u32(selected ? Tokens::BRAND_TX : (hov ? Tokens::TX1 : Tokens::TX2)), label);
            ImGui::PopFont();
            if (count >= 0) {
                ImGui::PushFont(Fonts::label());
                dl->AddText(ImVec2(chx + 10.0f + ts.x + 6.0f, chy + (chh - ImGui::GetFontSize()) * 0.5f + 1.0f),
                            u32(Tokens::TX3), nbuf);
                ImGui::PopFont();
            }
            chx += cw2 + chip_gap;
            return clk;
        };
        if (cat_chip("All", static_cast<int>(SymbolRegistry::instance().size()),
                     g_symbol_picker.selected_category < 0, -1))
            g_symbol_picker.selected_category = -1;
        for (int i = 0; i < static_cast<int>(cats.size()); ++i) {
            const bool sel = (g_symbol_picker.selected_category == i);
            const int cnt = i < static_cast<int>(counts.size()) ? counts[i] : -1;
            if (cat_chip(cats[i].c_str(), cnt, sel, i))
                g_symbol_picker.selected_category = sel ? -1 : i;
        }
        cat_h = (chy + chh + cat_bot) - cat_y;
        dl->AddLine(ImVec2(w0.x, cat_y + cat_h), ImVec2(w0.x + W, cat_y + cat_h), u32(Tokens::BD1));
    }

    // ── column header strip (bg-0, sortable; active col = accent) ────
    const float hdr_h = 30.0f;
    const float hdr_y = cat_y + cat_h;
    const float col_star_x  = w0.x + PADX;
    const float col_logo_x  = w0.x + PADX + 22.0f;   // coin logo, after the * zone
    const float col_sym_x   = w0.x + PADX + 48.0f;   // symbol, after the logo
    const float col_last_r  = w0.x + W * 0.46f;
    const float col_chg_r   = w0.x + W * 0.58f;
    const float col_vol_r   = w0.x + W * 0.72f;
    const float col_score_r = w0.x + W * 0.82f;
    const float col_type_r  = w0.x + W - PADX;
    {
        dl->AddRectFilled(ImVec2(w0.x, hdr_y), ImVec2(w0.x + W, hdr_y + hdr_h), u32(Tokens::BASE));
        dl->AddLine(ImVec2(w0.x, hdr_y + hdr_h), ImVec2(w0.x + W, hdr_y + hdr_h), u32(Tokens::BD1));

        ImGui::PushFont(Fonts::mono_sm());
        dl->AddText(ImVec2(col_star_x, hdr_y + (hdr_h - ImGui::GetFontSize()) * 0.5f), u32(Tokens::TX3), "*");
        ImGui::PopFont();

        auto& smode = g_symbol_picker.sort_mode;
        auto& asc = g_symbol_picker.sort_ascending;
        auto hdr_cell = [&](const char* label, float anchor_x, bool right_align,
                            SymbolPickerState::SortMode mode) {
            ImGui::PushFont(Fonts::label());
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const bool active = (smode == mode);
            const float tx2 = right_align ? anchor_x - ts.x : anchor_x;
            const float ty = hdr_y + (hdr_h - ts.y) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(tx2 - 12.0f, hdr_y));
            ImGui::PushID(label);
            const bool clk = ImGui::InvisibleButton("##hdr", ImVec2(ts.x + 14.0f, hdr_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clk) {
                if (active) asc = !asc;
                else { smode = mode; asc = (mode == SymbolPickerState::SortMode::Symbol); }
            }
            dl->AddText(ImVec2(tx2, ty),
                        u32(active ? Tokens::BRAND_TX : (hov ? Tokens::TX1 : Tokens::TX3)), label);
            if (active) {
                const float ax = right_align ? tx2 - 10.0f : tx2 + ts.x + 8.0f;
                const float ay = hdr_y + hdr_h * 0.5f;
                const ImU32 ac = u32(Tokens::BRAND_TX);
                if (asc) dl->AddTriangleFilled(ImVec2(ax - 3.5f, ay + 2.0f), ImVec2(ax + 3.5f, ay + 2.0f), ImVec2(ax, ay - 3.0f), ac);
                else     dl->AddTriangleFilled(ImVec2(ax - 3.5f, ay - 2.0f), ImVec2(ax + 3.5f, ay - 2.0f), ImVec2(ax, ay + 3.0f), ac);
            }
            ImGui::PopFont();
        };
        hdr_cell("SYMBOL", col_sym_x,   false, SymbolPickerState::SortMode::Symbol);
        hdr_cell("LAST",   col_last_r,  true,  SymbolPickerState::SortMode::Price);
        hdr_cell("24H %",  col_chg_r,   true,  SymbolPickerState::SortMode::Change24h);
        hdr_cell("VOLUME", col_vol_r,   true,  SymbolPickerState::SortMode::Volume);
        hdr_cell("SCORE",  col_score_r, true,  SymbolPickerState::SortMode::Score);
        ImGui::PushFont(Fonts::label());
        const ImVec2 tts = ImGui::CalcTextSize("TYPE");
        dl->AddText(ImVec2(col_type_r - tts.x, hdr_y + (hdr_h - tts.y) * 0.5f), u32(Tokens::TX3), "TYPE");
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(w0.x, hdr_y + hdr_h));

    // ── Build filtered + sorted list (cached - only rebuild when inputs change) ──
    auto& ticker_mgr = TickerManager::instance();
    auto& scanner_mgr = ScannerManager::instance();
    const auto& cats = g_symbol_picker.cached_categories;
    bool filter_dead = ticker_mgr.count() > 400;

    // Detect if any input has changed since last frame
    bool search_changed = (strcmp(g_symbol_picker.search_buf, g_symbol_picker.prev_search) != 0);
    bool cat_changed = (g_symbol_picker.selected_category != g_symbol_picker.prev_category);
    bool exch_changed = (g_symbol_picker.selected_exchange != g_symbol_picker.prev_exchange);
    bool sort_changed = (g_symbol_picker.sort_mode != g_symbol_picker.prev_sort_mode ||
                         g_symbol_picker.sort_ascending != g_symbol_picker.prev_sort_asc);

    // Live ticker/scanner drift only forces a rebuild every 2s - re-sorting
    // ~660 symbols with per-element manager lookups several times a second was
    // the picker's real cost (immediate-mode redraw is already clipped/cheap).
    const int64_t now_ms_pick = static_cast<int64_t>(ImGui::GetTime() * 1000.0);
    bool data_drift = (now_ms_pick - g_symbol_picker.last_data_rebuild_ms > 2000) &&
                      (ticker_mgr.last_update_ms() != g_symbol_picker.prev_ticker_update_ms ||
                       scanner_mgr.last_update_ms() != g_symbol_picker.prev_scanner_update_ms);

    if (g_symbol_picker.cache_dirty || search_changed || cat_changed || exch_changed || sort_changed || data_drift) {
        auto& visible = g_symbol_picker.cached_visible;
        visible.clear();
        visible.reserve(reg.size());
        int ex_total = 0;
        for (const auto& [key, meta] : reg.all()) {
            if (!meta.is_active) continue;
            // Venue filter: the picker shows one exchange at a time (toggle).
            if (meta.exchange != g_symbol_picker.selected_exchange) continue;
            ex_total++;
            // filter_dead drops symbols with no live ticker. Applied to binancef only:
            // HL now HAS a ticker feed (ticker24h.hl.global), but a brief HL feed gap
            // shouldn't wipe every HL row from the picker, so keep HL rows unconditional.
            if (filter_dead && meta.exchange == "binancef" && !ticker_mgr.get(meta.exchange, meta.symbol)) continue;
            if (!matches_search(meta.base_asset, g_symbol_picker.search_buf) &&
                !matches_search(meta.symbol, g_symbol_picker.search_buf))
                continue;
            if (!matches_category(meta, g_symbol_picker.selected_category, cats))
                continue;
            visible.push_back(&meta);
        }
        g_symbol_picker.exchange_total = ex_total;

        bool sort_asc = g_symbol_picker.sort_ascending;
        switch (g_symbol_picker.sort_mode) {
        case SymbolPickerState::SortMode::Symbol:
            std::sort(visible.begin(), visible.end(), [sort_asc](auto* a, auto* b) {
                return sort_asc ? a->base_asset < b->base_asset : a->base_asset > b->base_asset;
            });
            break;
        case SymbolPickerState::SortMode::Score:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double sa = 0, sb = 0;
                if (auto* s = scanner_mgr.get(a->exchange, a->symbol)) sa = s->edgedepth_score;
                if (auto* s = scanner_mgr.get(b->exchange, b->symbol)) sb = s->edgedepth_score;
                return sort_asc ? sa < sb : sa > sb;
            });
            break;
        case SymbolPickerState::SortMode::VPIN:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double va = 0, vb = 0;
                if (auto* s = scanner_mgr.get(a->exchange, a->symbol)) va = s->vpin;
                if (auto* s = scanner_mgr.get(b->exchange, b->symbol)) vb = s->vpin;
                return sort_asc ? va < vb : va > vb;
            });
            break;
        case SymbolPickerState::SortMode::Price:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double pa = 0, pb = 0;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) pa = t->last_price;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) pb = t->last_price;
                return sort_asc ? pa < pb : pa > pb;
            });
            break;
        case SymbolPickerState::SortMode::Change24h:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double ca = -9999, cb = -9999;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) ca = t->change_pct_24h;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) cb = t->change_pct_24h;
                return sort_asc ? ca < cb : ca > cb;
            });
            break;
        case SymbolPickerState::SortMode::Volume:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double va = 0, vb = 0;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) va = t->volume_quote;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) vb = t->volume_quote;
                return sort_asc ? va < vb : va > vb;
            });
            break;
        }

        // Save state for next-frame comparison
        strncpy(g_symbol_picker.prev_search, g_symbol_picker.search_buf, sizeof(g_symbol_picker.prev_search));
        g_symbol_picker.prev_category = g_symbol_picker.selected_category;
        g_symbol_picker.prev_exchange = g_symbol_picker.selected_exchange;
        g_symbol_picker.prev_sort_mode = g_symbol_picker.sort_mode;
        g_symbol_picker.prev_sort_asc = g_symbol_picker.sort_ascending;
        g_symbol_picker.prev_ticker_update_ms = ticker_mgr.last_update_ms();
        g_symbol_picker.prev_scanner_update_ms = scanner_mgr.last_update_ms();
        g_symbol_picker.last_data_rebuild_ms = now_ms_pick;
        g_symbol_picker.cache_dirty = false;
    }

    const auto& visible = g_symbol_picker.cached_visible;

    // ── keyboard navigation (arrows move highlight, Enter opens) ─────
    const int vis_n = static_cast<int>(visible.size());
    if (g_symbol_picker.keyboard_sel >= vis_n) g_symbol_picker.keyboard_sel = vis_n - 1;
    if (vis_n > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            g_symbol_picker.keyboard_sel = (g_symbol_picker.keyboard_sel < 0)
                ? 0 : std::min(g_symbol_picker.keyboard_sel + 1, vis_n - 1);
            g_symbol_picker.scroll_to_sel = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            g_symbol_picker.keyboard_sel = (g_symbol_picker.keyboard_sel <= 0)
                ? 0 : g_symbol_picker.keyboard_sel - 1;
            g_symbol_picker.scroll_to_sel = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && g_symbol_picker.keyboard_sel >= 0) {
            open_symbol(*visible[g_symbol_picker.keyboard_sel]);
        }
    }

    // ── Symbol list (clipped - only renders visible rows) - 1i rows ──
    const float row_h = 34.0f, foot_h = 36.0f;
    ImGui::BeginChild("SymbolList", ImVec2(0, -foot_h), false);
    ImDrawList* rdl = ImGui::GetWindowDrawList();

    if (g_symbol_picker.scroll_to_sel && g_symbol_picker.keyboard_sel >= 0) {
        const float top = g_symbol_picker.keyboard_sel * row_h;
        const float bot = top + row_h;
        const float sy = ImGui::GetScrollY();
        const float ch = ImGui::GetWindowHeight();
        if (top < sy) ImGui::SetScrollY(top);
        else if (bot > sy + ch) ImGui::SetScrollY(bot - ch);
        g_symbol_picker.scroll_to_sel = false;
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), row_h);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto* meta = visible[i];
            ImGui::PushID(i);
            const auto* ticker = ticker_mgr.get(meta->exchange, meta->symbol);

            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const bool ksel = (i == g_symbol_picker.keyboard_sel);
            const bool row_clk = ImGui::InvisibleButton("##row", ImVec2(W, row_h));
            const bool row_hov = ImGui::IsItemHovered();
            if (ksel || row_hov)
                rdl->AddRectFilled(rp, ImVec2(rp.x + W, rp.y + row_h), u32(Tokens::ELEV));
            if (ksel)
                rdl->AddRectFilled(rp, ImVec2(rp.x + 2.0f, rp.y + row_h), u32(Tokens::BRAND_TX));
            const float cy = rp.y + row_h * 0.5f;

            // ★ favorite marker + its own click zone at the left edge of the row.
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool on_star = mp.x >= col_star_x - 4.0f && mp.x <= col_star_x + 20.0f;
            const bool fav = g_symbol_picker.is_favorite(meta->symbol);
            {
                ImGui::PushFont(Fonts::mono());
                const ImVec4& sc = fav ? Tokens::BRAND_TX
                                       : ((row_hov && on_star) ? Tokens::TX2 : Tokens::TX4);
                rdl->AddText(ImVec2(col_star_x, cy - ImGui::GetFontSize() * 0.5f), u32(sc), "*");
                ImGui::PopFont();
            }

            // OPEN affordance on the keyboard-highlighted row.
            if (ksel) {
                ImGui::PushFont(Fonts::label());
                const char* op = "OPEN";
                const float ow = ImGui::CalcTextSize(op).x;
                const float ox = col_score_r + 22.0f;
                rdl->AddRect(ImVec2(ox - 6.0f, cy - 9.0f), ImVec2(ox + ow + 6.0f, cy + 9.0f), u32(Tokens::BRAND), 0.0f, 0, 1.0f);
                rdl->AddText(ImVec2(ox, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::BRAND_TX), op);
                ImGui::PopFont();
            }

            if (row_clk) {
                if (on_star) { g_symbol_picker.toggle_favorite(meta->symbol); persist_favorites(); }
                else { open_symbol(*meta); }
            }

        ImGui::PushFont(Fonts::mono());
        const float fs = ImGui::GetFontSize();
        const float ty = cy - fs * 0.5f;

        // Coin logo in the star→symbol gutter (monogram fallback while loading).
        LogoManager::instance().draw_coin(rdl, meta->base_asset,
                                          ImVec2(col_logo_x, cy - 9.0f), 18.0f);

        // Symbol - base bright, /QUOTE dimmed. Non-ASCII base assets (CJK etc.)
        // fall back to the uppercased raw symbol.
        {
            std::string display = meta->display_name();
            bool has_nonascii = false;
            for (char c : meta->base_asset) {
                if (static_cast<unsigned char>(c) > 127) { has_nonascii = true; break; }
            }
            if (has_nonascii) {
                std::string sym_upper = meta->symbol;
                std::transform(sym_upper.begin(), sym_upper.end(), sym_upper.begin(),
                    [](unsigned char c) { return std::toupper(c); });
                if (sym_upper.size() > 4 && sym_upper.substr(sym_upper.size() - 4) == "USDT") {
                    sym_upper = sym_upper.substr(0, sym_upper.size() - 4);
                }
                display = sym_upper + "/USDT";
            }
            const auto slash = display.find('/');
            if (slash != std::string::npos) {
                char base_buf[48];
                snprintf(base_buf, sizeof(base_buf), "%.*s", static_cast<int>(slash), display.c_str());
                rdl->AddText(ImVec2(col_sym_x, ty), u32(Tokens::TX1), base_buf);
                const float bw = ImGui::CalcTextSize(base_buf).x;
                rdl->AddText(ImVec2(col_sym_x + bw, ty), u32(Tokens::TX3), display.c_str() + slash);
            } else {
                rdl->AddText(ImVec2(col_sym_x, ty), u32(Tokens::TX1), display.c_str());
            }
        }

        // right-aligned mono numeral cell
        auto num_cell = [&](float right_x, const char* s, const ImVec4& col) {
            const float sw2 = ImGui::CalcTextSize(s).x;
            rdl->AddText(ImVec2(right_x - sw2, ty), u32(col), s);
        };

        // LAST
        if (ticker && ticker->last_price > 0.0) {
            char pbuf[32];
            meta->fmt.format_price(pbuf, sizeof(pbuf), ticker->last_price);
            num_cell(col_last_r, pbuf, Tokens::TX1);
        } else num_cell(col_last_r, "-", Tokens::TX4);

        // 24H % - signed up/down
        if (ticker) {
            char cbuf[24];
            snprintf(cbuf, sizeof(cbuf), "%+.2f%%", ticker->change_pct_24h);
            num_cell(col_chg_r, cbuf, ticker->change_pct_24h >= 0.0 ? Tokens::UP : Tokens::DOWN);
        } else num_cell(col_chg_r, "-", Tokens::TX4);

        // VOLUME
        if (ticker && ticker->volume_quote > 0.0) {
            char vbuf[32];
            format_volume(vbuf, sizeof(vbuf), ticker->volume_quote);
            num_cell(col_vol_r, vbuf, Tokens::TX2);
        } else num_cell(col_vol_r, "-", Tokens::TX4);

        // SCORE - accent for hot, text ramp below (no traffic lights)
        {
            const auto* scan = scanner_mgr.get(meta->exchange, meta->symbol);
            if (scan && scan->edgedepth_score > 0.0) {
                char sbuf[16];
                snprintf(sbuf, sizeof(sbuf), "%.0f", scan->edgedepth_score);
                const ImVec4& col = scan->edgedepth_score >= 70.0 ? Tokens::BRAND_TX
                                  : scan->edgedepth_score >= 40.0 ? Tokens::TX1
                                                                  : Tokens::TX3;
                num_cell(col_score_r, sbuf, col);
            } else num_cell(col_score_r, "-", Tokens::TX4);
        }
        ImGui::PopFont();

        // TYPE - first category or PERP, mono-sm text-2 (legible, not tiny)
        {
            ImGui::PushFont(Fonts::mono_sm());
            const char* tp = meta->categories.empty() ? "PERP" : meta->categories[0].c_str();
            const float tw2 = ImGui::CalcTextSize(tp).x;
            rdl->AddText(ImVec2(col_type_r - tw2, cy - ImGui::GetFontSize() * 0.5f),
                         u32(Tokens::TX2), tp);
            ImGui::PopFont();
        }

        ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::EndChild();

    // ── footer strip (bg-0): count + key hints ───────────────────────
    {
        const float fy = ImGui::GetCursorScreenPos().y;
        dl->AddRectFilled(ImVec2(w0.x + 1.0f, fy), ImVec2(w0.x + W - 1.0f, fy + foot_h - 1.0f),
                          u32(Tokens::BASE));
        dl->AddLine(ImVec2(w0.x, fy), ImVec2(w0.x + W, fy), u32(Tokens::BD1));
        char cbuf[24];
        snprintf(cbuf, sizeof(cbuf), "%zu", visible.size());
        ImGui::PushFont(Fonts::mono_sm());
        dl->AddText(ImVec2(w0.x + PADX, fy + 11.0f), u32(Tokens::TX1), cbuf);
        const float cw2 = ImGui::CalcTextSize(cbuf).x;
        dl->AddText(ImVec2(w0.x + PADX + cw2 + 6.0f, fy + 11.0f), u32(Tokens::TX3),
                    "SYMBOLS \xC2\xB7 STATS ARE 24H");
        const char* hint = "ARROWS NAVIGATE \xC2\xB7 ENTER OPENS \xC2\xB7 * FAVORITES \xC2\xB7 ESC CLOSES";
        const float hw = ImGui::CalcTextSize(hint).x;
        dl->AddText(ImVec2(w0.x + W - PADX - hw, fy + 11.0f), u32(Tokens::TX3), hint);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(W, foot_h - 1.0f));
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// Drain one add-widget request per frame (chart-toolbar "+ widget" in the
// embedded chromes). Mirrors the picker's open_symbol switch but uses the
// already-resolved pair/fmt/tick the chart handed us, so it needs no registry
// lookup and works with no WebSocket (a /demo pack has none).
void resolve_widget_add_request(
    std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx)
{
    auto& req = g_widget_add_request;
    if (!req.pending) return;
    req.pending = false;
    using PW = SymbolPickerState::PendingWidget;
    switch (req.type) {
        case PW::Orderbook:
            widgets.push_back(std::make_unique<OrderbookWidget>(req.pair, ctx, req.fmt, 25)); break;
        case PW::DOM:
            widgets.push_back(std::make_unique<DOMWidget>(req.pair, ctx, req.tick_size, 25)); break;
        case PW::Trades:
            widgets.push_back(std::make_unique<TradesWidget>(req.pair, ctx, req.fmt)); break;
        case PW::Charts:
            widgets.push_back(std::make_unique<ChartWidget>(req.pair, ctx, req.tick_size)); break;
        case PW::Stats:
            widgets.push_back(std::make_unique<StatsWidget>(req.pair, ctx, req.fmt)); break;
        case PW::Debug:
            widgets.push_back(std::make_unique<DebugWidget>(req.pair, ctx)); break;
        case PW::PaperTrading:
            widgets.push_back(std::make_unique<PositionsPanel>(ctx)); break;
        default: break;
    }
}

} // namespace Menu
