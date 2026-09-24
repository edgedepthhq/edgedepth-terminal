#include "rendering/app_shell.h"
#include "core/websocket.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "imgui.h"
#include "rendering/menu.h"
#include "rendering/theme.h"
#include "rendering/layout.h"
#include "core/workspace_manager.h"
#include "replayer/replay_manager.h"
#include "core/symbol_metadata.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "core/analytics_manager.h"
#include "core/stream_presence.h"
#include "stream_handler.h"
#include "ui/positions_panel.h"
#include "ui/chart_widget.h"
#include "ui/upsell_modal.h"
#include "core/entitlements.h"
#include "core/heatmap_colormap.h"
#include "core/display_time_zone.h"
#include "core/drawing_manager.h"
#include "ui/drawing/drawing_toolbar.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// app_shell.cpp - topbar + statsbar implementation
// ═══════════════════════════════════════════════════════════════════════════════

namespace AppShell {

namespace {
    using namespace Theme;

    // ── Shell state ──────────────────────────────────────────────────────────
    Terminal::Pair g_pair{"binancef", "btcusdt"};
    std::string    g_display_name = "BTC/USDT";   // from SymbolMetadata
    std::string    g_base_asset   = "BTC";        // for the pair-pill coin logo
    bool           g_perpetual    = true;         // pill sub-label: Perp / Dated
    PriceFormatter g_fmt;

    // Latest per-symbol stats (written by the stats stream callback)
    struct ShellStats {
        double  mark_price        = 0.0;
        double  funding           = 0.0;
        int64_t next_funding_time = 0;
        double  open_interest_usd = 0.0;
        double  liq_total_usd     = 0.0;
        double  liq_long_usd      = 0.0;
        double  liq_short_usd     = 0.0;
        bool    has_data          = false;
    };
    ShellStats g_stats;
    bool g_initialized = false;

    // A concise menu, not a dump of every IANA locality. Zones that currently
    // share an offset remain separate only when their DST or legal clock rules
    // differ.
    constexpr const char* kCommonTimeZones[] = {
        "UTC",
        "Pacific/Honolulu",
        "America/Anchorage",
        "America/Los_Angeles",
        "America/Phoenix",
        "America/Denver",
        "America/Chicago",
        "America/Mexico_City",
        "America/Bogota",
        "America/New_York",
        "America/Halifax",
        "America/St_Johns",
        "America/Argentina/Buenos_Aires",
        "America/Sao_Paulo",
        "Atlantic/Azores",
        "Europe/London",
        "Europe/Paris",
        "Africa/Lagos",
        "Africa/Johannesburg",
        "Africa/Cairo",
        "Africa/Nairobi",
        "Europe/Moscow",
        "Asia/Tehran",
        "Asia/Dubai",
        "Asia/Kabul",
        "Asia/Karachi",
        "Asia/Kolkata",
        "Asia/Kathmandu",
        "Asia/Dhaka",
        "Asia/Yangon",
        "Asia/Bangkok",
        "Asia/Singapore",
        "Asia/Tokyo",
        "Australia/Eucla",
        "Australia/Adelaide",
        "Australia/Brisbane",
        "Australia/Sydney",
        "Pacific/Guadalcanal",
        "Pacific/Auckland",
        "Pacific/Chatham",
        "Pacific/Kiritimati",
    };

    void compact_offset_label(const char* full, char* out, size_t out_size) {
        if (!full || std::strcmp(full, "UTC+00:00") == 0 || std::strcmp(full, "UTC") == 0) {
            std::snprintf(out, out_size, "UTC");
            return;
        }
        if (std::strlen(full) >= 9 && std::strncmp(full, "UTC", 3) == 0 &&
            (full[3] == '+' || full[3] == '-') && std::isdigit(full[4]) &&
            std::isdigit(full[5]) && full[6] == ':') {
            const int hour = (full[4] - '0') * 10 + (full[5] - '0');
            if (full[7] == '0' && full[8] == '0') {
                std::snprintf(out, out_size, "UTC%c%d", full[3], hour);
            } else {
                std::snprintf(out, out_size, "UTC%c%d:%.2s", full[3], hour, full + 7);
            }
            return;
        }
        std::snprintf(out, out_size, "%s", full);
    }

    void render_time_zone_picker(int64_t display_epoch_ms) {
        auto& service = DisplayTimeZone::instance();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::R3);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        // The popup is anchored above the bottom status bar. Its desktop height
        // is about one-third larger than the old 430px picker; short canvases use
        // all safe space available above the bar.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float popup_width = std::clamp(viewport->Size.x - 16.0f, 304.0f, 400.0f);
        const float available_height =
            std::max(280.0f, viewport->Size.y - Layout::STATUSBAR_H - 14.0f);
        const float popup_height = std::min(572.0f, available_height);
        ImGui::SetNextWindowSize(ImVec2(popup_width, popup_height), ImGuiCond_Appearing);
        if (Theme::begin_popup("##time_zone_picker")) {
            const auto select_zone = [&](const char* zone) {
                char full_offset[20] = "UTC+00:00";
                char compact_offset[20] = "UTC";
                service.offset_label_for_zone(display_epoch_ms, zone,
                                              full_offset, sizeof(full_offset));
                compact_offset_label(full_offset, compact_offset, sizeof(compact_offset));
                char row[192]{};
                std::snprintf(row, sizeof(row), "(%s) %s", compact_offset, zone);
                const bool selected = std::strcmp(zone, service.zone_name()) == 0;
                if (ImGui::MenuItem(row, nullptr, selected)) {
                    service.set_named(zone);
                    ImGui::CloseCurrentPopup();
                }
            };

            bool selected_zone_is_common = false;
            for (const char* zone : kCommonTimeZones) {
                if (std::strcmp(zone, service.zone_name()) == 0) {
                    selected_zone_is_common = true;
                    break;
                }
            }
            if (!selected_zone_is_common) select_zone(service.zone_name());

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sizeof(kCommonTimeZones) /
                                           sizeof(kCommonTimeZones[0])));
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    select_zone(kCommonTimeZones[static_cast<size_t>(index)]);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    // ── Small drawing/format helpers ─────────────────────────────────────────
    void fmt_compact_usd(char* buf, size_t n, double v) {
        if (v >= 1e9)      snprintf(buf, n, "$%.2fB", v / 1e9);
        else if (v >= 1e6) snprintf(buf, n, "$%.2fM", v / 1e6);
        else if (v >= 1e3) snprintf(buf, n, "$%.1fK", v / 1e3);
        else               snprintf(buf, n, "$%.0f", v);
    }

    // hh:mm:ss until next funding
    void fmt_funding_countdown(char* buf, size_t n, int64_t next_ms) {
        const int64_t now_ms = static_cast<int64_t>(time(nullptr)) * 1000;
        int64_t left = next_ms - now_ms;
        if (left < 0) left = 0;
        const int s = static_cast<int>(left / 1000);
        snprintf(buf, n, "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    }

    // Draw text glyph-by-glyph with per-character letter-spacing (ImGui has no
    // native tracking). `tracking` px is inserted after every glyph except the
    // last, so the EARLY ACCESS pill matches the web .beta badge's 0.06em track.
    // Pass an explicit (font,size) so it renders identically to a pushed font.
    void draw_tracked_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                           ImU32 col, const char* text, float tracking) {
        float x = pos.x;
        for (const char* p = text; *p; ++p) {
            const char buf[2] = { *p, '\0' };
            dl->AddText(font, size, ImVec2(x, pos.y), col, buf);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf).x + tracking;
        }
    }

    // brand mark: the EdgeDepth "D" - three forward depth streaks flowing into a
    // D bowl. Ported from the master SVG (viewBox 300x132); `h` is the glyph
    // height (the streak+bowl block spans 104 SVG units, y 14..118). Always the
    // fixed brand cyan (Tokens::LOGO): the mark is identity, not an on-state,
    // so the neutral chrome accent must not wash it to white.
    void draw_brand_mark(ImDrawList* dl, ImVec2 pos, float h) {
        const ImU32 col = u32(Tokens::LOGO);
        const float s = h / 104.0f;                 // SVG units -> px
        auto P = [&](float x, float y) {
            return ImVec2(pos.x + (x - 8.0f) * s, pos.y + (y - 14.0f) * s);
        };
        // three forward streaks (exact parallelograms from the master mark)
        dl->AddQuadFilled(P(58, 14),  P(138, 14), P(126, 40),  P(46, 40),  col);
        dl->AddQuadFilled(P(20, 53),  P(154, 53), P(142, 79),  P(8, 79),   col);
        dl->AddQuadFilled(P(49, 92),  P(129, 92), P(117, 118), P(37, 118), col);
        // the D: solid FILLED glyph (matches the master SVG's filled path, not a
        // stroked outline). Two horizontal bars + a right half-annulus bowl swept
        // between the outer (72x52) and inner (42x26) ellipse boundaries about the
        // bowl centre (208,66); the sweep joins the bars seamlessly at +/-pi/2.
        dl->AddQuadFilled(P(151, 14), P(208, 14), P(207, 40),  P(139, 40),  col);  // top bar
        dl->AddQuadFilled(P(138, 92), P(207, 92), P(208, 118), P(126, 118), col);  // bottom bar
        constexpr int NB = 28;
        for (int i = 0; i < NB; ++i) {
            const float t0 = -1.5707963f + 3.1415927f * static_cast<float>(i)     / NB;
            const float t1 = -1.5707963f + 3.1415927f * static_cast<float>(i + 1) / NB;
            const ImVec2 o0 = P(208.0f + 72.0f * cosf(t0), 66.0f + 52.0f * sinf(t0));
            const ImVec2 o1 = P(208.0f + 72.0f * cosf(t1), 66.0f + 52.0f * sinf(t1));
            const ImVec2 i0 = P(208.0f + 42.0f * cosf(t0), 66.0f + 26.0f * sinf(t0));
            const ImVec2 i1 = P(208.0f + 42.0f * cosf(t1), 66.0f + 26.0f * sinf(t1));
            dl->AddQuadFilled(o0, o1, i1, i0, col);
        }
    }

