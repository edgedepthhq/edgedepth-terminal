// ═══════════════════════════════════════════════════════════════════════════════
// data_context.cpp - Factory for replay DataContext instances
// ═══════════════════════════════════════════════════════════════════════════════

#include "data_context.h"
#include "../stream_handler.h"
#include "orderbook_manager.h"
#include "heatmap_manager.h"
#include "liquidation_heatmap_manager.h"
#include "candle_manager.h"
#include "debug_manager.h"
#include "volume_profile_manager.h"
#include "tpo_manager.h"
#include "footprint_manager.h"
#include "indicator_series.h"
#include "analytics_manager.h"
#include "preview_candle_store.h"

// Destructor + move ops must be in .cpp where all types are complete (unique_ptr requirement)
DataContext::~DataContext() = default;
DataContext::DataContext(DataContext&&) noexcept = default;
DataContext& DataContext::operator=(DataContext&&) noexcept = default;

DataContext DataContext::create_replay_context(
    const std::string& session_id,
    const std::vector<std::string>& symbols,
    int ws_handle,
    int64_t start_time_ms,
    int64_t default_timeframe_sec)
{
    DataContext ctx;
    ctx.source = DataSource::Replay;
    ctx.session_id = session_id;
    ctx.symbols = symbols;
    // Use the LIVE WS handle so we can request historical data from the server.
    // Replay binary data is routed here via MessageHandler, not via WS subscriptions.
    ctx.owned_streams = std::make_unique<StreamManager>(ws_handle);
    ctx.streams = ctx.owned_streams.get();
    ctx.streams->set_replay_mode(true);  // Don't send subscribe/unsubscribe to server
    // Stateless managers - default constructors
    ctx.owned_orderbooks = std::make_unique<OrderbookManager>();
    ctx.owned_orderbooks->set_replay_mode(true);  // post-event crossing resolution on archived depth
    ctx.orderbooks = ctx.owned_orderbooks.get();

    ctx.owned_heatmaps = std::make_unique<HeatmapManager>();
    ctx.heatmaps = ctx.owned_heatmaps.get();

    ctx.owned_liq_heatmaps = std::make_unique<LiquidationHeatmapManager>();
    ctx.liq_heatmaps = ctx.owned_liq_heatmaps.get();

    ctx.owned_vpvr = std::make_unique<VolumeProfileManager>();
    ctx.vpvr = ctx.owned_vpvr.get();

    ctx.owned_tpo = std::make_unique<TPOManager>();
    ctx.tpo = ctx.owned_tpo.get();

    ctx.owned_footprint = std::make_unique<FootprintManager>();
    ctx.footprint = ctx.owned_footprint.get();

    // Fresh SeriesCache per replay - the fetchVPINTimeline seed fills it at
    // original timestamps; the live cache stays intact for replay exit.
    ctx.owned_series = std::make_unique<IndicatorSeriesManager>();
    ctx.series = ctx.owned_series.get();
    // Fresh AnalyticsManager per replay - positioning/contagion frames from the
    // archive drip-feed populate it; the live manager stays intact for exit.
    ctx.owned_analytics = std::make_unique<AnalyticsManager>();
    ctx.analytics = ctx.owned_analytics.get();
    // CandleManager needs Pair + timeframe + StreamManager.
    // Use first symbol as primary. Replay will push candles directly via
    // MessageHandler → StreamManager dispatch, not via WS subscription.
    if (!symbols.empty()) {
        Terminal::Pair primary_pair{"binancef", symbols[0]};
        ctx.owned_candles = std::make_unique<CandleManager>(
            primary_pair, default_timeframe_sec, *ctx.streams);
        ctx.candles = ctx.owned_candles.get();
        // Register callbacks on replay StreamManager for incoming replay data
        ctx.candles->subscribe();
        ctx.candles->mark_ready_for_replay();
        ctx.candles->set_replay_time(start_time_ms);
        // Request historical candles UP TO the replay start time.
        // This gives the chart visual context - the candles BEFORE the replay point.
        // Uses the live WS handle to send the request to the server.
        ctx.streams->request_candles_before(
            primary_pair, default_timeframe_sec, start_time_ms,
            CandleManager::INITIAL_PRELOAD_CANDLES);
        // Scrub-preview store (ghost candles): fetch the FULL replay window in
        // one async batch on its dedicated stream. Non-gating - context_primed
        // never waits on it; the ghost pass simply stays empty until it lands.
        // The server resolves the window from its own session state, so the
        // request carries only pair + timeframe. Re-fetched on TF change by
        // ReplayManager::set_timeframe_ms.
        ctx.owned_preview = std::make_unique<PreviewCandleStore>();
        ctx.preview = ctx.owned_preview.get();
        ctx.preview->request(*ctx.streams, primary_pair, default_timeframe_sec);
        // NOTE: Liq heatmap data is delivered by the Replayer's DB drip-feed
        // (fetchLiqHeatmapTimeline), not by a client-side historical request.
        ctx.owned_debug = std::make_unique<DebugManager>(*ctx.streams, primary_pair);
        ctx.debug = ctx.owned_debug.get();
    }

    return ctx;
}
