// ═══════════════════════════════════════════════════════════════════════════════
// pack_replay_engine.cpp - .edpack replay: fetch, clock, drip, seek, seeds.
// See pack_replay_engine.h for the architecture note.
// ═══════════════════════════════════════════════════════════════════════════════

#include "pack_replay_engine.h"
#include "replay_manager.h"
#include "core/education_boot.h"
#include "core/message_handler.h"
#include "core/message_parser.h"
#include "stream_handler.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

PackReplayEngine* PackReplayEngine::s_active_ = nullptr;

// ─── JS→C++ fetch trampoline ────────────────────────────────────────────────
// Exported as __edpack_on_fetch (CMake EXPORTED_FUNCTIONS, BOTH lists). The JS
// side mallocs, calls, frees - we copy synchronously into a stashed response.
extern "C" {
EMSCRIPTEN_KEEPALIVE
void _edpack_on_fetch(int generation, int kind, int block_idx,
                      uint8_t* data, int len, int status) {
    if (auto* eng = PackReplayEngine::active_instance()) {
        eng->on_fetch(generation, kind, block_idx, data, len, status);
    }
}
}

PackReplayEngine::PackReplayEngine(ReplayManager* mgr) : mgr_(mgr) {}

PackReplayEngine::~PackReplayEngine() {
    stop();
    if (s_active_ == this) s_active_ = nullptr;
}

int64_t PackReplayEngine::wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t PackReplayEngine::market_now_ms() const {
    if (!playing_) return market_base_ms_;
    const int64_t elapsed = wall_ms() - wall_base_ms_;
    int64_t now = market_base_ms_ + static_cast<int64_t>(
        static_cast<double>(elapsed) * speed_);
    if (header_ready_ && now > header_.end_ts_ms()) now = header_.end_ts_ms();
    return now;
}

// ─── lifecycle ──────────────────────────────────────────────────────────────

void PackReplayEngine::begin(const std::string& url, float speed) {
    url_ = url;
    speed_ = std::max(0.1, std::min(10.0, static_cast<double>(speed)));
    phase_ = Phase::FetchPrefix;
    generation_++;
    s_active_ = this;

    // Serve replay-context historical requests from the pack (the request
    // hook is consulted only by StreamManagers in replay mode, so the live
    // pipeline is untouched).
    StreamManager::set_pack_request_hook([this](const std::string& msg) {
        return this->handle_request_json(msg);
    });

    // First read covers prefix + (usually) the whole header when there are
    // no candle seeds; with seeds the header runs to ~1-2MB, so the rest is
    // fetched in one follow-up range once the length is known.
    fetch_range(kFetchPrefix, -1, 0, 64 * 1024 - 1);
}

void PackReplayEngine::stop() {
    if (s_active_ == this) {
        StreamManager::set_pack_request_hook(nullptr);
    }
    generation_++;  // invalidate in-flight fetches
    fetch_inflight_ = false;
    fetch_responses_.clear();
    frame_queue_.clear();
    pending_candle_reqs_.clear();
    pending_footprint_reqs_.clear();
    pending_vpvr_reqs_.clear();
    pending_heatmap_reqs_.clear();
    if (tickvol_seed_state_ == SeedState::Fetching) tickvol_seed_state_ = SeedState::Needed;
    if (heatmap_seed_state_ == SeedState::Fetching) heatmap_seed_state_ = SeedState::Needed;
    emit_queue_.clear();
    playing_ = false;
    phase_ = Phase::Idle;
}

// ─── fetch plumbing ─────────────────────────────────────────────────────────

void PackReplayEngine::fetch_range(int kind, int block_idx, int64_t from, int64_t to) {
#ifdef __EMSCRIPTEN__
    fetch_inflight_ = true;
    EM_ASM({
        var url = UTF8ToString($0);
        var from = $1;
        var to = $2;
        var gen = $3;
        var kind = $4;
        var blk = $5;
        var xhr = new XMLHttpRequest();
        xhr.open('GET', url, true);
        xhr.responseType = 'arraybuffer';
        if (to >= from) {
            xhr.setRequestHeader('Range', 'bytes=' + from + '-' + to);
        }
        xhr.onload = function() {
            var ok = (xhr.status === 200 || xhr.status === 206);
            var bytes = ok ? new Uint8Array(xhr.response) : new Uint8Array(0);
            var ptr = _malloc(bytes.length > 0 ? bytes.length : 1);
            if (bytes.length > 0) HEAPU8.set(bytes, ptr);
            Module.__edpack_on_fetch(gen, kind, blk, ptr, bytes.length, xhr.status);
            _free(ptr);
        };
        xhr.onerror = function() {
            Module.__edpack_on_fetch(gen, kind, blk, 0, 0, 0);
        };
        xhr.send();
    }, url_.c_str(), static_cast<double>(from), static_cast<double>(to),
       generation_, kind, block_idx);
#else
    (void)kind; (void)block_idx; (void)from; (void)to;
    phase_ = Phase::Error;
#endif
}

