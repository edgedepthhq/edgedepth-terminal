#pragma once
#include "types/types.h"
#include "pb/messages.pb.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>

struct OrderbookKey {
    std::string exchange;
    std::string symbol;

    bool operator==(const OrderbookKey& other) const {
        return exchange == other.exchange && symbol == other.symbol;
    }
};

template <>
struct std::hash<OrderbookKey> {
    size_t operator()(const OrderbookKey& k) const noexcept {
        const size_t h1 = std::hash<std::string>{}(k.exchange);
        const size_t h2 = std::hash<std::string>{}(k.symbol);
        return h1 ^ (h2 << 1);
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Double-buffered orderbook storage
//
// WebSocket callbacks write to write_buf. Widgets read from read_buf.
// swap_buffers() copies write → read once per frame, guaranteeing widgets
// see a consistent snapshot for the entire frame duration.
//
// Why copy instead of pointer swap?
// The write buffer is continuously mutated (insert/erase on every depth update).
// A pointer swap would expose widgets to a half-mutated state during the next
// WS callback. Copying is ~50-100μs for a 1000-level orderbook (cache-friendly
// FlatMap memcpy) - well within frame budget.
// ═══════════════════════════════════════════════════════════════════════════════

struct DoubleBufferedOrderbook {
    Terminal::Orderbook write_buf;   // Mutated by data thread (mutex-protected)
    Terminal::Orderbook read_buf;    // Read by widgets on main thread (stable per frame)
    std::atomic<bool> dirty{false};  // Set on any write, cleared on swap
    mutable std::mutex write_mutex;  // Protects write_buf access
};

class OrderbookManager {
public:
    void apply_book_update_from_pb(
        const Terminal::Pair& pair,
        const pb::BookUpdate& update_pb
    );

    void apply_orderbook_snapshot_from_pb(
        const Terminal::Pair& pair,
        const pb::BookUpdate& snapshot_pb
    );

    void apply_book_ticker_from_pb(
        const Terminal::Pair& pair,
        const pb::BookTickerUpdate& ticker_pb
    );

    // Adopt a traded price for sources whose depth feed carries none.
    // No-op once the feed has supplied a real last_price (source_carries_price),
    // so live behaviour is untouched. See the definition for why the mid is not
    // good enough on its own.
    void note_trade_price(const Terminal::Pair& pair, double price);

    // Replay books get post-event crossing resolution (see the definition in
    // apply_book_update_from_pb). Live books are deliberately untouched: their
    // desync story is a separate open question (permanent grace, no client
    // re-request path) that must not be changed as a side effect here.
    void set_replay_mode(bool on) { replay_mode_ = on; }

    // Called by widgets - returns the READ buffer (stable for entire frame)
    const Terminal::Orderbook* get_orderbook(const Terminal::Pair& pair) const;

    // Called once per frame before update_and_render_widgets()
    // Copies write_buf → read_buf for any dirty orderbooks
    void swap_buffers();

    // Clear all orderbook data (used on replay seek)
    void clear_all() { orderbooks_.clear(); }

    // Zero last_update_id on all orderbooks (used when the data source changes,
    // e.g., >> forward skip kills a drip-feed - the batch-delivered data has a
    // completely different update_id chain). Zeroing bypasses the continuity
    // check for the first delta from the new source.
    void reset_update_ids() {
        for (auto& [key, db] : orderbooks_) {
            std::lock_guard lock(db.write_mutex);
            db.write_buf.last_update_id = 0;
        }
    }

private:
    bool replay_mode_ = false;
    DoubleBufferedOrderbook& get_or_create(const OrderbookKey& key);

    std::unordered_map<OrderbookKey, DoubleBufferedOrderbook> orderbooks_;

    static void prune_orderbook(Terminal::Orderbook& orderbook);
};
