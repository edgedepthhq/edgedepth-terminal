# EdgeDepth Terminal

**Open-source crypto order flow in your browser.**

See resting orders, watch trades as they happen, and replay recorded markets.
The C++/WebAssembly terminal behind [EdgeDepth](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal), ready to run with a local feed or your own data.

[![TUT replay in the OSS terminal: depth heatmap, trade bubbles and attached order-book ladder](assets/oss-realtime-20261001.gif)](https://github.com/edgedepthhq/edgedepth-terminal/raw/refs/heads/master/assets/oss-realtime-20261001.mp4)

*Actual OSS build, captured 1 October 2026. Public TUT recording from 9 August 2026, replayed locally without an account. [Download full-quality MP4](https://github.com/edgedepthhq/edgedepth-terminal/raw/refs/heads/master/assets/oss-realtime-20261001.mp4) · [Full-size screenshot](assets/oss-realtime-20261001.png)*

## Run it locally

Install **Git** and **Docker with Compose** (Docker Desktop on Windows/macOS), then:

```bash
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Open **[localhost:8080](http://localhost:8080)**. This starts the terminal and the
[community gateway](https://github.com/edgedepthhq/edgedepth-gateway) on Binance's public data. No account or API key required.

- **Live:** select a market, then choose **Real-time** to see depth and trade bubbles.
- **Replay:** choose **Replay → Open Replay Library** and pick a free recording. It plays locally, without a live feed.

Use Chrome or Firefox; Safari is unverified. [Setup, troubleshooting and source builds](docs/SETUP.md).
Prefer no installation? [Open the hosted terminal](https://app.edgedepth.com/terminal?utm_source=github&utm_medium=oss&utm_campaign=terminal).

## What's included

- **Charts:** candles down to 1 second, indicators, drawing tools and VWAP.
- **Order flow:** depth heatmaps, trade bubbles, an order-book ladder (DOM) and trade tape.
- **Volume analysis:** footprints showing buy/sell volume at each price, imbalances and volume profiles.
- **Replay:** pause, scrub and revisit `.edpack` recordings in the same workspace.
- **Your workspace:** dock panels, save layouts and connect your own feed.

| Depth and executed trades | Buy/sell volume inside each minute |
| --- | --- |
| [![OSS real-time replay with attached DOM](assets/oss-realtime-20261001.png)](assets/oss-realtime-20261001.png) | [![OSS footprint replay showing recorded buy and sell volume](assets/oss-footprint-20261001.png)](assets/oss-footprint-20261001.png) |

*Both views use the public TUT pack. Footprints require recorded volume at each price; older context candles may have none.*

## What self-hosting doesn't include

The **terminal is open source; EdgeDepth's hosted data and research services are separate.**

- **No hosted historical archive.** The gateway supplies public Binance data and candle history. Depth starts with what you observe; footprint/profile history is bounded. Packs contain only their recorded markets, times and streams.
- **No server-derived analytics or research engine.** Panels such as VPIN and positioning need compatible analytics streams; the community gateway doesn't supply them.
- **You operate the feed.** Exchange access, connection gaps and storage are yours to manage. Missing data stays missing.

Local real-time depth, sub-minute candles and pack replay need **no Pro subscription**.
[Data-source details](docs/DATA_SOURCES.md) explain coverage and custom feeds.

## When to use hosted EdgeDepth

| Option | Best for | What it adds |
| --- | --- | --- |
| **OSS** | Run, inspect and customize the terminal | Your feed and recordings, on your machine. |
| **Pro** | Follow markets without running the data stack | Managed multi-venue feeds, live depth, advanced analytics, up to 90 days of available tick replay and 50 research credits/month. |
| **Research** | Investigate more questions and revisit older events | Everything in Pro, 300 credits/month, deeper available tick replay, onboarding and a first-study review. |

Research lets you test a condition against recorded history, compare what followed
with a baseline, and inspect counterexamples. Web, REST API and MCP share your allowance.
Hosted Free includes 10 credits/month to try it.

**Research history and tick replay have different coverage.** A credit is not
necessarily one study, and historical results do not establish a profitable strategy.
[Compare current plans and coverage](https://edgedepth.com/pricing?utm_source=github&utm_medium=oss&utm_campaign=terminal).

## Build on it

C++20 · WebAssembly · Dear ImGui / ImPlot · SDL3 / WebGL2.

- [Build and pin versions](docs/SETUP.md) · [Architecture](ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md)
- [Bring your own data](docs/DATA_SOURCES.md#bring-your-own-data) · [CSV/Parquet walkthrough](docs/DATAFRAME_WORKFLOW.md) · [Wire protocol](protos/messages.proto)
- [Real-time depth guide](docs/REALTIME_DEPTH.md) · [Replay pack format](docs/EDPACK.md) · [Replay Library](replay-library/README.md)
- [Design tokens](design/README.md) · [Research MCP](https://github.com/edgedepthhq/edgedepth-research-mcp)

[AGPL-3.0](LICENSE) terminal · [MIT gateway](https://github.com/edgedepthhq/edgedepth-gateway) · [Third-party licenses](THIRD_PARTY_NOTICES.md).
