#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include "flat_map.h"

#include <nlohmann/json.hpp>

inline constexpr size_t MAX_ORDERBOOK_LEVELS = 100;
inline constexpr size_t MAX_HEATMAP_LEVELS = 200;

namespace Terminal {
    enum class Stream : uint32_t {
        Unknown = 0,
        Trades = 1,
        Candles = 2,
        Orderbook = 3,
        Stats = 4,
        Liquidations = 5,
        Volumes = 6,
        Ticker = 7,
        HistoricalCandles = 8,
        HistoricalVolumes = 9,
        HistoricalStats = 10,
        Heatmap = 11,
        LiquidationHeatmap = 12,
        HistoricalHeatmaps = 13,
        Analytics = 14,
        VPINState = 15,
        PositioningState = 16,
        TickVolume = 17,
        OrderbookSnapshots = 18,
        Contagion = 19,
        Pattern = 20,
        // 21 retired: legacy server-side pattern visualization stream.
        Alerts = 22,
        Debug = 23,
        HistoricalLiqHeatmaps = 24,
        VolumeProfile = 26,
        TPO = 27,
        PaperTrading = 28,
        Ticker24h = 29,
        Scanner = 30,
        // 31 reserved for FlowDeviation (liq Field v2 P3, gate-deferred)
        FlowDeviation = 31,
        HistoricalVPIN = 32,
        // Scrub-preview candle batch (replay only): the full replay window's
        // candles for ghost-rendering ahead of the playhead. Distinct id so it
        // can never be dispatched into the main CandleManager as real candles.
        ReplayPreviewCandles = 33,
        // HL ground-truth predictive liq levels (clearinghouseState census,
        // P2e): REAL open positions' liquidation prices, keyed by UNDERLYING
        // ({exchange:"hl", symbol:"BTC"}). Same LiquidationHeatmapUpdate wire
        // shape as LiquidationHeatmap but a distinct id so real-vs-modelled
        // never conflate; est_Nx = REAL leverage tiers, flow_intensity =
        // coverage_frac.
        LiquidationLevels = 34,
        // Admin-scoped live pattern geometry (PatternDetected payload).
        PatternAdmin = 35,
    };

    constexpr std::string_view stream_to_string(Stream s) {
        switch (s) {
        case Stream::Unknown: return "Unknown";
        case Stream::Trades: return "Trades";
        case Stream::Candles: return "Candles";
        case Stream::Orderbook: return "Orderbook";
        case Stream::Stats: return "Stats";
        case Stream::Liquidations: return "Liquidations";
        case Stream::Volumes: return "Volumes";
        case Stream::Ticker: return "Ticker";
        case Stream::HistoricalCandles: return "HistoricalCandles";
        case Stream::HistoricalVolumes: return "HistoricalVolumes";
        case Stream::HistoricalStats: return "HistoricalStats";
        case Stream::Heatmap: return "Heatmap";
        case Stream::LiquidationHeatmap: return "LiquidationHeatmap";
        case Stream::HistoricalHeatmaps: return "HistoricalHeatmaps";
        case Stream::Analytics: return "Analytics";
        case Stream::VPINState: return "VPINState";
        case Stream::PositioningState: return "PositioningState";
        case Stream::TickVolume: return "TickVolume";
        case Stream::OrderbookSnapshots: return "OrderbookSnapshots";
        case Stream::Contagion: return "Contagion";
        case Stream::Pattern: return "Pattern";
        case Stream::Alerts: return "Alerts";
        case Stream::Debug: return "Debug";
        case Stream::HistoricalLiqHeatmaps: return "HistoricalLiqHeatmaps";
        case Stream::VolumeProfile: return "VolumeProfile";
        case Stream::TPO: return "TPO";
        case Stream::PaperTrading: return "PaperTrading";
        case Stream::Ticker24h: return "Ticker24h";
        case Stream::Scanner: return "Scanner";
        case Stream::FlowDeviation: return "FlowDeviation";
        case Stream::HistoricalVPIN: return "HistoricalVPIN";
        case Stream::ReplayPreviewCandles: return "ReplayPreviewCandles";
        case Stream::LiquidationLevels: return "LiquidationLevels";
        case Stream::PatternAdmin: return "PatternAdmin";
        default: return "Unknown";
        }
    }

    std::string create_subscribe_message(
        const std::string& exchange,
        const std::string& symbol,
        Stream stream,
        int64_t timeframe = 0
    );

    std::string create_unsubscribe_message(
        const std::string& exchange,
        const std::string& symbol,
        Stream stream,
        int64_t timeframe = 0
    );

    struct Pair {
        std::string exchange;
        std::string symbol;
    };

    struct WSPayload {
        Pair pair;
        Stream stream;
        int64_t timeframe;
        std::vector<uint8_t> data;
    };

    struct BookEntry {
        double price;
        double size;
    };

    struct Trade {
        double price;
        double qty;
        int64_t timestamp_ms;
        bool is_buy;
    };

