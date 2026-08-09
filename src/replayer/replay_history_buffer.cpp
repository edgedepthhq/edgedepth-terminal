// ═══════════════════════════════════════════════════════════════════════════════
// replay_history_buffer.cpp - Client-side message history for instant rewind
// ═══════════════════════════════════════════════════════════════════════════════

#include "replay_history_buffer.h"
#include "core/orderbook_manager.h"
#include <cstdio>
#include <algorithm>

ReplayHistoryBuffer::ReplayHistoryBuffer(int64_t window_ms)
    : window_ms_(window_ms)
{
}

// ═══════════════════════════════════════════════════════════════════════════════
// Capture
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayHistoryBuffer::push(int64_t timestamp_ms,
                                Terminal::Stream stream,
                                int64_t timeframe,
                                const uint8_t* data, size_t len)
{
    entries_.push_back(HistoryEntry{
        .timestamp_ms = timestamp_ms,
        .data = std::vector<uint8_t>(data, data + len),
        .stream = stream,
        .timeframe = timeframe,
        .is_ob_snapshot = false,
    });
    total_bytes_ += len;

    // Trim every 100 pushes instead of every push - reduces overhead
    if (++trim_counter_ >= 100) {
        trim();
        trim_counter_ = 0;
    }
}

void ReplayHistoryBuffer::snapshot_orderbook(int64_t timestamp_ms,
                                              const OrderbookManager& ob_mgr,
                                              const Terminal::Pair& pair)
{
    // Rate-limit snapshots
    if (timestamp_ms - last_ob_snapshot_ms_ < OB_SNAPSHOT_INTERVAL_MS && last_ob_snapshot_ms_ != 0) {
        return;
    }

    const Terminal::Orderbook* ob = ob_mgr.get_orderbook(pair);
    if (!ob) return;

    // Don't snapshot an empty OB - happens when the OB is just created
    // but no data has been applied yet. An empty snapshot is worse than
    // no snapshot at all (the rewind will replay from buffer start instead).
    if (ob->bids.empty() && ob->asks.empty()) return;

    // Serialize OB state into a compact binary format
    OBSnapshot snap;
    snap.last_price = ob->last_price;
    snap.last_update_id = ob->last_update_id;
    snap.timestamp_ms = ob->timestamp_ms;

    // Copy bid levels
    snap.bids.reserve(ob->bids.size());
    for (const auto& [price, size] : ob->bids.data()) {
        snap.bids.push_back({price, size});
    }

    // Copy ask levels
    snap.asks.reserve(ob->asks.size());
    for (const auto& [price, size] : ob->asks.data()) {
        snap.asks.push_back({price, size});
    }

    // Serialize to bytes for storage in the deque
    // Format: [num_bids:u32][bids...][num_asks:u32][asks...][last_price:f64][update_id:i64][ts:i64]
    size_t level_size = sizeof(double) * 2;  // price + size
    size_t payload_size = sizeof(uint32_t) +
                          snap.bids.size() * level_size +
                          sizeof(uint32_t) +
                          snap.asks.size() * level_size +
                          sizeof(double) +    // last_price
                          sizeof(int64_t) +   // last_update_id
                          sizeof(int64_t);    // timestamp_ms

    std::vector<uint8_t> buf(payload_size);
    uint8_t* ptr = buf.data();

    auto write_u32 = [&](uint32_t v) { memcpy(ptr, &v, 4); ptr += 4; };
    auto write_f64 = [&](double v)   { memcpy(ptr, &v, 8); ptr += 8; };
    auto write_i64 = [&](int64_t v)  { memcpy(ptr, &v, 8); ptr += 8; };

    write_u32(static_cast<uint32_t>(snap.bids.size()));
    for (const auto& lev : snap.bids) {
        write_f64(lev.price);
        write_f64(lev.size);
    }
    write_u32(static_cast<uint32_t>(snap.asks.size()));
    for (const auto& lev : snap.asks) {
        write_f64(lev.price);
        write_f64(lev.size);
    }
    write_f64(snap.last_price);
    write_i64(snap.last_update_id);
    write_i64(snap.timestamp_ms);

    total_bytes_ += buf.size();
    entries_.push_back(HistoryEntry{
        .timestamp_ms = timestamp_ms,
        .data = std::move(buf),
        .stream = Terminal::Stream::Orderbook,
        .timeframe = 0,
        .is_ob_snapshot = true,
    });

    last_ob_snapshot_ms_ = timestamp_ms;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Query
// ═══════════════════════════════════════════════════════════════════════════════

bool ReplayHistoryBuffer::contains(int64_t timestamp_ms) const {
    if (entries_.empty()) return false;
    // Allow 1 second of slack - the target (from << button) snaps to round
    // second/minute boundaries, but the earliest buffered message may be
    // a few milliseconds later than that boundary.
    return timestamp_ms >= (entries_.front().timestamp_ms - 1000) &&
           timestamp_ms <= entries_.back().timestamp_ms;
}

int64_t ReplayHistoryBuffer::earliest_ms() const {
    return entries_.empty() ? 0 : entries_.front().timestamp_ms;
}

int64_t ReplayHistoryBuffer::latest_ms() const {
    return entries_.empty() ? 0 : entries_.back().timestamp_ms;
}

const ReplayHistoryBuffer::HistoryEntry*
ReplayHistoryBuffer::find_ob_snapshot_before(int64_t target_time_ms) const {
    const HistoryEntry* best = nullptr;
    // Walk backward from end - snapshots are sparse, so linear scan is fine
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->timestamp_ms > target_time_ms) continue;
        if (it->is_ob_snapshot) {
            best = &(*it);
            break;
        }
    }
    return best;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Replay (the core rewind operation)
