#include "core/realtime_history.h"
#include "ui/realtime_dom_frame.h"
#include "ui/realtime_navigation.h"
#include <cmath>
#include "core/orderbook_manager.h"
#include <cstdio>

static int failures = 0;
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int main() {
    const auto zoom_in = realtime_zoom(60000, 1, 0.1, true, false);
    const auto zoom_out = realtime_zoom(60000, -1, 0.1, true, false);
    expect(zoom_in.follow && zoom_in.span_ms < 60000, "zoom in retains follow and narrows time");
    expect(zoom_out.follow && zoom_out.span_ms > 60000, "zoom out retains follow and widens time");
    expect(!realtime_zoom(40000, 1, 0.1, false, false).follow, "zoom in preserves panned history");
    expect(realtime_zoom(40000, -1, 0.1, false, false).follow, "running zoom out deliberately resumes follow");
    for (float wheel : {-1.0f, 1.0f}) {
        const auto paused = realtime_zoom(40000, wheel, 0.1, false, true);
        expect(!paused.follow && paused.span_ms == 40000, "paused live/replay history uses ImPlot zoom without rearming follow");
        expect(realtime_zoom(40000, wheel, 0.1, true, true).follow, "paused following view can zoom around its frozen clock");
    }
    expect(realtime_zoom(5000, 1, 0.1, true, false).span_ms == 5000, "minimum zoom span");
    expect(realtime_zoom(300000, -1, 0.1, true, false).span_ms == 300000, "maximum zoom span");
    expect(!realtime_zoom(40000, 0, 0.1, false, false).follow, "no-input/focus recovery cannot rearm follow");
    RealtimeDOMFrame dense;
    dense.price_min = 78850; dense.price_max = 78920;
    dense.top = 100; dense.bottom = 800;
    expect(std::abs(dense.price_y(78865.4) - dense.price_y(78865.5) - 1.0f) < 0.001f,
        "BTC one-tick spread retains exact one-pixel price separation");
    dense.price_max = 78990;
    expect(std::abs(dense.price_y(78865.4) - dense.price_y(78865.5) - 0.5f) < 0.001f,
        "subpixel spread is never widened to a readable row");
    // Clearly synthetic unit fixture. Never a product capture.
    Terminal::Orderbook book;
    book.snapshot = true;
    book.bids.insert_or_assign(100, 2);
    book.asks.insert_or_assign(101, 3);
    RealtimeDepthHistory history;
    std::vector<RealtimeDepthHistory::SamplePtr> samples;
    history.observe(book, 1001);
    history.copy_since(0, samples);
    expect(samples.empty(), "no pre-seed depth");
    history.seed(); history.observe(book, 1017);
    history.observe(book, 1090);
    history.copy_since(0, samples);
    expect(samples.size() == 1 && samples[0]->timestamp_ms == 1017, "actual first clock per bin retained");
    expect(samples[0]->segment_start, "join is a sampling boundary");
    history.check_delta(100, 99, 102, 98);
    expect(history.valid(), "snapshot bridge accepts first overlapping diff");
    history.observe(book, 1111);
    history.copy_since(0, samples);
    expect(samples.size() == 2 && !samples.back()->segment_start, "continuation stays in same segment");
    history.check_delta(102, 105, 106, 104);
    expect(!history.valid(), "even a small sequence gap invalidates RT evidence");
    history.observe(book, 1217);
    history.check_delta(106, 107, 108, 106);
    history.observe(book, 1317);
    history.copy_since(0, samples);
    expect(samples.size() == 2, "apparently contiguous later deltas cannot heal missing depth");
    history.seed(); history.observe(book, 1417);
    history.copy_since(0, samples);
    expect(samples.back()->segment_start && samples.back()->timestamp_ms == 1417, "new seed starts new observed segment");
    const auto serial = samples.back()->serial;
    history.observe(book, 1300);
    history.copy_since(serial, samples);
    expect(samples.empty(), "older source clock cannot create new history");
    history.interrupt(); history.observe(book, 1500);
    history.copy_since(serial, samples);
    expect(samples.empty(), "transport interruption requires seed");
    history.seed();
    for (int64_t ts = 2000; ts < 402000; ts += 100) history.observe(book, ts);
    history.copy_since(0, samples);
    expect(samples.size() == RealtimeDepthHistory::max_samples, "time and sampling bound the history");
    expect(samples.front()->timestamp_ms == 102000, "expired observations evicted");
    history.trim_after(201000); history.copy_since(0, samples);
    expect(!history.valid() && samples.back()->timestamp_ms == 201000, "rewind excludes future observations and requires seed");
    RealtimeDepthHistory other;
    other.copy_since(0, samples);
    expect(samples.empty(), "separate market owners cannot leak depth");
    book.bids.insert_or_assign(102, 1);
    other.seed(); other.observe(book, 1234);
    expect(!other.valid(), "crossed source is not RT evidence");

    RealtimeTradeHistory trades;
    Terminal::Trade trade{100, 2, 1001, true};
    trades.append(trade); trades.append(trade);
    trades.append({101, 3, 1000, false});
    expect(trades.trades().size() == 3, "equal records preserve source multiplicity without guessed deduplication");
    expect(trades.trades().front().timestamp_ms == 1000 && !trades.trades().front().is_buy,
        "late records retain actual time and aggressor side");
    trades.trim_after(1000);
    expect(trades.trades().size() == 1, "as-of trim drops future trade records");
    trades.append({100, 0, 1001, true});
    trades.append({0, 1, 1001, true});
    expect(trades.trades().size() == 1, "invalid prints do not become bubbles");
    for (int i = 0; i < 25000; ++i) trades.append({100, 1, 2000 + i, true});
    expect(trades.trades().size() == RealtimeTradeHistory::max_trades, "trade count cap is strict");
    trades.append({100, 1, 500000, true});
    expect(trades.trades().size() == 1, "old trades expire by source clock");
    trades.clear(); expect(trades.trades().empty(), "seek clears traversal before records replay");
    OrderbookManager manager;
    const Terminal::Pair pair{"binancef", "btcusdt"};
    pb::BookUpdate seed;
    seed.set_snapshot(true); seed.set_timestamp_ms(10017); seed.set_last_update_id(100);
    auto* bid = seed.add_bids(); bid->set_price(100); bid->set_size(2);
    auto* ask = seed.add_asks(); ask->set_price(101); ask->set_size(3);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(manager.copy_realtime_since(pair, 0, samples) && samples.size() == 1, "production snapshot records depth");
    pb::BookTickerUpdate quote;
    quote.set_timestamp_ms(99999); quote.set_best_bid(105); quote.set_best_bid_qty(10);
    manager.apply_book_ticker_from_pb(pair, quote);
    manager.swap_buffers();
    expect(manager.get_orderbook(pair)->timestamp_ms == 10017 &&
        manager.get_orderbook(pair)->bids.begin()->first == 100, "ticker cannot rewrite depth state or clock");
    quote.set_timestamp_ms(10020); quote.set_best_ask(106); quote.set_best_ask_qty(2);
    manager.apply_book_ticker_from_pb(pair, quote);
    expect(manager.realtime_quote(pair, 10019).timestamp_ms == 0, "native BBO excludes future evidence");
    expect(manager.realtime_quote(pair, 10020).best_bid == 105, "native BBO retained independently");
    quote.set_timestamp_ms(10030); quote.set_best_bid(105.5);
    manager.apply_book_ticker_from_pb(pair, quote);
    expect(manager.realtime_quote(pair, 10025).best_bid == 105, "replay cutoff selects prior native BBO");
    expect(manager.realtime_quote(pair, 25031).timestamp_ms == 0, "stale native BBO withheld");
    expect(manager.realtime_quote({"binancef", "other"}, 10025).timestamp_ms == 0, "native quote is symbol-specific");
    const auto frozen_quote = manager.realtime_quote(pair, 10025);
    quote.set_timestamp_ms(10040); quote.set_best_bid(107); // crossed, rejected
    manager.apply_book_ticker_from_pb(pair, quote);
    expect(manager.realtime_quote(pair, 10040).best_bid == 105.5, "crossed native quote rejected");
    expect(frozen_quote.best_bid == 105, "published quote remains immutable during pause");
    pb::BookUpdate delta;
    delta.set_timestamp_ms(10117); delta.set_first_update_id(99);
    delta.set_last_update_id(102); delta.set_previous_update_id(98);
    manager.apply_book_update_from_pb(pair, delta);
    expect(manager.copy_realtime_since(pair, 0, samples) && samples.size() == 2, "production snapshot bridge");
    delta.set_timestamp_ms(10217); delta.set_first_update_id(104);
    delta.set_last_update_id(105); delta.set_previous_update_id(103);
    manager.apply_book_update_from_pb(pair, delta);
    expect(!manager.copy_realtime_since(pair, 0, samples) && samples.size() == 2, "legacy grace does not authorize RT gap carry");
    seed.set_timestamp_ms(10317); seed.set_last_update_id(106);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(manager.copy_realtime_since(pair, 0, samples) && samples.back()->segment_start, "production reseed restores RT");
    manager.set_realtime_transport_open(false);
    manager.set_realtime_transport_open(true);
    expect(manager.realtime_quote(pair, 10040).timestamp_ms == 0, "reconnect cannot revive old native BBO");
    seed.set_timestamp_ms(10417);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(manager.copy_realtime_since(pair, 0, samples) && samples.back()->segment_start,
        "reconnect snapshot starts a segment even with no intervening delta");
    manager.interrupt_realtime();
    expect(!manager.copy_realtime_since(pair, 0, samples), "disconnect immediately invalidates published RT state");
    seed.set_timestamp_ms(10417);
    manager.apply_orderbook_snapshot_from_pb(pair, seed, false);
    expect(!manager.copy_realtime_since(pair, 0, samples), "unverified buffer restore cannot manufacture evidence");
    manager.set_realtime_transport_open(false);
    seed.set_timestamp_ms(10517);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(!manager.copy_realtime_since(pair, 0, samples), "queued seed cannot revive disconnected transport");
    manager.set_realtime_transport_open(true);
    expect(!manager.copy_realtime_since(pair, 0, samples), "reopen alone cannot certify old book");
    seed.set_timestamp_ms(10617);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(manager.copy_realtime_since(pair, 0, samples), "fresh connected seed restores validity");
    const auto generation = manager.realtime_generation();
    manager.clear_all();
    expect(manager.realtime_generation() != generation, "full seek changes chart history generation");
    expect(!manager.copy_realtime_since(pair, 0, samples) && samples.empty(), "source reset clears RT history");
    const Terminal::Pair hl{"hl", "BTC"};
    seed.set_last_update_id(0); seed.set_timestamp_ms(20000);
    manager.apply_orderbook_snapshot_from_pb(hl, seed);
    seed.set_timestamp_ms(20200);
    manager.apply_orderbook_snapshot_from_pb(hl, seed);
    expect(manager.copy_realtime_since(hl, 0, samples) && samples.size() == 2, "full snapshots do not require sequence IDs");
    RealtimeBubbleScale small_scale, btc_scale;
    std::deque<Terminal::Trade> small_trades, btc_trades;
    for (int i = 1; i <= 100; ++i) {
        Terminal::Trade trade{};
        trade.timestamp_ms = 60000 + i;
        trade.price = 0.001; trade.qty = i * 1000; trade.is_buy = i % 2;
        small_trades.push_back(trade);
        trade.price = 80000; trade.qty = i * 0.01;
        btc_trades.push_back(trade);
    }
    small_scale.update(small_trades, 60100);
    btc_scale.update(btc_trades, 60100);
    expect(std::abs(small_scale.minimum() - 75) < 1e-8, "small-market scale uses quote notional percentile");
    expect(std::abs(btc_scale.minimum() - 60000) < 1e-8, "major-market scale adapts independently");
    const double frozen = small_scale.minimum();
    for (auto& trade : small_trades) trade.qty *= 100;
    small_scale.update(small_trades, 60100);
    expect(small_scale.minimum() == frozen, "paused clock cannot rescale bubbles");
    small_scale.update(small_trades, 65100);
    expect(small_scale.minimum() == frozen, "settled scale preserves historical size comparisons");
    RealtimeBubbleScale recalibrated;
    recalibrated.update(small_trades, 65100);
    expect(recalibrated.minimum() == 7500, "explicit recalibration uses current eligible records");
    small_scale.update(small_trades, 59000);
    expect(small_scale.minimum() == 0, "rewind excludes future records from scale");
    small_scale.update(small_trades, 60100);
    expect(small_scale.minimum() == 7500, "rewind starts an eligible scale anew");
    RealtimeDOMFrame frame;
    frame.frame = 10; frame.top = 100; frame.bottom = 900;
    frame.price_min = 90; frame.price_max = 110;
    expect(frame.projected(10) && !frame.projected(11), "DOM rejects a stale chart transform");
    expect(frame.price_y(100) == 500 && frame.price_y(101) == 460, "all prices share the chart screen transform");
    frame.top = 200; frame.bottom = 600;
    expect(frame.price_y(101) == 380, "resize changes DOM mapping in the same frame");
    frame.price_min = 99; frame.price_max = 103;
    expect(frame.price_y(100) == 500 && frame.price_y(101) == 400, "zoom and pan preserve absolute price alignment");
    auto linked_book = std::make_shared<RealtimeDepthHistory::Sample>();
    linked_book->timestamp_ms = 1000;
    frame.book = linked_book; frame.clock_ms = 1000; frame.synchronized = true;
    expect(frame.fresh(), "DOM accepts chart-eligible sampled book");
    frame.clock_ms = 999;
    expect(!frame.fresh(), "linked DOM never exposes future replay depth");
    frame.clock_ms = 16001;
    expect(!frame.fresh(), "linked DOM shares chart staleness boundary");
    frame.clock_ms = 1000; frame.paused = true;
    expect(frame.fresh(), "paused DOM uses frozen chart clock instead of wall clock");
    frame.synchronized = false;
    expect(!frame.fresh(), "seek or sequence failure withholds linked DOM despite retained book");
    if (!failures) std::puts("PASS: RT sequence, clocks, gaps, retention, rewind and trade multiplicity");
    return failures ? 1 : 0;
}
