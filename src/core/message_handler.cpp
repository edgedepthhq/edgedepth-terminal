#include "message_handler.h"
#include "message_parser.h"
#include "stream_presence.h"
#include "performance_tracker.h"
#include "data_thread.h"
#include <zstd.h>
#include <google/protobuf/arena.h>

#include "heatmap_manager.h"
#include "footprint_manager.h"
#include "paper_trading_manager.h"
#include "ticker_manager.h"
#include "scanner_manager.h"
#include "preview_candle_store.h"
#include <cmath>

// ── Static member initialization ────────────────────────────────────────────
int64_t MessageHandler::last_timestamp_ms = 0;

// ── Reusable arena for protobuf message allocation ──────────────────────────
// Instead of stack-allocating pb::Trade, pb::BookUpdate, etc. per message
// (which triggers malloc for internal repeated fields and strings), we reuse
// a single arena. Reset() reclaims all memory at once - zero per-object frees.
// ArenaOptions configures a 64KB initial block to avoid early growth.
static google::protobuf::ArenaOptions make_arena_options() {
    google::protobuf::ArenaOptions opts;
    opts.initial_block_size = 64 * 1024;  // 64KB - covers most messages
    opts.max_block_size = 256 * 1024;     // 256KB cap
    return opts;
}
static thread_local google::protobuf::ArenaOptions s_arena_opts = make_arena_options();
static thread_local google::protobuf::Arena s_arena(s_arena_opts);

void MessageHandler::handle_message(const std::string& data, const MessageContext& ctx) {
    if (data.empty()) return;
    // Reset arena - reclaims all memory from previous message at once (zero per-object frees)
    s_arena.Reset();
    const auto decompress_result = MessageParser::decompress_zstd(data);
    if (!decompress_result.success) {
        return;
    }
    const auto parse_result = MessageParser::parse_payload(decompress_result.data);
    if (!parse_result.success) {
        return;
    }
    route_message(parse_result.payload, ctx);
}

void MessageHandler::route_parsed(const pb::WSPayload& ws_payload, const MessageContext& ctx) {
    // Arena reset for inner proto allocations
    s_arena.Reset();
    route_message(ws_payload, ctx);
}