// ═══════════════════════════════════════════════════════════════════════════════

// Static helper: deserialize OB snapshot from binary buffer
static ReplayHistoryBuffer::OBSnapshot deserialize_ob_snapshot(const std::vector<uint8_t>& buf) {
    ReplayHistoryBuffer::OBSnapshot snap;
    const uint8_t* ptr = buf.data();

    auto read_u32 = [&]() -> uint32_t { uint32_t v; memcpy(&v, ptr, 4); ptr += 4; return v; };
    auto read_f64 = [&]() -> double   { double v;   memcpy(&v, ptr, 8); ptr += 8; return v; };
    auto read_i64 = [&]() -> int64_t  { int64_t v;  memcpy(&v, ptr, 8); ptr += 8; return v; };

    uint32_t num_bids = read_u32();
    snap.bids.resize(num_bids);
    for (uint32_t i = 0; i < num_bids; ++i) {
        snap.bids[i].price = read_f64();
        snap.bids[i].size  = read_f64();
    }

    uint32_t num_asks = read_u32();
    snap.asks.resize(num_asks);
    for (uint32_t i = 0; i < num_asks; ++i) {
        snap.asks[i].price = read_f64();
        snap.asks[i].size  = read_f64();
    }

    snap.last_price     = read_f64();
    snap.last_update_id = read_i64();
    snap.timestamp_ms   = read_i64();
    return snap;
}

bool ReplayHistoryBuffer::replay_from(int64_t target_time_ms,
                                       int64_t current_time_ms,
                                       OBRestoreFn ob_restore,
                                       ReplayFn replay_msg)
{
    if (!contains(target_time_ms)) {
        return false;
    }

    // Step 1: Find latest OB snapshot at or before target time
    const HistoryEntry* snap_entry = find_ob_snapshot_before(target_time_ms);
    int64_t replay_start_ms = target_time_ms;

    if (snap_entry) {
        OBSnapshot snap = deserialize_ob_snapshot(snap_entry->data);
        ob_restore(snap);
        replay_start_ms = snap_entry->timestamp_ms;
    } else {
        replay_start_ms = earliest_ms();
    }

    // Step 2: Replay entries from replay_start up to current_time.
    // target_time = where we're going TO (04:33:00)
    // current_time = where we WERE (04:33:45)
    // We replay everything from target to current to rebuild widget state
    // as it was just before the user pressed <<.
    int replayed = 0;
    int skipped = 0;
    for (const auto& entry : entries_) {
        if (entry.timestamp_ms < replay_start_ms) {
            ++skipped;
            continue;
        }
        if (entry.timestamp_ms > current_time_ms) {
            break;  // Stop - everything past here hasn't been seen yet
        }
        if (entry.is_ob_snapshot) {
            continue;
        }
        replay_msg(entry.data.data(), entry.data.size());
        ++replayed;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Maintenance
// ═══════════════════════════════════════════════════════════════════════════════

void ReplayHistoryBuffer::clear() {
    entries_.clear();
    total_bytes_ = 0;
    last_ob_snapshot_ms_ = 0;
}

size_t ReplayHistoryBuffer::ob_snapshot_count() const {
    size_t count = 0;
    for (const auto& e : entries_) {
        if (e.is_ob_snapshot) ++count;
    }
    return count;
}

void ReplayHistoryBuffer::trim() {
    if (entries_.empty()) return;

    int64_t latest = entries_.back().timestamp_ms;
    int64_t cutoff = latest - window_ms_;

    // Trim by time window
    while (!entries_.empty() && entries_.front().timestamp_ms < cutoff) {
        total_bytes_ -= entries_.front().data.size();
        entries_.pop_front();
        ++trimmed_count_;
    }

    // Trim by byte budget
    while (!entries_.empty() && total_bytes_ > MAX_BYTES) {
        total_bytes_ -= entries_.front().data.size();
        entries_.pop_front();
        ++trimmed_count_;
    }
}
