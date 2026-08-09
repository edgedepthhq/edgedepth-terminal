#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// app_context.h - Single dependency injection point for the application
//
// All managers accessible through one struct. Passed to widgets, menus,
// and handlers instead of 3-8 individual pointer/reference parameters.
//
// Replaces the pattern of:
//   ChartWidget(pair, candle_mgr, stream_mgr, heatmap_mgr, ob_mgr,
//               replay_mgr, liq_heatmap_mgr, tick_size)
// With:
//   ChartWidget(pair, ctx, tick_size)
//
// Adding a new manager: add one field here, done. No constructor cascades.
// ═══════════════════════════════════════════════════════════════════════════════

class StreamManager;
class OrderbookManager;
class HeatmapManager;
class LiquidationHeatmapManager;
class CandleManager;
class ReplayManager;
class DebugManager;
class VolumeProfileManager;
class TPOManager;
class FootprintManager;
class PaperTradingManager;
class TickerManager;
class IndicatorSeriesManager;
class AnalyticsManager;
class DrawingManager;

struct AppContext {
    StreamManager*               streams     = nullptr;
    OrderbookManager*            orderbooks  = nullptr;
    HeatmapManager*              heatmaps    = nullptr;
    LiquidationHeatmapManager*   liq_heatmaps = nullptr;
    CandleManager*               candles     = nullptr;
    ReplayManager*               replayer    = nullptr;
    DebugManager*                debug       = nullptr;
    VolumeProfileManager*        vpvr        = nullptr;
    TPOManager*                  tpo         = nullptr;
    FootprintManager*            footprint   = nullptr;
    PaperTradingManager*         paper_trading = nullptr;
    TickerManager*               tickers     = nullptr;
    // Indicators V1 SeriesCache (VPIN pathfinder; hawkes/cfti/flow later).
    IndicatorSeriesManager*      series      = nullptr;
    // Analytics state (Positioning; Contagion in replay). VPIN stays in series.
    AnalyticsManager*            analytics   = nullptr;
    // Chart drawing tools (user annotations). Owned by AppState, NOT DataContext:
    // drawings survive live<->replay context swaps untouched.
    DrawingManager*              drawings    = nullptr;

    // Convenience accessors with assert-style safety
    [[nodiscard]] StreamManager&             stream_mgr()    const { return *streams; }
    [[nodiscard]] OrderbookManager&          ob_mgr()        const { return *orderbooks; }
    [[nodiscard]] HeatmapManager&            heatmap_mgr()   const { return *heatmaps; }
    [[nodiscard]] LiquidationHeatmapManager& liq_heatmap_mgr() const { return *liq_heatmaps; }
    [[nodiscard]] CandleManager&             candle_mgr()    const { return *candles; }
    [[nodiscard]] ReplayManager&             replay_mgr()    const { return *replayer; }
    [[nodiscard]] DebugManager&              debug_mgr()     const { return *debug; }
    [[nodiscard]] VolumeProfileManager&      vpvr_mgr()      const { return *vpvr; }
    [[nodiscard]] TPOManager&                tpo_mgr()       const { return *tpo; }
    [[nodiscard]] FootprintManager&          footprint_mgr() const { return *footprint; }
    [[nodiscard]] PaperTradingManager&       paper_trading_mgr() const { return *paper_trading; }
    [[nodiscard]] TickerManager&             ticker_mgr()  const { return *tickers; }
    [[nodiscard]] IndicatorSeriesManager&    series_mgr()  const { return *series; }
    [[nodiscard]] AnalyticsManager&          analytics_mgr() const { return *analytics; }
    [[nodiscard]] DrawingManager&            drawing_mgr() const { return *drawings; }
};