    struct Candle {
        int64_t timeframe;
        double open;
        double high;
        double low;
        double close;
        double volume;
        double vbuy;
        double vsell;
        int64_t tbuy;
        int64_t tsell;
        int64_t timestamp_ms;
        bool final;
    };

    struct Candles {
        int64_t timeframe;
        std::vector<std::unique_ptr<Candle>> values;
    };

    struct Volume {
        int64_t timeframe;
        double buy;
        double sell;
        double total;
        int64_t trades;
        double vwap;
        double vwap_buy;
        double vwap_sell;
        double delta;
        double imbalance;
        double cvd;
        double cvd_high;         // Intra-candle CVD max (for candlestick wicks)
        double cvd_low;          // Intra-candle CVD min (for candlestick wicks)
        bool final;
        double taker_buy_ratio;  
        double taker_sell_ratio; 
        int64_t timestamp_ms;
    };

    struct Volumes {
        int64_t timeframe;
        std::vector<std::unique_ptr<Volume>> values;
    };

    struct Stat {
        int64_t timeframe;
        double mark_price;
        double funding;
        bool final;
        double liq_long_volume;
        double liq_short_volume;
        double liq_long_usd;     
        double liq_short_usd;    
        double liq_total_usd;    
        double liq_ratio;        
        int64_t trade_buy;
        int64_t trade_sell;
        int64_t timestamp_ms;
        double open_interest_usd;
        int64_t next_funding_time;
        double oi_open;
        double oi_high;
        double oi_low;
        double oi_close;
    };

    struct Stats {
        int64_t timeframe;
        std::vector<std::unique_ptr<Stat>> values;
    };

    struct Liquidation {
        double price;
        double avg_price;
        double qty;
        bool is_buy;
        int64_t timestamp_ms;
    };

    // Live admin pattern overlay. The server sends exactly two swing-high
    // anchors (first swing and now); both resistance and support are evaluated
    // from their price-per-ms line equations over that shared bounded segment.
    struct PatternOverlay {
        std::string pattern_id;
        std::string type;
        std::string state;
        int64_t timeframe_ms = 0;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
        double upper_slope = 0.0;
        double upper_intercept = 0.0;
        double lower_slope = 0.0;
        double lower_intercept = 0.0;
        double convergence = -1.0;
        int touch_count = 0;
        double span_days = 0.0;

        [[nodiscard]] double resistance_at(int64_t unix_ms) const {
            return upper_slope * static_cast<double>(unix_ms) + upper_intercept;
        }
        [[nodiscard]] double support_at(int64_t unix_ms) const {
            return lower_slope * static_cast<double>(unix_ms) + lower_intercept;
        }
    };

    struct BookTicker {
        double best_bid;
        double best_bid_qty;
        double best_ask;
        double best_ask_qty;
        int64_t update_id;
        int64_t event_time;
        int64_t timestamp_ms;
    };
    
    struct Orderbook {
        FlatMap<double, double> asks;
        FlatMap<double, double, std::greater<>> bids;
        double last_price = 0.0;
        // Set once a depth update actually carries a price. Archives derived
        // from a source with no trade tape (cryptohftdata) send last_price=0 on
        // every update; this lets the book fall back to its own mid for the
        // whole replay instead of rendering 0.00, while a feed that does carry
        // prices takes over permanently on the first one.
        bool source_carries_price = false;
        int64_t last_update_id = 0;
        int64_t previous_update_id = 0;
        int64_t timestamp_ms = 0;
        bool snapshot = false;
        int update_count_since_prune = 0;
        int replay_grace_remaining = 0; // Skip continuity checks after replay OB seed
        // Count of INCREMENTAL depth deltas applied (NOT the seed/snapshot). The
        // replay seed fills the book fast; the live delta stream lags ~10s. This
        // counter distinguishes "seed loaded" (0 deltas) from "depth genuinely
        // streaming" (deltas flowing) — used by ReplayManager::context_primed.
        int64_t delta_updates = 0;
        struct Level {
            double price;
            double size;
            double cumulative;
        };
        [[nodiscard]] std::pair<std::vector<Level>, std::vector<Level>> top_levels(size_t depth = 25) const {
            std::vector<Level> top_asks;
            std::vector<Level> top_bids;
            double cum = 0.0;
            for (const auto& [price, qty] : asks.data()) {
                if (price > last_price) {
                    cum += qty;
                    top_asks.push_back({price, qty, cum});
                    if (top_asks.size() >= depth) break;
                }
            }
            cum = 0.0;
            for (const auto& [price, qty] : bids.data()) {
                if (price < last_price) {
                    cum += qty;
                    top_bids.push_back({price, qty, cum});
                    if (top_bids.size() >= depth) break;
                }
            }
            return {top_bids, top_asks};
        }
        [[nodiscard]] bool is_synchronized() const {
            return snapshot && !asks.empty() && !bids.empty();
        }
    };
    struct GetRange {
        uint32_t stream{};
        int64_t from{};
        int64_t to{};
        int64_t timeframe{};
        Pair pair;
    };
    struct CandleRange {
        Pair pair;
        int64_t timeframe;
        int64_t from;
        int64_t to;
        std::vector<std::unique_ptr<Candle>> candles;
    };
    struct MarkPriceUpdate {
        Pair pair;
        int64_t timestamp_ms{};
        double mark_price{};
        double funding{};
    };
    struct LiquidationUpdate {
        int64_t timestamp_ms;
        bool is_buy;
        double price;
        double size;
        Pair pair;
        std::vector<std::unique_ptr<Liquidation>> liquidations;
    };
    // Liquidation heatmap band (one price level)
    struct LiquidationBand {
        double price_mid;       // Center price of this band
        double obs_long_usd;    // Observed long liquidation $
        double obs_short_usd;   // Observed short liquidation $
        double est_long_usd;    // ML-estimated long liquidation $
        double est_short_usd;   // ML-estimated short liquidation $
        double intensity;       // Combined intensity metric [0..1]
        double reach_prob;      // ML: P(price reaches this band)
        double cascade_prob;    // ML: P(cascade | reached)
        // Per-leverage tier breakdown
        double est_5x_usd = 0.0;
        double est_10x_usd = 0.0;
        double est_25x_usd = 0.0;
        double est_50x_usd = 0.0;
        double est_75x_usd = 0.0;
        double est_100x_usd = 0.0;
        // Stop-loss density (from FlowLiquidityActor estimator)
        double long_stop_density = 0.0;   // Sell stops below price (longs defending) 0-1
        double short_stop_density = 0.0;  // Buy stops above price (shorts defending) 0-1
    };

