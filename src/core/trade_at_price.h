#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// trade_at_price.h - Real-time trade accumulator per price level
//
// Client-side accumulator that subscribes to the trade stream and builds
// a rolling map of buy/sell/delta volume per rounded price level.
// Used by the DOM widget to display BUYS, SELLS, DELTA columns.
//
// This mirrors the industry-standard approach used by Market Monkey Terminal,
// Sierra Chart, Quantower, ATAS, etc. - all accumulate locally from the tape.
// ═══════════════════════════════════════════════════════════════════════════════

#include "types/types.h"
#include "stream_handler.h"
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <deque>

struct TradeAtPriceLevel {
    double buy_volume  = 0.0;
    double sell_volume = 0.0;
    int64_t buy_count  = 0;
    int64_t sell_count = 0;

    [[nodiscard]] double delta() const { return buy_volume - sell_volume; }
    [[nodiscard]] double total() const { return buy_volume + sell_volume; }
};

// ─── Price Hasher ─────────────────────────────────────────────────────────────
// Hash rounded prices for unordered_map. We convert to int64 ticks to avoid
// floating-point hashing issues.

struct PriceHash {
    double tick_size;
    explicit PriceHash(double ts = 0.01) : tick_size(ts) {}

    size_t operator()(double price) const {
        int64_t ticks = static_cast<int64_t>(std::round(price / tick_size));
        return std::hash<int64_t>{}(ticks);
    }
};

struct PriceEqual {
    double tick_size;
    explicit PriceEqual(double ts = 0.01) : tick_size(ts) {}

    bool operator()(double a, double b) const {
        return std::abs(a - b) < tick_size * 0.5;
    }
};

// ─── Reset Modes ──────────────────────────────────────────────────────────────

enum class AccumulatorResetMode : uint8_t {
    Manual,      // Only reset when user clicks "Reset"
    Periodic,    // Reset every N seconds (configurable)
    Session,     // Reset at midnight UTC
};

// Common presets (in seconds)
namespace ResetPresets {
    inline constexpr int64_t ONE_MIN   = 60;
    inline constexpr int64_t FIVE_MIN  = 300;
    inline constexpr int64_t FIFTEEN_MIN = 900;
    inline constexpr int64_t ONE_HOUR  = 3600;
    inline constexpr int64_t SESSION   = 86400;  // 24h (midnight UTC)
}

// ─── TradeAtPriceAccumulator ──────────────────────────────────────────────────

class TradeAtPriceAccumulator {
public:
    TradeAtPriceAccumulator() = default;

    // Non-copyable, non-movable (owns a stream subscription)
    TradeAtPriceAccumulator(const TradeAtPriceAccumulator&) = delete;
    TradeAtPriceAccumulator& operator=(const TradeAtPriceAccumulator&) = delete;
    TradeAtPriceAccumulator(TradeAtPriceAccumulator&&) = delete;
    TradeAtPriceAccumulator& operator=(TradeAtPriceAccumulator&&) = delete;

    void init(const Terminal::Pair& pair, StreamManager& stream_mgr, double tick_size, bool deferred = false) {
        deferred_ = deferred;
        pair_ = pair;
        stream_mgr_ = &stream_mgr;
        tick_size_ = safe_tick(tick_size);
        levels_ = LevelMap(64, PriceHash(tick_size_), PriceEqual(tick_size_));
        last_reset_ms_ = deferred_ ? 0 : now_ms();

        stream_key_ = {pair, Terminal::Stream::Trades, 0};
        StreamHandler<Terminal::Trade> handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Trade& t) {
                static_cast<TradeAtPriceAccumulator*>(ptr)->on_trade(t);
            }
        };
        stream_mgr_->subscribe_trades(stream_key_, handler);
    }

    ~TradeAtPriceAccumulator() {
        if (stream_mgr_) {
            stream_mgr_->unsubscribe_trades(stream_key_, this);
        }
    }

    // ── Public API ──────────────────────────────────────────────────────────

    const TradeAtPriceLevel* get(double price) const {
        double rounded = round_to_tick(price);
        auto it = levels_.find(rounded);
        return it != levels_.end() ? &it->second : nullptr;
    }

    // Deferred mode consumes received records only through the chart clock.
    // Pausing simply stops advancing it; no live totals leak into a paused DOM.
    void advance_to(int64_t clock_ms) {
        if (!deferred_) return;
        if (pending_gap_) {
            asof_ms_ = 0;
            clear_totals();
            pending_gap_ = false;
            reset_after_gap_ = true;
        }
        while (!pending_.empty() && pending_.front().timestamp_ms <= clock_ms) {
            const auto trade = pending_.front();
            pending_.pop_front();
            if (!asof_ms_) asof_ms_ = trade.timestamp_ms;
            if (!last_reset_ms_) last_reset_ms_ = trade.timestamp_ms;
            asof_ms_ = std::max(asof_ms_, trade.timestamp_ms);
            check_auto_reset();
            accumulate(trade);
        }
    }
    const auto& levels() const { return levels_; }
    bool reset_after_gap() const { return reset_after_gap_; }
    void reset() {
        pending_.clear();
        pending_gap_ = false;
        asof_ms_ = 0;
        clear_totals();
        reset_after_gap_ = false;
    }

private:
    void clear_totals() {
        levels_.clear();
        max_buy_volume_ = 0.0;
        max_sell_volume_ = 0.0;
        max_delta_abs_ = 0.0;
        total_buy_ = 0.0;
        total_sell_ = 0.0;
        total_trades_ = 0;
        last_reset_ms_ = deferred_ ? asof_ms_ : now_ms();
        revision_++;
    }