    // (stat_cell removed - the v2 stats strip draws its six flex cells inline in
    //  render_statsbar below, positioned by SPEC ratios with bottom-anchored details.)

    // Segmented pill group (TF, Live/Replay). Draws a subtle INPUT container;
    // the active item gets an ACTIVE fill. Renders left-to-right from the current
    // cursor. Returns the clicked index, else -1. Widths use padx=11 each side -
    // callers that pre-reserve space must match (CalcTextSize + 22).
    int seg_control(const char* id, const char* const items[], int count,
                    int active_idx, float h = 26.0f, ImFont* font = nullptr) {
        int clicked = -1;
        if (!font) font = Fonts::ui_semibold();
        ImGui::PushID(id);
        ImGui::PushFont(font);
        constexpr float padx = 11.0f;
        float total = 0.0f;
        for (int i = 0; i < count; ++i)
            total += ImGui::CalcTextSize(items[i]).x + padx * 2.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + total, p0.y + h), u32(Tokens::INPUT), Radius::R2);
        for (int i = 0; i < count; ++i) {
            if (i) ImGui::SameLine(0.0f, 0.0f);
            const float w = ImGui::CalcTextSize(items[i]).x + padx * 2.0f;
            const bool on = (i == active_idx);
            if (Theme::choice_button(items[i], on, ImVec2(w, h))) clicked = i;
        }
        ImGui::PopFont();
        ImGui::PopID();
        return clicked;
    }

    // Icon button: transparent square until hovered. Caller draws the glyph into
    // the item rect via the window draw list afterward. Returns clicked.
    bool ico_btn(const char* id, float sz = 30.0f) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R1);
        const bool c = ImGui::Button(id, ImVec2(sz, sz));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return c;
    }

    // The primary (first) chart widget - the topbar TF segment drives it.
    ChartWidget* find_primary_chart(std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& w : widgets)
            if (w && w->type() == WidgetType::Chart)
                return static_cast<ChartWidget*>(w.get());
        return nullptr;
    }

    // ── account-menu line icons ──────────────────────────────────────────────
    // The bundled ImGui fonts (Hanken / JBM) carry no icon glyphs, so each menu
    // icon is drawn from primitives. Each fits an s×s box centred on c, stroked
    // in col at thickness th (filled icons use col directly).
    void ac_ic_star(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
        ImVec2 p[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -1.5708f + i * 0.628318f;
            const float rr = (i & 1) ? r * 0.42f : r;
            p[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
        }
        for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], col);
    }
    void ac_ic_lock(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
        const ImVec2 a(c.x - s * 0.34f, c.y - s * 0.04f), b(c.x + s * 0.34f, c.y + s * 0.40f);
        dl->AddRect(a, b, col, 1.5f, 0, th);
        dl->PathArcTo(ImVec2(c.x, a.y), s * 0.22f, 3.14159265f, 6.28318530f, 12);
        dl->PathStroke(col, 0, th);
    }

    // Native menu items keep account actions compact and keyboard navigable.
    void render_account_menu() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 288,
                                      vp->Pos.y + Layout::topbar_h()));
        ImGui::SetNextWindowSize(ImVec2(280, 0));
        if (!Theme::begin_popup("##account_menu")) return;
        const bool pro = Entitlements::is_pro();
        const std::string email = Entitlements::user_email();
        ImGui::PushFont(Fonts::ui_semibold());
        ImGui::TextWrapped("%s", email.empty() ? "Your account" : email.c_str());
        ImGui::PopFont();
        ImGui::TextColored(Tokens::TX2, "%s plan", Entitlements::tier_label());
        ImGui::Spacing();
        ImGui::Separator();
        auto nav = [](const char* path) {
#ifdef __EMSCRIPTEN__
            EM_ASM({ window.location.href = UTF8ToString($0); }, path);
#else
            (void)path;
#endif
        };
        if (ImGui::MenuItem("Account settings")) nav("/account");
        if (ImGui::MenuItem("Billing and credits")) nav("/account/billing");
        if (ImGui::MenuItem("Replay history")) nav("/account/replays");
        if (ImGui::BeginMenu("Plan access")) {
            if (pro) {
                // A Research claim reaches further back than the recorded book
                // exists; say the real reach, not the claim.
                const int64_t wall = Entitlements::wall_now_ms();
                const int64_t reach_ms = wall - Entitlements::replay_start_floor_ms(wall);
                const int reach_days = static_cast<int>(reach_ms / (24LL * 3600LL * 1000LL));
                if (reach_days < Entitlements::pro_lookback_days())
                    ImGui::Text("Replay: the recorded book, from %s (%d days)",
                                Entitlements::book_replay_from_label().c_str(), reach_days);
                else
                    ImGui::Text("Replay: up to %d days", Entitlements::pro_lookback_days());
                ImGui::TextUnformatted("All recorded symbols and archived events");
                ImGui::TextUnformatted("Full course catalog");
                ImGui::TextUnformatted("Live real-time and sub-minute charts");
                const std::string& renewal = Entitlements::renews_label();
                if (!renewal.empty()) ImGui::Text("Renews %s", renewal.c_str());
            } else {
                ImGui::Text("Free replay day: %s", Entitlements::free_window_day_label());
                ImGui::TextUnformatted("6 major replay symbols");
                ImGui::TextUnformatted("Recent and public events");
                ImGui::TextUnformatted("Free and public lessons");
                ImGui::TextUnformatted("Live real-time and sub-minute: Pro / Research");
            }
            ImGui::EndMenu();
        }
        if (!pro && ImGui::MenuItem("Explore Pro plans")) Entitlements::open_upgrade();
        ImGui::Separator();
        if (ImGui::MenuItem("Sign out")) nav("/logout");
        ImGui::EndPopup();
    }

    // topbar text button - quiet until hovered. w=0 → auto-fit to label.
    bool tb_button(const char* label, float w = 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R1);
        const bool clicked = ImGui::Button(label, ImVec2(w, 30));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return clicked;
    }

    // symbol pill - coin dot + name + sub-label + caret; opens the picker
    void symbol_pill() {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);

        char sub[40];
        snprintf(sub, sizeof(sub), "%s · %s",
                 widget_venue_label(g_pair.exchange), g_perpetual ? "Perp" : "Dated");

        ImGui::PushFont(Fonts::ui_semibold());
        const ImVec2 name_sz = ImGui::CalcTextSize(g_display_name.c_str());
        ImGui::PopFont();
        ImGui::PushFont(Fonts::label());
        const ImVec2 sub_sz = ImGui::CalcTextSize(sub);
        ImGui::PopFont();
        const float h = 30.0f, dot = 18.0f;
        const float text_w = std::max(name_sz.x, sub_sz.x);
        const float w = 7.0f + dot + 9.0f + text_w + 9.0f + 10.0f + 9.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Button("##symbol_pill", ImVec2(w, h));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = p.x + 7.0f;
        const float cy = p.y + h * 0.5f;
        // coin logo (monogram fallback while it loads / if missing)
        LogoManager::instance().draw_coin(dl, g_base_asset, ImVec2(x, cy - dot * 0.5f), dot);
        x += dot + 9.0f;
        // two-line text: name over exchange sub-label
        ImGui::PushFont(Fonts::ui_semibold());
        dl->AddText(ImVec2(x, cy - 14.0f), u32(Tokens::TX1), g_display_name.c_str());
        ImGui::PopFont();
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(x, cy + 2.0f), u32(Tokens::TX3), sub);
        ImGui::PopFont();
        x += text_w + 9.0f;
        // caret
        dl->AddTriangleFilled(ImVec2(x, cy - 2.0f), ImVec2(x + 8.0f, cy - 2.0f),
                              ImVec2(x + 4.0f, cy + 3.0f), u32(Tokens::TX3));

        if (clicked) {
            Menu::g_symbol_picker.pending = Menu::SymbolPickerState::PendingWidget::Charts;
            Menu::g_symbol_picker.open = true;
            Menu::g_symbol_picker.search_buf[0] = '\0';
            Menu::g_symbol_picker.replace_mode = true;
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Find a symbol");
    }
}  // namespace

    // Market header (stats strip) visibility - toggled by its close (X) button and
    // the top-bar layout menu; drives total_height() so the dockspace reclaims the
    // space when hidden. Session-scoped for v1.
    bool g_market_header_open = true;

    float total_height() {
        // + a small breathing gap so docked panels don't butt the header hairline
        return Theme::Layout::topbar_h() +
               (g_market_header_open ? Theme::Layout::STATSBAR_H : 0.0f);
    }

    void init(const AppContext& ctx, const Terminal::Pair& active_pair) {
        if (g_initialized) return;
        g_pair = active_pair;
        const auto* meta = SymbolRegistry::instance().get(g_pair.exchange, g_pair.symbol);
        if (meta) {
            g_display_name = meta->display_name();
            g_base_asset   = meta->base_asset.empty() ? g_pair.symbol : meta->base_asset;
            g_perpetual    = meta->perpetual;
            g_fmt = meta->fmt;
        } else {
            g_display_name = g_pair.symbol;
            g_base_asset   = g_pair.symbol;
        }

        // per-symbol stats - shell keeps its own always-on subscription
        StreamKey stats_key{g_pair, Terminal::Stream::Stats, 0};
        StreamHandler<Terminal::Stat> handler{
            .widget_ptr = &g_stats,
            .callback = [](void* ptr, const Terminal::Stat& s) {
                auto* st = static_cast<ShellStats*>(ptr);
                st->mark_price        = s.mark_price;
                st->funding           = s.funding;
                st->next_funding_time = s.next_funding_time;
                st->open_interest_usd = s.open_interest_usd;
                st->liq_total_usd     = s.liq_total_usd;
                st->liq_long_usd      = s.liq_long_usd;
                st->liq_short_usd     = s.liq_short_usd;
                st->has_data          = true;
            }
        };
        ctx.stream_mgr().subscribe_stats(stats_key, handler);

        // global ticker24h - 24h change/volume for the statsbar (and later,
        // the watchlist). Stays subscribed for the app lifetime.
        StreamKey ticker_key{Terminal::Pair{"binancef", "global"},
                             Terminal::Stream::Ticker24h, 0};
        ctx.stream_mgr().send_subscribe(ticker_key);

        // per-symbol positioning - long/short account + liquidation aggregates for
        // the strip. Lands in the AnalyticsManager via message_handler.
        StreamKey pos_key{g_pair, Terminal::Stream::PositioningState, 0};
        ctx.stream_mgr().send_subscribe(pos_key);

        g_initialized = true;
    }

