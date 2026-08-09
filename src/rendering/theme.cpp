#include "rendering/theme.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <implot.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/em_asm.h>
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// theme.cpp - edgedepth design system implementation
// Style mapping per the design system's ImGui notes §1, tokens per tokens.json.
// ═══════════════════════════════════════════════════════════════════════════════

namespace Theme {
    namespace {
        // font atlas
        ImFont* f_ui          = nullptr;
        ImFont* f_ui_semibold = nullptr;
        ImFont* f_heading     = nullptr;
        ImFont* f_label       = nullptr;
        ImFont* f_mono_xs     = nullptr;
        ImFont* f_mono_sm     = nullptr;
        ImFont* f_mono        = nullptr;
        ImFont* f_mono_md     = nullptr;
        ImFont* f_mono_lg     = nullptr;

        // tweak state
        Accent           g_accent  = Accent::Teal;
        CandleConvention g_candles = CandleConvention::TealMag;
        int              g_density = 8;

        ImVec4 with_a(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
    }

    int density() { return g_density; }
    void set_density(int d) { g_density = std::clamp(d, 3, 10); }
    // Anchored to the theme-tokens density values at the default dial (d8):
    // watchlist rows 24px, DOM/tape rows 18px; the dial trades ±1.1/±0.9 px per step.
    float row_h()       { return Layout::WATCHLIST_ROW_H - (g_density - 8) * 1.1f; }
    float row_h_dense() { return Layout::DOMTAPE_ROW_H   - (g_density - 8) * 0.9f; }

    Accent accent() { return g_accent; }
    CandleConvention candles() { return g_candles; }

    void set_accent(Accent a) {
        g_accent = a;
        uint32_t hex, tx; float soft, line;
        switch (a) {
            case Accent::Indigo: hex = 0x6d8bff; tx = 0x8ba3ff; soft = 0.15f; line = 0.55f; break;
            case Accent::Amber:  hex = 0xf0b350; tx = 0xf6c76e; soft = 0.15f; line = 0.55f; break;
            case Accent::Mono:   hex = 0xaab8c4; tx = 0xc0ccd6; soft = 0.13f; line = 0.45f; break;
            case Accent::Teal:   // design default: accent #35c9c4, textTint #3fe0d0
            default:             hex = 0x35c9c4; tx = 0x3fe0d0; soft = 0.14f; line = 0.55f; break;
        }
        Tokens::BRAND      = from_hex(hex);
        Tokens::BRAND_SOFT = from_hex(hex, soft);
        Tokens::BRAND_LINE = from_hex(hex, line);
        Tokens::BRAND_TX   = from_hex(tx);
        apply_dark_theme();  // re-map style colors that reference BRAND
    }

    void set_candle_convention(CandleConvention c) {
        g_candles = c;
        uint32_t up, down; float line = 0.50f;
        switch (c) {
            case CandleConvention::Classic: up = 0x26d07a; down = 0xff4d5e; break;
            case CandleConvention::Muted:   up = 0x41a583; down = 0xcf6173; line = 0.45f; break;
            case CandleConvention::TealMag: // design tokens: up #2fd6ad / down #ee5c78
            default:                        up = 0x2fd6ad; down = 0xee5c78; break;
        }
        Tokens::UP        = from_hex(up);
        Tokens::DOWN      = from_hex(down);
        Tokens::UP_SOFT   = from_hex(up, 0.13f);
        Tokens::DOWN_SOFT = from_hex(down, 0.13f);
        Tokens::UP_LINE   = from_hex(up, line);
        Tokens::DOWN_LINE = from_hex(down, line);
    }

