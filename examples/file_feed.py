#!/usr/bin/env python3
"""
Play a CSV or Parquet file of trades into the terminal. No protoc, no backend.

    pip install websockets          # plus pyarrow, for .parquet
    python3 examples/file_feed.py mytrades.csv --symbol btcusdt

Then open the terminal against it:

    http://localhost:8000/?ws=ws://localhost:8765

The wire encoding lives in synthetic_feed.py next door and is imported, not
copied; read that file for what the protobuf frames actually look like.

Columns, matched case-insensitively, in this order of preference:

    time  : ts, time, timestamp, ts_ms, datetime, date, t
    price : price, px, p
    size  : qty, size, quantity, amount, volume, q
    side  : side, is_buy, direction, buyer_maker, is_buyer_maker, m  (optional)

Epoch seconds, millis, micros and nanos are told apart by magnitude, and ISO-8601
strings parse too. `buyer_maker`-style columns are inverted for you: buyer is the
maker means the aggressor was the seller.

The file is split at --history (default 0.8): the earlier rows arrive as candles
when the terminal asks for history, the rest replay as live trades at --speed.
The DOM stays empty, because a trade file carries no book and a fabricated one
would just be a lie on the screen.
"""

import argparse
import asyncio
import csv
import json
import sys
from datetime import datetime

import websockets

from synthetic_feed import (
    STREAM_HISTORICAL_CANDLES,
    STREAM_TRADES,
    SENTINEL_SYMBOL,
    candles_msg,
    envelope,
    pair_msg,
    trade_msg,
)

TS_KEYS    = ("ts", "time", "timestamp", "ts_ms", "datetime", "date", "t")
PRICE_KEYS = ("price", "px", "p")
QTY_KEYS   = ("qty", "size", "quantity", "amount", "volume", "q")
SIDE_KEYS  = ("side", "is_buy", "direction", "buyer_maker", "is_buyer_maker", "m")
# These name the MAKER, so the aggressor is the other side.
INVERTED_SIDE_KEYS = ("buyer_maker", "is_buyer_maker", "m")
SELL_WORDS = ("sell", "s", "ask", "short", "false", "0")


def _column(row, keys):
    """First matching column name, case-insensitively. None if absent."""
    lower = {k.lower(): k for k in row if k}
    for k in keys:
        if k in lower:
            return lower[k]
    return None


def _ts_ms(v):
    if isinstance(v, datetime):
        return int(v.timestamp() * 1000)
    try:
        n = float(v)
    except (TypeError, ValueError):
        s = str(v).strip().replace("Z", "+00:00")
        return int(datetime.fromisoformat(s).timestamp() * 1000)
    # Magnitude tells the unit apart; anything below ~1973 in seconds is not
    # market data worth plotting.
    if n > 1e17:  return int(n / 1e6)   # nanos
    if n > 1e14:  return int(n / 1e3)   # micros
    if n > 1e11:  return int(n)         # millis
    return int(n * 1000)                # seconds


def _is_buy(v, inverted):
    if v is None:
        return None
    if isinstance(v, bool):
        buy = v
    else:
        buy = str(v).strip().lower() not in SELL_WORDS
    return (not buy) if inverted else buy


def load(path):
    """-> [(ts_ms, price, qty, is_buy)], oldest first."""
    if path.lower().endswith(".parquet"):
        import pyarrow.parquet as pq          # only needed for parquet
        rows = pq.read_table(path).to_pylist()
    else:
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"{path}: no rows")

    ts_c, px_c, qty_c = (_column(rows[0], k) for k in (TS_KEYS, PRICE_KEYS, QTY_KEYS))
    side_c = _column(rows[0], SIDE_KEYS)
    missing = [n for n, c in (("time", ts_c), ("price", px_c), ("size", qty_c)) if not c]
    if missing:
        sys.exit(f"{path}: no {'/'.join(missing)} column in {list(rows[0])}")
    inverted = side_c is not None and side_c.lower() in INVERTED_SIDE_KEYS

    out, last = [], None
    for r in rows:
        ts, price, qty = _ts_ms(r[ts_c]), float(r[px_c]), abs(float(r[qty_c]))
        buy = _is_buy(r[side_c], inverted) if side_c else None
        if buy is None:
            # ponytail: tick rule when the file has no side column. Cheap and
            # standard; swap for Lee-Ready if the mislabelled ticks matter.
            buy = last is None or price >= last
        last = price
        out.append((ts, price, qty, buy))
    out.sort(key=lambda t: t[0])
    return out