public:
    // ── Reset mode configuration ────────────────────────────────────────────

    void set_reset_mode(AccumulatorResetMode mode, int64_t interval_sec = ResetPresets::FIVE_MIN) {
        reset_mode_ = mode;
        reset_interval_ms_ = interval_sec * 1000;
        reset();  // Fresh start when changing modes
    }

    AccumulatorResetMode reset_mode() const { return reset_mode_; }
    int64_t reset_interval_sec() const { return reset_interval_ms_ / 1000; }

    // Call from update() or render() to check if periodic reset is due
    void check_auto_reset() {
        if (reset_mode_ == AccumulatorResetMode::Manual) return;

        int64_t now = deferred_ ? asof_ms_ : now_ms();

        if (reset_mode_ == AccumulatorResetMode::Periodic) {
            if (reset_interval_ms_ > 0 && (now - last_reset_ms_) >= reset_interval_ms_) {
                clear_totals();
            }
        } else if (reset_mode_ == AccumulatorResetMode::Session) {
            // Reset at midnight UTC - check if we've crossed a day boundary
            int64_t last_day = last_reset_ms_ / 86400000LL;
            int64_t curr_day = now / 86400000LL;
            if (curr_day > last_day) {
                clear_totals();
            }
        }
    }

    // Max values for normalizing bar widths
    double max_buy_volume()  const { return max_buy_volume_; }
    double max_sell_volume() const { return max_sell_volume_; }
    double max_delta_abs()   const { return max_delta_abs_; }
    double max_total()       const { return std::max(max_buy_volume_, max_sell_volume_); }

    // Total aggregates
    double total_buy_volume()  const { return total_buy_; }
    double total_sell_volume() const { return total_sell_; }
    double total_delta()       const { return total_buy_ - total_sell_; }
    int64_t total_trades()     const { return total_trades_; }

    // Monotonic change counter - bumps on every trade AND on reset. Consumers
    // (DOM row cache) use it to skip reformatting when nothing changed.
    uint64_t revision() const { return revision_; }

    // Re-key the levels onto a new tick grid. The instrument's tick can land
    // after the widget is built (metadata fetch, pack header), and every key in
    // levels_ was hashed on the OLD grid, so the map has to be rebuilt rather
    // than reinterpreted. Keeps the stream subscription: only the buckets go.
    void set_tick_size(double tick_size) {
        const double t = safe_tick(tick_size);
        if (t == tick_size_) return;
        tick_size_ = t;
        levels_ = LevelMap(64, PriceHash(t), PriceEqual(t));
        max_buy_volume_ = 0.0;
        max_sell_volume_ = 0.0;
        max_delta_abs_ = 0.0;
        total_buy_ = 0.0;
        total_sell_ = 0.0;
        total_trades_ = 0;
        revision_++;
    }

private:
    using LevelMap = std::unordered_map<double, TradeAtPriceLevel, PriceHash, PriceEqual>;

    Terminal::Pair pair_;
    StreamManager* stream_mgr_ = nullptr;
    StreamKey stream_key_;
    double tick_size_ = 0.01;
    LevelMap levels_;

    // Cached max values (updated on each trade for O(1) bar normalization)
    double max_buy_volume_  = 0.0;
    double max_sell_volume_ = 0.0;
    double max_delta_abs_   = 0.0;
    double total_buy_       = 0.0;
    double total_sell_      = 0.0;
    int64_t total_trades_   = 0;

    // Reset configuration
    AccumulatorResetMode reset_mode_ = AccumulatorResetMode::Periodic;
    int64_t reset_interval_ms_ = ResetPresets::FIVE_MIN * 1000;  // Default 5m
    int64_t last_reset_ms_ = 0;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // An unknown tick (0) must never reach the hasher: price/0 is inf and the
    // int64 cast in PriceHash is then undefined. Until the real tick binds via
    // set_tick_size, bucket at a grid finer than any real instrument, which is
    // the same thing as not bucketing at all.
    static double safe_tick(double t) { return t > 0.0 ? t : 1e-9; }

    double round_to_tick(double price) const {
        return std::round(price / tick_size_) * tick_size_;
    }

    bool deferred_ = false, reset_after_gap_ = false;
    int64_t asof_ms_ = 0;
    std::deque<Terminal::Trade> pending_;

    void on_trade(const Terminal::Trade& trade) {
        if (!(trade.price > 0) || !(trade.qty > 0) || trade.timestamp_ms <= 0 ||
            !std::isfinite(trade.price) || !std::isfinite(trade.qty)) return;
        if (deferred_) {
            // Bound the pause backlog without silently presenting partial CVD.
            // The next advance starts fresh if reception exceeded this budget.
            if (pending_.size() >= 20000) {
                pending_.clear();
                pending_gap_ = true;
            }
            auto it = std::upper_bound(pending_.begin(), pending_.end(), trade.timestamp_ms,
                [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
            pending_.insert(it, trade);
            return;
        }
        accumulate(trade);
    }
    bool pending_gap_ = false;
    void accumulate(const Terminal::Trade& trade) {
        double rounded = round_to_tick(trade.price);
        auto& level = levels_[rounded];

        if (trade.is_buy) {
            level.buy_volume += trade.qty;
            level.buy_count++;
            total_buy_ += trade.qty;
        } else {
            level.sell_volume += trade.qty;
            level.sell_count++;
            total_sell_ += trade.qty;
        }
        total_trades_++;

        // Update cached maxes for bar normalization
        max_buy_volume_  = std::max(max_buy_volume_,  level.buy_volume);
        max_sell_volume_ = std::max(max_sell_volume_, level.sell_volume);
        max_delta_abs_   = std::max(max_delta_abs_,   std::abs(level.delta()));
        revision_++;
    }

    uint64_t revision_ = 0;
};
