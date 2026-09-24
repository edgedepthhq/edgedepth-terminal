#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// data_context.h - Dual-context state isolation for live/replay modes
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
// code, different data source - the pointer swap is invisible to widgets.
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

// DataContext - owns a complete set of data managers for one source.
// Live context: wraps existing g_app managers (non-owning).
// Replay context: owns fresh managers, destroyed on stop via RAII.
struct DataContext {
    DataSource source = DataSource::Live;
    std::string session_id;
    // The session's markets, all on ONE venue (a replay session is
    // single-venue). symbols[0] is the PRIMARY: the app-level candle manager,
    // the scrub preview and the debug log are its. symbols[1..] are the
    // COMPARE markets played on the same clock; each gets a CandleManager of
    // its own below, and the DOM/tape/heatmap managers are keyed by pair so
    // they need nothing extra.
    std::vector<std::string> symbols;
    std::string exchange = "binancef";

    // Owned managers - unique_ptr for RAII cleanup on replay stop.
    // For the live context these are null (g_app owns those managers).
    std::unique_ptr<StreamManager>              owned_streams;
    std::unique_ptr<OrderbookManager>           owned_orderbooks;
    std::unique_ptr<HeatmapManager>             owned_heatmaps;
    std::unique_ptr<LiquidationHeatmapManager>  owned_liq_heatmaps;
    std::unique_ptr<CandleManager>              owned_candles;
    // One per compare market (symbols[1..]), same StreamManager, same
    // replay-ready state as the primary. A compare ChartWidget reads its
    // manager through candles_for(); the app-level `candles` pointer stays the
    // primary's so nothing that follows "the" chart changes meaning.
    std::vector<std::unique_ptr<CandleManager>> owned_compare_candles;
    // Managers of secondary charts opened DURING this replay (a second chart
    // of a session market with its own timeframe). Owned by the ChartWidget,
    // which registers here on construction and leaves in its destructor;
    // listed so every seek, trim and suppress reaches them too.
    std::vector<CandleManager*> chart_candles;
    std::unique_ptr<DebugManager>               owned_debug;
    std::unique_ptr<VolumeProfileManager>       owned_vpvr;
    std::unique_ptr<TPOManager>                 owned_tpo;
    std::unique_ptr<FootprintManager>           owned_footprint;
    std::unique_ptr<IndicatorSeriesManager>     owned_series;
    std::unique_ptr<AnalyticsManager>           owned_analytics;
    // Scrub-preview candle store (ghost render source). Replay contexts only;
    // stays null for the live context (no future to preview in live mode).
    std::unique_ptr<PreviewCandleStore>         owned_preview;

    // Non-owning raw pointers - always valid, point to either owned or
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

    Terminal::Pair primary_pair() const {
        return Terminal::Pair{exchange, symbols.empty() ? std::string() : symbols.front()};
    }
    // The CandleManager playing `pair` in this context: the primary's for
    // symbols[0], the matching compare manager for symbols[1..], null for a
    // pair this session does not play.
    CandleManager* candles_for(const Terminal::Pair& pair) const;
    // Apply fn to the primary AND every compare CandleManager. Every seek /
    // trim / suppress the replay drives on "the" candle manager must reach the
    // compare managers too, or the second chart keeps candles the first one
    // just dropped. Event paths only (seek, rewind), never the render loop.
    template <typename Fn>
    void each_candles(Fn&& fn) const {
        if (candles) fn(*candles);
        for (const auto& cm : owned_compare_candles) if (cm) fn(*cm);
        for (CandleManager* cm : chart_candles) fn(*cm);
    }

    // Factory: create a replay DataContext with fresh managers.
    // ws_handle: the LIVE WebSocket handle - needed to request historical data from server.
    // start_time_ms: replay start timestamp - historical candles are loaded up to this point.
    static DataContext create_replay_context(
        const std::string& session_id,
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        int ws_handle,
        int64_t start_time_ms,
        int64_t default_timeframe_sec = 300);

    // No copy - unique_ptrs own the managers.
    // Destructor + move ops defined in .cpp where full types are visible.
    DataContext() = default;
    ~DataContext();
    DataContext(DataContext&&) noexcept;
    DataContext& operator=(DataContext&&) noexcept;
    DataContext(const DataContext&) = delete;
    DataContext& operator=(const DataContext&) = delete;
};
