#include "core/orderbook_manager.h"
#include <cstdio>

DoubleBufferedOrderbook& OrderbookManager::get_or_create(const OrderbookKey& key) {
    auto [it, inserted] = orderbooks_.try_emplace(key);
    if (inserted) {
    }
    return it->second;
}

void OrderbookManager::apply_book_update_from_pb(
    const Terminal::Pair& pair,
    const pb::BookUpdate& update_pb)
{
    const OrderbookKey key{pair.exchange, pair.symbol};
    auto& db = get_or_create(key);
    std::lock_guard<std::mutex> lock(db.write_mutex);
    auto& orderbook = db.write_buf;

    if (!orderbook.snapshot) {
        static int reject_count = 0;
        if (++reject_count <= 5 || reject_count % 100 == 0) {
        }
        return;
    }
    int64_t last_update_id = update_pb.last_update_id();
    int64_t prev_last_update_id = update_pb.previous_update_id();

    // Skip outdated events
    if (last_update_id <= orderbook.last_update_id) {
        return;
    }
    // During replay, skip strict LastUpdateId continuity checks entirely.
    // DB-backed replay delivers depth updates from 5-second batches where
    // multiple rows per timestamp can arrive in non-deterministic order.
    // The OB self-heals within seconds as deltas overwrite stale price levels.
    // For live trading, strict continuity remains enforced.
    if (orderbook.replay_grace_remaining > 0) {
        // Grace period: either permanent (replay) or countdown (other)
        if (orderbook.replay_grace_remaining != INT32_MAX) {
            orderbook.replay_grace_remaining--;
        }
    } else if (orderbook.last_update_id != 0) {
        if (prev_last_update_id != orderbook.last_update_id) {
            int64_t diff = prev_last_update_id - orderbook.last_update_id;
            if (diff > 0 && diff <= 20000) {
                // Small forward skip - tolerate (redelivery/minor gaps)
            } else if (diff > 20000) {
                orderbook.snapshot = false;
                return;
            } else if (diff < 0 && diff >= -20000) {
                return;
            } else {
                orderbook.snapshot = false;
                return;
            }
        }
    }

    for (const auto& level : update_pb.asks()) {
        if (level.size() == 0.0) {
            orderbook.asks.erase(level.price());
        } else {
            orderbook.asks.insert_or_assign(level.price(), level.size());
        }
    }
    for (const auto& level : update_pb.bids()) {
        if (level.size() == 0.0) {
            orderbook.bids.erase(level.price());
        } else {
            orderbook.bids.insert_or_assign(level.price(), level.size());
        }
    }
    // REPLAY-ONLY post-event crossing resolution - the backend write path's
    // rule (internal/bookreplay.applyDiff) mirrored client-side. Archived depth
    // streams carry vendor/producer resume events whose CONTENT is stale on a
    // formally intact pu chain - no update-id signal exists, so no reseed can
    // fire. A correct diff can never leave a correct book crossed (measured:
    // BTCUSDT 2026-06-11, 3.2M deltas, 0 crossings), so a book left crossed
    // after a whole event applied is vendor staleness: evict the crossing level
    // this event did NOT just state; when the event crossed itself, the ask
    // goes. Without this, a stale level pinned at the top renders a crossed DOM
    // until the price re-touches it - measured on WMTUSDT 2026-06-05, crossed
    // from 00:04 for the REST OF THE DAY (154,438 of 154,617 deltas). Must run
    // at the event boundary only: one event may cross transiently while its own
    // levels apply (bid moves up before the old ask's removal lands).
    if (replay_mode_) {
        auto stated = [](const auto& levels, double px) {
            for (const auto& l : levels) {
                if (l.size() > 0.0 && l.price() == px) return true;
            }
            return false;
        };
        while (!orderbook.bids.empty() && !orderbook.asks.empty()) {
            const double bb = orderbook.bids.begin()->first;
            const double ba = orderbook.asks.begin()->first;
            if (bb < ba) break;
            if (!stated(update_pb.bids(), bb)) {
                orderbook.bids.erase(bb);
            } else if (!stated(update_pb.asks(), ba)) {
                orderbook.asks.erase(ba);
            } else {
                orderbook.asks.erase(ba);
            }
        }
    }
    // Only adopt a price when the update actually carries one. Pure depth
    // deltas (no trade on this tick) send last_price=0; assigning that would
    // wipe the real price to 0.00 for a frame - the "price flashes to 0" glitch
    // seen on fast pairs (live AND replay). Keep the last known price instead.
    if (update_pb.last_price() > 0.0) {
        orderbook.last_price = update_pb.last_price();
        orderbook.source_carries_price = true;
    } else if (!orderbook.source_carries_price &&
               !orderbook.bids.empty() && !orderbook.asks.empty()) {
        // This source has never sent a price. cryptohftdata-derived archives
        // have no trade tape, so EVERY update is last_price=0 and the guard
        // above alone leaves the DOM at 0.00 for the entire replay (seen on
        // 2026-06-11 BTCUSDT, whose book is otherwise perfect). Track the book's
        // own mid until a real price shows up, then defer to it forever.
        const double bb = orderbook.bids.begin()->first;
        const double ba = orderbook.asks.begin()->first;
        if (bb > 0.0 && ba >= bb) {
            orderbook.last_price = (bb + ba) * 0.5;
        }
    }
    orderbook.last_update_id = last_update_id;
    orderbook.previous_update_id = prev_last_update_id;
    orderbook.timestamp_ms = update_pb.timestamp_ms();
    orderbook.delta_updates++;  // incremental depth tick (not seed) - see context_primed

    if (++orderbook.update_count_since_prune >= 100) {
        prune_orderbook(orderbook);
        orderbook.update_count_since_prune = 0;
    }
    db.dirty.store(true, std::memory_order_relaxed);
}

