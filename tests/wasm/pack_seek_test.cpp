#include "replayer/pack_replay_engine.h"
#include "replayer/replay_manager.h"
#include "core/message_handler.h"
#include "core/orderbook_manager.h"
#include "core/realtime_archive.h"
#include "stream_handler.h"
#include <cstdio>

std::function<bool(const std::string&)> StreamManager::pack_request_hook_;
// The production block decoder and seek state run here without HTTP or UI.
// The route is unused (the test feeds decoded records to the real book owner).
bool RealtimeArchive::prepare_replay(CandleManager*, int64_t) { return true; }
MessageContext ReplayManager::replay_message_context() const { return {}; }
void MessageHandler::route_parsed(const pb::WSPayload&, const MessageContext&) {}

struct PackReplayEngineTest {
    static int run() {
        int failures = 0;
        auto check = [&](bool ok, const char* why) {
            if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
        };
        PackReplayEngine engine(nullptr);
        engine.header_.set_start_ts_ms(1000);
        engine.header_.set_end_ts_ms(200000);
        engine.header_.add_blocks()->set_last_ts_ms(50000);
        engine.header_.add_blocks()->set_last_ts_ms(100000);
        engine.header_ready_ = true;
        engine.control_seek(61000);
        check(engine.next_block_ == 0, "seek reads all depth blocks after opening seed");
        engine.wall_base_ms_ -= 10000;
        check(engine.market_now_ms() == 61000, "clock holds throughout reconstruction");
        check(engine.emit_queue_.size() == 1, "seek ack is deferred until caller transition");
        auto old_generation = engine.generation_;
        engine.control_seek(60000);
        engine.on_fetch(old_generation, 2, 0, nullptr, 0, 206);
        check(engine.fetch_responses_.empty(), "previous seek response cannot enter new generation");

        pb::BookUpdate seed;
        seed.set_snapshot(true); seed.set_timestamp_ms(1000); seed.set_last_update_id(100);
        auto* bid = seed.add_bids(); bid->set_price(100); bid->set_size(2);
        auto* ask = seed.add_asks(); ask->set_price(101); ask->set_size(3);
        OrderbookManager books;
        books.set_replay_mode(true);
        Terminal::Pair pair{"binancef", "fixture"};
        books.apply_orderbook_snapshot_from_pb(pair, seed);
        std::string block;
        auto append = [&](int64_t ts, pb::Stream stream, const std::string& payload) {
            pb::PackFrame frame; frame.set_ts_ms(ts); frame.set_stream(stream); frame.set_payload(payload);
            auto bytes = frame.SerializeAsString(); uint32_t size = bytes.size();
            for (int i=0;i<4;++i) block.push_back(static_cast<char>((size >> (8*i)) & 255));
            block += bytes;
        };
        pb::BookUpdate delta;
        delta.set_first_update_id(101); delta.set_last_update_id(102); delta.set_previous_update_id(100);
        delta.set_timestamp_ms(2000);
        auto* remove = delta.add_bids(); remove->set_price(100); remove->set_size(0);
        auto* replace = delta.add_bids(); replace->set_price(99); replace->set_size(4);
        append(2000, pb::STREAM_ORDERBOOK, delta.SerializeAsString());
        append(3000, pb::STREAM_TRADES, "pre-target trade");
        delta.clear_bids(); delta.set_first_update_id(103); delta.set_last_update_id(104);
        delta.set_previous_update_id(102); delta.set_timestamp_ms(60000);
        append(60000, pb::STREAM_ORDERBOOK, delta.SerializeAsString());
        append(60000, pb::STREAM_TRADES, "target trade");
        delta.set_first_update_id(105); delta.set_last_update_id(106);
        delta.set_previous_update_id(104); delta.set_timestamp_ms(60100);
        append(60100, pb::STREAM_ORDERBOOK, delta.SerializeAsString());
        check(engine.decode_block_into_queue(block), "production block decoder accepts fixture");
        engine.have_full_file_ = true;
        engine.maybe_fetch_next_block();
        check(engine.next_block_ == 0, "catch-up drains one block before fetching another");
        int trades=0;
        for (const auto& frame : engine.frame_queue_) {
            if (frame.ts > engine.market_now_ms()) break;
            if (frame.stream == pb::STREAM_ORDERBOOK) {
                pb::BookUpdate update; update.ParseFromString(frame.payload);
                books.apply_book_update_from_pb(pair, update);
            } else { ++trades; check(frame.ts == (trades == 1 ? 3000 : 60000), "both historical and target trade timestamps remain original"); }
        }
        std::vector<RealtimeDepthHistory::SamplePtr> samples;
        check(books.copy_realtime_since(pair, 0, samples), "seed plus intervening deltas preserves strict RT continuity");
        books.swap_buffers();
        const auto* book=books.get_orderbook(pair);
        check(book->last_update_id == 104 && book->bids.begin()->first == 99,
              "target book includes pre-target removals and excludes future deltas");
        check(samples.back()->timestamp_ms == 60000 && trades == 2, "seek restores historical trades through the inclusive target");
        engine.wall_base_ms_ -= 10000;
        check(engine.is_seeking() && engine.playback_time_ms() == 60000,
              "display clock stays at target throughout seek reconstruction");
        engine.control_pause(); engine.wall_base_ms_ -= 10000;
        check(engine.market_now_ms() == 60000, "pause freezes target while reconstruction is pending");
        delta.set_previous_update_id(105); delta.set_first_update_id(106);
        books.apply_book_update_from_pb(pair, delta);
        check(!books.copy_realtime_since(pair, 0, samples), "real depth gap still invalidates reconstructed book");
        std::printf("pack seek regression: %d failures\n", failures);
        return failures;
    }
};
int main() { return PackReplayEngineTest::run() ? 1 : 0; }