void PackReplayEngine::on_fetch(int generation, int kind, int block_idx,
                                const uint8_t* data, int len, int status) {
    if (generation != generation_) {
        return;  // stale (post-seek / post-stop) - drop
    }
    fetch_inflight_ = false;
    FetchResponse resp;
    resp.kind = kind;
    resp.block_idx = block_idx;
    resp.status = status;
    if (data && len > 0) {
        resp.bytes.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
    }
    fetch_responses_.push_back(std::move(resp));
}

static uint32_t read_u32_le(const std::string& buf, size_t off) {
    return static_cast<uint32_t>(static_cast<uint8_t>(buf[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[off + 3])) << 24);
}

void PackReplayEngine::process_fetch_responses() {
    while (!fetch_responses_.empty()) {
        FetchResponse resp = std::move(fetch_responses_.front());
        fetch_responses_.pop_front();

        if (resp.status != 200 && resp.status != 206) {
            phase_ = Phase::Error;
            json j = {{"type", "error"},
                      {"error", "replay pack fetch failed"}};
            emit_to_manager("error", &j);
            return;
        }

        switch (resp.kind) {
        case kFetchPrefix:
            handle_prefix_bytes(std::move(resp.bytes), resp.status);
            break;
        case kFetchHeaderRest:
            prefix_buf_ += resp.bytes;
            if (parse_header_and_join()) phase_ = Phase::Ready;
            break;
        case kFetchBlock:
            if (!decode_block_into_queue(resp.bytes)) {
                phase_ = Phase::Error;
                return;
            }
            break;
        case kFetchTickVolSeed:
        case kFetchHeatmapSeed:
            on_seed_bytes(resp.kind, resp.bytes);
            break;
        default:
            break;
        }
    }
}

// ─── v2 lazy seeds ──────────────────────────────────────────────────────────

void PackReplayEngine::on_seed_bytes(int kind, const std::string& bytes) {
    if (kind == kFetchTickVolSeed) {
        if (tickvol_seed_.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
            tickvol_seed_state_ = SeedState::Ready;
        } else {
            tickvol_seed_state_ = SeedState::Absent;  // don't refetch a bad seed
        }
    } else {
        if (heatmap_seed_.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
            heatmap_seed_state_ = SeedState::Ready;
        } else {
            heatmap_seed_state_ = SeedState::Absent;
        }
    }
}

void PackReplayEngine::maybe_fetch_seeds() {
    // Resolve Unknown → Absent/Needed lazily from the header refs (a v1 pack
    // has no refs; a v2 pack without the source stream has zero-length refs).
    auto resolve = [&](SeedState& st, bool has_ref) {
        if (st == SeedState::Unknown) st = has_ref ? SeedState::Needed : SeedState::Absent;
    };
    if (!pending_footprint_reqs_.empty() || !pending_vpvr_reqs_.empty()) {
        resolve(tickvol_seed_state_,
                header_.has_tick_volume_seed_ref() &&
                header_.tick_volume_seed_ref().length() > 0);
    }
    if (!pending_heatmap_reqs_.empty()) {
        resolve(heatmap_seed_state_,
                header_.has_heatmap_seed_ref() &&
                header_.heatmap_seed_ref().length() > 0);
    }

    auto want = [&](SeedState& st, int kind, const pb::PackSeedRef& ref) {
        if (st != SeedState::Needed) return;
        if (have_full_file_) {
            // Whole-file mode: slice in place, no fetch.
            const int64_t abs = data_start_ + ref.offset();
            if (abs + ref.length() <= static_cast<int64_t>(full_file_.size())) {
                st = SeedState::Ready;  // on_seed_bytes may flip to Absent on parse fail
                on_seed_bytes(kind, full_file_.substr(static_cast<size_t>(abs),
                                                      static_cast<size_t>(ref.length())));
            } else {
                st = SeedState::Absent;
            }
            return;
        }
        if (fetch_inflight_) return;  // one fetch at a time - retry next tick
        st = SeedState::Fetching;
        const int64_t abs = data_start_ + ref.offset();
        fetch_range(kind, -1, abs, abs + ref.length() - 1);
    };
    want(tickvol_seed_state_, kFetchTickVolSeed, header_.tick_volume_seed_ref());
    want(heatmap_seed_state_, kFetchHeatmapSeed, header_.heatmap_seed_ref());
}