void OrderbookManager::apply_orderbook_snapshot_from_pb(
    const Terminal::Pair& pair,
    const pb::BookUpdate& snapshot_pb)
{
    OrderbookKey key{pair.exchange, pair.symbol};
    auto& db = get_or_create(key);
    std::lock_guard<std::mutex> lock(db.write_mutex);
    auto& orderbook = db.write_buf;

    orderbook.asks.clear();
    orderbook.bids.clear();
    orderbook.asks.reserve(snapshot_pb.asks_size());
    orderbook.bids.reserve(snapshot_pb.bids_size());

    for (const auto& level : snapshot_pb.asks()) {
        if (level.size() > 0.0) {
            orderbook.asks.insert_or_assign(level.price(), level.size());
        }
    }
    for (const auto& level : snapshot_pb.bids()) {
        if (level.size() > 0.0) {
            orderbook.bids.insert_or_assign(level.price(), level.size());
        }
    }
    if (snapshot_pb.last_price() > 0.0) {
        orderbook.last_price = snapshot_pb.last_price();
        orderbook.source_carries_price = true;
    } else if (!orderbook.source_carries_price &&
               !orderbook.bids.empty() && !orderbook.asks.empty()) {
        // Same fallback as the delta path: a seek re-seeds through here, and a
        // cryptohftdata-derived seed carries no price, so without this the DOM
        // drops back to 0.00 on every seek even after the delta path had filled it.
        const double bb = orderbook.bids.begin()->first;
        const double ba = orderbook.asks.begin()->first;
        if (bb > 0.0 && ba >= bb) {
            orderbook.last_price = (bb + ba) * 0.5;
        }
    }
    orderbook.last_update_id = snapshot_pb.last_update_id();
    orderbook.timestamp_ms = snapshot_pb.timestamp_ms();
    orderbook.snapshot = true;
    orderbook.update_count_since_prune = 0;
    orderbook.delta_updates = 0;  // seed is not a delta; restart the live-stream count
    // Grant permanent grace period for replay: skip continuity checks for the
    // entire replay session. DB-backed replay has inherent ordering issues
    // (multiple rows per 5-second bucket, non-deterministic row order) that
    // cause spurious DESYNC resets. The OB self-heals as deltas overwrite
    // stale levels. INT32_MAX = permanent (never decrements to 0).
    orderbook.replay_grace_remaining = INT32_MAX;
    db.dirty.store(true, std::memory_order_relaxed);
}

