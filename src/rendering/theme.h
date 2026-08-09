#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// theme.h — edgedepth design system: tokens + ImGui/ImPlot style + fonts
//
// Source of truth: the edgedepth design system — tokens.json (v0.1.0) and
// edgedepth.css (:root block). Dark-only.
// Near-black cool charcoal surfaces, teal-up / magenta-rose-down market data,
// cyan brand accent (used sparingly), amber for replay/events/POC.
//
// Semantic colors (UP/DOWN/BRAND/…) are runtime-mutable so the Tweaks panel
// can switch candle conventions and accents without a rebuild. Surfaces,
// borders and text ramp are fixed constexpr.
//
// Legacy aliases (Theme::Colors::BUY_GREEN etc.) are kept so existing widgets
// compile unchanged; new/reworked code should use Theme::Tokens:: directly.
// ═══════════════════════════════════════════════════════════════════════════════
#include <cstdint>

#include "imgui.h"

namespace Theme {

    // hex 0xRRGGBB → ImVec4
    constexpr ImVec4 from_hex(uint32_t rgb, float a = 1.0f) {
        return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                      ((rgb >> 8) & 0xFF) / 255.0f,
                      (rgb & 0xFF) / 255.0f, a);
    }

    // ImVec4 → ImU32 with optional alpha multiplier
    inline ImU32 u32(const ImVec4& c, float alpha_mul = 1.0f) {
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f),
                        static_cast<int>(c.w * alpha_mul * 255.0f));
    }

    // ── Runtime tweak enums (wired to the Tweaks panel in a later phase) ─────
    enum class Accent : uint8_t { Teal, Indigo, Amber, Mono };
    enum class CandleConvention : uint8_t { TealMag, Classic, Muted };

    namespace Tokens {
        // ── Surfaces — theme-tokens.json bg ramp (2026-07-02 redesign) ───────
        // bg-0 app background / chart canvas · bg-1 bars + panel chrome ·
        // bg-2 raised (hover rows, chips, active tab). Idle controls sit at
        // bg-1 (flat until interacted); hover/pressed raise to bg-2.
        inline constexpr ImVec4 BASE   = from_hex(0x0b0e13);  // bg-0
        inline constexpr ImVec4 PANEL  = from_hex(0x10141b);  // bg-1
        inline constexpr ImVec4 ELEV   = from_hex(0x161b24);  // bg-2
        inline constexpr ImVec4 INPUT  = from_hex(0x10141b);  // bg-1 — idle controls
        inline constexpr ImVec4 HOVER  = from_hex(0x161b24);  // bg-2
        inline constexpr ImVec4 ACTIVE = from_hex(0x161b24);  // bg-2 (accent marks "on")

        // ── Hairlines — line-1 borders/dividers, line-2 control borders ──────
        inline constexpr ImVec4 BD1  = from_hex(0x1e242e);          // line-1 — the only separation
        inline constexpr ImVec4 BD2  = from_hex(0x2a3140);          // line-2 — control borders
        inline constexpr ImVec4 BD3  = from_hex(0x566070, 0.55f);   // hover borders (text-3 hue)
        inline constexpr ImVec4 GRID = from_hex(0x1e242e, 0.55f);   // chart gridlines (line-1)

        // ── Text ramp — text-1/2/3 (+ dimmed text-3 for axis/disabled) ───────
        inline constexpr ImVec4 TX1 = from_hex(0xdfe6ee);  // primary numerals, titles
        inline constexpr ImVec4 TX2 = from_hex(0x8b95a5);  // labels, secondary data
        inline constexpr ImVec4 TX3 = from_hex(0x566070);  // captions, units, group labels
        inline constexpr ImVec4 TX4 = from_hex(0x566070, 0.72f);  // axis ticks, disabled

        // ── Semantic — runtime-mutable (candle convention / accent tweaks) ──
        inline ImVec4 UP         = from_hex(0x2fd6ad);         // up / bid / positive
        inline ImVec4 DOWN       = from_hex(0xee5c78);         // down / ask / negative
        inline ImVec4 UP_SOFT    = from_hex(0x2fd6ad, 0.13f);
        inline ImVec4 DOWN_SOFT  = from_hex(0xee5c78, 0.13f);
        inline ImVec4 UP_LINE    = from_hex(0x2fd6ad, 0.50f);
        inline ImVec4 DOWN_LINE  = from_hex(0xee5c78, 0.50f);
        inline ImVec4 BRAND      = from_hex(0x35c9c4);         // accent — the only on-state hue
        inline ImVec4 BRAND_SOFT = from_hex(0x35c9c4, 0.14f);
        inline ImVec4 BRAND_LINE = from_hex(0x35c9c4, 0.55f);
        inline ImVec4 BRAND_TX   = from_hex(0x3fe0d0);         // accent textTint (on-state text)
        inline constexpr ImVec4 WARN = from_hex(0xf0b350);     // funding, big prints, alerts
        inline constexpr ImVec4 WARN_SOFT = from_hex(0xf0b350, 0.12f);  // replay banner, high-regime chip fill
        // dark ink for text on solid BRAND fills (play orb, price chip, pills)
        inline constexpr ImVec4 BRAND_INK = from_hex(0x04181d);
        // resting-book blue (DOM v2): pending limit-order depth, side-agnostic. The
        // one neutral data hue — teal/rose stays reserved for EXECUTED flow. (Confirm
        // this hue or remap to a brand blue before shipping — dom.SPEC.md §8.)
        inline constexpr ImVec4 REST      = from_hex(0x4d8cd6);
        inline constexpr ImVec4 REST_SOFT = from_hex(0x4d8cd6, 0.30f);

        // ── Heatmap ramps (fixed LUT stops; full LUTs built in heatmap code) ─
        // viridis — liquidation field legend / colormap stops
        inline constexpr ImVec4 VIRIDIS[6] = {
            from_hex(0x440154), from_hex(0x414487), from_hex(0x2a788e),
            from_hex(0x22a884), from_hex(0x7ad151), from_hex(0xfde725)};
        // ocean — DOM depth cells (small=deep blue → large=green/yellow)
        inline constexpr ImVec4 OCEAN[6] = {
            from_hex(0x06101d), from_hex(0x0e2f5e), from_hex(0x1c6a8e),
            from_hex(0x26a59a), from_hex(0x7fd17a), from_hex(0xeaf06a)};
        // iceberg marker accent (DOM row tick)
        inline constexpr ImVec4 ICEBERG_VIOLET = from_hex(0xb07cff);
    }

    // ── Radii ────────────────────────────────────────────────────────────────
    // Chrome rules: docked panels are SQUARE with 1px line-1 borders only —
    // radius + shadow are reserved for floating chrome (settings panels, menus).
    namespace Radius {
        inline constexpr float R1 = 3.0f;   // small chips / table chips
        inline constexpr float R2 = 5.0f;   // buttons, inputs, frames
        inline constexpr float R3 = 6.0f;   // floating panels / popups / menus
    }

    // ── Layout metrics (px, logical) — theme-tokens.json `density` ──────────
    namespace Layout {
        inline constexpr float TOPBAR_H        = 44.0f;
        inline constexpr float STATSBAR_H      = 52.0f;   // v2 market header (was 62); closeable
        inline constexpr float PILLSROW_H      = 44.0f;   // v2 chart toolbar band (was 36)
        inline constexpr float STATUSBAR_H     = 22.0f;   // owns telemetry (WS · FPS · CLOCK)
        inline constexpr float TRANSPORT_H     = 66.0f;
        inline constexpr float WATCHLIST_W     = 256.0f;  // v2 dense-grid rail (was 384; ~1/3 narrower)
        inline constexpr float RIGHTCOL_W      = 420.0f;  // DOM over TAPE (was 300; v2 DOM needs ~440)
        inline constexpr float INDI_PANE_H     = 172.0f;
        inline constexpr float PANEL_HEADER_H  = 26.0f;
        inline constexpr float WATCHLIST_ROW_H = 24.0f;   // default-density watchlist row
        inline constexpr float DOMTAPE_ROW_H   = 18.0f;   // default-density DOM/tape row
        inline constexpr float PANEL_PAD       = 8.0f;
        inline constexpr float PRICE_GUTTER_R  = 66.0f;
        inline constexpr float TIME_AXIS_B     = 22.0f;
    }

    // ── Density (Tweaks-driven; 3–10, default 8) ─────────────────────────────
    int  density();
    void set_density(int d);                 // clamps to [3,10], re-derives row heights
    float row_h();                           // watchlist rows — d8 = WATCHLIST_ROW_H (24)
    float row_h_dense();                     // DOM/tape rows — d8 = DOMTAPE_ROW_H (18)

    // ── Runtime tweaks ───────────────────────────────────────────────────────
    Accent accent();
    CandleConvention candles();
    void set_accent(Accent a);                       // mutates BRAND* tokens
    void set_candle_convention(CandleConvention c);  // mutates UP*/DOWN* tokens

    // ── Fonts ────────────────────────────────────────────────────────────────
    // Hanken Grotesk = chrome/labels/headings. JetBrains Mono = ALL numerics
    // (naturally fixed-advance → tabular alignment for free).
    namespace Fonts {
        ImFont* ui();           // Hanken Regular 13 — default chrome text
        ImFont* ui_semibold();  // Hanken SemiBold 13 — emphasis, symbol names
        ImFont* heading();      // Hanken SemiBold 17 — panel/modal headings
        ImFont* label();        // Hanken SemiBold 9.5 — uppercase micro-labels
        ImFont* mono_xs();      // JBM SemiBold ~9.5 — EARLY ACCESS pill / micro badges
        ImFont* mono_sm();      // JBM Regular 10.5 — dense ladders/tape
        ImFont* mono();         // JBM Regular 12 — default numerics
        ImFont* mono_md();      // JBM Medium 14 — mark price, replay clock
        ImFont* mono_lg();      // JBM SemiBold 20 — large displays
    }

    // ── Style application ────────────────────────────────────────────────────
    void apply_dark_theme();      // full ImGui + ImPlot mapping from tokens
    void apply_trading_colors();  // table borders/headers (hairline style)
    bool load_fonts();            // builds the 8-face atlas (DPI-aware)

    // ── Tooltips ─────────────────────────────────────────────────────────────
    // The global WindowPadding is (0,0) (panels manage their own gutters) and
    // ImGui tooltip windows inherit it, so raw ImGui::SetTooltip/BeginTooltip
    // renders text flush against the border (looks clipped). All tooltips
    // route through these, which push floating-chrome padding + radius first.
    void begin_tooltip();
    void end_tooltip();
    void tooltip(const char* fmt, ...) IM_FMTARGS(1);  // SetTooltip replacement

    // ── Legacy compatibility layer — migrate widgets off these per-phase ─────
    namespace Colors {
        inline const ImVec4& BUY_GREEN       = Tokens::UP;
        inline const ImVec4& SELL_RED        = Tokens::DOWN;
        inline const ImVec4& BUY_GREEN_DIM   = Tokens::UP_LINE;
        inline const ImVec4& SELL_RED_DIM    = Tokens::DOWN_LINE;
        inline constexpr ImVec4 PRICE_YELLOW = Tokens::WARN;
        inline constexpr ImVec4 NEUTRAL_GRAY = Tokens::TX3;
        inline constexpr ImVec4 BACKGROUND_DARK = Tokens::BASE;
        inline constexpr ImVec4 PANEL_BG     = Tokens::PANEL;
        inline constexpr ImVec4 BORDER       = Tokens::BD2;
        inline constexpr ImVec4 TEXT_PRIMARY = Tokens::TX1;
        inline constexpr ImVec4 TEXT_SECONDARY = Tokens::TX2;
    }

    namespace FontSizes {  // legacy names, new scale
        inline constexpr float SMALL  = 13.0f;
        inline constexpr float NORMAL = 16.0f;
        inline constexpr float LARGE  = 20.0f;
        inline constexpr float XLARGE = 23.0f;
    }
    namespace Spacing {  // legacy names, new metrics
        inline constexpr float PADDING         = 8.0f;
        inline constexpr float ITEM_SPACING    = 6.0f;
        inline constexpr float WINDOW_ROUNDING = 0.0f;  // panels are square
        inline constexpr float FRAME_ROUNDING  = 5.0f;  // only pills/buttons round
    }

    // legacy font accessors → new atlas
    ImFont* get_regular_font();  // = Fonts::ui()
    ImFont* get_bold_font();     // = Fonts::ui_semibold()
    ImFont* get_large_font();    // = Fonts::mono_md()

    // legacy semantic helpers (read mutable tokens → tweak-aware)
    ImVec4 get_buy_color(float alpha = 1.0f);
    ImVec4 get_sell_color(float alpha = 1.0f);
    ImU32 get_buy_color_u32(uint8_t alpha = 255);
    ImU32 get_sell_color_u32(uint8_t alpha = 255);
}
