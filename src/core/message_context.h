#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// message_context.h - Context for message routing
//
// Subset of AppContext containing only the managers needed by MessageHandler.
// Eliminates the 7-parameter cascade through handle_message → route_message.
//
// When dispatch_queue is non-null (threaded mode), stream dispatches are queued
// for the main thread instead of being called directly. This prevents data
// races between the data thread and widget update/render on the main thread.
// ═══════════════════════════════════════════════════════════════════════════════

class StreamManager;
class OrderbookManager;
class HeatmapManager;
class LiquidationHeatmapManager;
class DebugManager;
class VolumeProfileManager;
class TPOManager;
class FootprintManager;
class PaperTradingManager;
class TickerManager;
class ScannerManager;
class IndicatorSeriesManager;
class AnalyticsManager;
class PreviewCandleStore;
class DispatchQueue;

struct MessageContext {
    StreamManager* streams = nullptr;
    OrderbookManager* orderbooks = nullptr;
    HeatmapManager* heatmaps = nullptr;
    LiquidationHeatmapManager* liq_heatmaps = nullptr;
    DebugManager* debug = nullptr;
    VolumeProfileManager* vpvr = nullptr;
    TPOManager* tpo = nullptr;
    FootprintManager* footprint = nullptr;
    PaperTradingManager* paper_trading = nullptr;
    TickerManager* ticker = nullptr;
    ScannerManager* scanner = nullptr;

    // When non-null, stream dispatches are queued instead of called directly.
    // Set this when processing messages on the data thread.
    DispatchQueue* dispatch_queue = nullptr;

    // Indicators V1 SeriesCache (VPIN et al). Declared LAST - existing call
    // sites build this struct positionally; keep it after dispatch_queue and
    // set it by member assignment.
    IndicatorSeriesManager* series = nullptr;
    // Analytics state (Positioning/Contagion). Same rule as series: set by
    // member assignment (msg_ctx.analytics = ...), never positionally.
    AnalyticsManager* analytics = nullptr;
    // Scrub-preview candle store (replay contexts only; null in live mode).
    // Same rule as series/analytics: set by member assignment only.
    PreviewCandleStore* preview = nullptr;
};