void OrderbookManager::apply_book_ticker_from_pb(
    const Terminal::Pair& pair,
    const pb::BookTickerUpdate& ticker_pb)
{
    OrderbookKey key{pair.exchange, pair.symbol};
    auto& db = get_or_create(key);
    std::lock_guard<std::mutex> lock(db.write_mutex);
    auto& orderbook = db.write_buf;

    if (!orderbook.is_synchronized()) {
        return;
    }
    if (ticker_pb.best_bid() > 0.0 && ticker_pb.best_bid_qty() > 0.0) {
        orderbook.bids.insert_or_assign(ticker_pb.best_bid(), ticker_pb.best_bid_qty());
    }
    if (ticker_pb.best_ask() > 0.0 && ticker_pb.best_ask_qty() > 0.0) {
        orderbook.asks.insert_or_assign(ticker_pb.best_ask(), ticker_pb.best_ask_qty());
    }
    orderbook.timestamp_ms = ticker_pb.timestamp_ms();
    db.dirty.store(true, std::memory_order_relaxed);
}

void OrderbookManager::note_trade_price(const Terminal::Pair& pair, double price) {
    if (price <= 0.0) return;
    const OrderbookKey key{pair.exchange, pair.symbol};
    auto it = orderbooks_.find(key);
    if (it == orderbooks_.end()) return;  // no book yet - the seed will bootstrap
    auto& db = it->second;
    std::lock_guard<std::mutex> lock(db.write_mutex);
    auto& orderbook = db.write_buf;

    // A feed that carries its own price stays authoritative.
    if (orderbook.source_carries_price) return;

    // Otherwise this is a source with no trade tape in its DEPTH stream
    // (cryptohftdata archives), where last_price would otherwise be the book
    // MID. The live actor sets last_price from the trade tape
    // (actor/orderbook/orderbook.go:147) and only falls back to a mid, so a mid
    // here renders a DOM centre half a tick off the traded price - 62138.75
    // printing as 62138.8 while the tape and chart read 62138.7. The trades
    // stream is delivered on these days too, so use it and match live exactly.
    // The mid fallback in the update paths remains, covering the frames between
    // the seed landing and the first trade.
    orderbook.last_price = price;
    db.dirty.store(true, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Double-buffer swap - called once per frame
// ═══════════════════════════════════════════════════════════════════════════════

void OrderbookManager::swap_buffers() {
    for (auto& [key, db] : orderbooks_) {
        if (db.dirty.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(db.write_mutex);
            // Copy write → read. FlatMap is a vector so this is a fast memcpy.
            // For a 1000-level orderbook: ~16KB of data, <50μs on modern hardware.
            db.read_buf = db.write_buf;
            db.dirty.store(false, std::memory_order_relaxed);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Read access - always returns the stable read buffer
// ═══════════════════════════════════════════════════════════════════════════════

const Terminal::Orderbook* OrderbookManager::get_orderbook(const Terminal::Pair& pair) const {
    const OrderbookKey key{pair.exchange, pair.symbol};
    const auto it = orderbooks_.find(key);
    return (it != orderbooks_.end()) ? &it->second.read_buf : nullptr;
}

void OrderbookManager::prune_orderbook(Terminal::Orderbook& orderbook) {
    constexpr size_t MAX_LEVELS = 1000;
    if (orderbook.asks.size() > MAX_LEVELS) {
        auto& asks_data = orderbook.asks.data();
        asks_data.erase(asks_data.begin() + MAX_LEVELS, asks_data.end());
    }
    if (orderbook.bids.size() > MAX_LEVELS) {
        auto& bids_data = orderbook.bids.data();
        bids_data.erase(bids_data.begin() + MAX_LEVELS, bids_data.end());
    }
}
