#include "core/realtime_history.h"
#include "ui/realtime_dom_frame.h"
#include "ui/realtime_navigation.h"
#include <cmath>
#include "rendering/realtime_trade_view.h"
#include "core/orderbook_manager.h"
#include <cstdio>

static int failures = 0;
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int test_archive_capture();
int main() {
    failures += test_archive_capture();
    RealtimeTradeHistory observed;
    size_t captured=0, resets=0;
    observed.set_observer([&](const Terminal::Trade* t){if(t)++captured;else ++resets;});
    Terminal::Trade execution{};execution.price=100;execution.qty=2;execution.timestamp_ms=1000;execution.is_buy=true;
    for(size_t i=0;i<25000;++i)observed.append(execution);
    expect(captured==25000 && observed.trades().size()==20000,"archive observer receives every record before working-set eviction");
    static RealtimeTradeView view;
    view.build(observed.trades(), 0, 2000, 1);
    double qty=0;for(size_t i=0;i<view.count;++i)qty+=view.records[i].qty;
    expect(view.grouped && view.count==1 && qty==40000,"dense bubble view preserves all loaded volume instead of deleting oldest markers");
    observed.clear();expect(resets==1,"archive observer follows source resets");
    for(int i=0;i<100;++i){execution.timestamp_ms=1000+i;observed.append(execution);}
    view.build(observed.trades(),0,2000,500,true);
    double grouped_qty=0;for(size_t i=0;i<view.count;++i)grouped_qty+=view.records[i].qty;
    expect(view.grouped && view.count==1 && grouped_qty==200,
        "grouped snapshot and raw tail stay aggregated below the marker limit");
    view.build(observed.trades(),0,2000,1);
    expect(!view.grouped && view.count==100 && view.records[0].timestamp_ms==1000,"detail view restores individual records and original timestamps");

    // Ten minutes at 100 executions/sec: count retirement precedes age
    // retirement, while a deliberately stalled archive cutoff never moves.
    RealtimeTradeHistory busy;
    std::deque<Terminal::Trade> displayed;
    bool compacted=false;
    for (int second=0;second<600;++second) {
        for(int i=0;i<100;++i) busy.append({100.0+(i%3),2.0,10000+second*1000+i,bool(i%2)});
        refresh_realtime_trade_tail(displayed,busy.trades(),9999,10000+second*1000+999);
        compacted=bound_realtime_trade_tail(displayed,busy.trades(),view) || compacted;
        expect(displayed.size()<=40000,"stalled archive trade display stays bounded");
        expect(displayed.back().timestamp_ms==10000+second*1000+99,"live bubbles keep advancing after retention boundaries");
    }
    double retained_qty=0,retained_notional=0;
    int64_t previous_trade=0;
    for(const auto& t:displayed) {
        retained_qty+=t.qty;retained_notional+=t.price*t.qty;
        expect(t.timestamp_ms>=previous_trade,"compacted tail stays chronological");previous_trade=t.timestamp_ms;
    }
    expect(compacted && std::abs(retained_qty-120000)<1e-6 && std::abs(retained_notional-12118800)<1e-5,
        "bounded stalled view preserves all observed quantity and price-weighted notional");
    const auto retained_size=displayed.size();
    refresh_realtime_trade_tail(displayed,busy.trades(),9999,610000);
    expect(displayed.size()==retained_size,"repeated stale-tail merge cannot duplicate executions");
    std::deque<Terminal::Trade> sparse{{100,1,400000,true},{100,2,400001,true}};
    displayed={{100,3,1000,true},{100,4,2000,true}};
    expect(!refresh_realtime_trade_tail(displayed,sparse,1000,400001) && displayed.size()==4,
        "time retirement below the count cap retains good history and the complete sparse tail");

    std::deque<Terminal::Trade> partial;
    partial.push_back({100,2,2000,true});
    for(int i=1;i<20000;++i) partial.push_back({101,1,2000+i,false});
    displayed={{100,2,2000,true},{100,2,2000,true},{100,2,2000,true}};
    refresh_realtime_trade_tail(displayed,partial,1000,22000);
    expect(displayed.size()==20002 && displayed[0].qty+displayed[1].qty+displayed[2].qty==6,
        "a partial oldest timestamp cannot erase already displayed duplicate multiplicity");

    expect(realtime_query_start_matches(true,1200,1000,100),"following clock can advance while a snapshot loads");
    expect(!realtime_query_start_matches(true,1000,1500,100),"zoom out cannot accept a later-starting snapshot");
    expect(!realtime_query_start_matches(false,1500,1000,100),"pan rejects an old range even at unchanged fidelity");
    expect(realtime_query_start_matches(false,1100,1000,100),"one aligned-bin margin remains valid");

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
    expect(realtime_zoom(1800000 / 0.88, -1, 0.1, true, false).span_ms == 1800000 / 0.88, "maximum zoom span");
    expect(!realtime_zoom(40000, 0, 0.1, false, false).follow, "no-input/focus recovery cannot rearm follow");
    expect(realtime_price_half_span(0.64224, 0.64249, 0.00001, 5) >= 0.0012,
           "quiet SD market fits at least 48 grouped depth rows");
    expect(realtime_price_half_span(90, 110, 0.01, 5) == 10,
           "volatile market still fits the full observed move");
    for (double tick : {0.00001, 0.01, 0.1}) for (int group : {1, 2, 5, 10, 20}) {
        const double step = tick * group, center = tick < 0.001 ? 0.184 : 90000;
        RealtimePriceWindow window;
        auto r = window.update(center - 100, center + 100, step, 800, 16, center, true);
        expect(step * 800 / (r.high - r.low) >= 16 - 1e-5,
            "TUT and BTC every fidelity reserve readable row height");
        const auto stable = window.update(r.low, r.high, step, 800, 16, center + step, true);
        expect(stable.low == r.low && stable.high == r.high, "safe zone does not recenter each tick");
        const auto moved = window.update(r.low, r.high, step, 800, 16, center + 100 * step, true);
        expect(std::abs((moved.high - moved.low) - (r.high - r.low)) < step * 1e-5,
            "volatile price shifts the fixed window without compressing rows");
        const auto frozen = window.update(r.low, r.high, step, 800, 16, center + 100 * step, false);
        expect(frozen.low == r.low && frozen.high == r.high, "pause/manual view freezes market following");
        r = window.update(r.low, r.high, step, 400, 16, center, false);
        expect(step * 400 / (r.high - r.low) >= 16 - 1e-5, "resize keeps row height readable");
        r = window.update(r.low, r.high, step * 2, 400, 16, center, false);
        expect(step * 2 * 400 / (r.high - r.low) >= 16 - 1e-5, "fidelity changes span rather than row height");
        r = window.update(center - 100, center + 100, step * 2, 400, 16, center, false);
        expect(step * 2 * 400 / (r.high - r.low) >= 16 - 1e-5, "zoom out cannot hide numeric rows");
    }
    expect(!realtime_pan_detaches(3, 2), "click jitter preserves RT follow");
    expect(!realtime_pan_detaches(4, 40), "vertical gesture preserves RT follow");
    expect(realtime_pan_detaches(30, 4), "intentional horizontal pan detaches RT follow");
    expect(realtime_view_has_live_edge(false, 1100, 1000), "detached viewport containing now keeps depth advancing");
    expect(!realtime_view_has_live_edge(false, 900, 1000), "past viewport does not project current depth into history");
    expect(realtime_view_has_live_edge(true, 900, 1000), "follow tolerates previous-frame viewport bounds");
    double fresh_span = 60000;
    for (int i=0;i<5;++i) fresh_span = realtime_zoom(fresh_span,-1,0.1,true,false).span_ms;
    expect(fresh_span > 90000, "fresh session can zoom out repeatedly without a coverage clamp");
    RealtimeDOMFrame dense;
    dense.price_min = 78850; dense.price_max = 78920;
    dense.top = 100; dense.bottom = 800;
    expect(std::abs(dense.price_y(78865.4) - dense.price_y(78865.5) - 1.0f) < 0.001f,
        "BTC one-tick spread retains exact one-pixel price separation");
    dense.price_max = 78990;
    expect(std::abs(dense.price_y(78865.4) - dense.price_y(78865.5) - 0.5f) < 0.001f,
        "subpixel spread is never widened to a readable row");
    for (int mult : {1, 2, 5, 10, 20}) {
        dense.native_tick = 0.00001; dense.bucket_ticks = mult;
        const double boundary = 20000 * dense.bucket_size();
        expect(dense.bucket_index(boundary) == 20000, "exact boundary belongs to next heatmap bucket");
        expect(dense.bucket_index(boundary - dense.native_tick) == 19999,
            "preceding native tick remains in previous heatmap bucket");
        expect(dense.bucket_index(boundary + (mult - 1) * dense.native_tick) == 20000,
            "all native ticks aggregate into the same fidelity bucket");
        dense.price_min = boundary - 10 * dense.bucket_size();
        dense.price_max = boundary + 10 * dense.bucket_size();
        expect(std::abs(dense.price_y(dense.bucket_center(20000)) -
            (dense.price_y(boundary) + dense.price_y(boundary + dense.bucket_size())) * 0.5f) < 0.001f,
            "DOM center lies halfway between heatmap boundaries at every fidelity");
    }
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

    // SOPH hosted seed at 2026-09-08 09:42 UTC lagged its exchange chain.
    // These real IDs/times reproduce the reader dropping pre-seed overlap.
    {
        OrderbookManager replay;
        replay.set_replay_mode(true);
        Terminal::Pair p{"binancef", "sophusdt"};
        pb::BookUpdate seed;
        seed.set_snapshot(true); seed.set_timestamp_ms(1788860520000LL);
        seed.set_last_update_id(11503682733156LL);
        auto* b = seed.add_bids(); b->set_price(0.01189); b->set_size(20);
        auto* a = seed.add_asks(); a->set_price(0.01190); a->set_size(30);
        replay.apply_orderbook_snapshot_from_pb(p, seed);
        pb::BookUpdate d;
        d.set_timestamp_ms(1788860520025LL); d.set_first_update_id(11503682741576LL);
        d.set_last_update_id(11503682748680LL); d.set_previous_update_id(11503682741575LL);
        replay.apply_book_update_from_pb(p, d);
        expect(!replay.copy_realtime_since(p, 0, samples), "actual hosted first transition rejects missing pre-seed chain");
        replay.clear_all(); replay.apply_orderbook_snapshot_from_pb(p, seed);
        d.set_timestamp_ms(1788860519887LL); d.set_first_update_id(11503682733905LL);
        d.set_last_update_id(11503682734885LL); d.set_previous_update_id(11503682733156LL);
        replay.apply_book_update_from_pb(p, d);
        expect(replay.copy_realtime_since(p, 0, samples), "retained pre-seed event bridges by exact previous ID");
        // A batched forward skip must keep this anchor, just like normal delivery.
        d.set_timestamp_ms(1788860519913LL); d.set_first_update_id(11503682734999LL);
        d.set_last_update_id(11503682736802LL); d.set_previous_update_id(11503682734885LL);
        replay.apply_book_update_from_pb(p, d);
        expect(replay.copy_realtime_since(p, 0, samples), "next batched delta preserves strict replay continuity");
    }

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
    expect(manager.realtime_ready(pair, 10017), "seed is eligible at its actual observed time");
    expect(!manager.realtime_ready(pair, 10016), "future depth cannot prime replay");
    expect(!manager.realtime_ready(pair, 26000), "old seed cannot prime a later seek");
    expect(!manager.realtime_ready(pair, 10317, 10317), "client waits for reconstruction already acknowledged on the JSON lane");
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
    expect(!manager.realtime_ready(pair, 10217), "legacy DOM ID cannot release a broken RT sequence");
    seed.set_timestamp_ms(10317); seed.set_last_update_id(106);
    manager.apply_orderbook_snapshot_from_pb(pair, seed);
    expect(manager.copy_realtime_since(pair, 0, samples) && samples.back()->segment_start, "production reseed restores RT");
    expect(manager.realtime_ready(pair, 10317), "valid reseed restores readiness");
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
