#include "positions_panel.h"
#include "rendering/theme.h"
#include "core/symbol_metadata.h"
#include "core/display_time_zone.h"
#include "imgui.h"
#include "implot.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>

// ASCII bullet for hit indicator. Avoids font-pack assumptions - most ImGui
// builds restrict to ASCII unless extended ranges were configured. If the
// theme later loads a glyph range that supports U+2713, swap to "✓".
#define ICON_HIT "*"

// tp_progress_pct returns how far the mark price has traveled from entry
// toward a given TP level, as a 0-100 percent. Returns -1 if not computable.
//   long:   pct = (mark - entry) / (tp - entry) * 100
//   short:  pct = (entry - mark) / (entry - tp) * 100
// Clamped: negative travel returns 0, beyond TP returns 100.
static double tp_progress_pct(const Terminal::PaperPosition& pos, double tp_level) {
    if (tp_level <= 0 || pos.entry_price <= 0) return -1.0;
    double num, den;
    if (pos.is_long()) {
        num = pos.mark_price - pos.entry_price;
        den = tp_level - pos.entry_price;
    } else {
        num = pos.entry_price - pos.mark_price;
        den = pos.entry_price - tp_level;
    }
    if (den <= 0) return -1.0; // misconfigured TP - silently skip
    double pct = (num / den) * 100.0;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// Color for a TP-progress percentage. Three bands:
//   0-50%   → secondary text (early, no signal)
//   50-90%  → amber (approaching)
//   90-100% → green (about to hit)
static ImVec4 tp_progress_color(double pct) {
    if (pct < 50.0)  return Theme::Colors::TEXT_SECONDARY;
    if (pct < 90.0)  return ImVec4(0.95f, 0.75f, 0.20f, 1.0f); // amber
    return Theme::Colors::BUY_GREEN;
}

PositionsPanel::PositionsPanel(const AppContext& ctx)
    : ctx_(ctx) {
    is_open = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Duration formatting helper
// ═══════════════════════════════════════════════════════════════════════════════

const char* PositionsPanel::format_duration(int64_t seconds, char* buf, size_t buf_size) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%llds", (long long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, buf_size, "%lldm", (long long)(seconds / 60));
    } else if (seconds < 86400) {
        int64_t h = seconds / 3600;
        int64_t m = (seconds % 3600) / 60;
        snprintf(buf, buf_size, "%lldh %lldm", (long long)h, (long long)m);
    } else {
        int64_t d = seconds / 86400;
        int64_t h = (seconds % 86400) / 3600;
        snprintf(buf, buf_size, "%lldd %lldh", (long long)d, (long long)h);
    }
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Account summary bar - horizontal compact row
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render_account_summary(const Terminal::PaperAccount& acct, double open_pnl) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    ImGui::BeginChild("##acct_summary", ImVec2(0, 32), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    float x = 8.0f;
    float y = 7.0f;
    auto label = [&](const char* name, const char* fmt, double val, bool color_val = false) {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "%s", name);
        x += ImGui::CalcTextSize(name).x + 4;
        ImGui::SetCursorPos(ImVec2(x, y));
        char buf[32];
        snprintf(buf, sizeof(buf), fmt, val);
        if (color_val) {
            ImVec4 c = val >= 0 ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
            ImGui::TextColored(c, "%s", buf);
        } else {
            ImGui::Text("%s", buf);
        }
        x += ImGui::CalcTextSize(buf).x + 20;
    };

    label("Balance:", "$%.2f", acct.balance);
    label("Equity:", "$%.2f", acct.equity);
    label("Open P&L:", "$%.2f", open_pnl, true);

    // Separator
    ImGui::SetCursorPos(ImVec2(x, 4));
    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "|");
    x += 16;

    char wr_buf[16];
    snprintf(wr_buf, sizeof(wr_buf), "%.1f%%", acct.win_rate());
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "WR:");
    x += ImGui::CalcTextSize("WR:").x + 4;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::Text("%s", wr_buf);
    x += ImGui::CalcTextSize(wr_buf).x + 20;

    label("PF:", "%.2f", acct.profit_factor);
    label("Max DD:", "%.1f%%", acct.max_drawdown_pct);

    // Show closed trades (W+L) as the trade count, not total_trades which
    // may include stale counts from account state persistence.
    int closed = acct.winning_trades + acct.losing_trades;
    char trades_buf[32];
    snprintf(trades_buf, sizeof(trades_buf), "%d (%dW / %dL)",
        closed, acct.winning_trades, acct.losing_trades);
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "Trades:");
    x += ImGui::CalcTextSize("Trades:").x + 4;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::Text("%s", trades_buf);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Active positions table
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render_positions_tab() {
    auto& mgr = ctx_.paper_trading_mgr();
    const auto& positions = mgr.positions();

    if (positions.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 40);
        float w = ImGui::GetContentRegionAvail().x;
        float tw = ImGui::CalcTextSize("No open positions").x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "No open positions");
        return;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##active_positions", 15, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Symbol",     0,                                0.0f, 0);
    ImGui::TableSetupColumn("Side",       ImGuiTableColumnFlags_NoSort,     0.0f, 1);
    ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_NoSort,     0.0f, 2);
    ImGui::TableSetupColumn("Entry",      ImGuiTableColumnFlags_NoSort,     0.0f, 3);
    ImGui::TableSetupColumn("Mark",       ImGuiTableColumnFlags_NoSort,     0.0f, 4);
    ImGui::TableSetupColumn("P&L",        ImGuiTableColumnFlags_DefaultSort, 0.0f, 5);
    ImGui::TableSetupColumn("P&L %",      0,                                0.0f, 6);
    ImGui::TableSetupColumn("MAE%",       0,                                0.0f, 7);
    ImGui::TableSetupColumn("MFE%",       0,                                0.0f, 8);
    ImGui::TableSetupColumn("Size",       0,                                0.0f, 9);
    ImGui::TableSetupColumn("Lev",        0,                                0.0f, 10);
    ImGui::TableSetupColumn("Stop",       ImGuiTableColumnFlags_NoSort,     0.0f, 11);
    ImGui::TableSetupColumn("TP1",        ImGuiTableColumnFlags_NoSort,     0.0f, 12);
    ImGui::TableSetupColumn("TP2",        ImGuiTableColumnFlags_NoSort,     0.0f, 13);
    ImGui::TableSetupColumn("Duration",   0,                                0.0f, 14);
    ImGui::TableHeadersRow();

    // ── Sort positions ──────────────────────────────────────────────────
    // Collect pointers for sorting (avoid copying)
    static std::vector<const Terminal::PaperPosition*> sorted;
    sorted.clear();
    sorted.reserve(positions.size());
    for (const auto& [id, pos] : positions) sorted.push_back(&pos);

    if (auto* sort_specs = ImGui::TableGetSortSpecs()) {
        if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
            sort_specs->SpecsDirty = false;
        }
        if (sort_specs->SpecsCount > 0) {
            auto col = sort_specs->Specs[0].ColumnUserID;
            bool asc = sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::sort(sorted.begin(), sorted.end(),
                [col, asc](const Terminal::PaperPosition* a, const Terminal::PaperPosition* b) {
                    double va = 0, vb = 0;
                    switch (col) {
                        case 0:  { int cmp = a->symbol.compare(b->symbol); return asc ? cmp < 0 : cmp > 0; }
                        case 5:  va = a->unrealized_pnl;       vb = b->unrealized_pnl;       break;
                        case 6:  va = a->unrealized_pnl_pct;   vb = b->unrealized_pnl_pct;   break;
                        case 7:  va = a->mae_pct;              vb = b->mae_pct;              break;
                        case 8:  va = a->mfe_pct;              vb = b->mfe_pct;              break;
                        case 9:  va = a->notional_usd;         vb = b->notional_usd;         break;
                        case 10: va = a->leverage;             vb = b->leverage;             break;
                        case 13: return false; // TP2 column - no sort
                        case 14: va = (double)a->duration_seconds; vb = (double)b->duration_seconds; break;
                        default: return false;
                    }
                    return asc ? va < vb : va > vb;
                });
        }
    }

    auto& reg = SymbolRegistry::instance();

    for (const auto* pos_ptr : sorted) {
        const auto& pos = *pos_ptr;
        ImGui::TableNextRow();

        // Symbol
        ImGui::TableNextColumn();
        // Uppercase symbol for display
        std::string sym_upper = pos.symbol;
        for (auto& c : sym_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        ImGui::Text("%s", sym_upper.c_str());

        // Side - colored
        ImGui::TableNextColumn();
        if (pos.is_long()) {
            ImGui::TextColored(Theme::Colors::BUY_GREEN, "LONG");
        } else {
            ImGui::TextColored(Theme::Colors::SELL_RED, "SHORT");
        }

        // Alert type
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "%s", pos.alert_type.c_str());

        // Entry price
        ImGui::TableNextColumn();
        PriceFormatter fmt = reg.get_formatter("binancef", pos.symbol);
        ImGui::Text(fmt.price_fmt, pos.entry_price);

        // Mark price
        ImGui::TableNextColumn();
        ImGui::Text(fmt.price_fmt, pos.mark_price);

        // Unrealized P&L - colored
        ImGui::TableNextColumn();
        ImVec4 pnl_color = pos.unrealized_pnl >= 0
            ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        ImGui::TextColored(pnl_color, "$%.2f", pos.unrealized_pnl);

        // P&L %
        ImGui::TableNextColumn();
        ImGui::TextColored(pnl_color, "%.2f%%", pos.unrealized_pnl_pct);

        // MAE%
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::SELL_RED, "%.2f%%", pos.mae_pct);

        // MFE%
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::BUY_GREEN, "%.2f%%", pos.mfe_pct);

        // Size (notional USD) - show remaining pill when partially closed
        ImGui::TableNextColumn();
        if (pos.is_partially_closed()) {
            ImGui::Text("$%.0f", pos.notional_usd);
            ImGui::SameLine();
            int pct = static_cast<int>(pos.remaining_qty_pct * 100.0 + 0.5);
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "(%d%%)", pct);
        } else {
            ImGui::Text("$%.0f", pos.notional_usd);
        }

        // Leverage
        ImGui::TableNextColumn();
        if (pos.leverage > 0) {
            ImGui::Text("%.0fx", pos.leverage);
        } else {
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "-");
        }

        // Stop level
        ImGui::TableNextColumn();
        if (pos.trailing_stop_level > 0) {
            ImGui::Text(fmt.price_fmt, pos.trailing_stop_level);
        } else {
            ImGui::Text(fmt.price_fmt, pos.stop_level);
        }

        // TP1 level - render with state context
        //  - if tp1_hit: green ✓ + price
        //  - else if tp1_level > 0: price + colored progress pct (50%/90% bands)
        //  - else if tp_strategy == "trail_only": "Trail" (no fixed TP by design)
        //  - else: "-" (no TP configured / pre-fix historical)
        ImGui::TableNextColumn();
        if (pos.tp1_hit) {
            ImGui::TextColored(Theme::Colors::BUY_GREEN, "%s ", ICON_HIT);
            ImGui::SameLine(0, 0);
            ImGui::Text(fmt.price_fmt, pos.tp1_level);
        } else if (pos.tp1_level > 0) {
            ImGui::Text(fmt.price_fmt, pos.tp1_level);
            double prog = tp_progress_pct(pos, pos.tp1_level);
            if (prog >= 0) {
                ImGui::SameLine();
                ImGui::TextColored(tp_progress_color(prog), "(%.0f%%)", prog);
            }
        } else if (pos.tp_strategy == "trail_only") {
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "Trail");
        } else {
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "-");
        }

        // TP2 level - same logic
        ImGui::TableNextColumn();
        if (pos.tp2_hit) {
            ImGui::TextColored(Theme::Colors::BUY_GREEN, "%s ", ICON_HIT);
            ImGui::SameLine(0, 0);
            ImGui::Text(fmt.price_fmt, pos.tp2_level);
        } else if (pos.tp2_level > 0) {
            ImGui::Text(fmt.price_fmt, pos.tp2_level);
            double prog = tp_progress_pct(pos, pos.tp2_level);
            if (prog >= 0) {
                ImGui::SameLine();
                ImGui::TextColored(tp_progress_color(prog), "(%.0f%%)", prog);
            }
        } else {
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "-");
        }

        // Duration - compute live from opened_at for open positions
        ImGui::TableNextColumn();
        char dur_buf[32];
        {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t live_dur = (now_ms - pos.opened_at) / 1000;
            if (live_dur < 0) live_dur = 0;
            format_duration(live_dur, dur_buf, sizeof(dur_buf));
        }
        ImGui::Text("%s", dur_buf);
    }
    ImGui::EndTable();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Equity curve
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render_equity_tab() {
    auto& mgr = ctx_.paper_trading_mgr();
    mgr.get_equity_curve(equity_cache_);

    if (equity_cache_.size() < 2) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 40);
        float w = ImGui::GetContentRegionAvail().x;
        float tw = ImGui::CalcTextSize("Awaiting equity data...").x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "Awaiting equity data...");
        return;
    }

    // Build SoA arrays for ImPlot
    const int n = static_cast<int>(equity_cache_.size());
    static std::vector<double> xs, ys;
    xs.resize(n);
    ys.resize(n);

    const double initial = mgr.account().initial_balance;
    for (int i = 0; i < n; ++i) {
        xs[i] = static_cast<double>(equity_cache_[i].timestamp_ms) / 1000.0;
        ys[i] = equity_cache_[i].equity - initial;  // cumulative P&L
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImPlot::BeginPlot("##equity_curve", avail)) {
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxes("Time", "Cumulative P&L ($)");
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, -HUGE_VAL, HUGE_VAL);

        // Zero reference line
        double x_range[2] = { xs.front(), xs.back() };
        double y_zero[2] = { 0.0, 0.0 };
        ImPlotSpec zero_spec;
        zero_spec.LineColor = ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
        zero_spec.LineWeight = 1.0f;
        ImPlot::PlotLine("##zero", x_range, y_zero, 2, zero_spec);

        // Equity line - green if positive final, red if negative
        ImPlotSpec line_spec;
        line_spec.LineColor = ys.back() >= 0
            ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        line_spec.LineWeight = 2.0f;
        ImPlot::PlotLine("P&L", xs.data(), ys.data(), n, line_spec);

        ImPlot::EndPlot();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Trade journal - closed trades
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render_journal_tab() {
    auto& mgr = ctx_.paper_trading_mgr();
    const auto& trades = mgr.closed_trades();

    if (trades.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 40);
        float w = ImGui::GetContentRegionAvail().x;
        float tw = ImGui::CalcTextSize("No closed trades yet").x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "No closed trades yet");
        return;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##trade_journal", 12, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time",     0, 0.0f, 0);
    ImGui::TableSetupColumn("Symbol",   0, 0.0f, 1);
    ImGui::TableSetupColumn("Side",     ImGuiTableColumnFlags_NoSort, 0.0f, 2);
    ImGui::TableSetupColumn("Type",     0, 0.0f, 3);
    ImGui::TableSetupColumn("Entry",    ImGuiTableColumnFlags_NoSort, 0.0f, 4);
    ImGui::TableSetupColumn("Exit",     ImGuiTableColumnFlags_NoSort, 0.0f, 5);
    ImGui::TableSetupColumn("P&L",      ImGuiTableColumnFlags_DefaultSort, 0.0f, 6);
    ImGui::TableSetupColumn("R-Mult",   0, 0.0f, 7);
    ImGui::TableSetupColumn("MAE%",     ImGuiTableColumnFlags_NoSort, 0.0f, 8);
    ImGui::TableSetupColumn("MFE%",     ImGuiTableColumnFlags_NoSort, 0.0f, 9);
    ImGui::TableSetupColumn("Exit Reason", 0, 0.0f, 10);
    ImGui::TableSetupColumn("Duration", 0, 0.0f, 11);
    ImGui::TableHeadersRow();

    auto& reg = SymbolRegistry::instance();

    for (const auto& pos : trades) {
        ImGui::TableNextRow();

        // Subtle win/loss row tint
        if (pos.realized_pnl >= 0) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                ImGui::GetColorU32(ImVec4(0.0f, 0.3f, 0.15f, 0.08f)));
        } else {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                ImGui::GetColorU32(ImVec4(0.3f, 0.0f, 0.05f, 0.08f)));
        }

        // Time (closed_at)
        ImGui::TableNextColumn();
        if (pos.closed_at > 0) {
            char time_str[24]{};
            DisplayTimeZone::instance().format(pos.closed_at,
                TimeZoneFormat::MonthDayTime, time_str, sizeof(time_str));
            ImGui::Text("%s", time_str);
        }

        // Symbol
        ImGui::TableNextColumn();
        std::string sym_upper = pos.symbol;
        for (auto& c : sym_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        ImGui::Text("%s", sym_upper.c_str());

        // Side
        ImGui::TableNextColumn();
        if (pos.is_long()) {
            ImGui::TextColored(Theme::Colors::BUY_GREEN, "LONG");
        } else {
            ImGui::TextColored(Theme::Colors::SELL_RED, "SHORT");
        }

        // Alert type
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "%s", pos.alert_type.c_str());

        // Entry / Exit price
        PriceFormatter fmt = reg.get_formatter("binancef", pos.symbol);
        ImGui::TableNextColumn();
        ImGui::Text(fmt.price_fmt, pos.entry_price);
        ImGui::TableNextColumn();
        ImGui::Text(fmt.price_fmt, pos.mark_price);  // mark_price = exit price for closed

        // Realized P&L - annotate with partial-fill indicator if TP fired
        ImGui::TableNextColumn();
        ImVec4 c = pos.realized_pnl >= 0
            ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        ImGui::TextColored(c, "$%.2f", pos.realized_pnl);
        if (pos.tp1_hit || pos.tp2_hit) {
            ImGui::SameLine();
            const char* tag = pos.tp2_hit ? "TP1+TP2" : "TP1";
            ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "(%s)", tag);
        }

        // R-multiple
        ImGui::TableNextColumn();
        ImGui::TextColored(c, "%.2fR", pos.r_multiple);

        // MAE% / MFE%
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::SELL_RED, "%.2f%%", pos.mae_pct);
        ImGui::TableNextColumn();
        ImGui::TextColored(Theme::Colors::BUY_GREEN, "%.2f%%", pos.mfe_pct);

        // Exit reason
        ImGui::TableNextColumn();
        ImGui::Text("%s", pos.exit_reason.c_str());

        // Duration
        ImGui::TableNextColumn();
        char dur_buf[32];
        format_duration(pos.duration_seconds, dur_buf, sizeof(dur_buf));
        ImGui::Text("%s", dur_buf);
    }
    ImGui::EndTable();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Performance by alert type
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render_by_type_tab() {
    auto& mgr = ctx_.paper_trading_mgr();
    const auto& trades = mgr.closed_trades();

    if (trades.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 40);
        float w = ImGui::GetContentRegionAvail().x;
        float tw = ImGui::CalcTextSize("No closed trades yet").x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(Theme::Colors::TEXT_SECONDARY, "No closed trades yet");
        return;
    }

    // Aggregate stats per alert type
    struct TypeStats {
        int count = 0;
        int wins = 0;
        double total_pnl = 0.0;
        double total_r = 0.0;
        int64_t total_duration = 0;
    };
    std::unordered_map<std::string, TypeStats> by_type;

    for (const auto& t : trades) {
        auto& s = by_type[t.alert_type];
        s.count++;
        if (t.realized_pnl > 0) s.wins++;
        s.total_pnl += t.realized_pnl;
        s.total_r += t.r_multiple;
        s.total_duration += t.duration_seconds;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##by_type", 6, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Alert Type");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Win Rate");
    ImGui::TableSetupColumn("Avg R");
    ImGui::TableSetupColumn("Total P&L");
    ImGui::TableSetupColumn("Avg Duration");
    ImGui::TableHeadersRow();

    for (const auto& [type, s] : by_type) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%s", type.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%d", s.count);

        ImGui::TableNextColumn();
        double wr = 100.0 * static_cast<double>(s.wins) / static_cast<double>(s.count);
        ImVec4 wr_color = wr >= 50.0 ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        ImGui::TextColored(wr_color, "%.1f%%", wr);

        ImGui::TableNextColumn();
        double avg_r = s.total_r / static_cast<double>(s.count);
        ImVec4 r_color = avg_r >= 0 ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        ImGui::TextColored(r_color, "%.2fR", avg_r);

        ImGui::TableNextColumn();
        ImVec4 pnl_color = s.total_pnl >= 0
            ? Theme::Colors::BUY_GREEN : Theme::Colors::SELL_RED;
        ImGui::TextColored(pnl_color, "$%.2f", s.total_pnl);

        ImGui::TableNextColumn();
        char dur_buf[32];
        int64_t avg_dur = s.total_duration / s.count;
        format_duration(avg_dur, dur_buf, sizeof(dur_buf));
        ImGui::Text("%s", dur_buf);
    }
    ImGui::EndTable();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main render - tab bar with all sections
// ═══════════════════════════════════════════════════════════════════════════════

void PositionsPanel::render() {
    if (!is_open) return;

    ImGui::SetNextWindowSize(ImVec2(900, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Paper Trading###paper_trading", &is_open)) {
        ImGui::End();
        return;
    }

    // Account summary bar always visible
    auto& mgr = ctx_.paper_trading_mgr();
    const auto& acct = mgr.account();

    // Calculate total open P&L
    double open_pnl = 0.0;
    for (const auto& [id, pos] : mgr.positions()) {
        open_pnl += pos.unrealized_pnl;
    }
    render_account_summary(acct, open_pnl);

    ImGui::Spacing();

    // Tab bar
    if (ImGui::BeginTabBar("##paper_tabs")) {
        if (ImGui::BeginTabItem("Positions")) {
            render_positions_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Equity")) {
            render_equity_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Journal")) {
            render_journal_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("By Type")) {
            render_by_type_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