    namespace {
        void apply_implot_style() {
            using namespace Tokens;
            ImPlotStyle& s = ImPlot::GetStyle();
            // chart sits directly on the app base - charcoal ground shows
            // through the liq field's transparent low-intensity cells
            s.Colors[ImPlotCol_PlotBg]        = BASE;
            s.Colors[ImPlotCol_FrameBg]       = BASE;
            s.Colors[ImPlotCol_PlotBorder]    = ImVec4(0, 0, 0, 0);
            s.PlotBorderSize = 0.0f;
            // gridlines - barely-there hairlines
            s.Colors[ImPlotCol_AxisGrid]      = GRID;
            s.Colors[ImPlotCol_AxisBg]        = BASE;
            s.Colors[ImPlotCol_AxisBgHovered] = BASE;
            s.Colors[ImPlotCol_AxisBgActive]  = BASE;
            s.Colors[ImPlotCol_AxisText]      = TX3;
            // crosshair - dashed look approximated by low-alpha solid
            s.Colors[ImPlotCol_Crosshairs]    = from_hex(0x96a8b8, 0.35f);
            s.Colors[ImPlotCol_Selection]     = BRAND;
            s.Colors[ImPlotCol_LegendBg]      = with_a(PANEL, 0.98f);
            s.Colors[ImPlotCol_LegendBorder]  = BD2;
            // no tick marks - labels only, like the reference
            s.MajorTickLen  = ImVec2(0, 0);
            s.MinorTickLen  = ImVec2(0, 0);
            s.MajorTickSize = ImVec2(0, 0);
            s.MinorTickSize = ImVec2(0, 0);
            s.MajorGridSize = ImVec2(1.0f, 1.0f);
            s.MinorGridSize = ImVec2(0.5f, 0.5f);
            s.MinorAlpha    = 0.25f;
            s.PlotPadding   = ImVec2(0, 0);
            s.LabelPadding  = ImVec2(4, 2);
            s.LegendPadding = ImVec2(10, 10);
        }
    }

    void apply_dark_theme() {
        using namespace Tokens;
        ImGuiStyle& s = ImGui::GetStyle();
        ImVec4* c = s.Colors;

        // ── windows / surfaces ───────────────────────────────────────────────
        c[ImGuiCol_WindowBg]          = PANEL;
        c[ImGuiCol_ChildBg]           = ImVec4(0, 0, 0, 0);  // children inherit
        c[ImGuiCol_PopupBg]           = with_a(PANEL, 0.98f);
        c[ImGuiCol_MenuBarBg]         = ELEV;
        c[ImGuiCol_Border]            = BD2;
        c[ImGuiCol_BorderShadow]      = ImVec4(0, 0, 0, 0);

        // ── frames (inputs, idle controls) ───────────────────────────────────
        c[ImGuiCol_FrameBg]           = INPUT;
        c[ImGuiCol_FrameBgHovered]    = HOVER;
        c[ImGuiCol_FrameBgActive]     = ACTIVE;

        // ── text ─────────────────────────────────────────────────────────────
        c[ImGuiCol_Text]              = TX1;
        c[ImGuiCol_TextDisabled]      = TX3;

        // ── title / tabs (docked panel chrome stays quiet) ───────────────────
        c[ImGuiCol_TitleBg]           = ELEV;
        c[ImGuiCol_TitleBgActive]     = ELEV;
        c[ImGuiCol_TitleBgCollapsed]  = PANEL;
        c[ImGuiCol_Tab]               = PANEL;
        c[ImGuiCol_TabHovered]        = HOVER;
        c[ImGuiCol_TabActive]         = ACTIVE;
        c[ImGuiCol_TabUnfocused]      = PANEL;
        c[ImGuiCol_TabUnfocusedActive]= ELEV;

        // ── selection / headers - brand-soft, used sparingly ─────────────────
        c[ImGuiCol_Header]            = BRAND_SOFT;
        c[ImGuiCol_HeaderHovered]     = HOVER;
        c[ImGuiCol_HeaderActive]      = with_a(BRAND, 0.22f);

        // ── buttons ──────────────────────────────────────────────────────────
        c[ImGuiCol_Button]            = INPUT;
        c[ImGuiCol_ButtonHovered]     = HOVER;
        c[ImGuiCol_ButtonActive]      = ACTIVE;
        c[ImGuiCol_CheckMark]         = BRAND;
        c[ImGuiCol_SliderGrab]        = BRAND;
        c[ImGuiCol_SliderGrabActive]  = BRAND;

        // ── scrollbars - thin, hairline thumbs ───────────────────────────────
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = BD2;
        c[ImGuiCol_ScrollbarGrabHovered] = BD3;
        c[ImGuiCol_ScrollbarGrabActive]  = BD3;

        // ── separators / resize ──────────────────────────────────────────────
        c[ImGuiCol_Separator]         = BD1;
        c[ImGuiCol_SeparatorHovered]  = BD3;
        c[ImGuiCol_SeparatorActive]   = BRAND_LINE;
        c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = BD3;
        c[ImGuiCol_ResizeGripActive]  = BRAND_LINE;

        // ── tables - hairlines, no zebra ─────────────────────────────────────
        c[ImGuiCol_TableHeaderBg]     = ELEV;
        c[ImGuiCol_TableBorderStrong] = BD2;
        c[ImGuiCol_TableBorderLight]  = BD1;
        c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]     = ImVec4(0, 0, 0, 0);

