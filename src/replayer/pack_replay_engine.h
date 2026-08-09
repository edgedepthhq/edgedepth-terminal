#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// pack_replay_engine.h - Client-side .edpack replay (Hot Replay Path B, slice 2)
//
// Plays a self-contained replay pack served static from R2/CDN with the box
// fully out of the per-viewer loop: no session POST, no WebSocket, no token
// for public showcase packs. The engine replaces the ServerSession+Replayer
// pair by reproducing their externally-visible behavior client-side:
//
//   - frames: the pack's [ts, stream, timeframe, payload] records ARE the
//     WSPayload frames the box streams for archive replay. The engine wraps
//     each in pb::WSPayload (pair from the header, event_time_ms = ts) and
//     feeds MessageHandler::route_parsed - the exact entry the WS binary
//     path uses during replay (see main.cpp on_ws_message).
//   - lifecycle JSON: replay_joined / replay_status (2s cadence, matching
//     the box's statusUpdateInterval) / replay_control acks / replay_seeked /
//     replay_finished are synthesized and pushed through
//     ReplayManager::handle_ws_message - the state machine is untouched.
//   - clock: the box's 50ms smooth-clock drip (market = anchor + wall×speed,
//     batch-deliver everything ≤ clock) runs in tick(), budgeted per frame.
//   - seeks: binary-search the header's block index, range-GET the blocks,
//     re-deliver the header's OB seed (BookUpdate{snapshot:true}) - the same
//     re-seed the box does on every archive seek.
//   - get_historical_candles: served locally from the header's baked
//     per-timeframe candle seeds, end-clamped to the playhead (F3 parity).
//     Installed as StreamManager's pack request hook; other historical
//     requests are swallowed (documented v1 scope - matches the box for
//     events older than DB retention).
//
// Fetch strategy mirrors the codebase's XHR pattern (EM_ASM + XMLHttpRequest,
// no -sFETCH): responses are stashed and processed in tick() for deterministic
// frame timing. Servers without Range support (python http.server in dev) get
// a whole-file fallback. Stale responses after a seek are dropped by a
// generation counter.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "pb/messages.pb.h"

class ReplayManager;

class PackReplayEngine {
public:
    explicit PackReplayEngine(ReplayManager* mgr);
    ~PackReplayEngine();

    // Kick off: fetch the pack prefix+header, then emit replay_joined.
    void begin(const std::string& url, float speed);

    // Per-frame driver (call from the main loop): processes fetch responses,
    // serves queued historical requests, prefetches blocks, drips due frames,
    // emits status, detects end-of-pack.
    void tick();

    // Teardown: uninstall the request hook, invalidate in-flight fetches.
    void stop();

    // control_replay actions routed from ReplayManager::send_control*.
    void control_pause();
    void control_resume();
    void control_set_speed(double speed);
    void control_seek(int64_t ts_ms);          // full seek (scrubber)
    // Lightweight reposition (>>/<<). book_cleared mirrors the client's
    // large-skip decision in skip_forward_to: when the client cleared its
    // book/candles expecting the box's consumer-reconnect, the engine must
    // re-seed (block seek) instead of clock-advancing, or the book stays
    // empty forever (deltas are rejected without a snapshot base).
    void control_skip_forward(int64_t ts_ms, bool book_cleared);

    bool active() const { return phase_ != Phase::Idle && phase_ != Phase::Error; }

    // StreamManager pack request hook target. Returns true when the request
    // was handled (or deliberately swallowed) - the caller skips the WS.
    bool handle_request_json(const std::string& msg);

    // JS XHR callback (via the extern "C" trampoline). Stashes only.
    void on_fetch(int generation, int kind, int block_idx,
                  const uint8_t* data, int len, int status);

    static PackReplayEngine* active_instance() { return s_active_; }

private:
    enum class Phase : uint8_t {
        Idle, FetchPrefix, FetchHeader, Ready, Ended, Error
    };
    enum FetchKind : int {
        kFetchPrefix = 0, kFetchHeaderRest = 1, kFetchBlock = 2,
        // v2 lazy seeds (header refs into the seed section before block 0).
        kFetchTickVolSeed = 3, kFetchHeatmapSeed = 4
    };
    // Lazy-seed lifecycle: Absent = v1 pack / stream missing (requests are
    // dropped, v1 behavior); Needed→Fetching→Ready on first use.
    enum class SeedState : uint8_t { Unknown, Absent, Needed, Fetching, Ready };