void MessageHandler::route_message(const pb::WSPayload& ws_payload, const MessageContext& ctx) {
    // PERF_TIMER("route_message");

    // Set timestamp from the outer WSPayload envelope. The backend sets event_time_ms
    // on every message during replay (from the NATS Binance-Time header). This replaces
    // the old hack of extracting timestamps from individual inner protos (Trade/BookUpdate).
    if (ws_payload.event_time_ms() != 0) {
        last_timestamp_ms = ws_payload.event_time_ms();
    }

    // One line covers every stream: panels ask StreamPresence whether their
    // driving stream has ever produced a frame (see stream_presence.h).
    StreamPresence::instance().note_frame(static_cast<uint32_t>(ws_payload.stream()));

    Terminal::Pair pair{
        ws_payload.pair().exchange(),
        ws_payload.pair().symbol()
    };
    const auto& inner_data = ws_payload.data().data();
    size_t inner_size = ws_payload.data().size();
    // Dispatch based on stream type
    switch (ws_payload.stream()) {
        case pb::Stream::STREAM_TRADES:
            handle_trade_message(pair, inner_data, inner_size, ctx.streams, ctx.orderbooks);
            break;
        case pb::Stream::STREAM_CANDLES:
            handle_candle_message(pair, ws_payload.timeframe(),
                                 inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_ORDERBOOK:
        case pb::Stream::STREAM_ORDERBOOK_SNAPSHOTS:  // OB seed from replay_seeds uses this stream type
            handle_orderbook_message(pair, inner_data, inner_size, ctx.orderbooks);
            break;
        case pb::Stream::STREAM_TICKER:
            handle_ticker_message(pair, inner_data, inner_size, ctx.orderbooks);
            break;
        case pb::Stream::STREAM_VOLUMES:
            handle_volumes_message(pair, ws_payload.timeframe(),
                                  inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_STATS:
            handle_stats_message(pair, ws_payload.timeframe(),
                                inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_HISTORICAL_CANDLES:
            handle_historical_candles(pair, ws_payload.timeframe(),
                                     inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_REPLAY_PREVIEW_CANDLES:
            // Ghost scrub-preview batch - replay contexts only (ctx.preview is
            // null in live mode and the store write is a main-thread concern,
            // so queue when a dispatch queue is installed, heatmap-style).
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[captured, tf = ws_payload.timeframe(),
                                           pv = ctx.preview](StreamManager&) {
                    handle_replay_preview_candles(tf, captured.data(), captured.size(), pv);
                }});
            } else {
                handle_replay_preview_candles(ws_payload.timeframe(),
                                              inner_data, inner_size, ctx.preview);
            }
            break;
        case pb::Stream::STREAM_HISTORICAL_VOLUMES:
            handle_historical_volumes(pair, ws_payload.timeframe(),
                                     inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_HISTORICAL_STATS:
            handle_historical_stats(pair, ws_payload.timeframe(),
                                   inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_LIQUIDATIONS:
            handle_liquidation_message(pair, inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_HISTORICAL_HEATMAPS:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, hm = ctx.heatmaps](StreamManager&) {
                    handle_historical_heatmap_batch(pair, captured.data(), captured.size(), hm);
                }});
            } else {
                handle_historical_heatmap_batch(pair, inner_data, inner_size, ctx.heatmaps);
            }
            break;
        case pb::Stream::STREAM_HEATMAP: {
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, hm = ctx.heatmaps](StreamManager&) {
                    std::string inner_str(captured);
                    const auto decompress_result = MessageParser::decompress_zstd(inner_str);
                    if (!decompress_result.success) {
                        return;
                    }
                    pb::HeatmapSnapshot snapshot_pb;
                    if (!snapshot_pb.ParseFromArray(decompress_result.data.data(), static_cast<int>(decompress_result.data.size()))) {
                        return;
                    }
                    hm->finalize_snapshot(pair, snapshot_pb);
                }});
            } else {
                std::string inner_str(static_cast<const char*>(inner_data), inner_size);
                const auto decompress_result = MessageParser::decompress_zstd(inner_str);
                if (!decompress_result.success) {
                    break;
                }
                pb::HeatmapSnapshot snapshot_pb;
                if (!snapshot_pb.ParseFromArray(decompress_result.data.data(), static_cast<int>(decompress_result.data.size()))) {
                    break;
                }
                ctx.heatmaps->finalize_snapshot(pair, snapshot_pb);
            }
            break;
        }
        case pb::Stream::STREAM_LIQUIDATION_HEATMAP:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, lhm = ctx.liq_heatmaps](StreamManager&) {
                    handle_liquidation_heatmap_message(pair, captured.data(), captured.size(), lhm);
                }});
            } else {
                handle_liquidation_heatmap_message(pair, inner_data, inner_size, ctx.liq_heatmaps);
            }
            break;
        case pb::Stream::STREAM_LIQUIDATION_LEVELS:
            // HL census (P2e): same payload type as the modelled heatmap, but a
            // distinct stream id → the manager's separate census store. Same
            // main-thread queue rule as the modelled snapshot.
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, lhm = ctx.liq_heatmaps](StreamManager&) {
                    handle_liquidation_levels_message(pair, captured.data(), captured.size(), lhm);
                }});
            } else {
                handle_liquidation_levels_message(pair, inner_data, inner_size, ctx.liq_heatmaps);
            }
            break;
        case pb::Stream::STREAM_PATTERN_ADMIN:
            handle_pattern_admin_message(pair, inner_data, inner_size, ctx.streams);
            break;
        case pb::Stream::STREAM_HISTORICAL_LIQ_HEATMAPS:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, lhm = ctx.liq_heatmaps](StreamManager&) {
                    handle_historical_liq_heatmap_batch(pair, captured.data(), captured.size(), lhm);
                }});
            } else {
                handle_historical_liq_heatmap_batch(pair, inner_data, inner_size, ctx.liq_heatmaps);
            }
            break;
        case pb::Stream::STREAM_DEBUG:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[captured, dm = ctx.debug](StreamManager&) {
                    // SafeLogger::log("Stream DEBUG received ");
                    handle_debug_message(captured.data(), captured.size(), dm);
                }});
            } else {
                // SafeLogger::log("Stream DEBUG received ");
                handle_debug_message(inner_data, inner_size, ctx.debug);
            }
            break;
        case pb::Stream::STREAM_VOLUME_PROFILE:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, vm = ctx.vpvr](StreamManager&) {
                    handle_volume_profile_message(pair, captured.data(), captured.size(), vm);
                }});
            } else {
                handle_volume_profile_message(pair, inner_data, inner_size, ctx.vpvr);
            }
            break;
        case pb::Stream::STREAM_TPO:
            if (ctx.dispatch_queue) {
                std::string captured(static_cast<const char*>(inner_data), inner_size);
                ctx.dispatch_queue->push({[pair, captured, tm = ctx.tpo](StreamManager&) {
                    handle_tpo_message(pair, captured.data(), captured.size(), tm);
                }});
            } else {
                handle_tpo_message(pair, inner_data, inner_size, ctx.tpo);
            }
            break;
        case pb::Stream::STREAM_TICK_VOLUME:
            if (ctx.footprint) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    ctx.dispatch_queue->push({[pair, captured, fm = ctx.footprint](StreamManager&) {
                        handle_tick_volume_message(pair, captured.data(), captured.size(), fm);
                    }});
                } else {
                    handle_tick_volume_message(pair, inner_data, inner_size, ctx.footprint);
                }
            }
            break;
        case pb::Stream::STREAM_PAPER_TRADING:
            if (ctx.paper_trading) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    ctx.dispatch_queue->push({[captured, ptm = ctx.paper_trading](StreamManager&) {
                        handle_paper_trading_message(captured.data(), captured.size(), ptm);
                    }});
                } else {
                    handle_paper_trading_message(inner_data, inner_size, ctx.paper_trading);
                }
            }
            break;
        case pb::Stream::STREAM_TICKER24H:
            if (ctx.ticker) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    ctx.dispatch_queue->push({[ex = pair.exchange, captured, tm = ctx.ticker](StreamManager&) {
                        handle_ticker24h_message(ex, captured.data(), captured.size(), tm);
                    }});
                } else {
                    handle_ticker24h_message(pair.exchange, inner_data, inner_size, ctx.ticker);
                }
            }
            break;
        case pb::Stream::STREAM_SCANNER:
            if (ctx.scanner) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    ctx.dispatch_queue->push({[ex = pair.exchange, captured, sm = ctx.scanner](StreamManager&) {
                        handle_scanner_message(ex, captured.data(), captured.size(), sm);
                    }});
                } else {
                    handle_scanner_message(pair.exchange, inner_data, inner_size, ctx.scanner);
                }
            }
            break;
        // VPIN (Indicators V1 S1b): finalized volume-bucket prints from the
        // live STATE_VPIN stream, the replay fetchVPINTimeline seed, or an
        // archive bundle's vpin.parquet. Routed to the SeriesCache on the
        // main thread (same queue rule as heatmap/pattern/debug - the cache
        // is written main-thread-only, read during render lock-free).
        case pb::Stream::STREAM_VPIN_STATE:
            if (ctx.series) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    Terminal::Pair pair_copy = pair;
                    ctx.dispatch_queue->push({[captured, pair_copy, sm = ctx.series](StreamManager&) {
                        handle_vpin_state_message(pair_copy, captured.data(), captured.size(), sm);
                    }});
                } else {
                    handle_vpin_state_message(pair, inner_data, inner_size, ctx.series);
                }
            }
            break;
        case pb::Stream::STREAM_HISTORICAL_VPIN:
            if (ctx.series) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    Terminal::Pair pair_copy = pair;
                    ctx.dispatch_queue->push({[captured, pair_copy, sm = ctx.series](StreamManager&) {
                        handle_historical_vpin_message(pair_copy, captured.data(), captured.size(), sm);
                    }});
                } else {
                    handle_historical_vpin_message(pair, inner_data, inner_size, ctx.series);
                }
            }
            break;
        // Positioning state (STREAM_POSITIONING_STATE=16): a clean single-type
        // PositioningStateUpdate carrying CVD, taker ratios, long/short account,
        // OI + 24h change, long/short liq USD, smart-money bias, cascade risk.
        // Routed to the AnalyticsManager on the main thread (same queue rule as
        // VPIN: parse + apply run main-thread, read lock-free at render).
        case pb::Stream::STREAM_POSITIONING_STATE:
            if (ctx.analytics) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    Terminal::Pair pair_copy = pair;
                    ctx.dispatch_queue->push({[captured, pair_copy, am = ctx.analytics](StreamManager&) {
                        handle_positioning_state_message(pair_copy, captured.data(), captured.size(), am);
                    }});
                } else {
                    handle_positioning_state_message(pair, inner_data, inner_size, ctx.analytics);
                }
            }
            break;
        // Contagion (STREAM_CONTAGION=19): market-wide ContagionSnapshot (market
        // stress + regime). Replay-only today (no live server StreamConfig), so
        // this populates in replay and stays redacted live.
        case pb::Stream::STREAM_CONTAGION:
            if (ctx.analytics) {
                if (ctx.dispatch_queue) {
                    std::string captured(static_cast<const char*>(inner_data), inner_size);
                    ctx.dispatch_queue->push({[captured, am = ctx.analytics](StreamManager&) {
                        handle_contagion_message(captured.data(), captured.size(), am);
                    }});
                } else {
                    handle_contagion_message(inner_data, inner_size, ctx.analytics);
                }
            }
            break;
        // STREAM_ANALYTICS stays suppressed deliberately: it interleaves several
        // message types (cfti/hmm/iceberg/smartmoney/orderbook/liq-cascade) on one
        // stream id with no discriminator - disambiguation is the S7 Toxicity-v2
        // job (Tier 2). ALERTS are not consumed on this client.
        case pb::Stream::STREAM_ANALYTICS:
        case pb::Stream::STREAM_ALERTS:
            break;
        default:
            if (ctx.dispatch_queue) {
                int stream_type = static_cast<int>(ws_payload.stream());
                ctx.dispatch_queue->push({[stream_type](StreamManager&) {
                }});
            } else {
            }
            break;
    }
}