        // ── plots / docking / nav ────────────────────────────────────────────
        c[ImGuiCol_PlotLines]         = BRAND;
        c[ImGuiCol_PlotHistogram]     = BRAND;
        c[ImGuiCol_DockingPreview]    = with_a(BRAND, 0.35f);
        c[ImGuiCol_DockingEmptyBg]    = BASE;
        c[ImGuiCol_NavHighlight]      = BRAND_LINE;
        c[ImGuiCol_ModalWindowDimBg]  = from_hex(0x030508, 0.62f);

        // ── metrics (IMGUI-NOTES §1) ─────────────────────────────────────────
        s.WindowRounding    = 0.0f;   // docked panels are square (chrome rules)
        s.ChildRounding     = 0.0f;
        s.FrameRounding     = 5.0f;   // --r2
        s.GrabRounding      = 5.0f;
        s.PopupRounding     = Radius::R3;   // floating chrome: radius 6
        s.TabRounding       = 5.0f;
        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.FrameBorderSize   = 1.0f;
        s.PopupBorderSize   = 1.0f;
        s.ScrollbarRounding = 6.0f;
        s.ScrollbarSize     = 9.0f;
        s.CellPadding       = ImVec2(6, 2);
        s.ItemSpacing       = ImVec2(8, 6);
        s.ItemInnerSpacing  = ImVec2(6, 4);
        s.FramePadding      = ImVec2(8, 5);
        s.WindowPadding     = ImVec2(0, 0);   // panels manage their own gutters
        s.GrabMinSize       = 10.0f;

        apply_implot_style();
    }

    void apply_trading_colors() {
        // retained for call-site compatibility - table styling now lives in
        // apply_dark_theme(); nothing extra to override here.
    }

