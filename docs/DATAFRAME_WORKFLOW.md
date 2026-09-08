# A dataframe on the terminal

Status: local candidate, 8 September 2026. These example and gateway changes
have not been published as release images. Use the candidate checkout to run
the commands; do not assume `latest` already contains them.

## Reproduce without exchange access

Start only the published terminal service (the Python process is the feed):

```bash
docker compose up -d --no-deps terminal
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r examples/requirements.txt
python examples/dataframe_demo.py --output demo-data
python examples/file_feed.py demo-data/synthetic-btcusdt.parquet --symbol btcusdt --speed 10
```

On Windows PowerShell, activate with `.venv\Scripts\Activate.ps1` and use
`python` in place of `python3`. These example commands were executed on Linux;
Windows and macOS have not been rerun for this candidate.

Open [the local terminal](http://localhost:8080/terminal/btcusdt?ws=ws%3A%2F%2Flocalhost%3A8765).
The browser connects to your host's loopback Python process, not Docker DNS.
Use the same command with `.csv` to test the other export. Both files normalize
to identical rows: 7,200 generated trades, 287.94 BTC total, price in USDT,
from 2026-09-07 00:00:00 to 01:59:59 UTC. This is synthetic test data, not a
market recording, trading signal or backtest result.

The default 80% history split begins playback at 01:36 UTC. At 10x the remaining
24 source minutes take about 144 seconds. Source timestamps remain unchanged;
the UI may display them in your local timezone. Missing source intervals remain
missing. The footer's live transport state does not turn this recording into
current market data.

Choose 5m or 15m candles to see the short slice. In the chart-type menu choose
Footprint (Cluster), then zoom until cells have room for text. In Layers enable
Volume Profile. TPO uses candle high/low ranges in 30m blocks: use 30m or a
smaller divisor. DOM, depth heatmap and RT book history remain unavailable
because this file contains no order-book observations.

The server is a small, sequential display adapter. New connections resume from
the furthest row already served by this process; reconnect cannot rewind old
trades into a retained chart. Restart the feed **and reload the page** to start
again. Multiple viewers can have different current cursors. There is no seek,
strategy execution, P&L evaluation, or `.edpack` exporter here.

## Normalize your recording

Export exactly one instrument. Supply its existing terminal market code using
`--symbol`, and `--exchange binancef` (default) or `hl`. The exchange label selects
the terminal pair; it does not contact that exchange or prove where a file came
from. Only that exact pair is served, so switching symbols cannot relabel its
prices. Inspect and filter your source before exporting.

| Field | Meaning and accepted input |
| --- | --- |
| `timestamp` | Source event time: epoch seconds, ms, us, ns, or timezone-aware ISO 8601. Use `--time-unit` to remove magnitude ambiguity. Nanoseconds are truncated to milliseconds without first converting the epoch to float. |
| `price` | Positive finite quote-currency price per base unit. |
| `qty` | Positive finite **base-asset quantity**. Convert contracts or quote notional before export using the instrument's actual contract specification. |
| `side` | Aggressor `buy` or `sell` (also b/s, true/false, 1/0). `buyer_maker` / `is_buyer_maker` is inverted. A maker-buy flag means the aggressor sold. Missing/unknown side is rejected; no price-based inference. |

The loader recognizes common column aliases, listed in `examples/file_feed.py`.
For the clearest contract, rename your dataframe columns to these four. Rows
are stably sorted by timestamp; equal-millisecond rows retain their input order.
Identical rows are not silently deduplicated, because two equal prints may be
real distinct trades. Deduplicate using source IDs before export if necessary.
Limits: 64 MiB input, 250,000 rows; Parquet is read in batches after checking its
metadata. The complete normalized slice stays in memory. Use smaller slices
for larger captures. The bind address defaults to `127.0.0.1`.

```python
# df is your already captured and validated one-instrument dataframe.
trades = df.rename(columns={"event_time": "timestamp", "amount": "qty"})
trades[["timestamp", "price", "qty", "side"]].to_parquet("my-trades.parquet", index=False)
```

```bash
python examples/file_feed.py my-trades.parquet --symbol btcusdt --time-unit ms
```

## What each source can supply

| Source | Candles / tape | DOM / RT depth | Footprint / profile | TPO |
| --- | --- | --- | --- | --- |
| `synthetic_feed.py` | Generated history / random trades | Generated book snapshots | No closed history | Candle-range approximation |
| `file_feed.py` | Supplied trade rows / derived candles | Unavailable | Supplied whole source minutes already reached | Candle-range approximation |
| Gateway candidate | Public received trades / REST and derived candles | Sequence-checked observed book | Bounded closed observed minutes after warmup | Candle-range approximation |
| `.edpack` | Only included streams, bounded by playhead | Only included continuous book sequence; seek limitations apply | Only included closed minutes at playhead | Available candle ranges |

Footprint and profile volumes are base units. Trade counts count supplied rows
or received messages, not inferred taker orders. A profile combines available
whole minutes in the requested range; its first/last timestamps do not prove
uninterrupted coverage between them. The opening partial source minute and
unfinished ending minute are excluded. History requests never read past the
playback cursor. The terminal's stored footprint history is capped at 250,000
price cells / 4,096 buckets across markets and can be requested again after
eviction. Coverage labels deliberately remain visible.

Protocol: one uncompressed `WSPayload` per binary WebSocket frame; stream 17
contains `TickVolumeUpdate` with raw nested `TickVolumeLevels`, and stream 26
contains `VolumeProfileResponse`. Raw nested protobuf is accepted by the client.
Footprint requests always finish with an empty stream-17 sentinel, including
empty ranges. Candle timeframes are seconds; tick-volume timeframes are ms.
Run [the example contract checks](../tests/examples/README.md) before changing
any field numbers or side/unit interpretation.

## Next connector decision

The CSV/Parquet path is the common boundary. A CCXT export helper is a sensible
next experiment only after users identify a specific missing venue and dataset.
Require explicit aggressor side, source IDs, timestamps, units and pagination
coverage for that one venue. CCXT's unified API does not make missing trade
history or side information exist. No CCXT, MT5 or broker execution support is
claimed by this example. A strategy framework can export its existing capture;
there is no need to make this viewer a strategy engine.