def to_bars(trades, timeframe_s):
    """OHLCV buckets in candle_msg order: (o, h, l, c, volume, bucket_ms)."""
    step, bars = timeframe_s * 1000, []
    for ts, price, qty, _ in trades:
        bucket = ts - ts % step
        if bars and bars[-1][5] == bucket:
            o, h, l, _c, v, _b = bars[-1]
            bars[-1] = (o, max(h, price), min(l, price), price, v + qty, bucket)
        else:
            bars.append((price, price, price, price, qty, bucket))
    return bars


async def serve(ws, history, live, speed):
    state = {"pair": None}

    async def control():
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            data = msg.get("data") or {}
            pair = data.get("pair") or {}
            symbol = pair.get("symbol")
            if not symbol or symbol == SENTINEL_SYMBOL:
                continue
            encoded = pair_msg(pair.get("exchange", ""), symbol)
            if state["pair"] is None:
                state["pair"] = encoded
                print(f"  streaming {pair.get('exchange')}/{symbol}", flush=True)
            if msg.get("method") == "get_historical_candles":
                tf = int(data.get("timeframe") or 60)
                bars = to_bars(history, tf)
                await ws.send(envelope(encoded, STREAM_HISTORICAL_CANDLES, tf,
                                       bars[-1][5] if bars else 0, candles_msg(tf, bars)))
                print(f"  sent {len(bars)} candles at {tf}s", flush=True)

    reader = asyncio.create_task(control())
    try:
        while state["pair"] is None:
            await asyncio.sleep(0.05)
        prev = live[0][0] if live else 0
        for ts, price, qty, buy in live:
            # Cap the wait so an overnight gap in the file is not an overnight
            # gap on the screen.
            await asyncio.sleep(min((ts - prev) / 1000.0 / speed, 5.0))
            prev = ts
            await ws.send(envelope(state["pair"], STREAM_TRADES, 0, ts,
                                   trade_msg(price, qty, buy, ts)))
        print("  file exhausted; holding the connection open", flush=True)
        await asyncio.Future()
    except websockets.ConnectionClosed:
        pass
    finally:
        reader.cancel()


def selftest():
    assert _ts_ms(1700000000) == 1700000000000
    assert _ts_ms(1700000000123) == 1700000000123
    assert _ts_ms(1700000000123456) == 1700000000123
    assert _ts_ms(1700000000123456789) == 1700000000123
    assert _ts_ms("2023-11-14T22:13:20Z") == 1700000000000
    assert _is_buy("SELL", False) is False and _is_buy("buy", False) is True
    assert _is_buy(True, True) is False          # buyer_maker=true -> seller hit
    assert _column({"Price": 1}, PRICE_KEYS) == "Price"
    bars = to_bars([(0, 10, 1, True), (1000, 12, 2, True), (60_000, 9, 1, False)], 60)
    assert bars == [(10, 12, 10, 12, 3, 0), (9, 9, 9, 9, 1, 60_000)], bars
    print("ok")


async def main(a):
    trades = load(a.file)
    cut = max(1, int(len(trades) * a.history))
    history, live = trades[:cut], trades[cut:]
    print(f"{a.file}: {len(trades)} trades, {len(history)} as history, "
          f"{len(live)} live at {a.speed}x")
    async with websockets.serve(lambda ws: serve(ws, history, live, a.speed),
                                "0.0.0.0", a.port, max_size=None):
        print(f"open the terminal with  ?ws=ws://localhost:{a.port}")
        await asyncio.Future()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("file", nargs="?", help=".csv or .parquet of trades")
    p.add_argument("--speed", type=float, default=1.0, help="replay multiplier")
    p.add_argument("--history", type=float, default=0.8,
                   help="fraction of the file sent as candles (0-1)")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--selftest", action="store_true")
    a = p.parse_args()
    if a.selftest:
        selftest()
    elif not a.file:
        p.error("a .csv or .parquet file is required")
    else:
        try:
            asyncio.run(main(a))
        except KeyboardInterrupt:
            pass