    void begin_tooltip() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::BeginTooltip();
    }

    void end_tooltip() {
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
    }

    void tooltip(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        begin_tooltip();
        ImGui::TextV(fmt, args);
        end_tooltip();
        va_end(args);
    }

    bool load_fonts() {
        ImGuiIO& io = ImGui::GetIO();

        float dpi_scale = 1.0f;
#ifdef __EMSCRIPTEN__
        dpi_scale = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
#endif
        // Rasterize at a minimum of 2x regardless of devicePixelRatio -
        // on 1x displays this supersamples the atlas for crisper glyphs.
        const float raster_scale = dpi_scale < 2.0f ? 2.0f : dpi_scale;

        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = true;

        // Default latin range + Δ (U+0394, DOM delta column) + … (U+2026)
        static const ImWchar glyph_ranges[] = {
            0x0020, 0x00FF,  // Basic Latin + Latin Supplement
            0x0394, 0x0394,  // Greek capital delta
            0x2026, 0x2026,  // horizontal ellipsis
            0,
        };

        io.Fonts->Clear();

        // ui_scale - single proportional knob for the whole terminal's logical
        // type size. This is the dial to tune readability; it is ORTHOGONAL to
        // raster_scale (which only governs glyph crispness via supersampling).
        // 1.0 = the original base sizes; 1.15 = ~15% larger across every face,
        // keeping the type hierarchy intact. Applied to logical_px BEFORE the
        // raster multiply, so atlas crispness is unaffected.
        constexpr float ui_scale = 1.15f;

        auto add = [&](const char* path, float logical_px) -> ImFont* {
            ImFont* f = io.Fonts->AddFontFromFileTTF(
                path, logical_px * ui_scale * raster_scale, &cfg, glyph_ranges);
            return f;
        };

        // UI face - Hanken Grotesk (chrome, labels, headings)
        f_ui          = add("/fonts/HankenGrotesk-Regular.ttf",  16.0f);
        f_ui_semibold = add("/fonts/HankenGrotesk-SemiBold.ttf", 16.0f);
        f_heading     = add("/fonts/HankenGrotesk-SemiBold.ttf", 20.0f);
        f_label       = add("/fonts/HankenGrotesk-SemiBold.ttf", 11.5f);

        // Numeric face - JetBrains Mono (every table/ladder/clock/price).
        // Base logical sizes; ui_scale (above) applies the global multiplier so
        // the dense DOM ladder / tape / depth widgets scale with everything else.
        // ImGui tables auto-size rows to the font, so row heights track this.
        f_mono_xs     = add("/fonts/JetBrainsMono-SemiBold.ttf",  9.5f);
        f_mono_sm     = add("/fonts/JetBrainsMono-Regular.ttf",  15.0f);
        f_mono        = add("/fonts/JetBrainsMono-Regular.ttf",  16.0f);
        f_mono_md     = add("/fonts/JetBrainsMono-Medium.ttf",   17.0f);
        f_mono_lg     = add("/fonts/JetBrainsMono-SemiBold.ttf", 23.0f);

        if (!f_ui) f_ui = io.Fonts->AddFontDefault();
        if (!f_ui_semibold) f_ui_semibold = f_ui;
        if (!f_heading)     f_heading     = f_ui_semibold;
        if (!f_label)       f_label       = f_ui_semibold;
        if (!f_mono)        f_mono        = f_ui;
        if (!f_mono_xs)     f_mono_xs     = f_mono;
        if (!f_mono_sm)     f_mono_sm     = f_mono;
        if (!f_mono_md)     f_mono_md     = f_mono;
        if (!f_mono_lg)     f_mono_lg     = f_mono_md;

        io.FontDefault = f_ui;

        // Fonts rasterize at logical_px * raster_scale for crispness; ImGui
        // works in logical pixels, so scale back down by the same factor.
        io.FontGlobalScale = 1.0f / raster_scale;

        return f_ui != nullptr;
    }

    namespace Fonts {
        ImFont* ui()          { return f_ui; }
        ImFont* ui_semibold() { return f_ui_semibold; }
        ImFont* heading()     { return f_heading; }
        ImFont* label()       { return f_label; }
        ImFont* mono_xs()     { return f_mono_xs; }
        ImFont* mono_sm()     { return f_mono_sm; }
        ImFont* mono()        { return f_mono; }
        ImFont* mono_md()     { return f_mono_md; }
        ImFont* mono_lg()     { return f_mono_lg; }
    }

    ImFont* get_regular_font() { return f_ui; }
    ImFont* get_bold_font()    { return f_ui_semibold; }
    ImFont* get_large_font()   { return f_mono_md; }

    ImVec4 get_buy_color(float alpha) {
        return ImVec4(Tokens::UP.x, Tokens::UP.y, Tokens::UP.z, alpha);
    }

    ImVec4 get_sell_color(float alpha) {
        return ImVec4(Tokens::DOWN.x, Tokens::DOWN.y, Tokens::DOWN.z, alpha);
    }

    ImU32 get_buy_color_u32(uint8_t alpha) {
        const ImVec4& c = Tokens::UP;
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f), alpha);
    }

    ImU32 get_sell_color_u32(uint8_t alpha) {
        const ImVec4& c = Tokens::DOWN;
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f), alpha);
    }
}