void PackReplayEngine::handle_prefix_bytes(std::string&& bytes, int status) {
    if (status == 200) {
        // Server ignored the Range header and sent the WHOLE file (bare
        // python http.server). Keep ONE copy; header parsing + block reads
        // slice it in place - no duplicate buffer (a real pack is ~400MB).
        full_file_ = std::move(bytes);
        have_full_file_ = true;
    } else {
        prefix_buf_ += bytes;
    }
    const std::string& src = have_full_file_ ? full_file_ : prefix_buf_;

    if (src.size() < 9) {
        phase_ = Phase::Error;
        return;
    }
    if (std::memcmp(src.data(), "EDPK", 4) != 0) {
        phase_ = Phase::Error;
        return;
    }
    const uint8_t version = static_cast<uint8_t>(src[4]);
    if (version < 1 || version > 2) {
        phase_ = Phase::Error;
        return;
    }
    const uint32_t header_len = read_u32_le(src, 5);
    data_start_ = 9 + static_cast<int64_t>(header_len);

    if (src.size() >= static_cast<size_t>(data_start_)) {
        if (parse_header_and_join()) phase_ = Phase::Ready;
        return;
    }
    // Header extends past what we have - fetch the remainder in one range.
    phase_ = Phase::FetchHeader;
    fetch_range(kFetchHeaderRest, -1,
                static_cast<int64_t>(src.size()), data_start_ - 1);
}

bool PackReplayEngine::parse_header_and_join() {
    const std::string& src = have_full_file_ ? full_file_ : prefix_buf_;
    const size_t header_len = static_cast<size_t>(data_start_) - 9;
    if (!header_.ParseFromArray(src.data() + 9, static_cast<int>(header_len))) {
        phase_ = Phase::Error;
        return false;
    }
    prefix_buf_.clear();
    prefix_buf_.shrink_to_fit();  // header is parsed; drop the staging buffer
    header_ready_ = true;

    // Anchor the clock at the window start - the box's playback loop starts
    // running on join; the client's Buffering gate holds only the DISPLAY.
    market_base_ms_ = header_.start_ts_ms();
    wall_base_ms_ = wall_ms();
    playing_ = true;
    last_status_wall_ = 0;
    next_block_ = 0;
    skip_before_ms_ = 0;
    ob_seed_delivered_ = false;
    finished_emitted_ = false;

    // NOTE: the start-at / deep-link target (pack_seek_to_ms) is deliberately
    // NOT applied here. A silent engine-level reposition desyncs the chart's
    // request anchor (it asks for candles at the manager's start time and
    // never learns about the jump). Both surfaces go through ONE deliberate
    // ReplayManager::seek once the session is primed - EventRuntime's
    // deep-link path when embedded, main.cpp's pack-standalone latch
    // otherwise - which drives the full re-request cascade.

    // Synthesize replay_joined - same envelope the box sends (plus symbols,
    // which the archive path passes via request_archive_replay's hint; the
    // header is authoritative here). ReplayManager transitions to Buffering
    // and creates the replay DataContext.
    json j = {
        {"type", "replay_joined"},
        {"data", {
            {"session_id", std::string("pack:") + header_.event_id()},
            {"type", "archive"},
            {"symbols", json::array({header_.symbol()})},
            {"speed", speed_},
            {"start_time", header_.start_ts_ms()},
            {"end_time", header_.end_ts_ms()},
            {"event_id", header_.event_id()}
        }}
    };
    emit_to_manager("replay_joined", &j);
    return true;
}

void PackReplayEngine::emit_to_manager(const char* type, void* json_obj) {
    // Deferred: see header. Dump+reparse is negligible (tiny lifecycle JSON,
    // ≤2s cadence) and keeps json out of this header.
    emit_queue_.emplace_back(type, static_cast<json*>(json_obj)->dump());
}

void PackReplayEngine::drain_emit_queue() {
    while (!emit_queue_.empty()) {
        auto [type, dumped] = std::move(emit_queue_.front());
        emit_queue_.pop_front();
        if (!mgr_) continue;
        json j = json::parse(dumped, nullptr, false);
        if (j.is_discarded()) continue;  // cannot happen for our own dumps
        mgr_->handle_ws_message(type.c_str(), &j);
    }
}

// ─── per-frame driver ───────────────────────────────────────────────────────

void PackReplayEngine::tick() {
    if (phase_ == Phase::Idle) return;

    process_fetch_responses();
    // Drain BEFORE the Error early-out so a queued error message still
    // reaches the manager (otherwise a fetch failure spins forever).
    drain_emit_queue();
    if (phase_ != Phase::Ready && phase_ != Phase::Ended) return;

    // The replay DataContext appears when ReplayManager processes our
    // replay_joined; everything downstream needs it.
    if (!mgr_ || !mgr_->replay_context()) return;

    if (!ob_seed_delivered_) {
        deliver_ob_seed(market_base_ms_);
        ob_seed_delivered_ = true;
    }

    // Hold the market clock while the client's buffering gate is priming
    // (state Buffering → is_loading). The box can't observe client priming so
    // its server clock runs and the first seconds of a slow prime are skipped;
    // the engine CAN observe it, so a pack replay always starts at frame one.
    // Frames ≤ the held clock (pre-start context rows) still deliver below,
    // which is exactly what primes the gate.
    if (playing_ && mgr_->is_loading()) {
        wall_base_ms_ = wall_ms();  // no wall time accrues while held
    }

    // Post-seek prime: hold the clock until the first post-seek block has
    // decoded through the target (see header). Without this the seek acks +
    // auto-resumes instantly (box semantics) and the clock runs over a stale
    // book for the CDN round-trip - visible as "playing without depth".
    if (seek_priming_) {
        if (queued_through_ms_ >= skip_before_ms_ ||
            next_block_ >= header_.blocks_size()) {
            seek_priming_ = false;
        } else if (playing_) {
            wall_base_ms_ = wall_ms();
        }
    }

    serve_pending_requests();
    maybe_fetch_seeds();
    maybe_fetch_next_block();
    deliver_due_frames();
    emit_status();
    maybe_finish();
}

