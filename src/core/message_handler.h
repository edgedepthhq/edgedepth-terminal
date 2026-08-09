#pragma once
#include <string>

#include "message_context.h"
#include "heatmap_manager.h"
#include "liquidation_heatmap_manager.h"
#include "pb/messages.pb.h"
#include "stream_handler.h"
#include "orderbook_manager.h"
#include "debug_manager.h"
#include "volume_profile_manager.h"
#include "tpo_manager.h"
#include "footprint_manager.h"
#include "paper_trading_manager.h"
#include "ticker_manager.h"
#include "scanner_manager.h"
#include "indicator_series.h"
#include "analytics_manager.h"

class MessageHandler {
public:
    // Main entry point — context-based (preferred)
    static void handle_message(const std::string& data, const MessageContext& ctx);

    // Route a pre-parsed WSPayload — use when the caller has already
    // decompressed + parsed (avoids double work in the replay capture path).
    static void route_parsed(const pb::WSPayload& ws_payload, const MessageContext& ctx);

    // Timestamp of the last successfully routed message (event time, not wall clock).
    // Set by route_message from WSPayload.event_time_ms — the replay event timestamp
    // carried on the outer protobuf envelope. Falls back to 0 during live mode.
    // Used by the history buffer to timestamp captured entries during replay.
    static int64_t last_timestamp_ms;

private:
    // Internal routing
    static void route_message(
        const pb::WSPayload& ws_payload,
        const MessageContext& ctx
    );

    // Individual message type handlers (parse from raw bytes)
    // orderbook_mgr may be null; when set, the trade also updates the book's
    // last_price for sources whose depth feed carries none.
    static void handle_trade_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        StreamManager* stream_mgr,
        OrderbookManager* orderbook_mgr = nullptr
    );

    static void handle_candle_message(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_orderbook_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        OrderbookManager* orderbook_mgr
    );

    static void handle_ticker_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        OrderbookManager* orderbook_mgr
    );

    static void handle_volumes_message(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_stats_message(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_historical_candles(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_historical_volumes(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    // Scrub-preview candle batch (STREAM_REPLAY_PREVIEW_CANDLES): parses a
    // pb::Candles and replaces the replay context's PreviewCandleStore. Never
    // touches StreamManager dispatch — ghost data must not reach CandleManager.
    static void handle_replay_preview_candles(
        int64_t timeframe,
        const void* data, size_t size,
        PreviewCandleStore* preview
    );

    static void handle_historical_stats(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_liquidation_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    static void handle_pattern_admin_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        StreamManager* stream_mgr
    );

    // Conversion helpers (convert protobuf to Terminal types and dispatch)
    static void handle_trade(
        const Terminal::Pair& pair,
        const pb::Trade& trade_pb,
        StreamManager* stream_mgr
    );

    static void handle_candle(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Candle& candle_pb,
        StreamManager* stream_mgr
    );

    static void handle_candles_batch(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Candles& candles_pb,
        StreamManager* stream_mgr
    );

    static void handle_volume(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Volume& volume_pb,
        StreamManager* stream_mgr
    );

    static void handle_volumes_batch(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Volumes& volumes_pb,
        StreamManager* stream_mgr
    );

    static void handle_stat(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Stat& stat_pb,
        StreamManager* stream_mgr
    );

    static void handle_stats_batch(
        const Terminal::Pair& pair,
        int64_t timeframe,
        const pb::Stats& stats_pb,
        StreamManager* stream_mgr
    );

    static void handle_liquidation(
        const Terminal::Pair& pair,
        const pb::Liquidation& liq_pb,
        StreamManager* stream_mgr
    );

    static void handle_debug_message(
        const void* data, size_t size,
        DebugManager* debug_mgr
   );

    static void handle_historical_heatmap_batch(const Terminal::Pair &pair, const void *data, size_t size,
                                         HeatmapManager *heatmap_mgr);
    static void handle_heatmap_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb, HeatmapManager* heatmap_mgr);

    static void handle_liquidation_heatmap_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        LiquidationHeatmapManager* liq_heatmap_mgr
    );

    // HL census liq levels (STREAM_LIQUIDATION_LEVELS=34, P2e): same
    // LiquidationHeatmapUpdate payload as the modelled heatmap, routed into
    // the manager's SEPARATE census store (real-vs-modelled never conflate).
    static void handle_liquidation_levels_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        LiquidationHeatmapManager* liq_heatmap_mgr
    );

    static void handle_historical_liq_heatmap_batch(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        LiquidationHeatmapManager* liq_heatmap_mgr
    );

    static void handle_volume_profile_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        VolumeProfileManager* vpvr_mgr
    );

    static void handle_tpo_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        TPOManager* tpo_mgr
    );

    static void handle_tick_volume_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        FootprintManager* fp_mgr
    );

    static void handle_paper_trading_message(
        const void* data, size_t size,
        PaperTradingManager* paper_mgr
    );

    static void handle_ticker24h_message(
        const std::string& exchange,
        const void* data, size_t size,
        TickerManager* ticker_mgr
    );

    static void handle_scanner_message(
        const std::string& exchange,
        const void* data, size_t size,
        ScannerManager* scanner_mgr
    );

    // Indicators V1 (S1b): VPIN pathfinder — live/seeded VPINStateUpdate
    // frames and the get_historical_vpin batch, both into the SeriesCache.
    static void handle_vpin_state_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        IndicatorSeriesManager* series_mgr
    );

    static void handle_historical_vpin_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        IndicatorSeriesManager* series_mgr
    );

    // Analytics state (Tier 1): PositioningStateUpdate (STREAM_POSITIONING_STATE)
    // and market-wide ContagionSnapshot (STREAM_CONTAGION), parsed into the
    // AnalyticsManager on the main thread (same queue rule as VPIN).
    static void handle_positioning_state_message(
        const Terminal::Pair& pair,
        const void* data, size_t size,
        AnalyticsManager* analytics_mgr
    );

    static void handle_contagion_message(
        const void* data, size_t size,
        AnalyticsManager* analytics_mgr
    );

};