namespace {
    bool g_tweaks_open = false;   // Tweaks panel visibility (toggled by the theme icon)

    // ── Timeframe model - favourites bar + grouped dropdown ─────────────────
    // Sub-minute timeframes (pro=true) are a Pro entitlement; free users see them
    // locked. Favourites (max 6) drive the inline bar; default mirrors the design.
    struct TFItem { const char* label; int sec; bool pro; };
    inline const TFItem TF_CATALOG[] = {
        {"1s", 1, true}, {"5s", 5, true}, {"15s", 15, true}, {"30s", 30, true},
        {"1m", 60, false}, {"3m", 180, false}, {"5m", 300, false}, {"15m", 900, false}, {"30m", 1800, false},
        {"1h", 3600, false}, {"2h", 7200, false}, {"4h", 14400, false}, {"6h", 21600, false}, {"12h", 43200, false},
        {"1D", 86400, false}, {"1W", 604800, false},
    };
    inline constexpr int TF_COUNT = 16;

    std::vector<int> g_tf_favs = {60, 300, 900, 3600, 14400, 86400};  // secs, max 6
    char  g_tf_custom[16] = "";

    const char* tf_label(int sec) {
        for (const auto& t : TF_CATALOG) if (t.sec == sec) return t.label;
        return "?";
    }
    bool tf_is_fav(int sec) {
        for (int s : g_tf_favs) if (s == sec) return true;
        return false;
    }
    void tf_toggle_fav(int sec) {
        for (size_t i = 0; i < g_tf_favs.size(); ++i)
            if (g_tf_favs[i] == sec) { g_tf_favs.erase(g_tf_favs.begin() + i); return; }
        if (g_tf_favs.size() < 6) g_tf_favs.push_back(sec);
    }
    // Parse "1s" / "90s" / "7m" / "3h" / "2D" / "1W" → seconds (0 = invalid).
    int tf_parse(const char* s) {
        while (*s == ' ') ++s;
        long n = 0; bool any = false;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; any = true; }
        if (!any) return 0;
        char u = *s; if (u >= 'A' && u <= 'Z') u = static_cast<char>(u + 32);
        switch (u) {
            case 's': return static_cast<int>(n);
            case 'm': return static_cast<int>(n * 60);
            case 'h': return static_cast<int>(n * 3600);
            case 'd': return static_cast<int>(n * 86400);
            case 'w': return static_cast<int>(n * 604800);
            default:  return 0;
        }
    }

    // Sub-minute candles are Pro. A locked pill (or a sub-minute custom entry)
    // opens the upgrade dialog over the terminal; it used to navigate the whole
    // tab to /pricing, throwing away the live session behind it.
    void open_subminute_upsell() {
        ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Timeframe, nullptr,
                                         "subminute_candles");
        ImGui::CloseCurrentPopup();
    }

    // One timeframe pill in the dropdown (2c). Left-click selects (closes the
    // menu), right-click toggles favourite; Pro-locked pills open the upsell.
    // Active duration uses an underline and stronger text;
    // the favourite star affordance sits on the pill's top-right corner (accent).
    void tf_pill(ChartWidget* chart, const TFItem& t, int64_t cur, bool pro, ImDrawList* dl) {
        using namespace Theme;
        const bool on     = (t.sec == cur);
        const bool locked = (t.pro && !pro);
        const bool fav    = tf_is_fav(t.sec);
        const float h = 26.0f, padx = 9.0f, lockpad = locked ? 11.0f : 0.0f;
        ImGui::PushFont(Fonts::mono_sm());
        const ImVec2 ts = ImGui::CalcTextSize(t.label);
        ImGui::PopFont();
        float w = ts.x + padx * 2.0f + lockpad;
        if (w < 40.0f) w = 40.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(t.sec);
        const bool clicked = ImGui::InvisibleButton("##tfp", ImVec2(w, h));
        const bool hov     = ImGui::IsItemHovered();
        const bool rclick  = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();
        if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(Tokens::HOVER));
        if (on) dl->AddLine(ImVec2(p.x + 8, p.y + h - 1),
                            ImVec2(p.x + w - 8, p.y + h - 1), u32(Tokens::TX1), 2);
        const ImVec4 tcol = locked ? Tokens::TX4 : (on ? Tokens::TX1 : (hov ? Tokens::TX1 : Tokens::TX2));
        ImGui::PushFont(Fonts::mono_sm());
        dl->AddText(ImVec2(p.x + (w - ts.x - lockpad) * 0.5f, p.y + (h - ts.y) * 0.5f), u32(tcol), t.label);
        ImGui::PopFont();
        if (locked)     ac_ic_lock(dl, ImVec2(p.x + w - 9.0f, p.y + h * 0.5f), 10.0f, u32(Tokens::TX4), 1.2f);
        else if (fav)   ac_ic_star(dl, ImVec2(p.x + w - 1.0f, p.y), 4.0f, u32(Tokens::TX3));
        else if (hov)   ac_ic_star(dl, ImVec2(p.x + w - 1.0f, p.y), 4.0f, u32(Tokens::TX4));
        if (clicked) {
            if (locked) open_subminute_upsell();
            else if (chart) { chart->change_timeframe(t.sec); ImGui::CloseCurrentPopup(); }
        }
        if (rclick && !locked) tf_toggle_fav(t.sec);
        if (hov) Theme::tooltip(locked ? "Sub-minute candles are a Pro timeframe"
                                          : (fav ? "Right-click to unfavourite" : "Right-click to favourite"));
    }

    // The dropdown - grouped catalogue, favourites counter, custom field. Same
    // chrome, section headers and PRO tag as the other chart menus.
    void render_tf_menu(ChartWidget* chart) {
        using namespace Theme;
        const int64_t cur = chart ? chart->timeframe_seconds() : 0;
        const bool pro = Entitlements::is_pro();
        if (Theme::begin_menu("##tf_menu", 360.0f)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float menu_w = ImGui::GetContentRegionAvail().x;

            // TIMEFRAME ........................ * FAVOURITES N/6 (FULL BAR)
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                Theme::menu_section("TIMEFRAME");
                char fc[40];
                const int nfav = static_cast<int>(g_tf_favs.size());
                if (nfav >= 6) snprintf(fc, sizeof(fc), "FAVOURITES %d/6 \xC2\xB7 FULL BAR", nfav);
                else           snprintf(fc, sizeof(fc), "FAVOURITES %d/6", nfav);
                ImGui::PushFont(Fonts::label());
                const float fcw = ImGui::CalcTextSize(fc).x;
                const float fx = p.x + menu_w - 10.0f - fcw;
                dl->AddText(ImVec2(fx, p.y + 8.0f), u32(Tokens::TX3), fc);
                ac_ic_star(dl, ImVec2(fx - 8.0f, p.y + 8.0f + ImGui::GetFontSize() * 0.5f), 4.0f,
                           u32(Tokens::TX3));
                ImGui::PopFont();
            }
            Theme::menu_separator();

            // Real-time mode is NOT in this menu any more. It used to be a bare
            // "RT" pill in here, with no Pro badge for Free viewers, and Search
            // Console showed people googling "enable bubbles on edgedepth" after
            // seeing them in the welcome replay: the toggle was invisible until
            // the caret was opened. It is now a labelled pill drawn BESIDE this
            // control (render_tf_control), locked for Free on hosted live.

            auto draw_group = [&](const char* label, int lo, int hi, bool gated) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                Theme::menu_section(label);
                if (gated && !pro) {
                    ImGui::PushFont(Fonts::label());
                    const float lw = ImGui::CalcTextSize(label).x;
                    ImGui::PopFont();
                    const ImVec2 tag = Theme::pro_tag_size();
                    Theme::draw_pro_tag(dl, ImVec2(p.x + 10.0f + lw + 8.0f, p.y + 8.0f - (tag.y - 11.5f) * 0.5f - 1.0f));
                }
                ImGui::Indent(4.0f);
                for (int i = lo; i <= hi; ++i) {
                    if (i > lo) ImGui::SameLine(0.0f, 4.0f);
                    tf_pill(chart, TF_CATALOG[i], cur, pro, dl);
                }
                ImGui::Unindent(4.0f);
            };
            draw_group("SECONDS", 0, 3, true);
            draw_group("MINUTES", 4, 8, false);
            draw_group("HOURS", 9, 13, false);
            draw_group("DAYS", 14, 15, false);

            ImGui::Dummy(ImVec2(0, 4));
            Theme::menu_separator();
            Theme::menu_note("Click sets the chart. Right-click pins it to the bar (max 6).");
            ImGui::Dummy(ImVec2(0, 4));

            // Custom field + Add.
            ImGui::Indent(10.0f);
            ImGui::PushFont(Fonts::ui());
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Tokens::TX3, "Custom");
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens::INPUT);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::SetNextItemWidth(std::max(120.0f, menu_w - 140.0f));
            const bool entered = ImGui::InputTextWithHint("##tf_custom", "e.g. 7m, 90s, 3h",
                                     g_tf_custom, sizeof(g_tf_custom), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, Tokens::ELEV);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BD2);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ELEV);
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
            const bool add = ImGui::Button("Add") || entered;
            ImGui::PopStyleColor(5);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            ImGui::Unindent(10.0f);
            ImGui::Dummy(ImVec2(0, 4));
            if (add) {
                const int sec = tf_parse(g_tf_custom);
                if (sec <= 0) {
                    // ignore empty / unparseable input
                } else if (sec < 60 && !pro) {
                    open_subminute_upsell();
                } else if (chart) {
                    chart->change_timeframe(sec);
                    g_tf_custom[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // Inline favourites bar + caret + the Real-time pill. Drawn in the topbar
    // flow; advances the ImGui cursor past itself (via a trailing Dummy) so
    // following SameLine items align.
    //
    //   [1m][5m][15m][1h][4h][1D][v]   [o Real-time v]
    //
    // Real-time mode used to be a two-letter "RT" pill INSIDE the caret menu
    // and, while on, replaced the whole favourites row with one "RT" chip. It
    // now sits beside the selector with its full name, a lock for Free viewers
    // on hosted live (the same PRO idiom as the sub-minute group), and a caret
    // that opens RT settings while it is on. Favourites stay visible in RT
    // (dimmed); clicking one leaves real-time for that candle timeframe, which
    // is what change_timeframe already does.
    float render_tf_control(ChartWidget* chart) {
        using namespace Theme;
        const int64_t cur = chart ? chart->timeframe_seconds() : 0;
        const int current_tf = static_cast<int>(cur);
        const bool realtime = chart && chart->rt_mode();
        const bool rt_locked = chart && chart->rt_mode_locked();
        const bool compact = ImGui::GetContentRegionAvail().x < 760.0f;
        const std::span<const int> visible_favs = compact
            ? std::span<const int>(&current_tf, 1) : std::span<const int>(g_tf_favs);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(Fonts::ui());
        const float h = 30.0f, padx = 11.0f, caretw = 30.0f;
        static const char* const kRtLabel = "Real-time";
        // Pill geometry: dot + label + trailing glyph (lock while locked, caret
        // while on), inset 3px inside the 30px band like the "on" favourite chip.
        const float rt_gap = 10.0f, rt_padx = 10.0f, rt_dot = 6.0f, rt_dot_gap = 7.0f;
        const float rt_label_w = ImGui::CalcTextSize(kRtLabel).x;
        const float rt_trail_w = rt_locked ? Theme::pro_tag_size().x + 8.0f : realtime ? 16.0f : 0.0f;
        const float rt_w = rt_padx + rt_dot + rt_dot_gap + rt_label_w + rt_trail_w + rt_padx;
        float total = caretw + 1.0f;
        for (int sec : visible_favs) total += ImGui::CalcTextSize(tf_label(sec)).x + padx * 2.0f;
        total += rt_gap + rt_w;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // Flat favourites share the toolbar surface; selection carries the accent.

        float x = p0.x;
        for (int sec : visible_favs) {
            const char* lbl = tf_label(sec);
            const float w = ImGui::CalcTextSize(lbl).x + padx * 2.0f;
            const bool on = !realtime && (sec == cur);
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
            ImGui::PushID(sec);
            const bool clk = ImGui::InvisibleButton("##fav", ImVec2(w, h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clk && chart) chart->change_timeframe(sec);
            if (hov && realtime) Theme::tooltip("Leave real-time mode and show %s candles", lbl);
            // The active duration is a text tab, never a filled tile.
            if (on) dl->AddLine(ImVec2(x + padx, p0.y + h - 1),
                                ImVec2(x + w - padx, p0.y + h - 1), u32(Tokens::TX1), 2);
            if (hov) {
                dl->AddRectFilled(ImVec2(x + 1, p0.y + 1), ImVec2(x + w - 1, p0.y + h - 1), u32(Tokens::HOVER));
            }
            const ImVec2 ts = ImGui::CalcTextSize(lbl);
            // In real-time the timeframes do not apply (RT is a trade-driven
            // view), so the row dims to TX3 instead of pretending one is active.
            const ImVec4 fav_col = on ? Tokens::BRAND_TX
                                 : hov ? Tokens::TX1
                                 : realtime ? Tokens::TX3 : Tokens::TX2;
            dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f), u32(fav_col), lbl);
            x += w;
        }
        // caret cell - bg-2, line-2 left hairline, accent caret (2c)
        const bool menu_open = ImGui::IsPopupOpen("##tf_menu");
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        if (ImGui::InvisibleButton("##tf_caret", ImVec2(caretw, h))) ImGui::OpenPopup("##tf_menu");
        const bool chov = ImGui::IsItemHovered();
        if (chov) Theme::tooltip(realtime
            ? "All timeframes and favourites. Picking one leaves real-time mode and shows candles."
            : "All timeframes and favourites.");
        if (chov) dl->AddRectFilled(ImVec2(x + 1, p0.y + 1),
            ImVec2(x + caretw - 1, p0.y + h - 1), u32(Tokens::HOVER));
        {
            const float cx = x + caretw * 0.5f, cy = p0.y + h * 0.5f;
            const ImU32 cc = u32(Tokens::TX2);
            if (menu_open) dl->AddTriangleFilled(ImVec2(cx - 4, cy + 2), ImVec2(cx + 4, cy + 2), ImVec2(cx, cy - 3), cc);
            else           dl->AddTriangleFilled(ImVec2(cx - 4, cy - 2), ImVec2(cx + 4, cy - 2), ImVec2(cx, cy + 3), cc);
        }
        x += caretw + 1.0f;

        // ── Real-time pill ────────────────────────────────────────────────
        // on     = accent-soft fill, accent border, accent text, filled dot
        // off    = INPUT fill, BD2 border, TX2 text, hollow dot
        // locked = off colours + the mint PRO tag every Pro gate carries;
        //          click opens the upsell with the real-time preview
        //          (set_rt_mode owns that gate, so replay and local packs
        //          stay open without a second copy of the rule here).
        const float rx = x + rt_gap;
        const ImVec2 rp0(rx + 3.0f, p0.y + 3.0f), rp1(rx + rt_w - 3.0f, p0.y + h - 3.0f);
        ImGui::SetCursorScreenPos(ImVec2(rx, p0.y));
        const bool rt_clicked = ImGui::InvisibleButton("##rt_pill", ImVec2(rt_w, h));
        const bool rt_hov = ImGui::IsItemHovered();
        const bool settings_open = ImGui::IsPopupOpen("##rt_settings");
        const bool rt_hot = rt_hov || settings_open;
        if (rt_hov && rt_locked) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (rt_hot) dl->AddRectFilled(rp0, rp1, u32(Tokens::HOVER));
        if (realtime) dl->AddLine(ImVec2(rp0.x + 6, rp1.y),
            ImVec2(rp1.x - 6, rp1.y), u32(Tokens::TX1), 2);
        const ImVec4 rt_text_col = realtime ? Tokens::BRAND_TX
                                 : rt_locked ? (rt_hot ? Tokens::TX1 : Tokens::TX2)
                                 : (rt_hot ? Tokens::TX1 : Tokens::TX2);
        const ImU32 rt_ink = u32(rt_text_col);
        const float rt_mid = p0.y + h * 0.5f;
        float gx = rp0.x + rt_padx - 3.0f;
        // status dot: filled while streaming, hollow otherwise
        const ImVec2 dot_c(gx + rt_dot * 0.5f, rt_mid);
        if (realtime) dl->AddCircleFilled(dot_c, rt_dot * 0.5f, u32(Tokens::BRAND), 12);
        else          dl->AddCircle(dot_c, rt_dot * 0.5f, rt_ink, 12, 1.0f);
        gx += rt_dot + rt_dot_gap;
        {
            const ImVec2 ts = ImGui::CalcTextSize(kRtLabel);
            dl->AddText(ImVec2(gx, rt_mid - ts.y * 0.5f), rt_ink, kRtLabel);
            gx += rt_label_w;
        }
        const float trail_x0 = gx;                     // caret hit zone starts here
        if (rt_locked) {
            const ImVec2 tag = Theme::pro_tag_size();
            Theme::draw_pro_tag(dl, ImVec2(gx + 8.0f, rt_mid - tag.y * 0.5f));
        } else if (realtime) {
            const float cx = gx + 9.0f;
            const ImU32 cc = u32(Tokens::BRAND_TX);
            if (settings_open) dl->AddTriangleFilled(ImVec2(cx - 4, rt_mid + 2), ImVec2(cx + 4, rt_mid + 2), ImVec2(cx, rt_mid - 3), cc);
            else               dl->AddTriangleFilled(ImVec2(cx - 4, rt_mid - 2), ImVec2(cx + 4, rt_mid - 2), ImVec2(cx, rt_mid + 3), cc);
        }
        if (rt_hov) {
            if (rt_locked)     Theme::tooltip("Real-time mode: observed depth, every trade as a bubble, and the spread on one axis.\nLive real-time is a Pro view. Recorded replays open it for everyone.");
            else if (realtime) Theme::tooltip("Real-time mode is on. Click to return to candles, or use the caret for real-time settings (trade bubbles, 1s candles, depth).");
            else               Theme::tooltip("Switch to real-time mode: observed depth, every trade as a bubble, and the spread on one axis.");
        }
        if (rt_clicked && chart) {
            const bool on_caret = realtime && !rt_locked && ImGui::GetIO().MousePos.x >= trail_x0 - 4.0f;
            if (on_caret) ImGui::OpenPopup("##rt_settings");
            else          chart->toggle_rt_mode();
        }
        ImGui::PopFont();

        // layout anchor so the next SameLine item flows from the bar's right edge
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(total, h));

        // dropdown, pinned under the bar
        ImGui::SetNextWindowPos(ImVec2(p0.x, p0.y + h + 5.0f), ImGuiCond_Appearing);
        render_tf_menu(chart);
        // RT settings, pinned under the pill
        ImGui::SetNextWindowPos(ImVec2(rx, p0.y + h + 5.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(340, ImGui::GetMainViewport()->WorkSize.y - 80));
        if (Theme::begin_popup("##rt_settings")) {
            ImGui::TextColored(Tokens::TX2, "REAL-TIME SETTINGS");
            ImGui::Separator();
            if (chart && realtime) chart->render_realtime_settings();
            ImGui::EndPopup();
        }
        return total;
    }

    void render_topbar(std::vector<std::unique_ptr<Widget>>& widgets,
                       const AppContext& ctx) {
        using namespace Theme;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, Layout::topbar_h()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::ELEV);
        ImGui::Begin("##topbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoScrollbar);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // bottom hairline
        dl->AddLine(ImVec2(vp->Pos.x, vp->Pos.y + Layout::topbar_h() - 1.0f),
                    ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + Layout::topbar_h() - 1.0f),
                    u32(Tokens::BD1));

        const float cy = (Layout::TOPBAR_H - 30.0f) * 0.5f;

        // ── brand - D mark + edgedepth wordmark + EARLY ACCESS pill, links to
        //    the homepage. Mirrors the web AppHeader / design-system Brand lockup:
        //    the mark reads at ~wordmark height, a 9px mark->word gap and a 12px
        //    word->pill gap, and a hairline accent pill. ──
        const float mark_h = 12.0f;                            // streak-block height (~= wordmark)
        const float mark_w = 272.0f * mark_h / 104.0f;         // full mark render width (~31px)
        const float gap1 = 9.0f, gap2 = 12.0f;                 // mark->word, word->pill
        ImGui::PushFont(Fonts::ui_semibold());
        const float word_w  = ImGui::CalcTextSize("edgedepth").x;
        const float word_fs = ImGui::GetFontSize();
        ImGui::PopFont();
        // EARLY ACCESS pill - the web .beta badge is Inter (--font-sans) 600 at
        // 10px with 0.06em tracking, a 1px --accent border and --accent-text
        // ink. Fonts::label() is the terminal's Inter SemiBold micro-label face;
        // the mono face it used before read as a different product.
        static const char* const kBeta = "EARLY ACCESS";
        ImGui::PushFont(Fonts::label());
        ImFont*      beta_font = ImGui::GetFont();
        const float  beta_fs   = ImGui::GetFontSize();
        const ImVec2 beta_ts0  = ImGui::CalcTextSize(kBeta);
        ImGui::PopFont();
        const float beta_track  = beta_fs * 0.06f;              // 0.06em letter-spacing (web .beta)
        const float beta_text_w = beta_ts0.x + beta_track * static_cast<float>(strlen(kBeta) - 1);
        const ImVec2 beta_ts(beta_text_w, beta_ts0.y);
        const float beta_padx = 6.0f, beta_pady = 2.0f;
        const float beta_w = beta_ts.x + beta_padx * 2.0f;
        const float beta_h = beta_ts.y + beta_pady * 2.0f;
        const float brand_w = mark_w + gap1 + word_w + gap2 + beta_w;

        ImGui::SetCursorPos(ImVec2(12.0f, cy));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const bool brand_click = ImGui::InvisibleButton("##brand_home", ImVec2(brand_w, 30.0f));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float mid = bp.y + 15.0f;                        // vertical centre of the band

        draw_brand_mark(dl, ImVec2(bp.x, mid - mark_h * 0.5f), mark_h);

        ImGui::PushFont(Fonts::ui_semibold());
        dl->AddText(ImVec2(bp.x + mark_w + gap1, mid - word_fs * 0.5f), u32(Tokens::TX1), "edgedepth");
        ImGui::PopFont();

        const float beta_x = bp.x + mark_w + gap1 + word_w + gap2;
        draw_tracked_text(dl, beta_font, beta_fs,
                          ImVec2(beta_x + beta_padx, mid - beta_ts.y * 0.5f),
                          u32(Tokens::LOGO_TX), kBeta, beta_track);

        if (brand_click) {
#ifdef __EMSCRIPTEN__
            EM_ASM({ window.location.href = "/"; });
#endif
        }

        // divider
        ImGui::SameLine(0.0f, 14.0f);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, vp->Pos.y + 12.0f),
                        ImVec2(p.x, vp->Pos.y + Layout::TOPBAR_H - 12.0f),
                        u32(Tokens::BD2));
        }
        ImGui::SameLine(0.0f, 14.0f);

        // ── symbol pill ──────────────────────────────────────────────────────
        ImGui::SetCursorPosY(cy);
        symbol_pill();

        // Timeframe selectors + Indicators now live in the chart toolbar (v2 3b);
        // the top bar no longer duplicates them.
        ChartWidget* chart = find_primary_chart(widgets);
        (void)chart;

        // ── right cluster, placed right-to-left so it stays flush-right ───────
        // Courses · | · Live/Replay · | · Default · +  (theme)  (fullscreen)
        const float top_y = vp->Pos.y + (vp->Size.x < 1150 ? 38.0f : 0.0f);
        const float icy   = top_y + (Layout::TOPBAR_H - 30.0f) * 0.5f;
        const float ico   = 30.0f, igap = 5.0f, dvw = 13.0f;
        ImGui::PushFont(Fonts::ui_semibold());
        const float courses_w = ImGui::CalcTextSize("Courses").x + 20.0f;
        const float default_w = ImGui::CalcTextSize("Workspace").x + 22.0f;
        const float live_w    = ImGui::CalcTextSize("Live").x   + 22.0f;  // padx 11*2
        const float replay_w  = ImGui::CalcTextSize("Replay").x + 22.0f;
        ImGui::PopFont();
        const float seg_w = live_w + replay_w;
        const bool  replaying = ctx.replayer && ctx.replayer->is_active();

        float rx = vp->Pos.x + ImGui::GetWindowWidth() - 10.0f;  // right edge (screen x)
        auto vdiv = [&]() {
            rx -= dvw;
            const float lx = rx + dvw * 0.5f;
            dl->AddLine(ImVec2(lx, top_y + 12.0f),
                        ImVec2(lx, top_y + Layout::TOPBAR_H - 12.0f), u32(Tokens::BD2));
        };

        // One quiet account action; identity and entitlement details live inside.
        if (Entitlements::hosted()) {
            rx -= 38.0f;
            ImGui::SetCursorScreenPos(ImVec2(rx, icy));
            if (ico_btn("##account", 32.0f)) ImGui::OpenPopup("##account_menu");
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImU32 ink = u32(Tokens::TX2);
            dl->AddCircle(ImVec2(a.x + 16, a.y + 11), 4, ink, 16, 1.5f);
            dl->PathArcTo(ImVec2(a.x + 16, a.y + 25), 8, 3.14159265f, 6.28318530f, 16);
            dl->PathStroke(ink, 0, 1.5f);
            if (ImGui::IsItemHovered()) Theme::tooltip("Account");
        }

        // fullscreen
        rx -= ico; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        const bool full_clk = ico_btn("##tb_full", ico);
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float m = 9.0f, L = 5.0f; const ImU32 c = u32(Tokens::TX2);
            dl->AddLine(ImVec2(a.x+m, a.y+m), ImVec2(a.x+m+L, a.y+m), c, 1.4f);
            dl->AddLine(ImVec2(a.x+m, a.y+m), ImVec2(a.x+m, a.y+m+L), c, 1.4f);
            dl->AddLine(ImVec2(b.x-m, a.y+m), ImVec2(b.x-m-L, a.y+m), c, 1.4f);
            dl->AddLine(ImVec2(b.x-m, a.y+m), ImVec2(b.x-m, a.y+m+L), c, 1.4f);
            dl->AddLine(ImVec2(a.x+m, b.y-m), ImVec2(a.x+m+L, b.y-m), c, 1.4f);
            dl->AddLine(ImVec2(a.x+m, b.y-m), ImVec2(a.x+m, b.y-m-L), c, 1.4f);
            dl->AddLine(ImVec2(b.x-m, b.y-m), ImVec2(b.x-m-L, b.y-m), c, 1.4f);
            dl->AddLine(ImVec2(b.x-m, b.y-m), ImVec2(b.x-m, b.y-m-L), c, 1.4f);
        }
        if (full_clk) {
#ifdef __EMSCRIPTEN__
            EM_ASM({ if (document.fullscreenElement) document.exitFullscreen();
                     else document.documentElement.requestFullscreen(); });
#endif
        }
        rx -= igap;

        // drawing tools - pencil + dropdown (mirrors the left rail)
        rx -= ico; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        if (ctx.drawings) drawing::render_topbar_button(ctx.drawing_mgr());
        rx -= igap;

        // theme icon → Tweaks panel (accent · candles · density · heat)
        rx -= ico; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        if (ico_btn("##tb_theme", ico)) g_tweaks_open = !g_tweaks_open;
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImVec2 c((a.x+b.x)*0.5f, (a.y+b.y)*0.5f); const ImU32 col = u32(Tokens::TX2);
            dl->AddCircle(c, 4.0f, col, 12, 1.4f);
            for (int i = 0; i < 8; ++i) {
                const float ang = i * 0.785398f; const float dx = cosf(ang), dy = sinf(ang);
                dl->AddLine(ImVec2(c.x+dx*6.0f, c.y+dy*6.0f),
                            ImVec2(c.x+dx*8.0f, c.y+dy*8.0f), col, 1.2f);
            }
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Appearance");
        rx -= igap;

        // (The "+ add widget" button moved to the chart toolbar as "+ widget",
        //  beside the layers control, so it renders in EVERY chrome including the
        //  embedded /demo + event replays where this native topbar is suppressed.)

        // Default (layout) menu
        rx -= default_w; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        if (tb_button("Workspace", default_w)) ImGui::OpenPopup("##tb_layout");

        vdiv();

        // Live <-> Replay toggle (functional)
        rx -= seg_w;
        ImGui::SetCursorScreenPos(ImVec2(rx, top_y + (Layout::TOPBAR_H - 26.0f) * 0.5f));
        static const char* const VIEW_LBL[] = {"Live", "Replay"};
        const int view_clicked = seg_control("viewseg", VIEW_LBL, 2, replaying ? 1 : 0);
        if (view_clicked == 0 && replaying && ctx.replayer) {
            ctx.replayer->stop();                 // Live → exit replay, back to live
        } else if (view_clicked == 1 && !replaying) {
            // Replay always starts from a chosen PAST candle (right-click → "Replay
            // from here") - the toggle is a status + exit control, not a way to
            // fabricate an arbitrary window. Nudge discovery of the right-click entry.
            ImGui::OpenPopup("##replay_hint");
        }
        if (Theme::begin_popup("##replay_hint")) {
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
            ImGui::TextUnformatted("Right-click a candle on the chart, then \xE2\x80\x9CReplay from here\xE2\x80\x9D.");
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::MenuItem("Open Replay Library")) {
                Menu::g_widget_add_request.type =
                    Menu::SymbolPickerState::PendingWidget::ReplayLibrary;
                Menu::g_widget_add_request.pending = true;
            }
            ImGui::EndPopup();
        }

        vdiv();

        // Courses. Hosted only: self-hosted this was a same-origin relative
        // navigation to localhost:8080/courses, which the bundled nginx 404s,
        // so it replaced the terminal with an error page.
        //
        // The target is absolute and opens a new tab even on the hosted build,
        // because /courses does not exist there either. The live hub is /learn;
        // this line predates it and had been dead on every host since.
        if (Entitlements::hosted()) {
            rx -= courses_w; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
            if (tb_button("Courses", courses_w)) {
#ifdef __EMSCRIPTEN__
                EM_ASM({
                    window.open('https://app.edgedepth.com/learn?utm_source=terminal'
                                + '&utm_medium=topbar&utm_campaign=courses',
                                '_blank', 'noopener');
                });
#endif
            }
        }

        // (Top-bar Indicators popup removed in v2 3b; the chart toolbar owns it.
        //  The "##tb_widget" add-widget popup likewise moved to the chart toolbar
        //  ("+ widget", beside the layers control) - see ChartWidget::render_controls.)
        {
            static bool show_demo = false, show_metrics = false;
            if (Theme::begin_popup("##tb_layout")) {
                if (ImGui::MenuItem("Reset Layout")) workspace::reset_default();
                workspace::menu();
                ImGui::MenuItem("Market Header", nullptr, &g_market_header_open);
                if (ImGui::BeginMenu("Chart shortcuts")) {
                    ImGui::TextUnformatted("Shift + drag: select a move");
                    ImGui::TextUnformatted("Right-click: inspect or replay");
                    ImGui::TextUnformatted("Esc: clear selection or close menu");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::MenuItem("ImGui Demo", nullptr, &show_demo);
                ImGui::MenuItem("Metrics", nullptr, &show_metrics);
                ImGui::EndPopup();
            }
            if (show_demo) ImGui::ShowDemoWindow(&show_demo);
            if (show_metrics) ImGui::ShowMetricsWindow(&show_metrics);
        }

        // Account dropdown (opened by the user pill above, which only exists on
        // the hosted build).
        if (Entitlements::hosted()) render_account_menu();

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}  // namespace

