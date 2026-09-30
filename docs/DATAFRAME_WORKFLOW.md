# Load CSV or Parquet trades

[Data sources](DATA_SOURCES.md) · [Setup](SETUP.md)

Turn a one-instrument trade file into candles, tape, footprints and volume
profiles. No exchange connection or account is needed. This is a sequential
viewer, not a backtester or order-book reconstruction tool.

## Try the example

From the repository root, with Docker Compose and Python 3.10+ installed:

```bash
docker compose up -d --no-deps terminal
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r examples/requirements.txt
python examples/dataframe_demo.py --output demo-data
python examples/file_feed.py demo-data/synthetic-btcusdt.parquet --symbol btcusdt --speed 10
```

On PowerShell, use `python` instead of `python3` and activate with
`.venv\Scripts\Activate.ps1`. The walkthrough has been executed on Linux;
Windows/macOS runtime checks are not claimed.

Open **[the local terminal](http://localhost:8080/terminal/btcusdt?ws=ws%3A%2F%2Flocalhost%3A8765)**.
Choose 5m or 15m candles. For per-price volume, choose **Footprint (Cluster)** and
zoom in, or enable **Layers → Volume Profile**. DOM and depth are unavailable.

The demo writes identical CSV and Parquet files: **7,200 generated trades**, not
market data, covering 7 September 2026, 00:00–01:59:59 UTC (287.94 BTC total).
The default 80% history split starts playback at 01:36 UTC; the rest takes about
144 seconds at 10x. Source times stay unchanged even if the UI uses local time.
The footer's live connection label does not mean the data is current.

Restart the feed **and reload the page** to start over. Reconnects resume from
the furthest row already served; viewers may have different cursors. For seekable
replay, use [`.edpack`](EDPACK.md). This adapter does not export packs or run trades.

## Use your own file

Export one instrument with these four columns:

| Column | Required value |
| --- | --- |
| `timestamp` | Source time: epoch seconds/ms/us/ns, or timezone-aware ISO 8601. Prefer an explicit `--time-unit`. Nanoseconds are truncated to ms without a float conversion. |
| `price` | Positive, finite quote-currency price per base unit. |
| `qty` | Positive, finite **base-asset quantity**. Convert contracts or quote notional using the instrument's specification first. |
| `side` | Aggressor `buy`/`sell`; b/s, true/false and 1/0 are also accepted. Maker-side aliases `buyer_maker` and `is_buyer_maker` are inverted: maker buy means aggressor sell. Unknown or missing side is rejected. |

```python
# df contains your validated trades for ONE instrument.
trades = df.rename(columns={"event_time": "timestamp", "amount": "qty"})
trades[["timestamp", "price", "qty", "side"]].to_parquet("my-trades.parquet", index=False)
```

```bash
python examples/file_feed.py my-trades.parquet --symbol btcusdt --time-unit ms
```

- Use the existing lowercase terminal symbol. `--exchange` accepts `binancef`
  (default) or `hl`; it labels the pair, not the file's provenance. Only that pair
  is served. Use the browser URL printed by the feed for the chosen exchange.
- Maximum **64 MiB / 250,000 rows**. The normalized slice stays in memory.
  Export smaller slices for larger captures. Default bind: `127.0.0.1:8765`.
- Future times and invalid prices, quantities or sides are rejected. Rows are
  stably time-sorted; equal timestamps keep input order. Duplicates are retained.
  Deduplicate by source trade ID before export if needed.
- Run `python examples/file_feed.py --help` for speed, history and port options.

## Coverage and units

Footprints and profiles use available **whole source minutes already reached**.
The opening partial minute and unfinished ending minute are excluded; history
never reads beyond the playback cursor. Missing intervals stay missing, and a
profile's start/end labels do not prove continuous coverage.

Volume is in base units; counts are supplied rows, not inferred taker orders.
The client caches at most 250,000 price cells / 4,096 footprint buckets across
markets and can request evicted data again. TPO uses candle ranges, not tick
occupancy: choose 30m or a smaller divisor of 30m.

## Adapter reference

[`file_feed.py`](../examples/file_feed.py) lists accepted column aliases and
sends one uncompressed `WSPayload` per binary WebSocket frame:

- Stream 17: `TickVolumeUpdate` with raw nested `TickVolumeLevels`. Every footprint
  history request ends with an empty stream-17 sentinel, including empty ranges.
- Stream 26: `VolumeProfileResponse`.
- Candle timeframes use seconds; tick-volume timeframes use milliseconds.

Run [the schema-backed example checks](../tests/examples/README.md) before
changing wire fields or side/unit handling. There is no CCXT, MT5 or broker
execution integration; export a compatible trade file from your existing tools.
