#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// stats_widget.h - Stats panel v2 (premium microstructure surface).
//
// Dockable panel (~360 wide) that replaces the old floating Stats widget. Header +
// price, then three collapsible sections (order flow / positioning / liquidations
// + risk), a signal chip, and a settings disclosure row. Live data comes from the
// thin Stat stream (mark, funding, buys/sells, open interest, liquidations); the
// premium microstructure rows (VPIN, CVD, orderbook imbalance, smart money, OI
// velocity, nearest wall, cascade, contagion, signal) render as PLACEHOLDERS
// pending the backend analytics feed (roadmap O2 / analytics.proto) - no
// fabricated bindings. Pro-gated via Entitlements::is_pro(): the free tier redacts
// Pro rows and funnels the UNLOCK affordance into ui::UpsellModal.
//
// Style: colors ONLY via Theme::Tokens; numerals in JetBrains Mono (Fonts::mono*),
// chrome/labels in Hanken (Fonts::ui/label). Design bundle:
// watchlist_stats_DOM_redesign/edgedepth-stats-panel-v2/stats-panel.SPEC.md
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include "core/symbol_metadata.h"
#include "core/app_context.h"
#include "rendering/theme.h"
#include <string>
#include <cstdint>

class StatsWidget : public Widget {
public:
    StatsWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt);
    ~StatsWidget() override;
    void render() override;
    void update() override;
    WidgetType type() const override { return WidgetType::Stats; }
    const char* title() const override { return title_.c_str(); }
    void handle_stat(const Terminal::Stat& stat);

private:
    // Collapsible sections - order flow leads (premium value first).
    enum Section : uint8_t { SEC_ORDER_FLOW = 0, SEC_POSITIONING, SEC_LIQ_RISK, SEC_COUNT };

    Terminal::Pair    pair_;
    std::string       title_;
    StreamKey         stream_key_;
    const AppContext& ctx_;
    PriceFormatter    fmt_;
    Terminal::Stat    current_stat_{};
    Terminal::Stat    prev_stat_{};
    double            tps_ = 0.0;          // trades/sec (best-effort from stat deltas)
    bool              have_prev_ = false;
    bool              positioning_subscribed_ = false;  // live PositioningState (lazy, self-healing)

    // Session UI state
    bool section_open_[SEC_COUNT] = { true, true, true };
    bool settings_open_ = false;

    // Rendering (v2) - see stats_widget.cpp
    void render_status_strip(bool live);   // dot . STATS.SYM . PRO/FREE
    void render_replay_banner();           // replay only: AS OF ...
    void render_header_price();            // mark (live) + 24h/H-L/VOL (placeholder)
    bool render_section_header(Section s, const char* name, bool pro_section);
    void render_settings_row();            // pinned bottom disclosure

    // ── Section bodies (step 2) ─────────────────────────────────────────────
    void render_order_flow();
    void render_positioning();
    void render_liq_risk();
    void section_subhead(const char* txt);   // e.g. "SMART MONEY"

    // One metric row: label (left) + value (right) + optional chip / detail /
    // meter. Pro rows are redacted (label kept crisp, value/meter replaced with a
    // muted bar + lock) for free-tier viewers. Numerals JBM mono; colors Tokens.
    struct Row {
        const char* label      = "";
        bool        pro        = false;
        const char* value      = "--";
        ImVec4      value_col  = Theme::Tokens::TX1;
        const char* detail     = nullptr;
        ImVec4      detail_col = Theme::Tokens::TX3;
        int         meter      = 0;   // 0 none / 1 proportional / 2 directional / 3 fill
        float       meter_a    = 0.0f;
        ImVec4      meter_c1   = Theme::Tokens::UP;
        ImVec4      meter_c2   = Theme::Tokens::DOWN;
        const char* chip       = nullptr;  // regime chip drawn before the value
        int         chip_level = 0;        // amber escalation 0..3
    };
    void metric_row(const Row& r);
};