    struct QFrame {
        int64_t ts;
        uint32_t stream;
        int64_t tf;
        std::string payload;
    };
    struct FetchResponse {
        int kind;
        int block_idx;
        int status;
        std::string bytes;
    };
    struct PendingCandleReq {
        int64_t tf_sec;
        int64_t count;
        int64_t end_ms;  // 0 = playhead
    };
    struct PendingFootprintReq {
        int64_t start_ms;
        int64_t end_ms;
    };
    struct PendingVpvrReq {
        int64_t start_ms;
        int64_t end_ms;
        double tick_per_row;
    };
    struct PendingHeatmapReq {
        int64_t start_ms;
        int64_t end_ms;
        int64_t timeframe;
    };

    // ── fetch plumbing ──
    void fetch_range(int kind, int block_idx, int64_t from, int64_t to);
    void process_fetch_responses();
    void handle_prefix_bytes(std::string&& bytes, int status);
    bool parse_header_and_join();

    // ── playback ──
    void maybe_fetch_next_block();
    bool decode_block_into_queue(const std::string& raw_block);
    void deliver_ob_seed(int64_t at_ts);
    void deliver_due_frames();
    void emit_status();
    void maybe_finish();
    void serve_pending_requests();
    void route_frame(int64_t ts, uint32_t stream, int64_t tf, const std::string& payload);
    // Queues the message; drained at the top of the next tick(). The box's
    // acks arrive asynchronously over the WS, AFTER the caller's own state
    // transition - a synchronous emit inverts that order (e.g. seek() sends
    // the control THEN transitions to Seeking; a synchronous replay_seeked
    // is consumed while the state is still Playing and the Seeking state is
    // never cleared). Deferring one frame restores box ordering.
    void emit_to_manager(const char* type, void* json_obj);
    void drain_emit_queue();
    int block_index_for_ts(int64_t ts) const;
    void local_seek(int64_t target_ts);
    int64_t market_now_ms() const;
    static int64_t wall_ms();

    // ── v2 lazy seeds (footprint/VPVR + book-depth heatmap backfills) ──
    // Kick the seed fetch when a pending request needs one; slices directly
    // from full_file_ in whole-file mode.
    void maybe_fetch_seeds();
    void on_seed_bytes(int kind, const std::string& bytes);
    void serve_footprint_requests();
    void serve_vpvr_requests();
    void serve_heatmap_requests();

    ReplayManager* mgr_;
    Phase phase_ = Phase::Idle;
    std::string url_;
    int generation_ = 0;

    // Header + derived
    pb::PackHeader header_;
    bool header_ready_ = false;
    int64_t data_start_ = 0;  // 9 + header_len (absolute file offset)
    std::string prefix_buf_;  // accumulates prefix + header bytes
    std::string full_file_;   // whole-file fallback (Range unsupported)
    bool have_full_file_ = false;

    // Candle seeds parsed on demand: tf_sec → Candles
    std::map<int64_t, pb::Candles> candle_seed_cache_;

    // Block/frame pipeline
    std::deque<FetchResponse> fetch_responses_;
    std::deque<QFrame> frame_queue_;
    int next_block_ = 0;         // next block index to fetch
    bool fetch_inflight_ = false;
    int64_t skip_before_ms_ = 0; // straddling-block trim after a seek
    int64_t queued_through_ms_ = 0;  // last ts appended to the queue

    // Clock (box playbackLoop model)
    bool playing_ = false;
    double speed_ = 1.0;
    int64_t market_base_ms_ = 0;
    int64_t wall_base_ms_ = 0;

    // Post-seek prime: local_seek() acks + auto-resumes instantly (box
    // semantics), but the blocks covering the target still have to arrive
    // from the CDN - without a hold the clock runs over a stale book/chart
    // for the fetch round-trip. Pin the wall base (same trick as the
    // buffering-gate hold) until the first post-seek block has decoded
    // through the target.
    bool seek_priming_ = false;

    // v2 lazy seeds: raw bytes parsed once into caches; state machines drive
    // the on-demand fetch. Absent on v1 packs → matching requests drop.
    SeedState tickvol_seed_state_ = SeedState::Unknown;
    SeedState heatmap_seed_state_ = SeedState::Unknown;
    pb::TickVolumeUpdateBatch tickvol_seed_;
    pb::HeatmapSnapshotBatch heatmap_seed_;

    // Lifecycle
    bool ob_seed_delivered_ = false;
    bool finished_emitted_ = false;
    int64_t last_status_wall_ = 0;
    std::deque<PendingCandleReq> pending_candle_reqs_;
    std::deque<PendingFootprintReq> pending_footprint_reqs_;
    std::deque<PendingVpvrReq> pending_vpvr_reqs_;
    std::deque<PendingHeatmapReq> pending_heatmap_reqs_;
    std::deque<std::pair<std::string, std::string>> emit_queue_;  // (type, json dump)

    static PackReplayEngine* s_active_;
};