void PackReplayEngine::maybe_fetch_next_block() {
    if (next_block_ >= header_.blocks_size()) return;

    // Buffer-ahead policy (box: 5min target / 2min refill, but blocks are the
    // granularity here): keep the decoded queue covering the playhead plus a
    // speed-scaled lead.
    const int64_t lead_ms = static_cast<int64_t>(
        std::max(15000.0, 15000.0 * speed_));
    const bool need_more =
        frame_queue_.empty() || queued_through_ms_ < market_now_ms() + lead_ms;
    if (!need_more) return;

    const auto& entry = header_.blocks(next_block_);

    if (have_full_file_) {
        // Whole-file mode: slice + decode locally, one block per tick.
        const int64_t abs_off = data_start_ + entry.offset();
        if (abs_off + entry.length() > static_cast<int64_t>(full_file_.size())) {
            phase_ = Phase::Error;
            return;
        }
        std::string raw = full_file_.substr(static_cast<size_t>(abs_off),
                                            static_cast<size_t>(entry.length()));
        next_block_++;
        if (!decode_block_into_queue(raw)) phase_ = Phase::Error;
        return;
    }

    if (fetch_inflight_) return;
    const int64_t abs_off = data_start_ + entry.offset();
    const int blk = next_block_;
    next_block_++;
    fetch_range(kFetchBlock, blk, abs_off, abs_off + entry.length() - 1);
}

bool PackReplayEngine::decode_block_into_queue(const std::string& raw_block) {
    auto decomp = MessageParser::decompress_zstd(raw_block);
    if (!decomp.success) {
        return false;
    }
    const std::string& buf = decomp.data;
    size_t off = 0;
    int added = 0;
    while (off + 4 <= buf.size()) {
        const uint32_t rec_len = read_u32_le(buf, off);
        off += 4;
        if (off + rec_len > buf.size()) {
            return false;
        }
        pb::PackFrame frame;
        if (!frame.ParseFromArray(buf.data() + off, static_cast<int>(rec_len))) {
            return false;
        }
        off += rec_len;
        // Straddling-block trim after a seek - mirrors the box reader's
        // skipBeforeMs row skip.
        if (skip_before_ms_ > 0 && frame.ts_ms() < skip_before_ms_) continue;
        QFrame qf;
        qf.ts = frame.ts_ms();
        qf.stream = frame.stream();
        qf.tf = frame.timeframe();
        qf.payload = std::move(*frame.mutable_payload());  // no copy - depth blobs are hot
        queued_through_ms_ = qf.ts;
        frame_queue_.push_back(std::move(qf));
        added++;
    }
    if (added > 0 && phase_ == Phase::Ended) phase_ = Phase::Ready;
    return true;
}

// ─── delivery ───────────────────────────────────────────────────────────────

void PackReplayEngine::route_frame(int64_t ts, uint32_t stream, int64_t tf,
                                   const std::string& payload) {
    pb::WSPayload ws;
    ws.mutable_pair()->set_exchange(
        header_.exchange().empty() ? "binancef" : header_.exchange());
    ws.mutable_pair()->set_symbol(header_.symbol());
    ws.set_stream(static_cast<pb::Stream>(stream));
    ws.set_timeframe(tf);
    ws.set_data(payload);
    ws.set_event_time_ms(ts);
    MessageContext ctx = mgr_->replay_message_context();
    MessageHandler::route_parsed(ws, ctx);
}

void PackReplayEngine::deliver_ob_seed(int64_t at_ts) {
    if (header_.ob_seed().empty()) {
        return;
    }
    // Same wire form the box's seedArchiveOrderbook delivers: a
    // BookUpdate{snapshot:true} on the orderbook stream at the seek target.
    route_frame(at_ts, static_cast<uint32_t>(pb::STREAM_ORDERBOOK), 0, header_.ob_seed());
}

void PackReplayEngine::deliver_due_frames() {
    if (frame_queue_.empty()) return;
    // Respect the manager's rewind gate - identical to the WS path dropping
    // binary frames while a rewind is in flight.
    if (mgr_->is_rewind_pending() || mgr_->is_drip_feeding()) return;

    const int64_t cutoff = market_now_ms();
    // Per-frame budget: the box delivers everything ≤ clock per 50ms tick and
    // the network paces the client; here we self-pace so a burst (skip, fast
    // speed, post-buffer catch-up) can't blow the render budget.
    static constexpr int    kMaxPerTick = 6000;
    static constexpr double kBudgetMs   = 3.0;
    const auto t0 = std::chrono::steady_clock::now();
    int delivered = 0;
    while (!frame_queue_.empty() && delivered < kMaxPerTick) {
        const QFrame& f = frame_queue_.front();
        if (f.ts > cutoff) break;
        if ((delivered & 31) == 31) {
            const double spent = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (spent >= kBudgetMs) break;
        }
        route_frame(f.ts, f.stream, f.tf, f.payload);
        frame_queue_.pop_front();
        delivered++;
    }
}