void MessageHandler::handle_trade_message(const Terminal::Pair& pair, const void* data, size_t size,
                                          StreamManager* stream_mgr, OrderbookManager* orderbook_mgr) {
    auto* trade_pb = google::protobuf::Arena::CreateMessage<pb::Trade>(&s_arena);
    if (!trade_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    // Feed the tape to the book so a depth source that carries no price of its
    // own tracks the TRADED price, exactly as the live actor does. No-op for
    // feeds that do carry one.
    if (orderbook_mgr) {
        orderbook_mgr->note_trade_price(pair, trade_pb->price());
    }
    handle_trade(pair, *trade_pb, stream_mgr);
}

void MessageHandler::handle_candle_message(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    auto* candle_pb = google::protobuf::Arena::CreateMessage<pb::Candle>(&s_arena);
    if (!candle_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    handle_candle(pair, timeframe, *candle_pb, stream_mgr);
}

void MessageHandler::handle_orderbook_message(const Terminal::Pair& pair, const void* data, size_t size, OrderbookManager* orderbook_mgr) {
    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::BookUpdate>(&s_arena);
    if (update_pb->ParseFromArray(data, static_cast<int>(size))) {
        if (update_pb->snapshot()) {
            orderbook_mgr->apply_orderbook_snapshot_from_pb(pair, *update_pb);
            return;
        }
        orderbook_mgr->apply_book_update_from_pb(pair, *update_pb);
        return;
    }
}

void MessageHandler::handle_ticker_message(const Terminal::Pair& pair, const void* data, size_t size, OrderbookManager* orderbook_mgr) {
    auto* ticker_pb = google::protobuf::Arena::CreateMessage<pb::BookTickerUpdate>(&s_arena);
    if (!ticker_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    orderbook_mgr->apply_book_ticker_from_pb(pair, *ticker_pb);
}

void MessageHandler::handle_volumes_message(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    // Try batch first - only accept if it has values (same proto3 wire-type issue as stats)
    auto* volumes_pb = google::protobuf::Arena::CreateMessage<pb::Volumes>(&s_arena);
    if (volumes_pb->ParseFromArray(data, static_cast<int>(size)) && volumes_pb->values_size() > 0) {
        handle_volumes_batch(pair, timeframe, *volumes_pb, stream_mgr);
        return;
    }
    // Try single volume
    auto* volume_pb = google::protobuf::Arena::CreateMessage<pb::Volume>(&s_arena);
    if (volume_pb->ParseFromArray(data, static_cast<int>(size))) {
        handle_volume(pair, timeframe, *volume_pb, stream_mgr);
        return;
    }
}

void MessageHandler::handle_stats_message(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    // Try batch first - but only accept if it actually contains values.
    // A single Stat can be "successfully" parsed as Stats with zero values
    // due to protobuf wire type mismatches being silently skipped in proto3.
    auto* stats_pb = google::protobuf::Arena::CreateMessage<pb::Stats>(&s_arena);
    if (stats_pb->ParseFromArray(data, static_cast<int>(size)) && stats_pb->values_size() > 0) {
        handle_stats_batch(pair, timeframe, *stats_pb, stream_mgr);
        return;
    }
    // Try single stat
    auto* stat_pb = google::protobuf::Arena::CreateMessage<pb::Stat>(&s_arena);
    if (stat_pb->ParseFromArray(data, static_cast<int>(size))) {
        handle_stat(pair, timeframe, *stat_pb, stream_mgr);
        return;
    }
}

void MessageHandler::handle_historical_candles(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    auto* candles_pb = google::protobuf::Arena::CreateMessage<pb::Candles>(&s_arena);
    if (!candles_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    handle_candles_batch(pair, timeframe, *candles_pb, stream_mgr);
}

void MessageHandler::handle_replay_preview_candles(int64_t timeframe,
                                                   const void* data, size_t size,
                                                   PreviewCandleStore* preview) {
    if (!preview) return;  // live mode or store torn down - batch is moot
    auto* candles_pb = google::protobuf::Arena::CreateMessage<pb::Candles>(&s_arena);
    if (!candles_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    // The envelope timeframe is the SERVED tf in seconds (the backend may have
    // snapped the request up to fit its batch cap); inner Candles.timeframe
    // matches it. Prefer the inner value when present, envelope otherwise.
    int64_t tf_sec = candles_pb->timeframe() > 0 ? candles_pb->timeframe() : timeframe;
    std::vector<Terminal::Candle> candles;
    candles.reserve(candles_pb->values_size());
    for (const auto& candle_pb : candles_pb->values()) {
        Terminal::Candle candle{};
        candle.timestamp_ms = candle_pb.timestamp_ms();
        candle.timeframe = candle_pb.timeframe();
        candle.open = candle_pb.open();
        candle.high = candle_pb.high();
        candle.low = candle_pb.low();
        candle.close = candle_pb.close();
        candle.volume = candle_pb.volume();
        candle.vbuy = candle_pb.vbuy();
        candle.vsell = candle_pb.vsell();
        candle.tbuy = candle_pb.tbuy();
        candle.tsell = candle_pb.tsell();
        candle.final = candle_pb.final();
        candles.push_back(candle);
    }
    preview->apply_batch(tf_sec, std::move(candles));
}

void MessageHandler::handle_historical_volumes(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    auto* volumes_pb = google::protobuf::Arena::CreateMessage<pb::Volumes>(&s_arena);
    if (!volumes_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    handle_volumes_batch(pair, timeframe, *volumes_pb, stream_mgr);
}

void MessageHandler::handle_historical_stats(const Terminal::Pair& pair, int64_t timeframe, const void* data, size_t size, StreamManager* stream_mgr) {
    auto* stats_pb = google::protobuf::Arena::CreateMessage<pb::Stats>(&s_arena);
    if (!stats_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    handle_stats_batch(pair, timeframe, *stats_pb, stream_mgr);
}

void MessageHandler::handle_liquidation_message(const Terminal::Pair& pair, const void* data, size_t size, StreamManager* stream_mgr) {
    auto* liq_pb = google::protobuf::Arena::CreateMessage<pb::Liquidation>(&s_arena);
    if (!liq_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    handle_liquidation(pair, *liq_pb, stream_mgr);
}

void MessageHandler::handle_pattern_admin_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    StreamManager* stream_mgr) {
    if (!stream_mgr) return;

    auto* pattern_pb = google::protobuf::Arena::CreateMessage<pb::PatternDetected>(&s_arena);
    if (!pattern_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    Terminal::PatternOverlay pattern;
    pattern.pattern_id = pattern_pb->pattern_id();
    pattern.type = pattern_pb->type();
    pattern.state = pattern_pb->state();
    pattern.timeframe_ms = pattern_pb->timeframe();
    if (pattern.timeframe_ms <= 0) return;
    pattern.touch_count = std::max(0, pattern_pb->touch_count());
    pattern.span_days = std::max(0.0, pattern_pb->duration_hours() / 24.0);

    // Terminal states still need dispatching even when geometry is absent so
    // the chart can remove the previous forming candidate immediately.
    if (pattern.state == "forming") {
        if (!pattern_pb->has_trendline() ||
            pattern_pb->trendline().swing_highs_size() != 2) {
            return;
        }
        const auto& trendline = pattern_pb->trendline();
        const int64_t first_ms = trendline.swing_highs(0).unix_ms();
        const int64_t now_ms = trendline.swing_highs(1).unix_ms();
        if (first_ms <= 0 || now_ms <= first_ms) {
            return;
        }

        pattern.start_ms = first_ms;
        pattern.end_ms = now_ms;
        pattern.upper_slope = trendline.upper_slope();
        pattern.upper_intercept = trendline.upper_intercept();
        pattern.lower_slope = trendline.lower_slope();
        pattern.lower_intercept = trendline.lower_intercept();
        pattern.convergence = trendline.convergence();

        const double resistance_first = pattern.resistance_at(first_ms);
        const double resistance_now = pattern.resistance_at(now_ms);
        const double support_first = pattern.support_at(first_ms);
        const double support_now = pattern.support_at(now_ms);
        if (!std::isfinite(resistance_first) || !std::isfinite(resistance_now) ||
            !std::isfinite(support_first) || !std::isfinite(support_now) ||
            resistance_first <= 0.0 || resistance_now <= 0.0 ||
            support_first <= 0.0 || support_now <= 0.0) {
            return;
        }
    }

    const StreamKey key{pair, Terminal::Stream::PatternAdmin, 0};
    stream_mgr->dispatch_pattern(key, pattern);
}

// =============================================================================
// Conversion helpers (protobuf -> Terminal types)
// =============================================================================

void MessageHandler::handle_trade(const Terminal::Pair& pair, const pb::Trade& trade_pb, StreamManager* stream_mgr) {
    Terminal::Trade trade{};
    trade.price = trade_pb.price();
    trade.qty = trade_pb.qty();
    trade.is_buy = trade_pb.is_buy();
    trade.timestamp_ms = trade_pb.timestamp_ms();
    const StreamKey key{pair, Terminal::Stream::Trades, 0};
    stream_mgr->dispatch_trade(key, trade);
}

void MessageHandler::handle_debug_message(const void* data, size_t size,DebugManager* debug_mgr) {
    if (!debug_mgr) return;
    debug_mgr->handle_batch(data, size);
}

void MessageHandler::handle_candle(const Terminal::Pair& pair, int64_t timeframe, const pb::Candle& candle_pb, StreamManager* stream_mgr) {
    Terminal::Candle candle{};
    candle.timestamp_ms = candle_pb.timestamp_ms();
    candle.timeframe = candle_pb.timeframe();
    candle.open = candle_pb.open();
    candle.high = candle_pb.high();
    candle.low = candle_pb.low();
    candle.close = candle_pb.close();
    candle.volume = candle_pb.volume();
    candle.vbuy = candle_pb.vbuy();
    candle.vsell = candle_pb.vsell();
    candle.tbuy = candle_pb.tbuy();
    candle.tsell = candle_pb.tsell();
    candle.final = candle_pb.final();

    const StreamKey key{pair, Terminal::Stream::Candles, timeframe};
    stream_mgr->dispatch_candle(key, candle);
}

void MessageHandler::handle_candles_batch(const Terminal::Pair& pair, int64_t timeframe, const pb::Candles& candles_pb, StreamManager* stream_mgr) {
    const StreamKey key{pair, Terminal::Stream::Candles, timeframe};
    std::vector<Terminal::Candle> candles;
    candles.reserve(candles_pb.values_size());
    for (const auto& candle_pb : candles_pb.values()) {
        Terminal::Candle candle;
        candle.timestamp_ms = candle_pb.timestamp_ms();
        candle.timeframe = candle_pb.timeframe();
        candle.open = candle_pb.open();
        candle.high = candle_pb.high();
        candle.low = candle_pb.low();
        candle.close = candle_pb.close();
        candle.volume = candle_pb.volume();
        candle.vbuy = candle_pb.vbuy();
        candle.vsell = candle_pb.vsell();
        candle.tbuy = candle_pb.tbuy();
        candle.tsell = candle_pb.tsell();
        candle.final = candle_pb.final();

        candles.push_back(candle);
    }
    stream_mgr->dispatch_candles(key, candles);
}

void MessageHandler::handle_volume(const Terminal::Pair& pair, int64_t timeframe, const pb::Volume& volume_pb, StreamManager* stream_mgr) {
    Terminal::Volume volume;
    volume.timestamp_ms = volume_pb.timestamp_ms();
    volume.timeframe = volume_pb.timeframe();
    volume.buy = volume_pb.buy();
    volume.sell = volume_pb.sell();
    volume.total = volume_pb.total();
    volume.trades = volume_pb.trades();
    volume.vwap = volume_pb.vwap();
    volume.vwap_buy = volume_pb.vwap_buy();
    volume.vwap_sell = volume_pb.vwap_sell();
    volume.delta = volume_pb.delta();
    volume.imbalance = volume_pb.imbalance();
    volume.cvd = volume_pb.cvd();
    volume.cvd_high = volume_pb.cvd_high();
    volume.cvd_low = volume_pb.cvd_low();
    volume.final = volume_pb.final();
    volume.taker_buy_ratio = volume_pb.taker_buy_ratio();
    volume.taker_sell_ratio = volume_pb.taker_sell_ratio();

    StreamKey key{pair, Terminal::Stream::Volumes, timeframe};
    stream_mgr->dispatch_volume(key, volume);
}

void MessageHandler::handle_volumes_batch(const Terminal::Pair& pair, int64_t timeframe, const pb::Volumes& volumes_pb, StreamManager* stream_mgr) {
    StreamKey key{pair, Terminal::Stream::Volumes, timeframe};

    for (const auto& volume_pb : volumes_pb.values()) {
        Terminal::Volume volume;
        volume.timestamp_ms = volume_pb.timestamp_ms();
        volume.timeframe = volume_pb.timeframe();
        volume.buy = volume_pb.buy();
        volume.sell = volume_pb.sell();
        volume.total = volume_pb.total();
        volume.trades = volume_pb.trades();
        volume.vwap = volume_pb.vwap();
        volume.vwap_buy = volume_pb.vwap_buy();
        volume.vwap_sell = volume_pb.vwap_sell();
        volume.delta = volume_pb.delta();
        volume.imbalance = volume_pb.imbalance();
        volume.cvd = volume_pb.cvd();
        volume.cvd_high = volume_pb.cvd_high();
        volume.cvd_low = volume_pb.cvd_low();
        volume.final = volume_pb.final();
        volume.taker_buy_ratio = volume_pb.taker_buy_ratio();
        volume.taker_sell_ratio = volume_pb.taker_sell_ratio();

        stream_mgr->dispatch_volume(key, volume);
    }
}

void MessageHandler::handle_stat(const Terminal::Pair& pair, int64_t timeframe, const pb::Stat& stat_pb, StreamManager* stream_mgr) {
    Terminal::Stat stat;
    stat.mark_price = stat_pb.mark_price();
    stat.funding = stat_pb.funding();
    stat.liq_long_volume = stat_pb.liq_long_volume();
    stat.liq_short_volume = stat_pb.liq_short_volume();
    stat.liq_long_usd = stat_pb.liq_long_usd();
    stat.liq_short_usd = stat_pb.liq_short_usd();
    stat.liq_total_usd = stat_pb.liq_total_usd();
    stat.liq_ratio = stat_pb.liq_ratio();
    stat.trade_buy = stat_pb.trade_buy();
    stat.trade_sell = stat_pb.trade_sell();
    stat.timestamp_ms = stat_pb.timestamp_ms();
    stat.timeframe = stat_pb.timeframe();
    stat.final = stat_pb.final();
    stat.open_interest_usd = stat_pb.open_interest_usd();
    stat.next_funding_time = stat_pb.next_funding_time();
    stat.oi_open = stat_pb.oi_open();
    stat.oi_high = stat_pb.oi_high();
    stat.oi_low = stat_pb.oi_low();
    stat.oi_close = stat_pb.oi_close();

    StreamKey key{pair, Terminal::Stream::Stats, timeframe};
    stream_mgr->dispatch_stat(key, stat);
}

void MessageHandler::handle_stats_batch(const Terminal::Pair& pair, int64_t timeframe, const pb::Stats& stats_pb, StreamManager* stream_mgr) {
    StreamKey key{pair, Terminal::Stream::Stats, timeframe};
    for (const auto& stat_pb : stats_pb.values()) {
        Terminal::Stat stat;
        stat.mark_price = stat_pb.mark_price();
        stat.funding = stat_pb.funding();
        stat.liq_long_volume = stat_pb.liq_long_volume();
        stat.liq_short_volume = stat_pb.liq_short_volume();
        stat.liq_long_usd = stat_pb.liq_long_usd();
        stat.liq_short_usd = stat_pb.liq_short_usd();
        stat.liq_total_usd = stat_pb.liq_total_usd();
        stat.liq_ratio = stat_pb.liq_ratio();
        stat.trade_buy = stat_pb.trade_buy();
        stat.trade_sell = stat_pb.trade_sell();
        stat.timestamp_ms = stat_pb.timestamp_ms();
        stat.timeframe = stat_pb.timeframe();
        stat.final = stat_pb.final();
        stat.open_interest_usd = stat_pb.open_interest_usd();
        stat.next_funding_time = stat_pb.next_funding_time();
        stat.oi_open = stat_pb.oi_open();
        stat.oi_high = stat_pb.oi_high();
        stat.oi_low = stat_pb.oi_low();
        stat.oi_close = stat_pb.oi_close();
        stream_mgr->dispatch_stat(key, stat);
    }
}

void MessageHandler::handle_liquidation(const Terminal::Pair& pair, const pb::Liquidation& liq_pb, StreamManager* stream_mgr) {
    Terminal::Liquidation liq{};
    liq.price = liq_pb.price();
    liq.avg_price = liq_pb.avg_price();
    liq.qty = liq_pb.qty();
    liq.is_buy = liq_pb.is_buy();
    liq.timestamp_ms = liq_pb.timestamp_ms();
    const StreamKey key{pair, Terminal::Stream::Liquidations, 0};
    stream_mgr->dispatch_liquidation(key, liq);
}

void MessageHandler::handle_liquidation_heatmap_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    LiquidationHeatmapManager* liq_heatmap_mgr)
{
    if (!liq_heatmap_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::LiquidationHeatmapUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    liq_heatmap_mgr->apply_update(pair, *update_pb);
}

void MessageHandler::handle_liquidation_levels_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    LiquidationHeatmapManager* liq_heatmap_mgr)
{
    if (!liq_heatmap_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::LiquidationHeatmapUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    // The census store is keyed by UNDERLYING ({"hl","BTC"}), which is what the chart
    // subscribes to - but the ENVELOPE pair is the venue pair the frame arrived under.
    // Live those agree (the subject is liquidation_levels.hl.BTC); in REPLAY they do
    // not: an archived binancef/btcusdt event carries census frames stamped with the
    // event's own pair, which would file real HL levels under a key nothing reads and
    // render an empty layer that looks perfectly healthy upstream. The payload's own
    // Pair is authoritative and set by both the live publisher and the archive bake,
    // so prefer it and fall back to the envelope only when absent.
    Terminal::Pair key = pair;
    if (update_pb->has_pair() && !update_pb->pair().symbol().empty()) {
        key = Terminal::Pair{update_pb->pair().exchange(), update_pb->pair().symbol()};
    }
    liq_heatmap_mgr->apply_census_update(key, *update_pb);
}

void MessageHandler::handle_historical_liq_heatmap_batch(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    LiquidationHeatmapManager* liq_heatmap_mgr)
{
    if (!liq_heatmap_mgr) return;

    auto* heatmap_batch_pb = google::protobuf::Arena::CreateMessage<pb::HeatmapSnapshotBatch>(&s_arena);
    if (!heatmap_batch_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    const int total = heatmap_batch_pb->snapshot_blobs_size();
    int applied = 0;

    for (int i = 0; i < total; i++) {
        const std::string& blob = heatmap_batch_pb->snapshot_blobs(i);

        // Blobs are marshaled LiquidationHeatmapUpdate (wire format, NOT compressed)
        auto* inner_update = google::protobuf::Arena::CreateMessage<pb::LiquidationHeatmapUpdate>(&s_arena);
        if (!inner_update->ParseFromArray(blob.data(), static_cast<int>(blob.size()))) {
            continue;
        }

        liq_heatmap_mgr->apply_timeline_snapshot(pair, *inner_update);
        applied++;
    }

    // Force grid rebuild so newly loaded liq heatmap data becomes visible
    liq_heatmap_mgr->mark_timeline_dirty(pair);
}

void MessageHandler::handle_heatmap_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb, HeatmapManager* heatmap_mgr) {
    heatmap_mgr->apply_snapshot(pair, snapshot_pb);
}

void MessageHandler::handle_historical_heatmap_batch(const Terminal::Pair &pair, const void *data, size_t size, HeatmapManager *heatmap_mgr) {
    if (!heatmap_mgr) {
        return;
    }
    google::protobuf::Arena protobuf_arena;
    auto* batch_pb = google::protobuf::Arena::CreateMessage<pb::HeatmapSnapshotBatch>(&protobuf_arena);
    if (!batch_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    const int total_snapshots = batch_pb->snapshot_blobs_size();
    std::vector<int64_t> snapshot_times;
    for (int i = 0; i < total_snapshots; i++) {
        const std::string& compressed_blob = batch_pb->snapshot_blobs(i);
        auto decompress_result = MessageParser::decompress_zstd(compressed_blob);
        if (!decompress_result.success) {
            continue;
        }
        auto* snapshot_pb = google::protobuf::Arena::CreateMessage<pb::HeatmapSnapshot>(&protobuf_arena);
        if (!snapshot_pb->ParseFromArray(
            decompress_result.data.data(),
            static_cast<int>(decompress_result.data.size()))) {
            continue;
            }
        snapshot_times.push_back(snapshot_pb->timestamp_ms());
        handle_heatmap_snapshot(pair, *snapshot_pb, heatmap_mgr);
    }

    bool sorted = std::is_sorted(snapshot_times.begin(), snapshot_times.end());

    if (!sorted) {
        for (int i = 0; i < std::min(10, (int)snapshot_times.size()); i++) {
        }
    }

    // Force grid rebuild so newly loaded historical data becomes visible.
    // process_snapshot() doesn't set grid_dirty_ for out-of-viewport data,
    // so after a batch load we need to explicitly trigger a rebuild.
    heatmap_mgr->mark_dirty(pair);
}

// ── VPVR (Volume Profile Visible Range) ─────────────────────────────────────
void MessageHandler::handle_volume_profile_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    VolumeProfileManager* vpvr_mgr)
{
    if (!vpvr_mgr) return;

    auto* resp_pb = google::protobuf::Arena::CreateMessage<pb::VolumeProfileResponse>(&s_arena);
    if (!resp_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    vpvr_mgr->on_profile_response(pair.symbol, *resp_pb);
}

// ── TPO (Time Price Opportunity / Market Profile) ───────────────────────────
// TPO is now computed client-side from candle data. No backend messages needed.
void MessageHandler::handle_tpo_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    TPOManager* tpo_mgr)
{
    // No-op: TPO sessions are built client-side in TPOManager::build_sessions()
    (void)pair; (void)data; (void)size; (void)tpo_mgr;
}


// ── Footprint (Tick Volume for footprint chart) ─────────────────────────────
void MessageHandler::handle_tick_volume_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    FootprintManager* fp_mgr)
{
    if (!fp_mgr) return;

    // Try batch format first (historical delivery)
    auto* batch_pb = google::protobuf::Arena::CreateMessage<pb::TickVolumeUpdateBatch>(&s_arena);
    if (batch_pb->ParseFromArray(data, static_cast<int>(size)) && batch_pb->updates_size() > 0) {
        for (const auto& update : batch_pb->updates()) {
            fp_mgr->on_tick_volume_update(pair.symbol, update);
        }
        return;
    }

    // Fallback: single TickVolumeUpdate (live updates + sentinel)
    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::TickVolumeUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    fp_mgr->on_tick_volume_update(pair.symbol, *update_pb);
}


// ── Paper Trading ────────────────────────────────────────────────────────────
void MessageHandler::handle_paper_trading_message(
    const void* data, size_t size,
    PaperTradingManager* paper_mgr)
{
    if (!paper_mgr) return;

    // Try snapshot first (sent on connect - contains account + all positions)
    auto* snapshot_pb = google::protobuf::Arena::CreateMessage<pb::PaperTradingSnapshot>(&s_arena);
    if (snapshot_pb->ParseFromArray(data, static_cast<int>(size)) &&
        snapshot_pb->has_account()) {
        paper_mgr->apply_snapshot(*snapshot_pb);
        return;
    }

    // Fallback: single position update (open/update/close events)
    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::PaperPositionUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    paper_mgr->apply_position_update(*update_pb);
}


// ── Ticker 24h ───────────────────────────────────────────────────────────────
void MessageHandler::handle_ticker24h_message(
    const std::string& exchange,
    const void* data, size_t size,
    TickerManager* ticker_mgr)
{
    if (!ticker_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::Ticker24hUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    ticker_mgr->apply_update(exchange, *update_pb);
}

// ── Scanner ──────────────────────────────────────────────────────────────────
void MessageHandler::handle_scanner_message(
    const std::string& exchange,
    const void* data, size_t size,
    ScannerManager* scanner_mgr)
{
    if (!scanner_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::MarketScannerUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    scanner_mgr->apply_update(exchange, *update_pb);
}

// ═══════════════════════════════════════════════════════════════════════════
// Indicators V1 (S1b) - VPIN pathfinder
// ═══════════════════════════════════════════════════════════════════════════

// One VPINStateUpdate = one finalized volume-bucket print (the backend only
// publishes from completeBucket) - insert straight into the SeriesCache as a
// grain point. Live, replay seed, and archive frames are the same bytes.
void MessageHandler::handle_vpin_state_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    IndicatorSeriesManager* series_mgr)
{
    if (!series_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::VPINStateUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    Series::VPINPoint p;
    p.ts_ms     = update_pb->timestamp_ms();
    p.vpin      = static_cast<float>(update_pb->vpin());
    p.imbalance = static_cast<float>(update_pb->order_imbalance());
    p.hmm_conf  = static_cast<float>(update_pb->hmm_confidence());
    p.hmm_state = static_cast<int16_t>(update_pb->hmm_state());
    p.regime    = Series::regime_index(update_pb->regime().c_str());
    series_mgr->add_vpin(pair.symbol, p);
}

void MessageHandler::handle_historical_vpin_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    IndicatorSeriesManager* series_mgr)
{
    if (!series_mgr) return;

    auto* batch_pb = google::protobuf::Arena::CreateMessage<pb::VPINHistoryBatch>(&s_arena);
    if (!batch_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }

    std::vector<Series::VPINPoint> pts;
    pts.reserve(batch_pb->values_size());
    for (const auto& v : batch_pb->values()) {
        Series::VPINPoint p;
        p.ts_ms     = v.time_ms();
        p.vpin      = static_cast<float>(v.vpin());
        p.imbalance = static_cast<float>(v.order_imbalance());
        p.hmm_conf  = static_cast<float>(v.hmm_confidence());
        p.hmm_state = static_cast<int16_t>(v.hmm_state());
        p.regime    = Series::regime_index(v.toxicity_regime().c_str());
        pts.push_back(p);
    }
    series_mgr->add_vpin_batch(pair.symbol, pts.data(), pts.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// Analytics state (Tier 1) - Positioning + Contagion
// ═══════════════════════════════════════════════════════════════════════════

// PositioningStateUpdate = one poll of Binance positioning (~every 5s): global +
// top-trader long/short account, OI + 24h change, funding, long/short liq USD,
// taker ratios, CVD, smart-money bias, cascade risk. Single-type stream (no
// multiplex). Stored latest-per-symbol in the AnalyticsManager.
void MessageHandler::handle_positioning_state_message(
    const Terminal::Pair& pair,
    const void* data, size_t size,
    AnalyticsManager* analytics_mgr)
{
    if (!analytics_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::PositioningStateUpdate>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    analytics_mgr->apply_positioning(pair.symbol, *update_pb);
}

// ContagionSnapshot = market-wide stress (not per-symbol). Replay-only today.
void MessageHandler::handle_contagion_message(
    const void* data, size_t size,
    AnalyticsManager* analytics_mgr)
{
    if (!analytics_mgr) return;

    auto* update_pb = google::protobuf::Arena::CreateMessage<pb::ContagionSnapshot>(&s_arena);
    if (!update_pb->ParseFromArray(data, static_cast<int>(size))) {
        return;
    }
    analytics_mgr->apply_contagion(*update_pb);
}
