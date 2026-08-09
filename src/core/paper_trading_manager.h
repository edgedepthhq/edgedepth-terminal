#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// paper_trading_manager.h - Manages paper trading state from backend stream
//
// Receives PaperTradingSnapshot (on connect) and PaperPositionUpdate (live)
// via STREAM_PAPER_TRADING. Exposes const accessors for the PositionsPanel.
//
// Not per-symbol - global singleton. Max 10 concurrent positions.
// ═══════════════════════════════════════════════════════════════════════════════

#include "types/paper_trading_types.h"
#include <unordered_map>
#include <deque>
#include <array>
#include <span>
#include <cstdint>

namespace pb {
    class PaperTradingSnapshot;
    class PaperPositionUpdate;
    class PaperAccountState;
}

class PaperTradingManager {
public:
    static constexpr int EQUITY_RING_SIZE = 1440;  // 24h at 1/min
    static constexpr int MAX_CLOSED = 200;

    struct EquityPoint {
        int64_t timestamp_ms = 0;
        double equity = 0.0;
    };

    // ── Protobuf handlers (called from MessageHandler) ──────────────────
    void apply_snapshot(const pb::PaperTradingSnapshot& snapshot);
    void apply_position_update(const pb::PaperPositionUpdate& update);

    // ── Const accessors for UI ──────────────────────────────────────────
    [[nodiscard]] const std::unordered_map<std::string, Terminal::PaperPosition>& positions() const {
        return positions_;
    }
    [[nodiscard]] const Terminal::PaperAccount& account() const { return account_; }
    [[nodiscard]] const std::deque<Terminal::PaperPosition>& closed_trades() const {
        return closed_trades_;
    }
    [[nodiscard]] int equity_count() const { return equity_count_; }
    [[nodiscard]] const EquityPoint* equity_data() const { return equity_ring_.data(); }
    [[nodiscard]] int equity_head() const { return equity_head_; }

    // Returns linearized equity curve (oldest → newest) into caller's buffer
    void get_equity_curve(std::vector<EquityPoint>& out) const;

private:
    void apply_account(const pb::PaperAccountState& pb);
    Terminal::PaperPosition parse_position(const pb::PaperPositionUpdate& pb);
    void push_equity_point(int64_t timestamp_ms, double equity);

    // Active positions keyed by position_id
    std::unordered_map<std::string, Terminal::PaperPosition> positions_;

    // Account state
    Terminal::PaperAccount account_;

    // Equity curve ring buffer
    std::array<EquityPoint, EQUITY_RING_SIZE> equity_ring_{};
    int equity_head_  = 0;
    int equity_count_ = 0;

    // Recent closed trades (newest at front)
    std::deque<Terminal::PaperPosition> closed_trades_;
};