    // Full liquidation heatmap state for one symbol
    struct LiquidationHeatmapSnapshot {
        int64_t timestamp_ms = 0;
        double mark_price = 0.0;
        double total_long_risk_usd = 0.0;
        double total_short_risk_usd = 0.0;
        double net_risk_bias = 0.0;
        double band_width_pct = 0.0;
        double flow_intensity = 1.0;  // 0.0–1.0: how unusual was flow when snapshot was generated
        // Bayesian leverage uncertainty (Layer 1) — higher = more prior-dominated
        double bayesian_uncertainty = 0.0;
        // Hawkes cascade dynamics (Layer 4)
        double hawkes_branching_ratio = 0.0;  // α/β — <1 subcritical, ~1 critical, >1 supercritical
        double hawkes_severity = 0.0;          // 0-1 normalized cascade severity
        std::vector<LiquidationBand> bands;
    };

    struct HeatmapFullSnapshot {
        std::string mode;
        double tick_size;
        int64_t bucket_size;
        int64_t timestamp;
        std::vector<uint8_t> binary_data;
    };
    struct HeatmapDelta {
        std::string mode;
        double tick_size;
        int64_t bucket_size;
        int64_t timestamp;
        int64_t base_time;
        std::vector<uint8_t> binary_data;
    };
    struct HeatmapDeltas {
        std::vector<HeatmapDelta> values;
    };

    // ═══════════════════════════════════════════════════════════════
    // VPVR — Volume Profile Visible Range
    // ═══════════════════════════════════════════════════════════════
    struct VPVRLevel {
        double price = 0;
        double buy_volume = 0;
        double sell_volume = 0;
        double total_volume = 0;
        int64_t trade_count = 0;
        double volume_pct = 0;
        bool is_poc = false;
        bool in_value_area = false;
    };

    struct VPVRProfile {
        int64_t start_time = 0;
        int64_t end_time = 0;
        std::vector<VPVRLevel> levels;
        double poc = 0, vah = 0, val = 0;
        double total_volume = 0;
        double value_area_volume = 0;
        int32_t poc_index = -1;
        bool valid = false;
    };

    // ═══════════════════════════════════════════════════════════════
    // TPO — Time Price Opportunity / Market Profile
    // ═══════════════════════════════════════════════════════════════
    struct TPOBlock {
        double price = 0;
        int32_t block_count = 0;
        bool is_poc = false;
        bool is_value_area = false;
        bool is_single_print = false;
        bool is_initial_balance = false;
    };

    struct TPOPeriod {
        int64_t start_time = 0;
        int64_t end_time = 0;
        double high = 0;
        double low = 0;
        std::string label;
    };

    struct TPOSession {
        int64_t session_start = 0;
        int64_t session_end = 0;
        std::vector<TPOBlock> blocks;
        double poc = 0, vah = 0, val = 0;
        double ib_high = 0, ib_low = 0;
        bool has_poor_high = false;
        bool has_poor_low = false;
        double session_high = 0, session_low = 0;
        double tick_per_row = 0;
        int32_t total_blocks = 0;
        std::vector<TPOPeriod> periods;
    };
}
