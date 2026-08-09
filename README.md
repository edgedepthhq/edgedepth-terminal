# EdgeDepth Terminal

**A professional orderflow trading terminal that runs in your browser at 170+ FPS.**

C++20 compiled to WebAssembly. Dear ImGui + ImPlot for immediate-mode rendering, SDL3 + WebGL2 underneath, protobuf over WebSocket for data. No Electron, no DOM in the hot path, no garbage collector between you and the tape.

![EdgeDepth Terminal](assets/screenshot.png)

This is the full source of the terminal that powers [edgedepth.com](https://edgedepth.com): the same canvas, the same widgets, the same render loop. It is not a demo build or a stripped-down "community edition."

## Why this exists

Web trading UIs are usually React apps fighting the DOM for every orderbook tick. This terminal takes the approach used by native trading software, an immediate-mode GUI redrawn every frame on the GPU, and ships it through WebAssembly. A full orderflow stack (chart, DOM ladder, tape, heatmap) renders at 170+ FPS in a browser tab with frame times around 5ms.

There is no good open-source terminal in this class. Bookmap, Exocharts, and similar orderflow tools are closed and paid. This one is open: read it, build it, point it at your own data.

## Features

- **Chart engine**: custom ImPlot candlesticks, multi-timeframe (1m to 1D), buy/sell volume + CVD, indicators (RSI, MACD, Volume, OI, funding), drawing tools, layered overlays
- **DOM ladder**: full depth-of-market with grouping, USD/coin modes, cumulative view, book imbalance
- **Trade tape**: live time & sales with size highlighting
- **Orderbook heatmap**: GPU-rendered depth history via a shader-based renderer
- **Volume profile (VPVR), TPO / Market Profile, footprint**: built client-side from per-price tick volume
- **Liquidation heatmap layers**: the dense liquidation Field, leverage-tier levels, and profile rendering. The Field is computed client-side from candles, so it works on any feed
- **Market replay**: deterministic replay engine with scrubbing, and self-contained `.edpack` files that play entirely client-side with no server
- **Paper trading**: simulated positions against live data
- **Docking layout**: drag, split, and persist panel arrangements (ImGui docking)
- **Watchlist / scanner**: 800+ Binance Futures pairs with 24h stats
- **Wire format**: zstd-compressed protobuf ([`protos/messages.proto`](protos/messages.proto)), decoded off the render thread

## Bring your own data

The terminal is a client. It speaks a documented protobuf-over-WebSocket wire format and connects to whatever feed you give it, resolved in this order:

1. `?ws=ws://localhost:8080/ws` (query parameter)
2. `window.__EDGEDEPTH_WS_URL__` (set by the host page before the WASM glue loads)
3. `wss://api.edgedepth.com/ws` (EdgeDepth's hosted backend, the default)

The schema in [`protos/messages.proto`](protos/messages.proto) is the whole contract. A feed that emits trades, candles, orderbook updates, stats, and liquidation events, all derivable from any exchange's public streams, lights up the chart, DOM, tape, orderbook, heatmap, liquidation Field, volume profile, TPO, and paper trading.

**Community gateway:** a small open-source Go service that bridges Binance's public WebSocket streams into this wire format (`docker compose up` for a live terminal on localhost) is on its way. Watch this org.

A few layers are driven by EdgeDepth's proprietary analytics streams: VPIN toxicity, positioning and smart-money flow, modelled liquidation estimates, pattern detection, and the scanner's composite scores. With a raw-data feed those panels simply stay empty and the terminal degrades gracefully. The hosted product at [edgedepth.com](https://edgedepth.com) provides them, along with deep historical replay and structured courses taught inside the terminal.

## Building

Prerequisites: [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html), `protobuf-compiler` (`protoc`), CMake 3.15+.

```bash
source /path/to/emsdk/emsdk_env.sh

git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal

# Threaded build (pthreads, needs COOP/COEP headers to run, see below)
mkdir build-threaded && cd build-threaded
emcmake cmake -DCMAKE_BUILD_TYPE=Release ..
emmake make -j$(nproc)
```

All other dependencies (ImGui docking, ImPlot, zstd, protobuf-lite, nlohmann/json) are fetched and pinned by CMake. No submodules, no system libraries.

Serve it. The threaded build requires cross-origin isolation for `SharedArrayBuffer`, and the bundled dev server sets the headers:

```bash
cd ..
python3 serve_threaded.py 8000 build-threaded
# open http://localhost:8000                             connects to EdgeDepth's hosted feed
# open http://localhost:8000?ws=ws://localhost:8080/ws   connects to your own feed
```

## Design system

The terminal's visual language (surfaces, text ramp, market-data semantics, heatmap LUTs) lives in [`design/`](design/README.md) as tokens, CSS, and ImGui mapping notes. `src/rendering/theme.{h,cpp}` mirrors those tokens into ImGui/ImPlot styles at runtime, so restyling starts there rather than in widget code.

## Architecture in one paragraph

A dedicated worker thread owns the WebSocket, zstd decompression, and protobuf decode. Parsed messages flow through lock-guarded managers (orderbook, candles, heatmap, liquidations) that the render thread reads. The main loop is a single ImGui frame: every widget draws from current manager state, every frame, and the GPU does the rest, including the orderbook heatmap and liquidation Field, which render as shader-driven texture quads rather than per-cell draw calls. The replay engine substitutes the live feed with a time-ordered historical stream and drives the same managers, so live and replay are pixel-identical code paths.

## Contributing

Issues and PRs welcome. The most valuable contributions right now:

- Feed adapters for other exchanges and venues (the wire format is the contract)
- Widget improvements and new indicators
- Build and tooling portability (it should compile anywhere emsdk runs)

Please keep PRs focused. The render loop has strict conventions: no allocation in the frame path, `PriceFormatter` for all price text, theme tokens for all colors. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[AGPL-3.0](LICENSE). You can use, modify, and self-host freely. If you host a modified version for others, you must publish your changes. Fonts are OFL-licensed (Hanken Grotesk, JetBrains Mono), and third-party library licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

Built by [EdgeDepth](https://edgedepth.com): real-time crypto microstructure, liquidation heatmaps, orderflow analytics, and courses taught inside the live terminal.