void PackReplayEngine::emit_status() {
    if (!playing_) return;
    const int64_t now_wall = wall_ms();
    if (last_status_wall_ != 0 && now_wall - last_status_wall_ < 2000) return;
    last_status_wall_ = now_wall;

    const int64_t now = market_now_ms();
    const int64_t range = header_.end_ts_ms() - header_.start_ts_ms();
    const double progress = range > 0
        ? static_cast<double>(now - header_.start_ts_ms()) / static_cast<double>(range)
        : 0.0;
    json j = {
        {"type", "replay_status"},
        {"data", {
            {"status", "playing"},
            {"current_time", now},
            {"progress", progress},
            {"extra", {{"speed", speed_}}}
        }}
    };
    emit_to_manager("replay_status", &j);
}

void PackReplayEngine::maybe_finish() {
    if (finished_emitted_ || phase_ != Phase::Ready) return;
    const bool all_fetched = next_block_ >= header_.blocks_size();
    if (all_fetched && frame_queue_.empty() && !fetch_inflight_ &&
        market_now_ms() >= header_.end_ts_ms()) {
        finished_emitted_ = true;
        phase_ = Phase::Ended;
        playing_ = false;
        market_base_ms_ = header_.end_ts_ms();
        json j = {
            {"type", "replay_finished"},
            {"data", {
                {"symbol", header_.symbol()},
                {"session_id", std::string("pack:") + header_.event_id()},
                {"reason", "completed"}
            }}
        };
        emit_to_manager("replay_finished", &j);
    }
}

// ─── controls (routed from ReplayManager::send_control*) ────────────────────

void PackReplayEngine::control_pause() {
    market_base_ms_ = market_now_ms();
    playing_ = false;
    json j = {{"type", "replay_control"}, {"data", {{"status", "paused"}}}};
    emit_to_manager("replay_control", &j);
}

void PackReplayEngine::control_resume() {
    wall_base_ms_ = wall_ms();
    playing_ = true;
    json j = {{"type", "replay_control"}, {"data", {{"status", "playing"}}}};
    emit_to_manager("replay_control", &j);
}

void PackReplayEngine::control_set_speed(double speed) {
    market_base_ms_ = market_now_ms();
    wall_base_ms_ = wall_ms();
    speed_ = std::max(0.1, std::min(10.0, speed));
}