namespace {
    void render_statsbar(const AppContext& ctx) {
        using namespace Theme;
        if (!g_market_header_open) return;   // hidden - dockspace reclaims the space
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + Layout::topbar_h()));
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, Layout::STATSBAR_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::PANEL);
        ImGui::Begin("##statsbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoScrollbar);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(vp->Pos.x, vp->Pos.y + Layout::topbar_h() + Layout::STATSBAR_H - 1.0f),
                    ImVec2(vp->Pos.x + vp->Size.x,
                           vp->Pos.y + Layout::topbar_h() + Layout::STATSBAR_H - 1.0f),
                    u32(Tokens::BD1));

        // Positioning (long/short + liq) read from the AnalyticsManager via ctx;
        // mark/OI/funding via g_stats, 24h change/vol via ticker.
        const PositioningState* pos = ctx.analytics_mgr().get_positioning(Terminal::pair_key(g_pair.exchange, g_pair.symbol));
        const TickerEntry* tick = TickerManager::instance().get(g_pair.exchange, g_pair.symbol);
        char v[80], sm[64];

        // A single reading line. Secondary context remains one action away.
        char last[80] = "Unavailable", change[32] = "Unavailable";
        char oi[32] = "Unavailable", funding[32] = "Unavailable";
        char volume[32] = "Unavailable", mark[80] = "Unavailable";
        if (tick && tick->last_price > 0) {
            g_fmt.format_price(last, sizeof(last), tick->last_price);
            snprintf(change, sizeof(change), "%+.2f%%", tick->change_pct_24h);
            fmt_compact_usd(volume, sizeof(volume), tick->volume_quote);
        }
        if (g_stats.has_data) {
            g_fmt.format_price(mark, sizeof(mark), g_stats.mark_price);
            snprintf(funding, sizeof(funding), "%+.4f%%", g_stats.funding * 100.0);
            // This legacy field carries contract quantity; convert with mark.
            const double notional = g_stats.open_interest_usd * g_stats.mark_price;
            if (notional > 0) fmt_compact_usd(oi, sizeof(oi), notional);
        }
        const float y = vp->Pos.y + Layout::topbar_h() + 9;
        float x = vp->Pos.x + 14;
        auto metric = [&](const char* label, const char* value, const ImVec4& color) {
            ImGui::PushFont(Fonts::ui());
            const float label_w = ImGui::CalcTextSize(label).x;
            ImGui::PushFont(Fonts::ui_semibold());
            const float value_w = ImGui::CalcTextSize(value).x;
            ImGui::PopFont();
            if (x + label_w + 8 + value_w > vp->Pos.x + vp->Size.x - 148) {
                ImGui::PopFont();
                return;
            }
            dl->AddText(ImVec2(x, y), u32(Tokens::TX3), label);
            x += ImGui::CalcTextSize(label).x + 8;
            ImGui::PopFont();
            ImGui::PushFont(Fonts::ui_semibold());
            dl->AddText(ImVec2(x, y), u32(color), value);
            x += ImGui::CalcTextSize(value).x + 28;
            ImGui::PopFont();
        };
        metric("Last", last, Tokens::TX1);
        metric("24h", change, tick ? (tick->change_pct_24h >= 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX3);
        if (vp->Size.x >= 850) metric("Funding", funding, Tokens::TX2);
        if (vp->Size.x >= 1150) metric("Open interest", oi, Tokens::TX2);
        if (vp->Size.x >= 1500) metric("Volume 24h", volume, Tokens::TX2);
        ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + vp->Size.x - 132, y - 5));
        if (ImGui::Button("Market details", ImVec2(120, 28))) ImGui::OpenPopup("##market_details");
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 368,
            vp->Pos.y + Layout::topbar_h() + Layout::STATSBAR_H));
        ImGui::SetNextWindowSize(ImVec2(360, 0));
        if (Theme::begin_popup("##market_details")) {
            Theme::section_label("Market context");
            auto row = [&](const char* label, const char* value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(Tokens::TX2, "%s", label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value);
            };
            if (ImGui::BeginTable("##market_values", 2, ImGuiTableFlags_SizingStretchProp)) {
                row("Last traded price", last);
                row("Mark price", mark);
                row("Change / 24h", change);
                if (tick) {
                    const double denom = 1 + tick->change_pct_24h / 100;
                    const double delta = denom != 0 ? tick->last_price *
                        (tick->change_pct_24h / 100) / denom : 0;
                    g_fmt.format_price(v, sizeof(v), delta);
                    row("Price change / 24h", v);
                }
                row("Volume / 24h", volume);
                row("Open interest / USD", oi);
                row("Funding rate", funding);
                if (g_stats.has_data && g_stats.next_funding_time > 0) {
                    fmt_funding_countdown(sm, sizeof(sm), g_stats.next_funding_time);
                    row("Next funding in", sm);
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            Theme::section_label("Long / short accounts");
            if (pos && pos->global_long_account > 0) {
                const double lg = std::clamp(pos->global_long_account, 0.0, 1.0);
                ImGui::Text("Long %.0f%%   /   Short %.0f%%", lg * 100, (1 - lg) * 100);
            } else ImGui::TextDisabled("Unavailable");
            ImGui::TextWrapped("Share of accounts on each side. This is not position size or open-interest share.");
            ImGui::Separator();
            Theme::section_label("Reported liquidations / 24h");
            double longs = pos ? pos->long_liq_usd : 0;
            double shorts = pos ? pos->short_liq_usd : 0;
            if (longs <= 0 && shorts <= 0 && g_stats.has_data) {
                longs = g_stats.liq_long_usd;
                shorts = g_stats.liq_short_usd;
            }
            if (longs + shorts > 0) {
                fmt_compact_usd(v, sizeof(v), longs + shorts);
                ImGui::Text("Total %s", v);
                fmt_compact_usd(v, sizeof(v), longs);
                fmt_compact_usd(sm, sizeof(sm), shorts);
                ImGui::Text("Long %s   /   Short %s", v, sm);
            } else ImGui::TextDisabled("Unavailable");
            ImGui::Separator();
            if (ImGui::MenuItem("Hide market strip")) g_market_header_open = false;
            ImGui::EndPopup();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }
    // ── Tweaks panel - opened by the topbar theme icon. Wires the runtime theme
    // hooks: accent swatches, candle convention, density, the primary chart's
    // liq-heatmap opacity, and the liq colormap (Ember/Inferno/Magma/Viridis).
    void render_tweaks_panel(std::vector<std::unique_ptr<Widget>>& widgets) {
        if (!g_tweaks_open) return;
        using namespace Theme;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 296.0f,
                                       vp->Pos.y + Layout::topbar_h() + 6.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(286.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::ELEV);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, Tokens::PANEL);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        if (ImGui::Begin("Appearance", &g_tweaks_open,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings)) {
            auto section = [&](const char* s) {
                ImGui::Dummy(ImVec2(0, 3));
                ImGui::PushFont(Fonts::label());
                ImGui::TextColored(Tokens::TX4, "%s", s);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 2));
            };

            Theme::render_appearance_controls();

            // Density → Theme::set_density (clamps 3..10, re-derives row height)
            section("DENSITY");
            int d = density();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##tw_density", &d, 3, 10, "density %d")) set_density(d);

            // Heat strength → primary chart liq-heatmap opacity
            if (ChartWidget* c = find_primary_chart(widgets)) {
                section("HEAT STRENGTH");
                float h = c->liq_opacity();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##tw_heat", &h, 0.0f, 1.0f, "%.2f")) c->set_liq_opacity(h);
            }

            // Liq colormap → HeatmapColormap::set_liq_map (routes the Field + Profile +
            // legend LUT; Ember-K is the design default, the rest stay selectable).
            section("LIQ COLORMAP");
            auto lmap_btn = [&](const char* label, HeatmapColormap::LiqMap m) {
                const bool on = (HeatmapColormap::liq_map() == m);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? Tokens::BRAND_SOFT : Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Tokens::BRAND_SOFT : Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? Tokens::BRAND : Tokens::TX2);
                if (ImGui::Button(label)) HeatmapColormap::set_liq_map(m);
                ImGui::PopStyleColor(4);
            };
            lmap_btn("Ember",   HeatmapColormap::LiqMap::Ember);
            ImGui::SameLine(0.0f, 5.0f);
            lmap_btn("Inferno", HeatmapColormap::LiqMap::Inferno);
            ImGui::SameLine(0.0f, 5.0f);
            lmap_btn("Magma",   HeatmapColormap::LiqMap::Magma);
            ImGui::SameLine(0.0f, 5.0f);
            lmap_btn("Viridis", HeatmapColormap::LiqMap::Viridis);

            // WS2 A/B: Field render path - texture quad (GPU) vs per-rect (CPU).
            // The rect path stays selectable until the texture pass is approved.
            if (ChartWidget* c = find_primary_chart(widgets)) {
                section("LIQ FIELD RENDER");
                auto fr_btn = [&](const char* label, bool tex) {
                    const bool on = (c->liq_field_use_texture() == tex);
                    ImGui::PushStyleColor(ImGuiCol_Button, on ? Tokens::BRAND_SOFT : Tokens::INPUT);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Tokens::BRAND_SOFT : Tokens::HOVER);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
                    ImGui::PushStyleColor(ImGuiCol_Text, on ? Tokens::BRAND : Tokens::TX2);
                    if (ImGui::Button(label)) c->set_liq_field_use_texture(tex);
                    ImGui::PopStyleColor(4);
                };
                fr_btn("Texture (GPU)", true);
                ImGui::SameLine(0.0f, 5.0f);
                fr_btn("Rects (CPU)", false);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }

    // ── Bottom status bar - owns telemetry (WS · FPS · CLOCK), off the tape ──
    // Chrome rules: 1px line-1 top border, bg-1 fill, mono micro-text. Height =
    // Layout::STATUSBAR_H (30); the dockspace + replay bar reserve it via
    // LayoutManager::status_reserve.
    void draw_statusbar(const AppContext& ctx, const std::string& symbol_lc, const WebSocketClient* transport, bool local_pack) {
        using namespace Theme;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float bar_y = vp->Pos.y + vp->Size.y - Layout::STATUSBAR_H;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bar_y));
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, Layout::STATUSBAR_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::PANEL);
        ImGui::Begin("##statusbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoScrollbar);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(vp->Pos.x, bar_y), ImVec2(vp->Pos.x + vp->Size.x, bar_y),
                    u32(Tokens::BD1));

        ImGui::PushFont(Fonts::ui());
        const float th = ImGui::GetFontSize();
        const float ty = bar_y + (Layout::STATUSBAR_H - th) * 0.5f;

        // left - connection + market context
        std::string sym = symbol_lc;
        for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (ctx.replay_mgr().is_active() && !local_pack)
            transport = ctx.replay_mgr().active_socket();
        const bool ws_ok = local_pack || (transport && transport->is_connected());
        char status[80] = "WS DISCONNECTED";
        if (local_pack) snprintf(status, sizeof(status), "LOCAL REPLAY");
        else if (transport) transport->format_connection_status(status, sizeof(status));
        const char* mode = ctx.replay_mgr().transport_interrupted() ? "Replay interrupted" :
            ctx.replay_mgr().is_active() ? "Replay" :
            ctx.stream_mgr().live_subscriptions_paused() ? "Paused" : "Live";
        char left[192];
        snprintf(left, sizeof(left), "%s  ·  %s  ·  %s", sym.c_str(), mode, local_pack ? "Local recording" : ws_ok ? "Connected" : "Disconnected");
        const float dot_x = vp->Pos.x + 14.0f;
        // coin logo at the far left (monogram fallback while loading / if missing)
        const float sb_logo = 13.0f;
        LogoManager::instance().draw_coin(dl, g_base_asset,
            ImVec2(dot_x, bar_y + (Layout::STATUSBAR_H - sb_logo) * 0.5f), sb_logo);
        const float sb_x = dot_x + sb_logo + 8.0f;
        dl->AddCircleFilled(ImVec2(sb_x + 3.0f, bar_y + Layout::STATUSBAR_H * 0.5f), 3.0f,
                            u32(ws_ok ? Tokens::TX2 : Tokens::WARN));
        dl->AddText(ImVec2(sb_x + 12.0f, ty), u32(Tokens::TX2), left);

        // Self-hosted feeds never deliver the hosted analytics streams. One
        // low-key pointer in the shell explains the panels that stay empty.
        // Keyed on frame absence, not entitlements: bare mode defaults to
        // Pro, so entitlements cannot tell the feeds apart. Hidden while a
        // replay runs (replay data is not the live feed's fault) and on
        // narrow bars where it would collide with the telemetry.
        // "Feed is real" = any live market frame ever arrived; ws_ok is the
        // stats strip's has_data, which itself rides a hosted-ish stream
        // (ticker24h) and is false on networks that do not serve it.
        auto& presence = StreamPresence::instance();
        const bool feed_alive =
            presence.seen(static_cast<uint32_t>(Terminal::Stream::Trades)) ||
            presence.seen(static_cast<uint32_t>(Terminal::Stream::Orderbook));
        if (feed_alive && !ctx.replay_mgr().is_active() && vp->Size.x >= 1080.0f &&
            presence.absent(
                static_cast<uint32_t>(Terminal::Stream::PositioningState))) {
            const char* note = "SELF-HOSTED FEED \xc2\xb7 OPEN A FULL REPLAY";
            const float note_x = sb_x + 12.0f + ImGui::CalcTextSize(left).x + 26.0f;
            const ImVec2 nts = ImGui::CalcTextSize(note);
            ImGui::SetCursorScreenPos(ImVec2(note_x, bar_y + 2.0f));
            const bool note_clicked = ImGui::InvisibleButton(
                "##selfhosted_note", ImVec2(nts.x, Layout::STATUSBAR_H - 4.0f));
            const bool note_hov = ImGui::IsItemHovered();
            dl->AddText(ImVec2(note_x, ty),
                        u32(note_hov ? Tokens::BRAND_TX : Tokens::TX3), note);
            if (note_hov) {
                Theme::tooltip("Your raw feed does not carry every hosted analytics layer.\n"
                               "Click to open a curated event with recorded order book,\n"
                               "tape, liquidations, footprint and volume profile data.");
            }
            if (note_clicked) {
                Menu::g_widget_add_request.type =
                    Menu::SymbolPickerState::PendingWidget::ReplayLibrary;
                Menu::g_widget_add_request.pending = true;
            }
        }

        // right - replay state + presentation telemetry + interactive display zone/clock.
        // The selected zone only formats the epoch; it never changes replay time.
        const bool replaying = ctx.replay_mgr().is_active();
        const float fr = ImGui::GetIO().Framerate;
        char telemetry[112];
        snprintf(telemetry, sizeof(telemetry), "%.0f fps", fr);

        const int64_t display_epoch_ms = replaying
            ? ctx.replay_mgr().interpolated_time_ms()
            : static_cast<int64_t>(time(nullptr)) * 1000;
        auto& time_zone = DisplayTimeZone::instance();
        char clock[16] = "--:--:--";
        time_zone.format(display_epoch_ms, TimeZoneFormat::TimeSeconds, clock, sizeof(clock));
        char zone[160]{};
        const bool compact_zone = true;
        time_zone.visible_label(display_epoch_ms, compact_zone, zone, sizeof(zone));
        char zone_clock[192];
        snprintf(zone_clock, sizeof(zone_clock), "%s \xc2\xb7 %s", zone, clock);

        const float zone_pad_x = 7.0f;
        const float zone_w = ImGui::CalcTextSize(zone_clock).x + zone_pad_x * 2.0f;
        const float zone_x = vp->Pos.x + vp->Size.x - 7.0f - zone_w;
        const ImVec2 zone_p(zone_x, bar_y + 2.0f);
        ImGui::SetCursorScreenPos(zone_p);
        const bool zone_clicked = ImGui::InvisibleButton(
            "##status_time_zone", ImVec2(zone_w, Layout::STATUSBAR_H - 4.0f));
        dl->AddText(ImVec2(zone_x + zone_pad_x, ty),
                    u32(Tokens::TX2), zone_clock);
        dl->AddTriangleFilled(ImVec2(zone_x + zone_w - 6.0f, ty + th * 0.45f),
                              ImVec2(zone_x + zone_w - 2.0f, ty + th * 0.45f),
                              ImVec2(zone_x + zone_w - 4.0f, ty + th * 0.7f),
                              u32(Tokens::TX3));
        if (zone_clicked) ImGui::OpenPopup("##time_zone_picker");
        ImGui::SetNextWindowPos(ImVec2(zone_x + zone_w, bar_y - 5.0f),
                                ImGuiCond_Appearing, ImVec2(1.0f, 1.0f));
        render_time_zone_picker(display_epoch_ms);

        const float telemetry_w = ImGui::CalcTextSize(telemetry).x;
        const float left_end = sb_x + 12.0f + ImGui::CalcTextSize(left).x;
        if (zone_x - 24.0f - telemetry_w > left_end + 24.0f) {
            const ImVec2 p(zone_x - 20.0f - telemetry_w, bar_y + 2.0f);
            ImGui::SetCursorScreenPos(p);
            ImGui::InvisibleButton("##status_performance", ImVec2(telemetry_w + 10.0f, Layout::STATUSBAR_H - 4.0f));
            dl->AddText(ImVec2(p.x, ty), u32(Tokens::TX3), telemetry);
            if (ImGui::IsItemHovered()) {
                // The counter is loop cadence, so say what bounds it: a rAF
                // loop cannot exceed the monitor's refresh rate by design.
                const char* cadence = "";
#ifdef __EMSCRIPTEN__
                int scheduler_mode = -1, scheduler_value = 0;
                emscripten_get_main_loop_timing(&scheduler_mode, &scheduler_value);
                if (scheduler_mode == EM_TIMING_RAF && scheduler_value > 1)
                    cadence = "\nPresenting every few refreshes while another window"
                              " has focus; move the pointer here for the full rate";
                else if (scheduler_mode == EM_TIMING_RAF)
                    cadence = "\nPresented once per display refresh (vsync);"
                              " frames above the monitor rate are never shown";
#endif
                Theme::tooltip("%.0f frames/s · %.1f ms per presented frame%s\n%s",
                               fr, fr > 0.0f ? 1000.0f / fr : 0.0f, cadence, status);
            }
        }
        ImGui::PopFont();

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}  // namespace

    // Public wrapper so the chart toolbar (chart_widget) can host the timeframe
    // bar; render_tf_control lives in this TU's anonymous namespace.
    float render_chart_tf(ChartWidget* chart) { return render_tf_control(chart); }

    // Public wrapper: draw ONLY the bottom telemetry bar (no topbar/statsbar).
    // Used by the embedded event (archive) + demo (pack) chromes, which suppress
    // the native shell but still want the live terminal's status strip. Symbol +
    // connection flag are passed in because AppShell::init (which sets g_pair and
    // subscribes stats) is intentionally skipped in those modes - its live
    // ticker24h subscription would leak into a historical replay.
    void render_statusbar(const AppContext& ctx, const std::string& symbol_lc, const WebSocketClient* transport, bool local_pack) {
        draw_statusbar(ctx, symbol_lc, transport, local_pack);
    }

    // A planned hub restart (redeploy) closes the socket with an ETA. Say so,
    // in our own words, instead of leaving a stale chart: a countdown while it
    // is down, a short "back" note once reconnected. Unplanned drops keep the
    // statusbar's WS RETRY telemetry only.
    void draw_restart_banner(const WebSocketClient* transport) {
        using namespace Theme;
        if (!transport) return;
        const bool down = transport->planned_restart_active();
        const double back_age = transport->reconnected_age_ms();
        const bool just_back = !down && back_age >= 0 && back_age < 6000 && transport->last_outage_ms() > 0;
        if (!down && !just_back) return;

        char title[96];
        const char* body;
        if (down) {
            const double s = transport->planned_restart_remaining_ms() / 1000.0;
            if (s > 0.5) snprintf(title, sizeof(title), "Deploying an update - back in about %.0f s", s);
            else snprintf(title, sizeof(title), "Deploying an update - reconnecting");
            body = "EdgeDepth is in early access and we ship improvements several times a day, "
                   "so short pauses like this one happen for now. Your layout and streams resume on their own.";
        } else {
            snprintf(title, sizeof(title), "Back online - refreshing the last %.0f seconds",
                     transport->last_outage_ms() / 1000.0);
            body = "Live streams are resubscribed; the candles for the pause are being refetched.";
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::PushFont(Fonts::ui());
        const float max_w = std::min(720.0f, vp->Size.x - 40.0f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                       vp->Pos.y + Layout::topbar_h() + Layout::STATSBAR_H + 14.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(max_w, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, down ? Tokens::WARN : Tokens::UP);
        ImGui::Begin("##restart_banner", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 org = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        dl->AddRectFilled(org, ImVec2(org.x + sz.x, org.y + sz.y),
                          u32(down ? Tokens::WARN_SOFT : Tokens::UP_SOFT), 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + max_w - 32.0f);
        ImGui::TextUnformatted(body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
    }

    void render(std::vector<std::unique_ptr<Widget>>& widgets,
                const AppContext& ctx, const WebSocketClient* transport) {
        // Cmd/Ctrl-K → Finder (the symbol picker modal; row select switches symbol).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_K)) {
            Menu::g_symbol_picker.pending = Menu::SymbolPickerState::PendingWidget::Charts;
            Menu::g_symbol_picker.open = true;
            Menu::g_symbol_picker.search_buf[0] = '\0';
            Menu::g_symbol_picker.replace_mode = true;
        }
        render_topbar(widgets, ctx);
        render_statsbar(ctx);
        draw_statusbar(ctx, g_pair.symbol, transport, false);
        draw_restart_banner(transport);
        render_tweaks_panel(widgets);
        // the symbol picker popup is opened from the topbar / Cmd-K now
        Menu::render_symbol_picker_popup(widgets, ctx);
    }
}  // namespace AppShell
