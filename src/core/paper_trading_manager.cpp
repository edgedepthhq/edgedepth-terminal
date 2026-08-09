#include "paper_trading_manager.h"
#include "pb/messages.pb.h"

// ═══════════════════════════════════════════════════════════════════════════════
// Protobuf → Terminal struct conversion
// ═══════════════════════════════════════════════════════════════════════════════

Terminal::PaperPosition PaperTradingManager::parse_position(const pb::PaperPositionUpdate& pb) {
    Terminal::PaperPosition p;
    p.position_id        = pb.position_id();
    p.symbol             = pb.symbol();
    p.side               = pb.side();
    p.alert_type         = pb.alert_type();
    p.entry_price        = pb.entry_price();
    p.mark_price         = pb.mark_price();
    p.quantity           = pb.quantity();
    p.notional_usd       = pb.notional_usd();
    p.leverage           = pb.leverage();
    p.unrealized_pnl     = pb.unrealized_pnl();
    p.unrealized_pnl_pct = pb.unrealized_pnl_pct();
    p.mae_pct            = pb.mae_pct();
    p.mfe_pct            = pb.mfe_pct();
    p.stop_level         = pb.stop_level();
    p.tp1_level          = pb.tp1_level();
    p.tp2_level          = pb.tp2_level();
    p.trailing_stop_level = pb.trailing_stop_level();
    p.funding_costs      = pb.funding_costs();
    p.entry_atr          = pb.entry_atr();
    p.ml_score           = pb.ml_score();
    p.opened_at          = pb.opened_at();
    p.closed_at          = pb.closed_at();
    p.exit_reason        = pb.exit_reason();
    p.realized_pnl       = pb.realized_pnl();
    p.r_multiple         = pb.r_multiple();
    p.slippage_entry     = pb.slippage_entry();
    p.slippage_exit      = pb.slippage_exit();
    p.duration_seconds   = pb.duration_seconds();
    // Partial-fill state (proto fields 29-32, added 2026-04-16)
    p.tp1_hit            = pb.tp1_hit();
    p.tp2_hit            = pb.tp2_hit();
    p.remaining_qty_pct  = pb.remaining_qty_pct();
    p.tp_strategy        = pb.tp_strategy();
    return p;
}

void PaperTradingManager::apply_account(const pb::PaperAccountState& pb) {
    account_.balance          = pb.balance();
    account_.equity           = pb.equity();
    account_.open_pnl         = pb.open_pnl();
    account_.initial_balance  = pb.initial_balance();
    account_.total_trades     = pb.total_trades();
    account_.winning_trades   = pb.winning_trades();
    account_.losing_trades    = pb.losing_trades();
    account_.total_pnl        = pb.total_pnl();
    account_.total_funding    = pb.total_funding();
    account_.peak_equity      = pb.peak_equity();
    account_.max_drawdown_pct = pb.max_drawdown_pct();
    account_.profit_factor    = pb.profit_factor();
    account_.timestamp_ms     = pb.timestamp_ms();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Snapshot handler - full state load on client connect
// ═══════════════════════════════════════════════════════════════════════════════

void PaperTradingManager::apply_snapshot(const pb::PaperTradingSnapshot& snapshot) {
    // Account state
    if (snapshot.has_account()) {
        apply_account(snapshot.account());
        push_equity_point(account_.timestamp_ms, account_.equity);
    }

    // Replace all open positions
    positions_.clear();
    for (const auto& pos_pb : snapshot.positions()) {
        auto pos = parse_position(pos_pb);
        positions_[pos.position_id] = std::move(pos);
    }

    // Replace closed trades
    closed_trades_.clear();
    for (const auto& pos_pb : snapshot.recent_closed()) {
        closed_trades_.push_back(parse_position(pos_pb));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Incremental position update - open, update, or close
// ═══════════════════════════════════════════════════════════════════════════════

void PaperTradingManager::apply_position_update(const pb::PaperPositionUpdate& pb) {
    auto pos = parse_position(pb);

    if (pos.is_open()) {
        // Open or update - upsert into active positions
        positions_[pos.position_id] = std::move(pos);
    } else {
        // Closed - remove from active, add to journal
        positions_.erase(pos.position_id);
        closed_trades_.push_front(std::move(pos));
        while (static_cast<int>(closed_trades_.size()) > MAX_CLOSED) {
            closed_trades_.pop_back();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Equity ring buffer
// ═══════════════════════════════════════════════════════════════════════════════

void PaperTradingManager::push_equity_point(int64_t timestamp_ms, double equity) {
    equity_ring_[equity_head_] = {timestamp_ms, equity};
    equity_head_ = (equity_head_ + 1) % EQUITY_RING_SIZE;
    if (equity_count_ < EQUITY_RING_SIZE) {
        equity_count_++;
    }
}

void PaperTradingManager::get_equity_curve(std::vector<EquityPoint>& out) const {
    out.clear();
    if (equity_count_ == 0) return;
    out.reserve(equity_count_);

    // Start from oldest entry
    int start = (equity_count_ < EQUITY_RING_SIZE)
        ? 0
        : equity_head_;  // head points to next write = oldest when full

    for (int i = 0; i < equity_count_; ++i) {
        int idx = (start + i) % EQUITY_RING_SIZE;
        out.push_back(equity_ring_[idx]);
    }
}