int PackReplayEngine::block_index_for_ts(int64_t ts) const {
    // First block whose last_ts >= ts (blocks are time-ordered).
    int lo = 0, hi = header_.blocks_size();
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (header_.blocks(mid).last_ts_ms() < ts) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void PackReplayEngine::local_seek(int64_t target_ts) {
    target_ts = std::max(header_.start_ts_ms(),
                         std::min(target_ts, header_.end_ts_ms()));
    generation_++;  // drop stale block fetches
    fetch_inflight_ = false;
    fetch_responses_.clear();
    frame_queue_.clear();
    queued_through_ms_ = 0;
    skip_before_ms_ = target_ts;
    next_block_ = block_index_for_ts(target_ts);
    // A seed fetch in flight was invalidated by the generation bump - re-arm
    // so the pending indicator requests retry after the seek settles.
    if (tickvol_seed_state_ == SeedState::Fetching) tickvol_seed_state_ = SeedState::Needed;
    if (heatmap_seed_state_ == SeedState::Fetching) heatmap_seed_state_ = SeedState::Needed;

    // Clock at the target; the box auto-resumes after a full seek. The clock
    // HOLDS (seek_priming_) until the target block has decoded, so playback
    // never runs ahead of the re-seeded book while the CDN round-trips.
    market_base_ms_ = target_ts;
    wall_base_ms_ = wall_ms();
    playing_ = true;
    seek_priming_ = true;
    finished_emitted_ = false;
    if (phase_ == Phase::Ended) phase_ = Phase::Ready;

    // Re-seed the book at the target - the box's handleArchiveSeek re-delivers
    // the bundle's row-0 snapshot on every seek (client cleared its book).
    deliver_ob_seed(target_ts);
}

void PackReplayEngine::control_seek(int64_t ts_ms) {
    local_seek(ts_ms);
    json j = {{"type", "replay_seeked"},
              {"data", {{"timestamp", std::max(header_.start_ts_ms(),
                                               std::min(ts_ms, header_.end_ts_ms()))}}}};
    emit_to_manager("replay_seeked", &j);
}

void PackReplayEngine::control_skip_forward(int64_t ts_ms, bool book_cleared) {
    const int64_t target = std::max(header_.start_ts_ms(),
                                    std::min(ts_ms, header_.end_ts_ms()));
    if (target >= market_now_ms()) {
        if (book_cleared) {
            // Large forward skip: the client cleared its book + reset its
            // candle timeline expecting the box's consumer-reconnect (fresh
            // OB seed at the target, no intermediate frames). Clock-advance
            // here would drip megabytes of intermediates into a cleared book
            // whose deltas are all rejected (no snapshot base) - the DOM
            // stays empty forever. Serve it as a block seek instead: jump
            // the pipeline, re-deliver the OB seed, skip intermediates -
            // the box reconnect's externally-visible behavior. local_seek
            // auto-resumes, matching the manager (large skips transition
            // Seeking → Playing on replay_seeked) and the box (auto-resume
            // after seek).
            local_seek(target);
        } else {
            // Small forward skip: clock-only advance - the intermediates
            // already queued (or about to be fetched) batch-deliver as the
            // clock passes them, exactly like the box's SkipForwardTo (the
            // client kept its book; update-id continuity was reset).
            market_base_ms_ = target;
            wall_base_ms_ = wall_ms();
            finished_emitted_ = false;
        }
    } else {
        // Backward: the client cleared its book and trimmed candles; the box's
        // clock-only rewind would leave a data gap until the clock re-reaches
        // the old position, so serve it as a full local re-read instead -
        // byte-identical frames, same replay_seeked ack.
        local_seek(target);
    }
    json j = {{"type", "replay_seeked"}, {"data", {{"timestamp", target}}}};
    emit_to_manager("replay_seeked", &j);
}

// ─── historical requests (StreamManager pack hook) ──────────────────────────

bool PackReplayEngine::handle_request_json(const std::string& msg) {
    if (!active()) return false;
    try {
        auto j = json::parse(msg);
        const std::string method = j.value("method", "");
        if (method == "get_historical_candles") {
            const auto& data = j.at("data");
            PendingCandleReq req;
            req.tf_sec = data.value("timeframe", static_cast<int64_t>(300));
            req.count = static_cast<int64_t>(data.value("count", 500));
            req.end_ms = data.value("end_time", static_cast<int64_t>(0));
            pending_candle_reqs_.push_back(req);
            return true;
        }
        // v2 seed-served requests (footprint / VPVR / book-depth heatmap
        // backfill). Queued; served from the lazily-fetched seed section -
        // on a v1 pack (no refs) they drop, matching v1 behavior.
        if (method == "get_footprint_history") {
            const auto& data = j.at("data");
            pending_footprint_reqs_.push_back({
                data.value("start_time", static_cast<int64_t>(0)),
                data.value("end_time", static_cast<int64_t>(0))});
            return true;
        }
        if (method == "get_volume_profile") {
            const auto& data = j.at("data");
            pending_vpvr_reqs_.push_back({
                data.value("start_time", static_cast<int64_t>(0)),
                data.value("end_time", static_cast<int64_t>(0)),
                data.value("tick_per_row", 1.0)});
            return true;
        }
        if (method == "get_historical_heatmap") {
            const auto& data = j.at("data");
            pending_heatmap_reqs_.push_back({
                data.value("start_time", static_cast<int64_t>(0)),
                data.value("end_time", static_cast<int64_t>(0)),
                data.value("timeframe", static_cast<int64_t>(0))});
            return true;
        }
        // Everything else (OI / funding / VPIN historicals, subscribes) is
        // swallowed - the box path returns nothing for these on events older
        // than DB retention either. The drip frames carry the replay surface.
        return true;
    } catch (const std::exception& e) {
        return true;  // still swallow - no WS to fall back to
    }
}

void PackReplayEngine::serve_pending_requests() {
    serve_footprint_requests();
    serve_vpvr_requests();
    serve_heatmap_requests();
    while (!pending_candle_reqs_.empty()) {
        PendingCandleReq req = pending_candle_reqs_.front();
        pending_candle_reqs_.pop_front();

        // Locate + cache-parse the seed for this timeframe.
        auto it = candle_seed_cache_.find(req.tf_sec);
        if (it == candle_seed_cache_.end()) {
            const std::string* blob = nullptr;
            for (const auto& seed : header_.candle_seeds()) {
                if (seed.timeframe_sec() == req.tf_sec) { blob = &seed.candles(); break; }
            }
            if (!blob) {
                continue;  // mirrors the box's empty-result silence
            }
            pb::Candles parsed;
            if (!parsed.ParseFromArray(blob->data(), static_cast<int>(blob->size()))) {
                continue;
            }
            it = candle_seed_cache_.emplace(req.tf_sec, std::move(parsed)).first;
        }
        const pb::Candles& all = it->second;

        // F3 parity: strictly-before semantics match the box's `time < end`
        // query bound. An EXPLICIT end_ms is trusted as-is (clamped to the
        // pack window): it always comes from our own chart - replay-latest or
        // a seek target - and on the box the session seek is processed before
        // the fetch, so its position == the target. Clamping it to the LOCAL
        // playhead instead re-introduced the one-frame ordering hole where a
        // reset_for_seek batch (queued the frame before flush_pending_skip
        // applies the seek) got served at the OLD position - a 13h candle
        // hole behind the playhead after a long recorder jump. Open-ended
        // requests (end_ms=0) still clamp to the playhead: never reveal the
        // future on "latest" queries.
        const int64_t playhead = market_now_ms();
        const int64_t end = req.end_ms > 0
            ? std::min(req.end_ms, header_.end_ts_ms())
            : playhead;

        pb::Candles out;
        out.set_timeframe(req.tf_sec);
        int last_idx = -1;
        for (int i = 0; i < all.values_size(); i++) {
            if (all.values(i).timestamp_ms() < end) last_idx = i;
            else break;
        }
        const int64_t want = std::max<int64_t>(1, req.count);
        const int first_idx = std::max(0, last_idx - static_cast<int>(want) + 1);
        for (int i = first_idx; i <= last_idx; i++) {
            out.add_values()->CopyFrom(all.values(i));
        }
        if (out.values_size() == 0) {
            // Box behavior: zero rows → no response frame at all.
            continue;
        }
        std::string bytes;
        if (!out.SerializeToString(&bytes)) continue;
        route_frame(0, static_cast<uint32_t>(pb::STREAM_HISTORICAL_CANDLES),
                    req.tf_sec, bytes);
    }
}

// ─── v2 seed-served requests (footprint / VPVR / book-depth heatmap) ────────
// All three are served from the pack's lazy seed section with the SAME wire
// shapes the box sends, so the message handlers + managers are untouched:
//   footprint  → TickVolumeUpdateBatch on STREAM_TICK_VOLUME + ts=0 sentinel
//   VPVR       → VolumeProfileResponse on STREAM_VOLUME_PROFILE
//   heatmap    → HeatmapSnapshotBatch on STREAM_HISTORICAL_HEATMAPS
// Every response end-clamps to the playhead (F3 parity - never reveal the
// future). Requests queue until the seed is fetched; on a v1 pack (no seed
// refs) they drop, which is exactly v1's swallow behavior.

void PackReplayEngine::serve_footprint_requests() {
    if (pending_footprint_reqs_.empty()) return;
    if (tickvol_seed_state_ == SeedState::Absent) {
        pending_footprint_reqs_.clear();
        return;
    }
    if (tickvol_seed_state_ != SeedState::Ready) return;  // fetch in flight

    while (!pending_footprint_reqs_.empty()) {
        const PendingFootprintReq req = pending_footprint_reqs_.front();
        pending_footprint_reqs_.pop_front();

        const int64_t end = std::min(req.end_ms, market_now_ms());
        pb::TickVolumeUpdateBatch out;
        for (const auto& u : tickvol_seed_.updates()) {
            if (u.timestamp_ms() < req.start_ms) continue;
            if (u.timestamp_ms() > end) break;  // seed is ts-sorted (baked)
            out.add_updates()->CopyFrom(u);
        }
        if (out.updates_size() > 0) {
            std::string bytes;
            if (out.SerializeToString(&bytes)) {
                route_frame(0, static_cast<uint32_t>(pb::STREAM_TICK_VOLUME), 60000, bytes);
            }
        }
        // Sentinel ALWAYS (box parity): clears FootprintManager::loading_ so
        // the visible-range request loop keeps working.
        // A default TickVolumeUpdate{ts=0} serializes to zero bytes (proto3
        // defaults) - the client parses that to the ts=0 sentinel, same as
        // the box's marshalled sentinel.
        route_frame(0, static_cast<uint32_t>(pb::STREAM_TICK_VOLUME), 0, std::string());
    }
}

void PackReplayEngine::serve_vpvr_requests() {
    if (pending_vpvr_reqs_.empty()) return;
    if (tickvol_seed_state_ == SeedState::Absent) {
        pending_vpvr_reqs_.clear();
        return;
    }
    if (tickvol_seed_state_ != SeedState::Ready) return;

    while (!pending_vpvr_reqs_.empty()) {
        const PendingVpvrReq req = pending_vpvr_reqs_.front();
        pending_vpvr_reqs_.pop_front();

        const int64_t end = std::min(req.end_ms, market_now_ms());
        const double tpr = req.tick_per_row > 0 ? req.tick_per_row : 1.0;

        // Port of the box's buildAndSendVolumeProfile aggregation
        // (volume_profile_handler.go): group each tick-volume level into
        // floor(price/tpr)*tpr rows, then POC + 70% value area from the POC
        // outward. Same data source too - the tick_volume rows.
        struct Agg { double buy = 0, sell = 0, total = 0; int64_t trades = 0; };
        std::map<double, Agg> grouped;
        double total_vol = 0;
        for (const auto& u : tickvol_seed_.updates()) {
            if (u.timestamp_ms() < req.start_ms) continue;
            if (u.timestamp_ms() > end) break;
            const std::string& blob = u.levels_data();
            if (blob.empty()) continue;
            pb::TickVolumeLevels levels;
            const auto dec = MessageParser::decompress_zstd(blob);
            const bool parsed = dec.success
                ? levels.ParseFromArray(dec.data.data(), static_cast<int>(dec.data.size()))
                : levels.ParseFromArray(blob.data(), static_cast<int>(blob.size()));
            if (!parsed) continue;
            for (const auto& lv : levels.levels()) {
                const double row = std::floor(lv.price() / tpr) * tpr;
                Agg& a = grouped[row];
                a.buy += lv.buy_volume();
                a.sell += lv.sell_volume();
                a.total += lv.total_volume();
                a.trades += lv.trade_count();
            }
            total_vol += u.total_volume();
        }

        if (grouped.empty()) {
            // Box parity: empty profile response so the client knows the
            // request completed (clears the VPVR loading latch).
            route_frame(0, static_cast<uint32_t>(pb::STREAM_VOLUME_PROFILE), 0, std::string());
            continue;
        }

        // grouped is price-ascending (std::map). POC = max total volume.
        std::vector<std::pair<double, Agg>> rows(grouped.begin(), grouped.end());
        int poc = 0;
        for (int i = 1; i < static_cast<int>(rows.size()); i++) {
            if (rows[i].second.total > rows[poc].second.total) poc = i;
        }
        const double target = total_vol * 0.70;
        double va = rows[poc].second.total;
        int lo = poc, hi = poc;
        const int n = static_cast<int>(rows.size());
        while (va < target && (lo > 0 || hi < n - 1)) {
            const double above = hi < n - 1 ? rows[hi + 1].second.total : 0;
            const double below = lo > 0 ? rows[lo - 1].second.total : 0;
            if (above >= below && hi < n - 1)      { hi++; va += rows[hi].second.total; }
            else if (lo > 0)                        { lo--; va += rows[lo].second.total; }
            else if (hi < n - 1)                    { hi++; va += rows[hi].second.total; }
            else break;
        }

        pb::VolumeProfileResponse resp;
        resp.set_start_time(req.start_ms);
        resp.set_end_time(req.end_ms);
        resp.set_poc(rows[static_cast<size_t>(poc)].first);
        resp.set_vah(rows[static_cast<size_t>(hi)].first);
        resp.set_val(rows[static_cast<size_t>(lo)].first);
        resp.set_total_volume(total_vol);
        resp.set_value_area_volume(va);
        resp.set_poc_index(poc);
        resp.set_vah_index(hi);
        resp.set_val_index(lo);
        for (int i = 0; i < n; i++) {
            auto* l = resp.add_levels();
            l->set_price(rows[static_cast<size_t>(i)].first);
            l->set_buy_volume(rows[static_cast<size_t>(i)].second.buy);
            l->set_sell_volume(rows[static_cast<size_t>(i)].second.sell);
            l->set_total_volume(rows[static_cast<size_t>(i)].second.total);
            l->set_trade_count(rows[static_cast<size_t>(i)].second.trades);
            l->set_volume_percentage(total_vol > 0
                ? rows[static_cast<size_t>(i)].second.total / total_vol * 100.0 : 0.0);
            l->set_is_poc(i == poc);
            l->set_in_value_area(i >= lo && i <= hi);
        }
        std::string bytes;
        if (resp.SerializeToString(&bytes)) {
            route_frame(0, static_cast<uint32_t>(pb::STREAM_VOLUME_PROFILE), 0, bytes);
        }
    }
}

void PackReplayEngine::serve_heatmap_requests() {
    if (pending_heatmap_reqs_.empty()) return;
    if (heatmap_seed_state_ == SeedState::Absent) {
        pending_heatmap_reqs_.clear();
        return;
    }
    if (heatmap_seed_state_ != SeedState::Ready) return;

    while (!pending_heatmap_reqs_.empty()) {
        const PendingHeatmapReq req = pending_heatmap_reqs_.front();
        pending_heatmap_reqs_.pop_front();

        const int64_t end = std::min(req.end_ms, market_now_ms());
        pb::HeatmapSnapshotBatch out;
        const int n = std::min(heatmap_seed_.snapshot_blobs_size(),
                               header_.heatmap_seed_ts_ms_size());
        for (int i = 0; i < n; i++) {
            const int64_t ts = header_.heatmap_seed_ts_ms(i);
            if (ts < req.start_ms || ts > end) continue;
            out.add_snapshot_blobs(heatmap_seed_.snapshot_blobs(i));
        }
        if (out.snapshot_blobs_size() == 0) {
            // Box parity: zero rows → no response frame.
            continue;
        }
        std::string bytes;
        if (out.SerializeToString(&bytes)) {
            route_frame(0, static_cast<uint32_t>(pb::STREAM_HISTORICAL_HEATMAPS),
                        req.timeframe, bytes);
        }
    }
}
