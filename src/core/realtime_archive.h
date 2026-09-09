#pragma once
#include "core/realtime_history.h"
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>
class StreamManager;
class CandleManager;
class OrderbookManager;

// Shared by charts on the same data context. All methods run on the main
// thread; depth is copied through the orderbook owner's locked read API.
class RealtimeArchive : public std::enable_shared_from_this<RealtimeArchive> {
public:
    static constexpr int64_t target_ms = 1800000;
    static std::shared_ptr<RealtimeArchive> acquire(CandleManager&, OrderbookManager&, const Terminal::Pair&);
    ~RealtimeArchive();
    static bool prepare_replay(CandleManager*, int64_t clock);
    void update(int64_t clock);
    void request_startup(StreamManager&);
    static void receive_startup(const nlohmann::json&);
    int64_t startup_first = 0, startup_end = 0;
    std::string startup_status;
    void reset(bool discard_existing = true);
    void cancel_view();
    void append_trade(const Terminal::Trade&);
    bool query(int64_t from, int64_t to, int64_t cutoff, int64_t step, double tick);
    bool take_view(std::deque<RealtimeDepthHistory::SamplePtr>&, std::deque<Terminal::Trade>&, int64_t& step);
    int64_t first = 0, last = 0;
    double bytes = 0, total_bytes = 0;
    bool loading = false, view_trades_grouped = false;
    uint64_t generation = 0;
    size_t dropped = 0, view_trade_count = 0;
    std::string error;
private:
    RealtimeArchive(OrderbookManager&, const Terminal::Pair&);
    void gap(int64_t);
    void receive_seed(const nlohmann::json&);
    std::vector<double> seed_;
    std::unordered_set<int64_t> seen_ids_, seeded_ids_;
    int64_t first_depth_ms_ = 0;
    double startup_at_ = 0;
    bool startup_requested_ = false, startup_pending_ = false, identities_complete_ = true;
    void poll();
    bool make_room(size_t count);
    bool prepare_replay(int64_t clock);
    OrderbookManager& books_;
    Terminal::Pair pair_;
    std::string id_;
    std::vector<double> batch_, view_;
    std::vector<RealtimeDepthHistory::SamplePtr> pending_;
    uint64_t serial_ = 0, book_generation_ = 0;
    int64_t clock_ = 0, last_depth_ms_ = 0;
    double sent_at_ = 0;
    bool valid_ = false, lost_ = false;
};
