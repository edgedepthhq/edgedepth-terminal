#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// positions_panel.h - Paper trading positions, equity curve, and trade journal
//
// Global panel (not per-symbol). Reads from PaperTradingManager.
// Three tabs: Positions (active + account summary), Equity curve, Journal.
// ═══════════════════════════════════════════════════════════════════════════════

#include "ui/widget.h"
#include "core/app_context.h"
#include "core/paper_trading_manager.h"
#include "types/paper_trading_types.h"
#include <vector>

class PositionsPanel : public Widget {
public:
    explicit PositionsPanel(const AppContext& ctx);
    ~PositionsPanel() override = default;

    void update() override {}
    void render() override;
    [[nodiscard]] WidgetType type() const override { return WidgetType::PaperTrading; }
    [[nodiscard]] const char* title() const override { return "Paper Trading"; }
    [[nodiscard]] UpdateFrequency update_frequency() const override {
        return UpdateFrequency::Standard;
    }

private:
    const AppContext& ctx_;

    // Equity curve cache (linearized from ring buffer each frame)
    mutable std::vector<PaperTradingManager::EquityPoint> equity_cache_;

    void render_account_summary(const Terminal::PaperAccount& acct, double open_pnl);
    void render_positions_tab();
    void render_equity_tab();
    void render_journal_tab();
    void render_by_type_tab();

    // Helpers
    static const char* format_duration(int64_t seconds, char* buf, size_t buf_size);
};
