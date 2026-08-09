#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// data_context.h — Dual-context state isolation for live/replay modes
//
// The DemoNetDriver pattern: create parallel data pipelines that feed into
// the same rendering code. Live managers keep running during replay so
// exiting replay requires no re-sync.
//
// DataContext owns a complete set of managers for one data source.
// The AppState maintains two: live_ctx (always active) and replay_ctx
// (created on replay start, destroyed on stop). AppContext pointers are
// swapped to the active DataContext's managers.
//
// This avoids "if (is_replay)" branching in every widget. Same rendering
// code, different data source — the pointer swap is invisible to widgets.
// ═══════════════════════════════════════════════════════════════════════════════

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "../types/types.h"

class StreamManager;
class OrderbookManager;
class HeatmapManager;
class LiquidationHeatmapManager;
class CandleManager;
class DebugManager;
class VolumeProfileManager;
class TPOManager;
class FootprintManager;
class IndicatorSeriesManager;
class AnalyticsManager;
class PreviewCandleStore;

enum class DataSource : uint8_t {
    Live,
    Replay
};

// DataContext — owns a complete set of data managers for one source.
// Live context: wraps existing g_app managers (non-owning).
// Replay context: owns fresh managers, destroyed on stop via RAII.
struct DataContext {
    DataSource source = DataSource::Live;
    std::string session_id;
    std::vector<std::string> symbols;

    // Owned managers — unique_ptr for RAII cleanup on replay stop.
    // For the live context these are null (g_app owns those managers).
    std::unique_ptr<StreamManager>              owned_streams;
    std::unique_ptr<OrderbookManager>           owned_orderbooks;
    std::unique_ptr<HeatmapManager>             owned_heatmaps;
    std::unique_ptr<LiquidationHeatmapManager>  owned_liq_heatmaps;
    std::unique_ptr<CandleManager>              owned_candles;
    std::unique_ptr<DebugManager>               owned_debug;
    std::unique_ptr<VolumeProfileManager>       owned_vpvr;
    std::unique_ptr<TPOManager>                 owned_tpo;
    std::unique_ptr<FootprintManager>           owned_footprint;
    std::unique_ptr<IndicatorSeriesManager>     owned_series;
    std::unique_ptr<AnalyticsManager>           owned_analytics;
    // Scrub-preview candle store (ghost render source). Replay contexts only;
    // stays null for the live context (no future to preview in live mode).
    std::unique_ptr<PreviewCandleStore>         owned_preview;

    // Non-owning raw pointers — always valid, point to either owned or
    // external (g_app) managers depending on context type.
    StreamManager*              streams      = nullptr;
    OrderbookManager*           orderbooks   = nullptr;
    HeatmapManager*             heatmaps     = nullptr;
    LiquidationHeatmapManager*  liq_heatmaps = nullptr;
    CandleManager*              candles      = nullptr;
    DebugManager*               debug        = nullptr;
    VolumeProfileManager*       vpvr         = nullptr;
    TPOManager*                 tpo          = nullptr;
    FootprintManager*           footprint    = nullptr;
    IndicatorSeriesManager*     series       = nullptr;
    AnalyticsManager*           analytics    = nullptr;
    PreviewCandleStore*         preview      = nullptr;

    bool is_live() const { return source == DataSource::Live; }
    bool is_replay() const { return source == DataSource::Replay; }

    // Factory: create a replay DataContext with fresh managers.
    // ws_handle: the LIVE WebSocket handle — needed to request historical data from server.
    // start_time_ms: replay start timestamp — historical candles are loaded up to this point.
    static DataContext create_replay_context(
        const std::string& session_id,
        const std::vector<std::string>& symbols,
        int ws_handle,
        int64_t start_time_ms,
        int64_t default_timeframe_sec = 300);

    // No copy — unique_ptrs own the managers.
    // Destructor + move ops defined in .cpp where full types are visible.
    DataContext() = default;
    ~DataContext();
    DataContext(DataContext&&) noexcept;
    DataContext& operator=(DataContext&&) noexcept;
    DataContext(const DataContext&) = delete;
    DataContext& operator=(const DataContext&) = delete;
};
