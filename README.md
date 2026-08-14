# EdgeDepth Terminal

**An open-source orderflow terminal and local market-data replay/testing workbench that runs in your browser at 170+ FPS.**

C++20 compiled to WebAssembly. Dear ImGui + ImPlot for immediate-mode rendering, SDL3 + WebGL2 underneath, protobuf over WebSocket for data. No Electron, no DOM in the hot path, no garbage collector between you and the tape.

![The Replay Library replaying a TUT short squeeze: an empty terminal fills with chart, DOM and tape, then the climax prints](assets/replay-library-tut.gif)

*A real +856% short squeeze replayed from a static `.edpack` file in the built-in Replay Library: no feed, no account, no replay server. ([still screenshot](assets/screenshot.png))*

This is the full source of the terminal that powers [EdgeDepth](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal): the same canvas, the same widgets, the same render loop. It is not a demo build or a stripped-down "community edition."

Run it against a local exchange feed, point it at your own wire-compatible data,
or use deterministic `.edpack` recordings as repeatable fixtures. If you prefer
a managed feed and stored history, open the [hosted live terminal](https://app.edgedepth.com/terminal?utm_source=github&utm_medium=oss&utm_campaign=terminal).

## Quick start

The terminal and a live market data feed, both on your machine:

```bash
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Then open **http://localhost:8080**. No API key, no account, no signup. The
feed is [edgedepth-gateway](https://github.com/edgedepthhq/edgedepth-gateway),
a small MIT-licensed Go service that bridges Binance's public WebSocket
streams into this terminal's wire format.

Images are pulled prebuilt so this starts in seconds. To compile the
WebAssembly from source instead, `docker compose up --build` (that pulls the
Emscripten toolchain and takes a while).

**If the tape stays empty while the order book keeps moving**, your network
cannot reach Binance's `@aggTrade` stream. The gateway detects this and logs a
warning naming the fix after 30 seconds (`docker compose logs gateway`). Set
`BINANCE_TRADE_STREAM: "trade"` in `docker-compose.yml` and restart: the tape,
and the live candles built from it, fill in immediately. The market stats
strip above the chart is fed by separate streams and can stay empty
independently of this.

## Why this exists

Web trading UIs are usually React apps fighting the DOM for every orderbook tick. This terminal takes the approach used by native trading software, an immediate-mode GUI redrawn every frame on the GPU, and ships it through WebAssembly. A full orderflow stack (chart, DOM ladder, tape, heatmap) renders at 170+ FPS in a browser tab with frame times around 5ms.

Open-source trade aggregators and charting components exist, but complete browser orderflow terminals in this class are rare. Most mature orderflow tools are closed and paid. This one is open: read it, build it, point it at your own data.

## Features

- **Chart engine**: custom ImPlot candlesticks, multi-timeframe (1m to 1D), buy/sell volume + CVD, indicators (RSI, MACD, Volume, OI, funding), drawing tools, layered overlays
- **DOM ladder**: full depth-of-market with grouping, USD/coin modes, cumulative view, book imbalance
- **Trade tape**: live time & sales with size highlighting
- **Orderbook heatmap**: GPU-rendered depth history via a shader-based renderer
- **Volume profile (VPVR), TPO / Market Profile, footprint**: built client-side from per-price tick volume
- **Liquidation heatmap layers**: the dense liquidation Field, leverage-tier levels, and profile rendering. The Field is computed client-side from candles, so it works on any feed
- **Market replay**: deterministic replay engine with scrubbing, and self-contained `.edpack` files that play entirely client-side with no server
- **Replay Library**: a manifest-driven browser of free, curated `.edpack` recordings for local replay and regression testing
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

**Write your own feed:** [`examples/synthetic_feed.py`](examples/synthetic_feed.py) is a working feed in one file, with no `protoc` step and no protobuf package. It answers historical candle requests and streams trades plus an order book, which is enough to drive the chart, the tape and the DOM. Run it and open the terminal with `?ws=ws://localhost:8765` to see your own data on the screen, then swap the random walk for a strategy, a simulator, or a replay of your own capture:

```bash
pip install websockets
python3 examples/synthetic_feed.py
```

**Community gateway:** [edgedepth-gateway](https://github.com/edgedepthhq/edgedepth-gateway) is exactly that feed, MIT licensed. It serves trades, candles, orderbook, stats and liquidations from Binance's free public streams, and answers historical candle requests from their REST klines so the chart boots with real history. It also builds **1s, 5s, 15s and 30s candles** trade by trade from the raw stream, updating the building candle as each trade arrives. See the [Quick start](#quick-start) to run both together.

A few layers are driven by EdgeDepth's proprietary analytics streams: VPIN toxicity, positioning and smart-money flow, modelled liquidation estimates, pattern detection, and the scanner's composite scores. With a raw-data feed those panels simply stay empty and the terminal degrades gracefully; [which panels, and why](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal#empty-panels) lists them side by side. The [hosted product](https://app.edgedepth.com/terminal?utm_source=github&utm_medium=oss&utm_campaign=terminal) provides them, along with historical replay and structured courses taught inside the terminal.

The main dividing line is time. Your local terminal starts recording when you
install it. EdgeDepth has already been recording 660+ markets for months. Pro
lets you replay an arbitrary moment from the last 30 days.

Research serves a different job: testing how often a defined condition occurred
and what followed across a 90-day record. It includes a larger search budget plus
REST API and MCP access. See [plans](https://edgedepth.com/pricing?utm_source=github&utm_medium=oss&utm_campaign=terminal) only when you need stored history or archive-wide evidence.

## Replay Library and local test packs

No feed is required for a recording. In the top bar choose **Replay**, then
**Open Replay Library**. If a chart is already open, the same widget is under
**+ widget**, then **Replay Library**. A selected pack streams directly from
static hosting and replays locally with no account or replay server.

The checked-in [`replay-library/manifest.json`](replay-library/manifest.json)
is also the production catalog source. It currently lists four curated
recordings; additional picks can be published without rebuilding the terminal.

The terminal can play a self-contained `.edpack` recording entirely
client-side: orderbook, tape, liquidations, footprint and volume profile
included, with nothing but static file hosting behind it.

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
loads. See [`replay-library/README.md`](replay-library/README.md) for the
manifest and CORS contract. The self-hosted client contains no phone-home
analytics; public pack engagement can be measured from aggregate object
requests at the pack host.

## Building and platform support

The build target is WebAssembly, not a native operating-system executable. A
successful source build produces `index.html`, `index.js`, `index.wasm`, and
`index.data`. The build is threaded, so the server must return COOP and COEP
headers for `SharedArrayBuffer`; the bundled `serve_threaded.py` does this.

Source builds require:

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) **4.0.15 or newer**. SDL3 support is unavailable in Emscripten 3.x.
- **`protoc` 21.x**. The instructions below pin 21.12, which reports itself as `libprotoc 3.21.12`. Do not substitute a newer release family.
- CMake 3.15+ and Ninja.

All other dependencies are fetched and pinned by CMake. There are no
submodules or additional system libraries.

### Docker Desktop quick start

On Windows or macOS, install Docker Desktop and use Linux containers. Then:

```text
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Open `http://localhost:8080`. This pulls prebuilt images. To compile the
terminal from source inside the Linux build container, run
`docker compose up --build`. Docker Desktop is an alternative build path; it
does not exercise the native Windows toolchain described below.

### WSL2 source build

Install Ubuntu under WSL2 with `wsl --install -d Ubuntu` from an elevated
PowerShell window, then run the rest inside Ubuntu. Keeping the clone in the
WSL Linux filesystem avoids unnecessary `/mnt/c` filesystem overhead.

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake curl git ninja-build python3 unzip

mkdir -p "$HOME/.local/protoc-21.12"
curl -fsSL \
  -o /tmp/protoc-21.12-linux-x86_64.zip \
  https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-linux-x86_64.zip
unzip -q /tmp/protoc-21.12-linux-x86_64.zip -d "$HOME/.local/protoc-21.12"
export PATH="$HOME/.local/protoc-21.12/bin:$PATH"

git clone https://github.com/emscripten-core/emsdk.git "$HOME/emsdk"
cd "$HOME/emsdk"
./emsdk install 4.0.15
./emsdk activate 4.0.15
source ./emsdk_env.sh

cd "$HOME"
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal

emcmake cmake -S . -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wsl --target c_based_trader_client --parallel
for artifact in index.html index.js index.wasm index.data; do
  test -s "build-wsl/$artifact"
done
ls -l build-wsl/index.html build-wsl/index.js build-wsl/index.wasm build-wsl/index.data

cmake -S tests/native -B build-native-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-tests --config Release --parallel
cmake -E chdir build-native-tests ctest -C Release --output-on-failure

python3 serve_threaded.py 8000 build-wsl
```

Open `http://localhost:8000` from Windows. Add
`?ws=ws://localhost:8080/ws` to connect to your own feed.

The same commands are the supported Linux source-build path outside WSL2.

### Native Windows PowerShell and Ninja source build

Install Git, CMake 3.15+, Ninja, Python 3.8+, and Visual Studio 2022 Build
Tools with the Desktop development with C++ workload. Start a Developer
PowerShell for VS 2022 so the host compiler is available for the native tests.
The WASM build itself uses Emscripten's Clang.

From that PowerShell window, install the pinned tools for the current user:

```powershell
$ToolsRoot = Join-Path $env:LOCALAPPDATA "EdgeDepth\tools"
$EmsdkRoot = Join-Path $ToolsRoot "emsdk"
$ProtocRoot = Join-Path $ToolsRoot "protoc-21.12"
$ProtocZip = Join-Path $ToolsRoot "protoc-21.12-win64.zip"
New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null

git clone https://github.com/emscripten-core/emsdk.git $EmsdkRoot
Push-Location $EmsdkRoot
.\emsdk.ps1 install 4.0.15
.\emsdk.ps1 activate 4.0.15
. .\emsdk_env.ps1
Pop-Location

Invoke-WebRequest -Uri "https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-win64.zip" -OutFile $ProtocZip
Expand-Archive -LiteralPath $ProtocZip -DestinationPath $ProtocRoot -Force
$env:Path = "$(Join-Path $ProtocRoot 'bin');$env:Path"

emcc --version
protoc --version
ninja --version
```

The version checks must show Emscripten 4.0.15 and `libprotoc 3.21.12`.
Clone and build the terminal in the same Developer PowerShell session:

```powershell
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
Set-Location edgedepth-terminal

emcmake.bat cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --target c_based_trader_client --parallel

$Artifacts = @("index.html", "index.js", "index.wasm", "index.data") |
  ForEach-Object { Join-Path "build-windows" $_ }
$Invalid = $Artifacts | Where-Object {
  -not (Test-Path -LiteralPath $_ -PathType Leaf) -or (Get-Item -LiteralPath $_).Length -eq 0
}
if ($Invalid) { throw "Missing or empty build artifacts: $($Invalid -join ', ')" }
Get-Item -LiteralPath $Artifacts

cmake -S tests/native -B build-native-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-tests --config Release --parallel
cmake -E chdir build-native-tests ctest -C Release --output-on-failure

python .\serve_threaded.py 8000 build-windows
```

Open `http://localhost:8000`. For later PowerShell sessions, dot-source
`emsdk_env.ps1` again and add the 21.12 `bin` directory to `PATH` before
configuring a new build directory.

### MSYS2 status and caveats

MSYS2 is not in the supported or CI-tested matrix. It may work, but no MSYS2
build has been reproduced for this project, so the project does not claim
support yet. In particular, combining MSYS-style paths with native Windows
Emscripten, CMake, Ninja, or `protoc.exe` can trigger automatic path conversion
and produce malformed compiler, preload-file, or protobuf arguments.

Use native PowerShell/CMD for the Windows toolchain, or WSL2 for a consistent
Linux toolchain. If you experiment with MSYS2, keep every tool and path model
consistent and include the exact shell and tool versions in any build report.

## Design system

The terminal's visual language (surfaces, text ramp, market-data semantics, heatmap LUTs) lives in [`design/`](design/README.md) as tokens, CSS, and ImGui mapping notes. `src/rendering/theme.{h,cpp}` mirrors those tokens into ImGui/ImPlot styles at runtime, so restyling starts there rather than in widget code.

## Architecture in one paragraph

The browser-facing main thread owns the WebSocket callbacks, ImGui/ImPlot, and WebGL rendering. Live binary frames are copied to a data worker for zstd and protobuf processing; order-book updates use a protected write/read model, while most other updates return through a time-budgeted main-thread dispatch queue. Replay creates an isolated manager set, then points the same widgets and renderers at it. See [ARCHITECTURE.md](ARCHITECTURE.md) for the source-linked build, threading, live-data, replay, rendering, and browser-deployment design.

## Contributing

Issues and PRs welcome. The most valuable contributions right now:

- Feed adapters for other exchanges and venues (the wire format is the contract)
- Widget improvements and new indicators
- Build and tooling portability (it should compile anywhere emsdk runs)

Please keep PRs focused. The render loop has strict conventions: no allocation in the frame path, `PriceFormatter` for all price text, theme tokens for all colors. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Related projects

- [edgedepth-gateway](https://github.com/edgedepthhq/edgedepth-gateway) (MIT): the self-host feed this terminal connects to, bridging Binance's public streams into the wire format, with a pluggable exchange layer for adding venues.
- [edgedepth-research-mcp](https://github.com/edgedepthhq/edgedepth-research-mcp) (MIT): a Model Context Protocol server that lets Claude, Cursor, or any MCP client search EdgeDepth's recorded microstructure: every verified occurrence of a market condition, with forward outcomes and replay-linked evidence that opens in this terminal.

## License

[AGPL-3.0](LICENSE). You can use, modify, and self-host freely. If you host a modified version for others, you must publish your changes. Fonts are OFL-licensed (Hanken Grotesk, JetBrains Mono), and third-party library licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

Built by [EdgeDepth](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal): real-time crypto microstructure, liquidation heatmaps, orderflow analytics, and courses taught inside the live terminal.
