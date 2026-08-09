#include "rendering/app_shell.h"

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
#include "replayer/replay_manager.h"
#include "core/symbol_metadata.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "core/analytics_manager.h"
#include "stream_handler.h"
#include "ui/positions_panel.h"
#include "ui/chart_widget.h"
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
        if (ImGui::BeginPopup("##time_zone_picker")) {
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
    // last, so the EARLY ACCESS pill matches the web .beta badge's 0.12em track.
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
    // height (the streak+bowl block spans 104 SVG units, y 14..118).
    void draw_brand_mark(ImDrawList* dl, ImVec2 pos, float h) {
        const ImU32 col = u32(Tokens::BRAND);
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
            ImGui::PushStyleColor(ImGuiCol_Button, on ? Tokens::ACTIVE : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Tokens::ACTIVE : Tokens::HOVER);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? Tokens::TX1 : Tokens::TX3);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R1);
            if (ImGui::Button(items[i], ImVec2(w, h))) clicked = i;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
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
    void ac_ic_gear(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
        dl->AddCircle(c, s * 0.22f, col, 16, th);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.785398f, dx = cosf(a), dy = sinf(a);
            dl->AddLine(ImVec2(c.x + dx * s * 0.32f, c.y + dy * s * 0.32f),
                        ImVec2(c.x + dx * s * 0.46f, c.y + dy * s * 0.46f), col, th);
        }
    }
    void ac_ic_clock(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
        dl->AddCircle(c, s * 0.42f, col, 22, th);
        dl->AddLine(c, ImVec2(c.x, c.y - s * 0.26f), col, th);
        dl->AddLine(c, ImVec2(c.x + s * 0.20f, c.y + s * 0.08f), col, th);
    }
    void ac_ic_card(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
        const ImVec2 a(c.x - s * 0.46f, c.y - s * 0.30f), b(c.x + s * 0.46f, c.y + s * 0.30f);
        dl->AddRect(a, b, col, 2.0f, 0, th);
        dl->AddLine(ImVec2(a.x, a.y + s * 0.22f), ImVec2(b.x, a.y + s * 0.22f), col, th);
    }
    void ac_ic_logout(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
        const float w = s * 0.42f, h = s * 0.42f;
        dl->AddLine(ImVec2(c.x + s * 0.06f, c.y - h), ImVec2(c.x + w, c.y - h), col, th);
        dl->AddLine(ImVec2(c.x + w, c.y - h), ImVec2(c.x + w, c.y + h), col, th);
        dl->AddLine(ImVec2(c.x + w, c.y + h), ImVec2(c.x + s * 0.06f, c.y + h), col, th);
        const float ax = c.x - s * 0.46f;
        dl->AddLine(ImVec2(ax, c.y), ImVec2(c.x + s * 0.12f, c.y), col, th);
        dl->AddLine(ImVec2(ax, c.y), ImVec2(ax + s * 0.20f, c.y - s * 0.18f), col, th);
        dl->AddLine(ImVec2(ax, c.y), ImVec2(ax + s * 0.20f, c.y + s * 0.18f), col, th);
    }
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

    // Account dropdown - Free vs Pro, driven by Entitlements. Opened from the
    // topbar user pill via OpenPopup("##account_menu"). Implements artboard 3a:
    // square avatar + email + plan badge, YOUR ACCESS rows (values right, accent
    // for Pro), subscription/upgrade block, then hairline-grouped actions.
    void render_account_menu() {
        const bool pro = Entitlements::is_pro();
        const float W = 316.0f;
        ImGui::SetNextWindowSize(ImVec2(W, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        if (ImGui::BeginPopup("##account_menu")) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const std::string email = Entitlements::user_email();
            const char* email_c = email.empty() ? "you@edgedepth" : email.c_str();
            const ImVec2 o = ImGui::GetCursorScreenPos();

            // ── header: square avatar · email · plan badge (3a) ──────────────
            const float HEAD_H = 64.0f;
            const float av = 36.0f;
            const ImVec2 a0(o.x + 16.0f, o.y + 14.0f);
            dl->AddRectFilled(a0, ImVec2(a0.x + av, a0.y + av),
                              u32(pro ? Tokens::BRAND_SOFT : Tokens::ELEV));
            dl->AddRect(a0, ImVec2(a0.x + av, a0.y + av),
                        u32(pro ? Tokens::BRAND : Tokens::BD2), 0.0f, 0, 1.0f);
            {
                char c0 = email.empty() ? 'E' : email[0];
                if (c0 >= 'a' && c0 <= 'z') c0 = static_cast<char>(c0 - 32);
                const char ini[2] = { c0, 0 };
                ImGui::PushFont(Fonts::mono_md());
                const ImVec2 iw = ImGui::CalcTextSize(ini);
                dl->AddText(ImVec2(a0.x + (av - iw.x) * 0.5f, a0.y + (av - iw.y) * 0.5f),
                            u32(pro ? Tokens::BRAND_TX : Tokens::TX1), ini);
                ImGui::PopFont();
            }
            const float hx = a0.x + av + 12.0f;
            ImGui::PushFont(Fonts::mono());
            dl->PushClipRect(ImVec2(hx, o.y), ImVec2(o.x + W - 14.0f, o.y + HEAD_H), true);
            dl->AddText(ImVec2(hx, o.y + 12.0f), u32(Tokens::TX1), email_c);
            dl->PopClipRect();
            ImGui::PopFont();
            {
                // plan badge - Pro: solid accent block (ed-badge), Free: hairline pill
                ImGui::PushFont(Fonts::label());
                // Research (and staff, who resolve to it) is a superset of Pro -
                // label it as such rather than flattening everyone to PRO.
                const char* plan = !pro ? "FREE PLAN"
                                 : (Entitlements::current() == Entitlements::Tier::Research)
                                       ? "RESEARCH \xC2\xB7 FULL ACCESS"
                                       : "PRO \xC2\xB7 FOUNDER RATE";
                const ImVec2 ts = ImGui::CalcTextSize(plan);
                const float starw = pro ? 12.0f : 0.0f;
                const float chw = 7.0f + starw + ts.x + 7.0f, chh = 17.0f;
                const float chy = o.y + 31.0f;
                if (pro) {
                    dl->AddRectFilled(ImVec2(hx, chy), ImVec2(hx + chw, chy + chh), u32(Tokens::BRAND));
                    ac_ic_star(dl, ImVec2(hx + 7.0f + 4.0f, chy + chh * 0.5f), 4.5f, u32(Tokens::BRAND_INK));
                    dl->AddText(ImVec2(hx + 7.0f + starw, chy + (chh - ts.y) * 0.5f),
                                u32(Tokens::BRAND_INK), plan);
                } else {
                    dl->AddRect(ImVec2(hx, chy), ImVec2(hx + chw, chy + chh), u32(Tokens::BD2), 0.0f, 0, 1.0f);
                    dl->AddText(ImVec2(hx + 7.0f, chy + (chh - ts.y) * 0.5f), u32(Tokens::TX2), plan);
                    // amber founder marker (the mono atlas has no U+25C6, so draw the diamond)
                    const float fdx = hx + chw + 12.0f, fcy = chy + chh * 0.5f, dr = 3.3f;
                    const ImU32 wc = u32(Tokens::WARN);
                    dl->AddQuadFilled(ImVec2(fdx, fcy - dr), ImVec2(fdx + dr, fcy),
                                      ImVec2(fdx, fcy + dr), ImVec2(fdx - dr, fcy), wc);
                    dl->AddText(ImVec2(fdx + dr + 5.0f, fcy - ts.y * 0.5f), wc, "FOUNDER");
                }
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(W, HEAD_H));

            { const ImVec2 p = ImGui::GetCursorScreenPos();
              dl->AddLine(p, ImVec2(p.x + W, p.y), u32(Tokens::BD1)); }

            // ── your access ──────────────────────────────────────────────────
            { const ImVec2 p = ImGui::GetCursorScreenPos();
              ImGui::PushFont(Fonts::label());
              dl->AddText(ImVec2(o.x + 15.0f, p.y + 10.0f), u32(Tokens::TX4), "YOUR ACCESS");
              ImGui::PopFont(); }
            ImGui::Dummy(ImVec2(W, 26.0f));

            auto ent_row = [&](const char* label, const char* val, const ImVec4& vcol) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float rh = 32.0f, cy = p.y + rh * 0.5f;
                ImGui::PushFont(Fonts::ui());
                dl->AddText(ImVec2(o.x + 16.0f, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::TX2), label);
                ImGui::PopFont();
                ImGui::PushFont(Fonts::mono_sm());
                const float vw = ImGui::CalcTextSize(val).x;
                dl->AddText(ImVec2(o.x + W - 16.0f - vw, cy - ImGui::GetFontSize() * 0.5f), u32(vcol), val);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(W, rh));
            };
            if (pro) {
                // Reach is backend-driven (staff + research carry a signed
                // maxLookbackDays claim), so never hard-code the number here.
                char reach[32];
                snprintf(reach, sizeof(reach), "FULL %d DAYS", Entitlements::pro_lookback_days());
                ent_row("Replay window",   reach,            Tokens::BRAND_TX);
                ent_row("Replay symbols",  "ALL 550+ PAIRS", Tokens::BRAND_TX);
                ent_row("Archived events", "ALL ARCHIVED",   Tokens::BRAND_TX);
                ent_row("Guided courses",  "FULL CATALOG",   Tokens::BRAND_TX);
            } else {
                ent_row("Replay window",   Entitlements::free_window_day_label(), Tokens::TX1);
                ent_row("Replay symbols",  "6 MAJORS",        Tokens::TX1);
                ent_row("Event archive",   "RECENT + PUBLIC", Tokens::TX1);
                ent_row("Lessons",         "FREE + PUBLIC",   Tokens::TX1);
            }

            ImGui::Dummy(ImVec2(W, 6.0f));

            // ── subscription status (Pro) / upgrade block (Free) - 3a ────────
            if (pro) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float bx0 = o.x + 12.0f, bx1 = o.x + W - 12.0f, bh = 36.0f, bcy = p.y + bh * 0.5f;
                dl->AddRectFilled(ImVec2(bx0, p.y), ImVec2(bx1, p.y + bh), u32(Tokens::BASE));
                dl->AddRect(ImVec2(bx0, p.y), ImVec2(bx1, p.y + bh), u32(Tokens::BD1), 0.0f, 0, 1.0f);
                ImGui::PushFont(Fonts::ui());
                dl->AddText(ImVec2(bx0 + 12.0f, bcy - ImGui::GetFontSize() * 0.5f),
                            u32(Tokens::TX2), "Subscription");
                ImGui::PopFont();
                const std::string& rn = Entitlements::renews_label();
                char st[72];
                if (!rn.empty()) snprintf(st, sizeof(st), "ACTIVE \xC2\xB7 RENEWS %s", rn.c_str());
                else             snprintf(st, sizeof(st), "ACTIVE");
                for (char* c = st; *c; ++c)
                    if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 32);
                ImGui::PushFont(Fonts::mono_sm());
                const float sw = ImGui::CalcTextSize(st).x;
                dl->AddText(ImVec2(bx1 - 12.0f - sw, bcy - ImGui::GetFontSize() * 0.5f),
                            u32(Tokens::BRAND_TX), st);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(W, bh));
            } else {
                // Redesign 1d: 3-line upgrade block on accent-soft with a 1px accent
                // border. Rows: [star Upgrade to Pro | $20/MO], [30-DAY REPLAY ·
                // ALL PAIRS · FULL ARCHIVE], [Billed $240/YR · or $29/MO ·
                // Bitcoin $216/YR]. The whole block is one click target -> open_upgrade().
                // (No U+20BF bitcoin sign in the mono atlas, so the BTC line is spelled out.)
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float ux0 = o.x + 12.0f, ux1 = o.x + W - 12.0f, uh = 82.0f;
                ImGui::SetCursorScreenPos(ImVec2(ux0, p.y));
                const bool uclick = ImGui::InvisibleButton("##upsell", ImVec2(ux1 - ux0, uh));
                const bool uhov = ImGui::IsItemHovered();
                if (uclick) { Entitlements::open_upgrade(); ImGui::CloseCurrentPopup(); }
                dl->AddRectFilled(ImVec2(ux0, p.y), ImVec2(ux1, p.y + uh),
                                  u32(Tokens::BRAND, uhov ? 0.18f : 0.12f));
                dl->AddRect(ImVec2(ux0, p.y), ImVec2(ux1, p.y + uh), u32(Tokens::BRAND), 0.0f, 0, 1.0f);
                const float ix0 = ux0 + 12.0f, ix1 = ux1 - 12.0f;
                // row 1: star + Upgrade to Pro · $20/MO
                ImGui::PushFont(Fonts::mono());
                {
                    const float ry = p.y + 12.0f, fh = ImGui::GetFontSize();
                    ac_ic_star(dl, ImVec2(ix0 + 4.0f, ry + fh * 0.5f), 5.0f, u32(Tokens::BRAND_TX));
                    dl->AddText(ImVec2(ix0 + 15.0f, ry), u32(Tokens::BRAND_TX), "Upgrade to Pro");
                    const char* up = "$20/MO";
                    dl->AddText(ImVec2(ix1 - ImGui::CalcTextSize(up).x, ry), u32(Tokens::TX1), up);
                }
                ImGui::PopFont();
                // row 2: 30-DAY REPLAY · ALL PAIRS · FULL ARCHIVE
                ImGui::PushFont(Fonts::label());
                {
                    const float ry = p.y + 38.0f;
                    dl->AddText(ImVec2(ix0, ry), u32(Tokens::TX2),
                                "30-DAY REPLAY \xC2\xB7 ALL PAIRS \xC2\xB7 FULL ARCHIVE");
                }
                // row 3: exact annual charge, monthly alternative, Bitcoin price
                {
                    const float ry = p.y + 57.0f;
                    dl->AddText(ImVec2(ix0, ry), u32(Tokens::BRAND_TX),
                                "BILLED $240/YR \xC2\xB7 OR $29/MO \xC2\xB7 BITCOIN $216/YR");
                }
                ImGui::PopFont();
                ImGui::SetCursorScreenPos(ImVec2(o.x, p.y + uh));
            }

            ImGui::Dummy(ImVec2(W, 10.0f));

            // ── actions ──────────────────────────────────────────────────────
            { const ImVec2 p = ImGui::GetCursorScreenPos();
              dl->AddLine(p, ImVec2(p.x + W, p.y), u32(Tokens::BD1)); }
            ImGui::Dummy(ImVec2(W, 7.0f));

            auto nav = [](const char* path) {
#ifdef __EMSCRIPTEN__
                EM_ASM({ window.location.href = UTF8ToString($0); }, path);
#else
                (void)path;
#endif
            };
            auto act_row = [&](int icon, const char* label) -> bool {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float rh = 34.0f;
                const bool clicked = ImGui::InvisibleButton(label, ImVec2(W, rh));
                const bool hov = ImGui::IsItemHovered();
                if (hov)
                    dl->AddRectFilled(ImVec2(p.x + 1.0f, p.y), ImVec2(p.x + W - 1.0f, p.y + rh),
                                      u32(Tokens::ELEV));
                const float cy = p.y + rh * 0.5f;
                const ImU32 tcol = u32(hov ? Tokens::TX1 : Tokens::TX2);
                const ImU32 icol = u32(hov ? Tokens::TX2 : Tokens::TX3);
                const ImVec2 ic(o.x + 24.0f, cy);
                switch (icon) {
                    case 0: ac_ic_gear(dl, ic, 16.0f, icol, 1.5f);   break;
                    case 1: ac_ic_clock(dl, ic, 16.0f, icol, 1.5f);  break;
                    case 2: ac_ic_card(dl, ic, 16.0f, icol, 1.5f);   break;
                    case 3: ac_ic_logout(dl, ic, 16.0f, icol, 1.5f); break;
                }
                ImGui::PushFont(Fonts::ui());
                dl->AddText(ImVec2(o.x + 42.0f, cy - ImGui::GetFontSize() * 0.5f), tcol, label);
                ImGui::PopFont();
                return clicked;
            };
            if (act_row(0, "Account settings")) nav("/account");
            if (act_row(2, pro ? "Manage subscription" : "Billing + credits"))
                nav("/account/billing");
            if (act_row(1, "Replay history"))   nav("/account/replays");
            { const ImVec2 p = ImGui::GetCursorScreenPos();
              dl->AddLine(p, ImVec2(p.x + W, p.y), u32(Tokens::BD1)); }
            if (act_row(3, "Sign out")) nav("/logout");

            ImGui::Dummy(ImVec2(W, 5.0f));
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
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
        ImGui::PushStyleColor(ImGuiCol_Button, Tokens::INPUT);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);

        char sub[40];
        snprintf(sub, sizeof(sub), "%s · PERP",
                 g_pair.exchange == "binancef" ? "BINANCE" : g_pair.exchange.c_str());

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
        return Theme::Layout::TOPBAR_H +
               (g_market_header_open ? Theme::Layout::STATSBAR_H + 8.0f : 0.0f);
    }

    void init(const AppContext& ctx, const Terminal::Pair& active_pair) {
        if (g_initialized) return;
        g_pair = active_pair;
        const auto* meta = SymbolRegistry::instance().get(g_pair.exchange, g_pair.symbol);
        if (meta) {
            g_display_name = meta->display_name();
            g_base_asset   = meta->base_asset.empty() ? g_pair.symbol : meta->base_asset;
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
    double g_tf_err_until = 0.0;   // show the "Pro" notice until this ImGui::GetTime()

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

    // One timeframe pill in the dropdown (2c). Left-click selects (closes the
    // menu), right-click toggles favourite; Pro-locked pills route to upgrade.
    // Square corners; active = accent-soft fill + accent border + accent text;
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
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          u32(on ? Tokens::ELEV : Tokens::INPUT));
        dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                    u32(on ? Tokens::BD3 : (hov ? Tokens::BD3 : Tokens::BD2)), 0.0f, 0, 1.0f);
        const ImVec4 tcol = locked ? Tokens::TX4 : (on ? Tokens::TX1 : (hov ? Tokens::TX1 : Tokens::TX2));
        ImGui::PushFont(Fonts::mono_sm());
        dl->AddText(ImVec2(p.x + (w - ts.x - lockpad) * 0.5f, p.y + (h - ts.y) * 0.5f), u32(tcol), t.label);
        ImGui::PopFont();
        if (locked)     ac_ic_lock(dl, ImVec2(p.x + w - 9.0f, p.y + h * 0.5f), 10.0f, u32(Tokens::TX4), 1.2f);
        else if (fav)   ac_ic_star(dl, ImVec2(p.x + w - 1.0f, p.y), 4.0f, u32(Tokens::TX3));
        else if (hov)   ac_ic_star(dl, ImVec2(p.x + w - 1.0f, p.y), 4.0f, u32(Tokens::TX4));
        if (clicked) {
            if (locked) Entitlements::open_upgrade();
            else if (chart) { chart->change_timeframe(t.sec); ImGui::CloseCurrentPopup(); }
        }
        if (rclick && !locked) tf_toggle_fav(t.sec);
        if (hov) Theme::tooltip(locked ? "Sub-minute is a Pro timeframe"
                                          : (fav ? "Right-click to unfavourite" : "Right-click to favourite"));
    }

    // The dropdown - grouped catalogue, favourites counter, custom field.
    void render_tf_menu(ChartWidget* chart) {
        using namespace Theme;
        const int64_t cur = chart ? chart->timeframe_seconds() : 0;
        const bool pro = Entitlements::is_pro();
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 10.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f));
        if (ImGui::BeginPopup("##tf_menu")) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 win = ImGui::GetWindowPos();
            const float  ww  = ImGui::GetWindowWidth();

            // header strip: TIMEFRAME ..... ★ FAVOURITES N/6 · FULL BAR (2c)
            ImGui::PushFont(Fonts::label());
            ImGui::TextColored(Tokens::TX2, "TIMEFRAME");
            char fc[40];
            const int nfav = static_cast<int>(g_tf_favs.size());
            if (nfav >= 6) snprintf(fc, sizeof(fc), "FAVOURITES %d/6 \xC2\xB7 FULL BAR", nfav);
            else           snprintf(fc, sizeof(fc), "FAVOURITES %d/6", nfav);
            const float fcw = ImGui::CalcTextSize(fc).x;
            const float cw = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(cw - fcw);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            ac_ic_star(dl, ImVec2(sp.x - 7.0f, sp.y + ImGui::GetFontSize() * 0.5f), 4.0f, u32(Tokens::TX3));
            ImGui::TextColored(Tokens::TX2, "%s", fc);
            ImGui::PopFont();
            {   // full-width hairline under the header
                ImGui::Dummy(ImVec2(0, 6));
                const float hy = ImGui::GetCursorScreenPos().y;
                dl->AddLine(ImVec2(win.x, hy), ImVec2(win.x + ww, hy), u32(Tokens::BD1));
                ImGui::Dummy(ImVec2(0, 2));
            }

            // ── REAL-TIME (RT) toggle ─────────────────────────────────────
            // Compact pill (relocated from the old Line-only toolbar). Applies
            // to EVERY chart type: on = follow-live streaming that keeps the live
            // edge current (zoom/pan stay free). On = accent (BRAND_SOFT fill +
            // BRAND border + BRAND_TX text); off = INPUT fill + BD2 border, the
            // same idiom as tf_pill. Label sits to the left of the pill.
            {
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushFont(Fonts::label());
                ImGui::TextColored(Tokens::TX3, "REAL-TIME");
                ImGui::PopFont();
                ImGui::SameLine(0.0f, 10.0f);
                const bool on = chart && chart->rt_mode();
                const float rh = 22.0f, rpadx = 10.0f;
                ImGui::PushFont(Fonts::mono_sm());
                const ImVec2 rts = ImGui::CalcTextSize("RT");
                ImGui::PopFont();
                float rw = rts.x + rpadx * 2.0f;
                if (rw < 40.0f) rw = 40.0f;
                const ImVec2 rp = ImGui::GetCursorScreenPos();
                // vertical-center the pill on the label's line height
                const float roff = (ImGui::GetTextLineHeight() - rh) * 0.5f;
                const ImVec2 rp0 = ImVec2(rp.x, rp.y + roff);
                const bool rclicked = ImGui::InvisibleButton("##rt_tf", ImVec2(rw, rh));
                const bool rhov = ImGui::IsItemHovered();
                dl->AddRectFilled(rp0, ImVec2(rp0.x + rw, rp0.y + rh),
                                  u32(on ? Tokens::ELEV : Tokens::INPUT));
                dl->AddRect(rp0, ImVec2(rp0.x + rw, rp0.y + rh),
                            u32(on ? Tokens::BD3 : (rhov ? Tokens::BD3 : Tokens::BD2)),
                            0.0f, 0, 1.0f);
                ImGui::PushFont(Fonts::mono_sm());
                dl->AddText(ImVec2(rp0.x + (rw - rts.x) * 0.5f, rp0.y + (rh - rts.y) * 0.5f),
                            u32(on ? Tokens::TX1 : (rhov ? Tokens::TX1 : Tokens::TX2)), "RT");
                ImGui::PopFont();
                if (rclicked && chart) chart->toggle_rt_mode();
                if (rhov) Theme::tooltip("Real-time view - follow the live edge (any chart type)");
                ImGui::Dummy(ImVec2(0, 2));
            }

            auto draw_group = [&](const char* label, int lo, int hi, bool gated) {
                ImGui::Dummy(ImVec2(0, 5));
                ImGui::PushFont(Fonts::label());
                ImGui::TextColored(Tokens::TX3, "%s", label);
                ImGui::PopFont();
                if (gated && !pro) {
                    ImGui::SameLine(0.0f, 8.0f);
                    ImGui::PushFont(Fonts::label());
                    const ImVec2 ts = ImGui::CalcTextSize("PRO");
                    const ImVec2 bp = ImGui::GetCursorScreenPos();
                    const float bw = 15.0f + ts.x + 8.0f, bh = 15.0f;
                    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), u32(Tokens::WARN, 0.14f), 7.0f);
                    dl->AddRect(bp, ImVec2(bp.x + bw, bp.y + bh), u32(Tokens::WARN, 0.55f), 7.0f, 0, 1.0f);
                    ac_ic_lock(dl, ImVec2(bp.x + 8.0f, bp.y + bh * 0.5f), 8.0f, u32(Tokens::WARN), 1.2f);
                    dl->AddText(ImVec2(bp.x + 15.0f, bp.y + (bh - ts.y) * 0.5f), u32(Tokens::WARN), "PRO");
                    ImGui::Dummy(ImVec2(bw, bh));
                    ImGui::PopFont();
                }
                for (int i = lo; i <= hi; ++i) {
                    if (i > lo) ImGui::SameLine(0.0f, 6.0f);
                    tf_pill(chart, TF_CATALOG[i], cur, pro, dl);
                }
            };
            draw_group("SECONDS", 0, 3, true);
            draw_group("MINUTES", 4, 8, false);
            draw_group("HOURS", 9, 13, false);
            draw_group("DAYS", 14, 15, false);

            ImGui::Dummy(ImVec2(0, 10));
            ImGui::PushFont(Fonts::label());
            ImGui::TextColored(Tokens::TX3,
                "CLICK SETS THE CHART \xC2\xB7 RIGHT-CLICK PINS IT TO THE BAR (MAX 6)");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 8));

            // custom field + Add - bg-0 footer strip behind, top hairline (2c)
            {
                const float fy = ImGui::GetCursorScreenPos().y;
                dl->AddRectFilled(ImVec2(win.x + 1.0f, fy), ImVec2(win.x + ww - 1.0f, fy + 45.0f),
                                  u32(Tokens::BASE));
                dl->AddLine(ImVec2(win.x, fy), ImVec2(win.x + ww, fy), u32(Tokens::BD1));
            }
            ImGui::Dummy(ImVec2(0, 9));
            ImGui::PushFont(Fonts::ui());
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Tokens::TX3, "Custom");
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens::PANEL);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 58.0f);
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
            if (add) {
                const int sec = tf_parse(g_tf_custom);
                if (sec <= 0) {
                    // ignore empty / unparseable input
                } else if (sec < 60 && !pro) {
                    g_tf_err_until = ImGui::GetTime() + 5.0;
                } else if (chart) {
                    chart->change_timeframe(sec);
                    g_tf_custom[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            if (ImGui::GetTime() < g_tf_err_until) {
                ImGui::PushFont(Fonts::label());
                ImGui::PushStyleColor(ImGuiCol_Text, Tokens::WARN);
                ImGui::TextWrapped("Sub-minute timeframes are a Pro feature: upgrade to unlock 1s-30s.");
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // Inline favourites bar + caret. Drawn in the topbar flow; advances the ImGui
    // cursor past itself (via a trailing Dummy) so following SameLine items align.
    float render_tf_control(ChartWidget* chart) {
        using namespace Theme;
        const int64_t cur = chart ? chart->timeframe_seconds() : 0;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(Fonts::mono());
        const float h = 30.0f, padx = 13.0f, caretw = 30.0f;
        float total = caretw + 1.0f;
        for (int sec : g_tf_favs) total += ImGui::CalcTextSize(tf_label(sec)).x + padx * 2.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // 2c favourites bar - square, bg-1 fill, line-2 hairline frame
        dl->AddRectFilled(p0, ImVec2(p0.x + total, p0.y + h), u32(Tokens::INPUT));
        dl->AddRect(p0, ImVec2(p0.x + total, p0.y + h), u32(Tokens::BD2), 0.0f, 0, 1.0f);

        float x = p0.x;
        for (int sec : g_tf_favs) {
            const char* lbl = tf_label(sec);
            const float w = ImGui::CalcTextSize(lbl).x + padx * 2.0f;
            const bool on = (sec == cur);
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
            ImGui::PushID(sec);
            const bool clk = ImGui::InvisibleButton("##fav", ImVec2(w, h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clk && chart) chart->change_timeframe(sec);
            // active cell: monochrome raised (bg-2) + hairline border, no underline.
            // De-cyaned: teal is reserved for CTAs / links / live data, not on-states.
            if (on) {
                dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + w, p0.y + h), u32(Tokens::ELEV));
                dl->AddRect(ImVec2(x + 1, p0.y + 1), ImVec2(x + w - 1, p0.y + h - 1), u32(Tokens::BD3), 0.0f, 0, 1.0f);
            } else if (hov) {
                dl->AddRectFilled(ImVec2(x + 1, p0.y + 1), ImVec2(x + w - 1, p0.y + h - 1), u32(Tokens::HOVER));
            }
            const ImVec2 ts = ImGui::CalcTextSize(lbl);
            dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f),
                        u32(on ? Tokens::TX1 : (hov ? Tokens::TX1 : Tokens::TX2)), lbl);
            x += w;
        }
        // caret cell - bg-2, line-2 left hairline, accent caret (2c)
        const bool menu_open = ImGui::IsPopupOpen("##tf_menu");
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        if (ImGui::InvisibleButton("##tf_caret", ImVec2(caretw, h))) ImGui::OpenPopup("##tf_menu");
        const bool chov = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(x + 1, p0.y + 1), ImVec2(x + caretw - 1, p0.y + h - 1),
                          u32(chov ? Tokens::HOVER : Tokens::ELEV));
        dl->AddLine(ImVec2(x + 0.5f, p0.y + 1), ImVec2(x + 0.5f, p0.y + h - 1), u32(Tokens::BD2));
        {
            const float cx = x + caretw * 0.5f, cy = p0.y + h * 0.5f;
            const ImU32 cc = u32(Tokens::TX2);
            if (menu_open) dl->AddTriangleFilled(ImVec2(cx - 4, cy + 2), ImVec2(cx + 4, cy + 2), ImVec2(cx, cy - 3), cc);
            else           dl->AddTriangleFilled(ImVec2(cx - 4, cy - 2), ImVec2(cx + 4, cy - 2), ImVec2(cx, cy + 3), cc);
        }
        ImGui::PopFont();

        // layout anchor so the next SameLine item flows from the bar's right edge
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(total, h));

        // dropdown, pinned under the bar
        ImGui::SetNextWindowPos(ImVec2(p0.x, p0.y + h + 5.0f), ImGuiCond_Appearing);
        render_tf_menu(chart);
        return total;
    }

    void render_topbar(std::vector<std::unique_ptr<Widget>>& widgets,
                       const AppContext& ctx) {
        using namespace Theme;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, Layout::TOPBAR_H));
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
        dl->AddLine(ImVec2(vp->Pos.x, vp->Pos.y + Layout::TOPBAR_H - 1.0f),
                    ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + Layout::TOPBAR_H - 1.0f),
                    u32(Tokens::BD1));

        const float cy = (Layout::TOPBAR_H - 30.0f) * 0.5f;

        // ── brand - D mark + edgedepth wordmark + EARLY ACCESS pill, links to
        //    the homepage. Mirrors the web AppHeader / design-system Brand lockup:
        //    the mark reads at ~wordmark height, a 9px mark->word gap and a 12px
        //    word->pill gap, and a hairline accent pill. ──
        const float mark_h = 12.0f;                            // streak-block height (~= wordmark)
        const float mark_w = 272.0f * mark_h / 104.0f;         // full mark render width (~31px)
        const float gap1 = 9.0f, gap2 = 12.0f;                 // mark->word, word->pill
        ImGui::PushFont(Fonts::mono_md());
        const float word_w  = ImGui::CalcTextSize("edgedepth").x;
        const float word_fs = ImGui::GetFontSize();
        ImGui::PopFont();
        // EARLY ACCESS pill - match the web .beta badge (small SemiBold mono +
        // 0.12em tracking) instead of the oversized mono_sm regular it used before.
        static const char* const kBeta = "EARLY ACCESS";
        ImGui::PushFont(Fonts::mono_xs());
        ImFont*      beta_font = ImGui::GetFont();
        const float  beta_fs   = ImGui::GetFontSize();
        const ImVec2 beta_ts0  = ImGui::CalcTextSize(kBeta);
        ImGui::PopFont();
        const float beta_track  = beta_fs * 0.12f;              // 0.12em letter-spacing
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

        ImGui::PushFont(Fonts::mono_md());
        dl->AddText(ImVec2(bp.x + mark_w + gap1, mid - word_fs * 0.5f), u32(Tokens::TX1), "edgedepth");
        ImGui::PopFont();

        const float beta_x = bp.x + mark_w + gap1 + word_w + gap2;
        dl->AddRect(ImVec2(beta_x, mid - beta_h * 0.5f), ImVec2(beta_x + beta_w, mid + beta_h * 0.5f),
                    u32(Tokens::BRAND), 0.0f, 0, 1.0f);
        draw_tracked_text(dl, beta_font, beta_fs,
                          ImVec2(beta_x + beta_padx, mid - beta_ts.y * 0.5f),
                          u32(Tokens::BRAND_TX), kBeta, beta_track);

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
        const float top_y = vp->Pos.y;
        const float icy   = top_y + (Layout::TOPBAR_H - 30.0f) * 0.5f;
        const float ico   = 30.0f, igap = 5.0f, dvw = 13.0f;
        ImGui::PushFont(Fonts::ui_semibold());
        const float courses_w = ImGui::CalcTextSize("Courses").x + 20.0f;
        const float default_w = ImGui::CalcTextSize("Default").x + 22.0f;
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

        // ── account pill (far right) + notifications bell ────────────────────
        {
            const bool pro = Entitlements::is_pro();
            const std::string email = Entitlements::user_email();
            std::string uname = email.empty() ? "Account" : email.substr(0, email.find('@'));
            const char* tlabel = Entitlements::tier_label();
            ImGui::PushFont(Fonts::ui_semibold());
            const float nameW = ImGui::CalcTextSize(uname.c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(Fonts::label());
            const float tlW = ImGui::CalcTextSize(tlabel).x;
            ImGui::PopFont();
            const float av = 26.0f, ph = 32.0f, chev = 12.0f, ppad = 7.0f, g2 = 8.0f;
            const float pill_w = ppad + av + g2 + std::max(nameW, tlW) + 7.0f + chev + ppad;
            rx -= pill_w;
            const ImVec2 pp(rx, top_y + (Layout::TOPBAR_H - ph) * 0.5f);
            ImGui::SetCursorScreenPos(pp);
            ImGui::InvisibleButton("##acct_pill", ImVec2(pill_w, ph));
            if (ImGui::IsItemClicked()) ImGui::OpenPopup("##account_menu");
            if (ImGui::IsItemHovered())
                dl->AddRectFilled(pp, ImVec2(pp.x + pill_w, pp.y + ph), u32(Tokens::HOVER), Radius::R2);
            const ImVec2 ac(pp.x + ppad + av * 0.5f, pp.y + ph * 0.5f);
            dl->AddCircleFilled(ac, av * 0.5f, u32(pro ? Tokens::BRAND_SOFT : Tokens::INPUT));
            dl->AddCircle(ac, av * 0.5f, u32(pro ? Tokens::BRAND : Tokens::TX3, pro ? 0.9f : 0.4f),
                          22, pro ? 1.5f : 1.0f);
            {
                char c0 = email.empty() ? '?' : email[0];
                if (c0 >= 'a' && c0 <= 'z') c0 = static_cast<char>(c0 - 32);
                char ini[2] = { c0, 0 };
                ImGui::PushFont(Fonts::ui_semibold());
                const ImVec2 iw = ImGui::CalcTextSize(ini);
                dl->AddText(ImVec2(ac.x - iw.x * 0.5f, ac.y - iw.y * 0.5f),
                            u32(pro ? Tokens::BRAND : Tokens::TX2), ini);
                ImGui::PopFont();
            }
            const float tx = pp.x + ppad + av + g2;
            ImGui::PushFont(Fonts::ui_semibold());
            dl->AddText(ImVec2(tx, pp.y + 4.0f), u32(Tokens::TX1), uname.c_str());
            ImGui::PopFont();
            ImGui::PushFont(Fonts::label());
            dl->AddText(ImVec2(tx, pp.y + ph - 12.0f), u32(pro ? Tokens::BRAND : Tokens::TX3), tlabel);
            ImGui::PopFont();
            const float cvx = pp.x + pill_w - ppad - chev * 0.5f, cvy = pp.y + ph * 0.5f;
            dl->AddTriangleFilled(ImVec2(cvx - 4, cvy - 2), ImVec2(cvx + 4, cvy - 2),
                                  ImVec2(cvx, cvy + 3), u32(Tokens::TX3));
            rx -= g2;
        }

        // notifications bell (placeholder)
        rx -= ico; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        ico_btn("##tb_bell", ico);
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float bx = (a.x + b.x) * 0.5f, by = (a.y + b.y) * 0.5f - 1.0f;
            const ImU32 col = u32(Tokens::TX2);
            dl->AddLine(ImVec2(bx - 5, by + 5), ImVec2(bx - 3, by - 4), col, 1.4f);
            dl->AddLine(ImVec2(bx + 5, by + 5), ImVec2(bx + 3, by - 4), col, 1.4f);
            dl->AddLine(ImVec2(bx - 3, by - 4), ImVec2(bx + 3, by - 4), col, 1.4f);
            dl->AddLine(ImVec2(bx - 5, by + 5), ImVec2(bx + 5, by + 5), col, 1.4f);
            dl->AddCircleFilled(ImVec2(bx, by + 7.5f), 1.4f, col);
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Notifications");
        rx -= igap;

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
        if (ImGui::IsItemHovered()) Theme::tooltip("Tweaks");
        rx -= igap;

        // (The "+ add widget" button moved to the chart toolbar as "+ widget",
        //  beside the layers control, so it renders in EVERY chrome including the
        //  embedded /demo + event replays where this native topbar is suppressed.)

        // Default (layout) menu
        rx -= default_w; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        if (tb_button("Default", default_w)) ImGui::OpenPopup("##tb_layout");

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
        if (ImGui::BeginPopup("##replay_hint")) {
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
            ImGui::TextUnformatted("Right-click a candle on the chart, then \xE2\x80\x9CReplay from here\xE2\x80\x9D.");
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }

        vdiv();

        // Courses
        rx -= courses_w; ImGui::SetCursorScreenPos(ImVec2(rx, icy));
        if (tb_button("Courses", courses_w)) {
#ifdef __EMSCRIPTEN__
            EM_ASM({ window.location.href = '/courses'; });
#endif
        }

        // (Top-bar Indicators popup removed in v2 3b; the chart toolbar owns it.
        //  The "##tb_widget" add-widget popup likewise moved to the chart toolbar
        //  ("+ widget", beside the layers control) - see ChartWidget::render_controls.)
        {
            static bool show_demo = false, show_metrics = false;
            if (ImGui::BeginPopup("##tb_layout")) {
                if (ImGui::MenuItem("Reset Layout")) LayoutManager::reset_layout();
                if (ImGui::MenuItem("Save Layout"))
                    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
                ImGui::MenuItem("Market Header", nullptr, &g_market_header_open);
                ImGui::Separator();
                ImGui::MenuItem("ImGui Demo", nullptr, &show_demo);
                ImGui::MenuItem("Metrics", nullptr, &show_metrics);
                ImGui::EndPopup();
            }
            if (show_demo) ImGui::ShowDemoWindow(&show_demo);
            if (show_metrics) ImGui::ShowMetricsWindow(&show_metrics);
        }

        // Account dropdown (opened by the user pill above).
        render_account_menu();

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
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + Layout::TOPBAR_H));
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
        dl->AddLine(ImVec2(vp->Pos.x, vp->Pos.y + Layout::TOPBAR_H + Layout::STATSBAR_H - 1.0f),
                    ImVec2(vp->Pos.x + vp->Size.x,
                           vp->Pos.y + Layout::TOPBAR_H + Layout::STATSBAR_H - 1.0f),
                    u32(Tokens::BD1));

        // Positioning (long/short + liq) read from the AnalyticsManager via ctx;
        // mark/OI/funding via g_stats, 24h change/vol via ticker.
        const PositioningState* pos = ctx.analytics_mgr().get_positioning(g_pair.symbol);
        const TickerEntry* tick = TickerManager::instance().get(g_pair.exchange, g_pair.symbol);
        char v[80], sm[64];

        // Six flex cells across the full width, SPEC ratios. Label (top) over a
        // mono value; three cells carry a bottom-anchored detail on one baseline.
        const float y0       = vp->Pos.y + Layout::TOPBAR_H;
        const float strip_h  = Layout::STATSBAR_H;
        const float label_y  = y0 + 7.0f;
        const float value_y  = y0 + 20.0f;
        const float detail_y = y0 + strip_h - 14.0f;   // shared bottom baseline
        const float x_pad    = 16.0f;

        const float ratios[6] = {1.0f, 1.15f, 1.0f, 1.15f, 1.1f, 1.25f};
        float rsum = 0.0f; for (float r : ratios) rsum += r;
        float cx[7];
        cx[0] = vp->Pos.x;
        for (int i = 0; i < 6; ++i) cx[i + 1] = cx[i] + vp->Size.x * ratios[i] / rsum;

        for (int i = 1; i < 6; ++i)   // 1px line-1 separators (right border, not last)
            dl->AddLine(ImVec2(cx[i], y0 + 1.0f), ImVec2(cx[i], y0 + strip_h - 1.0f),
                        u32(Tokens::BD1));

        auto put_label = [&](int i, const char* s) {
            ImGui::PushFont(Fonts::label());   // Hanken micro-label (matches terminal chrome)
            dl->AddText(ImVec2(cx[i] + x_pad, label_y), u32(Tokens::TX3), s);
            ImGui::PopFont();
        };
        auto put_value = [&](int i, const char* s, const ImVec4& col) -> float {
            ImGui::PushFont(Fonts::mono_md());
            dl->AddText(ImVec2(cx[i] + x_pad, value_y), u32(col), s);
            const float w = ImGui::CalcTextSize(s).x;
            ImGui::PopFont();
            return cx[i] + x_pad + w;
        };
        auto put_detail = [&](int i, const char* s, const ImVec4& col, float a = 1.0f) {
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImVec2(cx[i] + x_pad, detail_y), u32(col, a), s);
            ImGui::PopFont();
        };

        // 1. MARK - neutral value
        put_label(0, "MARK");
        if (g_stats.has_data) { g_fmt.format_price(v, sizeof(v), g_stats.mark_price); put_value(0, v, Tokens::TX1); }
        else                    put_value(0, "-", Tokens::TX4);

        // 2. 24H CHANGE - signed teal/rose, with the absolute delta inline (text-3)
        put_label(1, "24H CHANGE");
        if (tick) {
            snprintf(v, sizeof(v), "%+.2f%%", tick->change_pct_24h);
            const float vx = put_value(1, v, tick->change_pct_24h >= 0.0 ? Tokens::UP : Tokens::DOWN);
            const double denom = 1.0 + tick->change_pct_24h / 100.0;
            const double delta = denom != 0.0 ? tick->last_price * (tick->change_pct_24h / 100.0) / denom : 0.0;
            char db[32]; g_fmt.format_price(db, sizeof(db), delta);
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImVec2(vx + 6.0f, value_y + 3.0f), u32(Tokens::TX3), db);
            ImGui::PopFont();
        } else put_value(1, "-", Tokens::TX4);

        // 3. OPEN INTEREST - neutral value. The stat's open_interest_usd field
        // carries CONTRACT QTY (backend note in stat.go), so notional USD =
        // contracts * mark.
        put_label(2, "OPEN INTEREST");
        const double oi_usd = g_stats.open_interest_usd * g_stats.mark_price;
        if (g_stats.has_data && oi_usd > 0.0) { fmt_compact_usd(v, sizeof(v), oi_usd); put_value(2, v, Tokens::TX1); }
        else put_value(2, "-", Tokens::TX4);

        // 4. FUNDING / COUNTDOWN - neutral value + dim countdown. (Review: the
        // washed amber read poorly; funding now matches the other values in white.)
        put_label(3, "FUNDING / COUNTDOWN");
        if (g_stats.has_data) {
            snprintf(v, sizeof(v), "%+.4f%%", g_stats.funding * 100.0);
            put_value(3, v, Tokens::TX1);
            if (g_stats.next_funding_time > 0) {
                fmt_funding_countdown(sm, sizeof(sm), g_stats.next_funding_time);
                char nb[80]; snprintf(nb, sizeof(nb), "NEXT %s", sm);
                put_detail(3, nb, Tokens::TX3);
            }
        } else put_value(3, "-", Tokens::TX4);

        // 5. LONG / SHORT - tri-color value (long up / slash text-3 / short down)
        // + 3px ratio bar, from the positioning account ratio (retail crowd).
        put_label(4, "LONG / SHORT");
        if (pos && pos->global_long_account > 0.0) {
            const float lg = static_cast<float>(pos->global_long_account);
            char lb[16], sb[16];
            snprintf(lb, sizeof(lb), "%.0f%%", lg * 100.0f);
            snprintf(sb, sizeof(sb), "%.0f%%", (1.0f - lg) * 100.0f);
            ImGui::PushFont(Fonts::mono_md());
            float tx = cx[4] + x_pad;
            dl->AddText(ImVec2(tx, value_y), u32(Tokens::UP), lb);
            tx += ImGui::CalcTextSize(lb).x;
            dl->AddText(ImVec2(tx, value_y), u32(Tokens::TX3), " / ");
            tx += ImGui::CalcTextSize(" / ").x;
            dl->AddText(ImVec2(tx, value_y), u32(Tokens::DOWN), sb);
            ImGui::PopFont();
            // 3px ratio bar on the bottom baseline (long up | short down).
            const float bx = cx[4] + x_pad, bw = (cx[5] - cx[4]) - x_pad * 2.0f;
            const float by = detail_y + 2.0f, lw = (bw - 1.0f) * lg;
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + lw, by + 3.0f), u32(Tokens::UP));
            dl->AddRectFilled(ImVec2(bx + lw + 1.0f, by), ImVec2(bx + bw, by + 3.0f), u32(Tokens::DOWN));
        } else put_value(4, "-", Tokens::TX4);

        // 6. LIQUIDATIONS 24H - neutral value + neutral L/S split (text-3). Long +
        // short USD from positioning (falls back to the stat aggregate if unset).
        put_label(5, "LIQUIDATIONS 24H");
        double liq_long  = pos ? pos->long_liq_usd  : 0.0;
        double liq_short = pos ? pos->short_liq_usd : 0.0;
        if (liq_long <= 0.0 && liq_short <= 0.0 && g_stats.has_data) {
            liq_long  = g_stats.liq_long_usd;
            liq_short = g_stats.liq_short_usd;
        }
        const double liq_tot = liq_long + liq_short;
        if (liq_tot > 0.0) {
            fmt_compact_usd(v, sizeof(v), liq_tot);
            put_value(5, v, Tokens::TX1);
            char lb[32], sb[32];
            fmt_compact_usd(lb, sizeof(lb), liq_long);
            fmt_compact_usd(sb, sizeof(sb), liq_short);
            char nb[80]; snprintf(nb, sizeof(nb), "L %s \xC2\xB7 S %s", lb, sb);
            put_detail(5, nb, Tokens::TX3);
        } else put_value(5, "-", Tokens::TX4);

        // Close (X): hides the market header; re-open via the top-bar layout menu.
        {
            const float xs = 16.0f;
            const ImVec2 xp(vp->Pos.x + vp->Size.x - xs - 8.0f, y0 + 5.0f);
            ImGui::SetCursorScreenPos(xp);
            const bool xclick = ImGui::InvisibleButton("##hdr_close", ImVec2(xs, xs));
            const bool xhov = ImGui::IsItemHovered();
            if (xhov) dl->AddRectFilled(xp, ImVec2(xp.x + xs, xp.y + xs), u32(Tokens::ELEV), Radius::R1);
            const ImU32 xc = u32(xhov ? Tokens::TX1 : Tokens::TX3);
            const float cx0 = xp.x + xs * 0.5f, cy0 = xp.y + xs * 0.5f, r = 3.5f;
            dl->AddLine(ImVec2(cx0 - r, cy0 - r), ImVec2(cx0 + r, cy0 + r), xc, 1.4f);
            dl->AddLine(ImVec2(cx0 - r, cy0 + r), ImVec2(cx0 + r, cy0 - r), xc, 1.4f);
            if (xhov) Theme::tooltip("Hide market header (re-open via the layout menu)");
            if (xclick) g_market_header_open = false;
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
                                       vp->Pos.y + Layout::TOPBAR_H + 6.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(286.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::ELEV);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, Tokens::PANEL);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        if (ImGui::Begin("Tweaks", &g_tweaks_open,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto section = [&](const char* s) {
                ImGui::Dummy(ImVec2(0, 3));
                ImGui::PushFont(Fonts::label());
                ImGui::TextColored(Tokens::TX4, "%s", s);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 2));
            };

            // Accent swatches → Theme::set_accent
            section("ACCENT");
            struct Sw { const char* id; Accent a; unsigned int c; };
            const Sw sws[4] = {
                {"##acc_teal",   Accent::Teal,   u32(from_hex(0x22c5db))},
                {"##acc_indigo", Accent::Indigo, u32(from_hex(0x6d8bff))},
                {"##acc_amber",  Accent::Amber,  u32(from_hex(0xf3b24a))},
                {"##acc_mono",   Accent::Mono,   u32(from_hex(0xaab8c4))},
            };
            const Accent cur_acc = accent();
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine(0.0f, 10.0f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float sz = 30.0f;
                if (ImGui::InvisibleButton(sws[i].id, ImVec2(sz, sz))) set_accent(sws[i].a);
                dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), sws[i].c, Radius::R2);
                if (cur_acc == sws[i].a)
                    dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + sz + 2, p.y + sz + 2),
                                u32(Tokens::TX1), Radius::R2, 0, 2.0f);
            }

            // Candle convention → Theme::set_candle_convention
            section("CANDLES");
            auto cand_btn = [&](const char* label, CandleConvention cc) {
                const bool on = (candles() == cc);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? Tokens::BRAND_SOFT : Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Tokens::BRAND_SOFT : Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? Tokens::BRAND : Tokens::TX2);
                if (ImGui::Button(label)) set_candle_convention(cc);
                ImGui::PopStyleColor(4);
            };
            cand_btn("Teal/Mag", CandleConvention::TealMag);
            ImGui::SameLine(0.0f, 5.0f);
            cand_btn("Green/Red", CandleConvention::Classic);
            ImGui::SameLine(0.0f, 5.0f);
            cand_btn("Muted", CandleConvention::Muted);

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
    // Layout::STATUSBAR_H (22); the dockspace + replay bar reserve it via
    // LayoutManager::status_reserve.
    void draw_statusbar(const AppContext& ctx, const std::string& symbol_lc, bool ws_ok) {
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

        ImGui::PushFont(Fonts::mono_sm());
        const float th = ImGui::GetFontSize();
        const float ty = bar_y + (Layout::STATUSBAR_H - th) * 0.5f;

        // left - connection + market context
        std::string sym = symbol_lc;
        for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        char left[128];
        snprintf(left, sizeof(left), "WS %s \xc2\xb7 BINANCE PERP \xc2\xb7 %s",
                 ws_ok ? "CONNECTED" : "-", sym.c_str());
        const float dot_x = vp->Pos.x + 14.0f;
        // coin logo at the far left (monogram fallback while loading / if missing)
        const float sb_logo = 13.0f;
        LogoManager::instance().draw_coin(dl, g_base_asset,
            ImVec2(dot_x, bar_y + (Layout::STATUSBAR_H - sb_logo) * 0.5f), sb_logo);
        const float sb_x = dot_x + sb_logo + 8.0f;
        dl->AddCircleFilled(ImVec2(sb_x + 3.0f, bar_y + Layout::STATUSBAR_H * 0.5f), 3.0f,
                            u32(ws_ok ? Tokens::UP : Tokens::TX4));
        dl->AddText(ImVec2(sb_x + 12.0f, ty), u32(Tokens::TX3), left);

        // right - replay state + frame telemetry + interactive display zone/clock.
        // The selected zone only formats the epoch; it never changes replay time.
        const bool replaying = ctx.replay_mgr().is_active();
        const float fr = ImGui::GetIO().Framerate;
        char telemetry[112];
        snprintf(telemetry, sizeof(telemetry), "%s \xc2\xb7 %.0f FPS \xc2\xb7 %.1fMS FRAME",
                 replaying ? "REPLAY" : "REPLAY READY",
                 fr, fr > 0.0f ? 1000.0f / fr : 0.0f);

        const int64_t display_epoch_ms = replaying
            ? ctx.replay_mgr().interpolated_time_ms()
            : static_cast<int64_t>(time(nullptr)) * 1000;
        auto& time_zone = DisplayTimeZone::instance();
        char clock[16] = "--:--:--";
        time_zone.format(display_epoch_ms, TimeZoneFormat::TimeSeconds, clock, sizeof(clock));
        char zone[160]{};
        const bool compact_zone = vp->Size.x < 920.0f;
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
        dl->AddText(ImVec2(zone_x - 10.0f - telemetry_w, ty), u32(Tokens::TX3), telemetry);
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
    void render_statusbar(const AppContext& ctx, const std::string& symbol_lc, bool ws_ok) {
        draw_statusbar(ctx, symbol_lc, ws_ok);
    }

    void render(std::vector<std::unique_ptr<Widget>>& widgets,
                const AppContext& ctx) {
        // Cmd/Ctrl-K → Finder (the symbol picker modal; row select switches symbol).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_K)) {
            Menu::g_symbol_picker.pending = Menu::SymbolPickerState::PendingWidget::Charts;
            Menu::g_symbol_picker.open = true;
            Menu::g_symbol_picker.search_buf[0] = '\0';
            Menu::g_symbol_picker.replace_mode = true;
        }
        render_topbar(widgets, ctx);
        render_statsbar(ctx);
        draw_statusbar(ctx, g_pair.symbol, g_stats.has_data);
        render_tweaks_panel(widgets);
        // the symbol picker popup is opened from the topbar / Cmd-K now
        Menu::render_symbol_picker_popup(widgets, ctx);
    }
}  // namespace AppShell
