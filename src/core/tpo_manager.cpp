#include "tpo_manager.h"
#include <cstdio>
#include <numeric>
#include <functional>
#include <unordered_set>

// ═══════════════════════════════════════════════════════════════════════════════
// Session boundary helpers
// ═══════════════════════════════════════════════════════════════════════════════

// Align a timestamp (ms) down to session start boundary.
// For daily sessions: floor to 00:00 UTC of that day.
static int64_t align_session_start(int64_t ts_ms, int session_hours) {
    const int64_t period_ms = static_cast<int64_t>(session_hours) * 3600LL * 1000LL;
    // Floor to midnight UTC for daily (works for 24h multiples too)
    if (session_hours == 24) {
        const int64_t day_ms = 86400LL * 1000LL;
        return (ts_ms / day_ms) * day_ms;
    }
    // For other periods, align to epoch
    return (ts_ms / period_ms) * period_ms;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Build sessions from candle data
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::build_sessions(
    const std::string& symbol,
    const double* timestamps,
    const double* highs,
    const double* lows,
    size_t candle_count,
    int64_t timeframe_sec,
    double tick_per_row)
{
    if (!timestamps || !highs || !lows || candle_count == 0) return;

    // Simple hash to avoid redundant rebuilds
    uint64_t hash = candle_count * 31 +
        static_cast<uint64_t>(timestamps[0]) +
        static_cast<uint64_t>(timestamps[candle_count - 1]) +
        static_cast<uint64_t>(tick_per_row * 1000.0);
    auto& prev_hash = last_build_hash_[symbol];
    if (hash == prev_hash) return;
    prev_hash = hash;

    const int64_t session_ms = static_cast<int64_t>(session_period_hours) * 3600LL * 1000LL;
    auto& sessions = data_[symbol];
    sessions.clear();

    // Group candles into sessions
    size_t i = 0;
    while (i < candle_count) {
        int64_t candle_ts = static_cast<int64_t>(timestamps[i]);
        int64_t sess_start = align_session_start(candle_ts, session_period_hours);
        int64_t sess_end = sess_start + session_ms;

        // Find all candles in this session
        size_t sess_first = i;
        while (i < candle_count &&
               static_cast<int64_t>(timestamps[i]) < sess_end) {
            ++i;
        }
        size_t sess_last = i; // exclusive

        if (sess_last <= sess_first) continue;

        TPOSession session;
        build_single_session(session, timestamps, highs, lows,
                             sess_first, sess_last,
                             sess_start, sess_end,
                             timeframe_sec, tick_per_row);
        sessions.push_back(std::move(session));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Build a single session from candles in range [start_idx, end_idx)
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::build_single_session(
    TPOSession& session,
    const double* timestamps,
    const double* highs,
    const double* lows,
    size_t start_idx, size_t end_idx,
    int64_t session_start_ms,
    int64_t session_end_ms,
    int64_t timeframe_sec,
    double tick_per_row)
{
    session.session_start_ms = session_start_ms;
    session.session_end_ms = session_end_ms;

    // Find session price range
    double sess_high = -1e18, sess_low = 1e18;
    for (size_t i = start_idx; i < end_idx; ++i) {
        if (highs[i] > sess_high) sess_high = highs[i];
        if (lows[i] < sess_low)   sess_low = lows[i];
    }
    session.session_high = sess_high;
    session.session_low = sess_low;

    // Auto tick_per_row if not specified
    double tpr = tick_per_row;
    if (tpr <= 0.0) {
        double range = sess_high - sess_low;
        if (range <= 0.0) range = 1.0;
        // Target ~40-60 rows per session
        tpr = range / 50.0;
        if (tpr < 1e-8) tpr = 1e-8;
    }
    session.tick_per_row = tpr;

    // Compute row grid
    int row_lo_idx = static_cast<int>(std::floor(sess_low / tpr));
    int row_hi_idx = static_cast<int>(std::floor(sess_high / tpr));
    int num_rows = row_hi_idx - row_lo_idx + 1;
    if (num_rows <= 0 || num_rows > 10000) return; // sanity

    session.rows.resize(num_rows);
    for (int r = 0; r < num_rows; ++r) {
        session.rows[r].row_idx = r;
        session.rows[r].price_lo = (row_lo_idx + r) * tpr;
        session.rows[r].price_hi = (row_lo_idx + r + 1) * tpr;
    }

    // Assign blocks: group candles into 30m periods regardless of actual timeframe.
    // Multiple candles within the same 30m window contribute to the same period
    // but we only record each period once per row (dedup via period_idx tracking).
    static constexpr int64_t PERIOD_MS = 30LL * 60 * 1000; // 30 minutes
    session.total_periods = 0;
    session.total_blocks = 0;

    // Track which (row, period) pairs have been assigned to avoid duplicates
    // when multiple candles fall within the same 30m period
    // Use a flat set: row_idx * 1000 + period_idx (safe for < 1000 periods/day)
    std::unordered_set<int64_t> assigned;

    for (size_t i = start_idx; i < end_idx; ++i) {
        int64_t candle_ts = static_cast<int64_t>(timestamps[i]);
        // Period index = which 30m slot within the session
        int period_idx = static_cast<int>((candle_ts - session_start_ms) / PERIOD_MS);
        session.total_periods = std::max(session.total_periods, period_idx + 1);

        // Which rows does this candle touch?
        int candle_row_lo = static_cast<int>(std::floor(lows[i] / tpr)) - row_lo_idx;
        int candle_row_hi = static_cast<int>(std::floor(highs[i] / tpr)) - row_lo_idx;
        candle_row_lo = std::max(0, candle_row_lo);
        candle_row_hi = std::min(num_rows - 1, candle_row_hi);

        for (int r = candle_row_lo; r <= candle_row_hi; ++r) {
            int64_t key = static_cast<int64_t>(r) * 1000 + period_idx;
            if (assigned.insert(key).second) {
                // First time this (row, period) pair seen
                session.rows[r].blocks.push_back({period_idx});
                session.total_blocks++;
            }
        }
    }

    // Cache max block count
    session.max_block_count = 0;
    for (const auto& row : session.rows) {
        int bc = static_cast<int>(row.blocks.size());
        if (bc > session.max_block_count) session.max_block_count = bc;
    }

    // Compute derived data
    compute_poc(session);
    compute_value_area(session);
    detect_single_prints(session);
    detect_poor_high_low(session);
    compute_initial_balance(session, highs, lows, start_idx, end_idx);
}

// ═══════════════════════════════════════════════════════════════════════════════
// POC — widest row. Tie-break: closest to session midpoint.
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::compute_poc(TPOSession& session) {
    if (session.rows.empty()) return;

    double midpoint = (session.session_high + session.session_low) * 0.5;
    int best_idx = 0;
    int best_count = 0;
    double best_dist = 1e18;

    for (size_t i = 0; i < session.rows.size(); ++i) {
        int count = static_cast<int>(session.rows[i].blocks.size());
        double row_mid = (session.rows[i].price_lo + session.rows[i].price_hi) * 0.5;
        double dist = std::abs(row_mid - midpoint);

        if (count > best_count || (count == best_count && dist < best_dist)) {
            best_idx = static_cast<int>(i);
            best_count = count;
            best_dist = dist;
        }
    }

    session.poc_row_idx = best_idx;
    session.poc_price = (session.rows[best_idx].price_lo +
                         session.rows[best_idx].price_hi) * 0.5;
    session.rows[best_idx].is_poc = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Value Area — expand from POC until value_area_pct of total blocks captured.
// Compare row above vs below, take whichever has more blocks.
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::compute_value_area(TPOSession& session) {
    if (session.rows.empty() || session.total_blocks == 0) return;

    int target_blocks = static_cast<int>(
        std::ceil(session.total_blocks * value_area_pct));

    int poc_idx = session.poc_row_idx;
    int num_rows = static_cast<int>(session.rows.size());

    // Start with POC row
    int accumulated = static_cast<int>(session.rows[poc_idx].blocks.size());
    session.rows[poc_idx].is_value_area = true;

    int upper = poc_idx + 1; // next row above to consider
    int lower = poc_idx - 1; // next row below to consider

    while (accumulated < target_blocks && (upper < num_rows || lower >= 0)) {
        int above_count = (upper < num_rows)
            ? static_cast<int>(session.rows[upper].blocks.size()) : -1;
        int below_count = (lower >= 0)
            ? static_cast<int>(session.rows[lower].blocks.size()) : -1;

        if (above_count >= below_count && above_count >= 0) {
            session.rows[upper].is_value_area = true;
            accumulated += above_count;
            upper++;
        } else if (below_count >= 0) {
            session.rows[lower].is_value_area = true;
            accumulated += below_count;
            lower--;
        } else {
            break;
        }
    }

    // VAH = top of highest VA row, VAL = bottom of lowest VA row
    session.vah = session.session_low;
    session.val = session.session_high;
    for (const auto& row : session.rows) {
        if (row.is_value_area) {
            if (row.price_hi > session.vah) session.vah = row.price_hi;
            if (row.price_lo < session.val) session.val = row.price_lo;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Single Prints — rows with exactly 1 block, where row above AND below
// each have >1 block. Top and bottom rows are excluded.
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::detect_single_prints(TPOSession& session) {
    int n = static_cast<int>(session.rows.size());
    if (n < 3) return;

    for (int i = 1; i < n - 1; ++i) {
        if (session.rows[i].blocks.size() == 1 &&
            session.rows[i - 1].blocks.size() > 1 &&
            session.rows[i + 1].blocks.size() > 1) {
            session.rows[i].is_single_print = true;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Poor High/Low — top/bottom row has ≥2 blocks (price stayed there)
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::detect_poor_high_low(TPOSession& session) {
    if (session.rows.empty()) return;
    session.has_poor_high = (session.rows.back().blocks.size() >= 2);
    session.has_poor_low = (session.rows.front().blocks.size() >= 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Initial Balance — range of first 2 periods (00:00-01:00 UTC)
// ═══════════════════════════════════════════════════════════════════════════════

void TPOManager::compute_initial_balance(
    TPOSession& session,
    const double* highs,
    const double* lows,
    size_t start_idx, size_t end_idx)
{
    session.ib_high = -1e18;
    session.ib_low = 1e18;
    bool found = false;

    for (size_t i = start_idx; i < end_idx; ++i) {
        // Check if this candle is in the first 2 periods
        // (period 0 or period 1, based on block assignment logic)
        // We use row blocks to tag IB rows, but for the range we use candle data
    }

    // Mark IB rows — rows touched by periods 0 and 1
    for (auto& row : session.rows) {
        for (const auto& blk : row.blocks) {
            if (blk.period_idx < 2) {
                row.is_initial_balance = true;
                if (row.price_hi > session.ib_high) session.ib_high = row.price_hi;
                if (row.price_lo < session.ib_low)  session.ib_low = row.price_lo;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        session.ib_high = 0.0;
        session.ib_low = 0.0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public accessors
// ═══════════════════════════════════════════════════════════════════════════════

const std::vector<TPOSession>*
TPOManager::get_sessions(const std::string& symbol) const {
    auto it = data_.find(symbol);
    if (it == data_.end() || it->second.empty()) return nullptr;
    return &it->second;
}

bool TPOManager::has_data(const std::string& symbol) const {
    auto it = data_.find(symbol);
    return it != data_.end() && !it->second.empty();
}

void TPOManager::toggle_expand(const std::string& symbol, size_t session_idx) {
    auto it = data_.find(symbol);
    if (it == data_.end()) return;
    if (session_idx < it->second.size()) {
        it->second[session_idx].expanded = !it->second[session_idx].expanded;
    }
}

void TPOManager::clear(const std::string& symbol) {
    data_.erase(symbol);
    last_build_hash_.erase(symbol);
}
