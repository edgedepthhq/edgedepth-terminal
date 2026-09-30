# Data sources and terminal features

[Back to the README](../README.md). The client renders what its feed or recording supplies.

## Features

- **Chart engine**: custom ImPlot candlesticks, multi-timeframe (1m to 1D), buy/sell volume + CVD, indicators (RSI, MACD, Volume, OI, funding), drawing tools, layered overlays
- **Trade bubbles on candles**: large prints from the live tape are drawn inside their own bar at their received price and time, sized by value and thinned to a screen budget so zooming reveals more. Any feed supplies live prints; recorded bubbles for earlier bars need a backend that answers `get_candle_bubbles`
- **Flow & Positioning**: a chart view aligning selected-minute aggression with raw open-interest contracts and reported liquidations, with missing and stale states kept explicit and a JSON export. Needs a backend that answers `get_flow_positioning`; the community gateway does not implement that endpoint, so it reports the evidence as unavailable rather than inventing it
- **DOM ladder**: independent depth with grouping, USD/coin modes and trade columns, or a default RT link sharing the chart's price positions, sampled book and pause state. See [the RT guide](REALTIME_DEPTH.md).
- **Trade tape**: live time & sales with size highlighting
- **Orderbook heatmap**: GPU-rendered depth history via a shader-based renderer
- **Volume profile (VPVR) and footprint**: use supplied closed-minute per-price volume. The gateway source retains bounded observations after warmup; the CSV/Parquet example serves available source minutes. Pack coverage depends on its contents. Missing intervals remain gaps.
- **TPO / Market Profile**: a candle-range approximation in 30-minute blocks, not tick-by-tick time occupancy. Choose 30m or a smaller timeframe dividing 30m. No available candles means no TPO.
- **Footprint imbalances**: same-price or diagonal buy/sell comparisons, configurable ratio and minimum volume, and consecutive same-side stacks. Uses available closed one-minute tick-volume buckets; replay excludes buckets ending after the playhead. Right-click a footprint view in the chart menu, then choose Imbalances.
- **Liquidation heatmap layers**: the dense liquidation Field, leverage-tier levels, and profile rendering. The Field is a client-side estimate computed from supplied candles, not observed positions
- **Market replay**: deterministic replay engine with scrubbing, and self-contained [`.edpack`](EDPACK.md) files that play entirely client-side with no server
- **Replay Library**: a manifest-driven browser of free, curated `.edpack` recordings for local replay and regression testing
- **Paper trading**: simulated positions against live data
- **Docking layout**: drag, split, and persist panel arrangements (ImGui docking)
- **Watchlist / scanner**: every symbol the feed lists, with 24h stats. Available symbols depend on the gateway and current exchange listings
- **Wire format**: zstd-compressed protobuf ([`protos/messages.proto`](../protos/messages.proto)), decoded off the render thread

## Why some replay candles have no footprint

Candles and per-price volume are separate streams. A pack can include older
candles for context without the trade-volume history needed to draw footprints.
The public TUT v2 pack records detailed streams from 06:50 UTC on 9 August
2026; earlier context candles do not imply footprint coverage. Replay displays
only closed-minute footprint data available at the playhead. The unfinished
minute waits for its close, and missing source minutes remain gaps.

## Workspaces and reference context

Use **Workspace** in the full live terminal to save a named setup, switch between
Order Flow, Liquidity and Replay Review presets, or export/import a JSON backup.
Layout and panel settings restore in the same browser. Version 1 supports one
panel of each type for the current market. Replay and hosted embeds leave your
live workspace alone. Browser storage can be cleared, so export setups you need
to keep.

**Layers** includes session VWAP and previous-day/week high, low and close.
Right-click a candle to anchor VWAP. VWAP uses completed HLC3 candles weighted by
base volume; it is not exact trade-price VWAP. Sessions start at midnight UTC and
weeks on Monday. Missing bars stop VWAP and suppress incomplete period levels.
Use **Load reference history** when offered; the data source still determines
coverage. TPO and Renko do not display these overlays.

## Bring your own data

The terminal is a client. It speaks a documented protobuf-over-WebSocket wire format and connects to whatever feed you give it, resolved in this order:

1. `?ws=ws://localhost:8080/ws` (query parameter)
2. `window.__EDGEDEPTH_WS_URL__` (set by the host page before the WASM glue loads)
3. `wss://api.edgedepth.com/ws` (EdgeDepth's hosted backend, the default)

The schema in [`protos/messages.proto`](../protos/messages.proto) is the contract.
Trades drive the tape and observed forming footprints; supplied candles drive
the chart and candle-range TPO. DOM and depth require actual book snapshots and
continuous deltas. Completed footprint snapshots and volume profiles need
per-price volume through `get_footprint_history` and `get_volume_profile`.
A trade CSV cannot reconstruct a historical order book.

**Write your own feed:** [`examples/synthetic_feed.py`](../examples/synthetic_feed.py) is a working feed in one file, with no `protoc` step and no protobuf package. It answers historical candle requests and streams trades plus an order book, which is enough to drive the chart, the tape and the DOM. Run it and open the terminal with `?ws=ws://localhost:8765` to see your own data on the screen, then swap the random walk for a strategy, a simulator, or a replay of your own capture:

```bash
pip install websockets
python3 examples/synthetic_feed.py
```

**Start with a dataframe:** [the reproducible CSV/Parquet walkthrough](DATAFRAME_WORKFLOW.md)
creates 7,200 explicitly generated trades and puts them on the chart, tape,
footprint and volume profile. No exchange access or credentials are required.

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r examples/requirements.txt
python examples/dataframe_demo.py --output demo-data
python examples/file_feed.py demo-data/synthetic-btcusdt.parquet --symbol btcusdt --speed 10
```

With the terminal running locally, open
[the generated BTC fixture](http://localhost:8080/terminal/btcusdt?ws=ws%3A%2F%2Flocalhost%3A8765).
The example requires explicit aggressor side, positive base-asset quantity and
source timestamps. It rejects unknown sides, negative sizes and future dates.
It advances sequentially through source time; use `.edpack` for seekable replay.

**Community gateway:** [edgedepth-gateway](https://github.com/edgedepthhq/edgedepth-gateway) is exactly that feed, MIT licensed. It serves trades, candles, orderbook, stats and liquidations from Binance's free public streams, and answers historical candle requests from their REST klines so the chart boots with real history. It also builds **1s, 5s, 15s and 30s candles** trade by trade from the raw stream, updating the building candle as each trade arrives. The gateway source also retains up to 60 minutes / 50,000 price-minute cells per active symbol for footprints and profiles. It starts at the next minute boundary after joining or detecting a gap, then closes the minute on a later trade. It has no historical trade backfill or disk persistence. See the [Quick start](SETUP.md#quick-start) to run both together.

A few layers are driven by EdgeDepth's proprietary analytics streams: VPIN toxicity, positioning, pattern detection, and the scanner's composite scores. With a raw-data feed those panels simply stay empty and the terminal degrades gracefully; [which panels, and why](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal#empty-panels) lists them side by side. Hosted availability depends on the current plan and supported data layer. The candle-derived liquidation Field is computed locally and does not require these server streams.

Run the terminal yourself with a live feed or a recording you already have.
The hosted product adds maintained feeds, stored market history and a connected
research workflow: define a condition, compare historical outcomes with a baseline,
inspect the available replay evidence, and save a search to revisit.

Research also provides REST API and MCP access. Searchable research history and
tick replay have different coverage and access limits; see the current
[plans](https://edgedepth.com/pricing?utm_source=github&utm_medium=oss&utm_campaign=terminal)
when you need hosted history or research capacity. Local replay remains part of
the open-source terminal and requires no hosted subscription.

## Replay Library and local test packs

No feed is required for a recording. In the top bar choose **Replay**, then
**Open Replay Library**. If a chart is already open, the same widget is under
**+ widget**, then **Replay Library**. A selected pack streams directly from
static hosting and replays locally with no account or replay server.

The checked-in [`replay-library/manifest.json`](../replay-library/manifest.json)
is also the production catalog source. It currently lists four curated
recordings; additional picks can be published without rebuilding the terminal.

The terminal can play a self-contained `.edpack` recording entirely
client-side, with nothing but static file hosting behind it. Orderbook, tape,
liquidations, footprint and volume profile work where the pack actually includes
those streams; a pack is not a promise of every layer.

`.edpack` is EdgeDepth's own deterministic replay container. The format is
documented in [`docs/EDPACK.md`](EDPACK.md): magic and version gating,
the protobuf header, the block index, framing, compression, what determinism
does and does not guarantee, and how a truncated pack fails.

```
?pack=<url-encoded pack URL>&packsym=<symbol>
```

Try one of the catalog's recordings directly: 30 tick-by-tick minutes from a
June 2026 ZEC selloff, including the order book, tape, liquidations, footprint
and volume profile data (53 MB):

```
http://localhost:8080/?pack=https%3A%2F%2Freplays.edgedepth.com%2Freplays%2Fzec_cascade_demo%2Fv1.edpack&packsym=zecusdt
```

The pack is fetched with HTTP range requests, block by block, as playback and
seeking need it. If you host packs yourself, the server (and any CDN in front
of it) must allow the `Range` header in its CORS policy and answer
`206 Partial Content`; a server that ignores `Range` and answers `200` with
the whole body forces the client to buffer the entire file into memory.

To use the widget with a private or local corpus, point it at another v1
manifest without rebuilding:

```text
?replayLibrary=http%3A%2F%2Flocalhost%3A9000%2Fmanifest.json
```

Or set `window.__EDGEDEPTH_REPLAY_LIBRARY_URL__` before the WebAssembly glue
loads. See [`replay-library/README.md`](../replay-library/README.md) for the
manifest and CORS contract. The self-hosted client contains no phone-home
analytics; public pack engagement can be measured from aggregate object
requests at the pack host.

## Connection and depth coverage

Live sockets retry automatically after closure, with a 1-30 second backoff.
The status bar distinguishes connecting, retrying, open/waiting, and frame age.
Frame age measures traffic on that socket, not completeness of every layer.
Paused subscriptions remain paused. A network replay interrupted by closure
keeps its last frame paused and asks you to reopen replay; local packs do not
need the socket.

Recent observed depth is retained for ten minutes across GPU rebuilds.
Authoritative history replaces older provisional columns when it arrives.
Unobserved minutes remain gaps, and history availability depends on the feed.
The current order book is never copied backward to fill those gaps.
