# Choose a data source

[README](../README.md) · [Setup](SETUP.md)

The terminal displays what your feed or recording contains. Start with one of
these paths:

| You want to… | Use | What you get |
| --- | --- | --- |
| Watch live Binance markets | [Community gateway](SETUP.md#quick-start) | Public trades, candles, book, stats and liquidations. No account or API key. |
| Replay a recorded event | [Replay Library](#replay-library-and-local-test-packs) | A free `.edpack` recording; no live feed or replay server. |
| View your own trade file | [CSV/Parquet guide](DATAFRAME_WORKFLOW.md) | Candles, tape, footprints and profiles from supplied trades. No order book. |
| Build a custom feed | [WebSocket example](#bring-your-own-data) | A working adapter you can replace with your own data. |

## Features and their data requirements

| View | Required data or limitation |
| --- | --- |
| Candles, indicators, drawings | Supplied candles. The gateway also builds 1s, 5s, 15s and 30s candles from received trades. |
| Tape and trade bubbles | Trades. Earlier candle bubbles need `get_candle_bubbles`; live records alone do not backfill them. |
| DOM and real-time depth | A book snapshot plus continuous deltas. A trade file cannot reconstruct an order book. See [RT depth](REALTIME_DEPTH.md). |
| Footprints, imbalances and volume profiles | Closed-minute volume at each price, supplied through `get_footprint_history` and `get_volume_profile`. |
| TPO / Market Profile | A candle-range approximation in 30-minute blocks, not measured tick occupancy. Use 30m or a smaller timeframe dividing 30m. |
| Liquidation Field | A local estimate from candles, not observed positions. |
| Flow & Positioning | A compatible `get_flow_positioning` backend; the community gateway does not supply it. |
| VPIN, positioning and other server analytics | Compatible analytics streams, absent from the community gateway. |
| Watchlist | Markets and stats listed by the feed. |

The gateway keeps up to **60 minutes / 50,000 price-minute cells per active
symbol** for footprints and profiles. Collection starts at the next minute
boundary after joining or a gap; a later trade closes each minute. There is no
historical trade backfill or disk persistence.

### Why some replay candles have no footprint

Candles and per-price volume are separate streams. Older context candles can
have no footprint data. For example, the public TUT v2 pack's detailed streams
start at **06:50 UTC on 9 August 2026**. Replay shows only completed minutes at
the playhead; the unfinished minute and missing source minutes have no footprint.
For imbalance controls, right-click a footprint chart and choose **Imbalances**.

## Bring your own data

The terminal chooses its WebSocket URL in this order:

1. `?ws=ws://localhost:8765`
2. `window.__EDGEDEPTH_WS_URL__`, set before the WebAssembly glue loads
3. `wss://api.edgedepth.com/ws`, the hosted default

The URL must be reachable by **your browser**, not just a Docker container.
The contract is [`protos/messages.proto`](../protos/messages.proto).

For a generated feed with candles, trades and a book, run:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install websockets
python examples/synthetic_feed.py
```

With the terminal running, open
[localhost:8080/?ws=ws://localhost:8765](http://localhost:8080/?ws=ws%3A%2F%2Flocalhost%3A8765).
[`synthetic_feed.py`](../examples/synthetic_feed.py) needs no `protoc` or protobuf
package. For existing trades, follow the [CSV/Parquet guide](DATAFRAME_WORKFLOW.md).

## Replay Library and local test packs

Choose **Replay → Open Replay Library**, or **+ widget → Replay Library**.
Each pack plays locally and supplies only its recorded markets, times and streams.

To open or host a pack, see [EDPACK](EDPACK.md). To use your own catalog, see
[the manifest guide](../replay-library/README.md). Pack hosts need HTTP range and
CORS support for efficient loading. The self-hosted client has no phone-home
analytics; the pack host can still see its own file requests.

## Workspaces and reference context

**Workspace** saves layouts and settings in your browser. Export JSON to keep a
portable backup. Version 1 supports one panel per type for the current market;
replay and embeds do not overwrite the live workspace.

**Layers** offers session/anchored VWAP and previous-day/week levels. Right-click
a candle to anchor VWAP. It uses completed HLC3 candles weighted by base volume,
not exact trade-price VWAP. Days start at 00:00 UTC, weeks on Monday. Missing bars
stop VWAP; incomplete periods have no levels. **Load reference history** uses
available source coverage. These overlays are absent from TPO and Renko.

## Connection and depth coverage

Live sockets reconnect with a 1–30 second backoff. Frame age in the status bar
measures socket traffic, not every layer's completeness. Paused subscriptions
stay paused. An interrupted network replay stays paused until reopened; packs
need no live socket. Missing depth stays missing: the current book is never
copied backward. [RT history limits](REALTIME_DEPTH.md#coverage).

Self-hosting includes the renderer and local replay. Hosted EdgeDepth adds
managed feeds, stored history and Research; it does not make every dataset
available in every view. [Compare current plans and coverage](https://edgedepth.com/pricing).
