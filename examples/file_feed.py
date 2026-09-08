#!/usr/bin/env python3
"""Replay one validated CSV/Parquet trade recording over the terminal wire.

See docs/DATAFRAME_WORKFLOW.md. Timestamps are preserved. This is sequential
file playback through the live transport, not a seekable .edpack replay.
"""
import argparse
import asyncio
import bisect
import csv
import json
import math
import time
from datetime import datetime
from collections import deque
from decimal import Decimal, InvalidOperation
from pathlib import Path

import websockets
from synthetic_feed import (
    STREAM_HISTORICAL_CANDLES, STREAM_TRADES, candle_msg, candles_msg,
    envelope, pair_msg, trade_msg, _buf, _f64, _int,
)

TS_KEYS = ("ts", "time", "timestamp", "ts_ms", "datetime", "date", "t")
PRICE_KEYS = ("price", "px", "p")
QTY_KEYS = ("qty", "size", "quantity", "amount", "volume", "q")
SIDE_KEYS = ("side", "is_buy", "buyer_maker", "is_buyer_maker", "m")
INVERTED_SIDE_KEYS = ("buyer_maker", "is_buyer_maker", "m")
MAX_ROWS = 250_000
MAX_BYTES = 64 * 1024 * 1024
MINUTE = 60_000


def _column(row, keys):
    lower = {k.lower(): k for k in row if k}
    return next((lower[k] for k in keys if k in lower), None)


def _ts_ms(value, unit="auto"):
    if isinstance(value, datetime):
        dt = value
    else:
        try:
            n = Decimal(str(value))
        except InvalidOperation:
            dt = datetime.fromisoformat(str(value).strip().replace("Z", "+00:00"))
        else:
            if not n.is_finite():
                raise ValueError("timestamp must be finite")
            if unit == "auto":
                unit = "ns" if n >= 10**17 else "us" if n >= 10**14 else "ms" if n >= 10**11 else "s"
            ts = int(n * {"s": 1000, "ms": 1, "us": Decimal("0.001"), "ns": Decimal("0.000001")}[unit])
            if not 0 < ts < 4_102_444_800_000:
                raise ValueError("timestamp must be between 1970 and 2100; check --time-unit")
            return ts
    if dt.tzinfo is None or dt.utcoffset() is None:
        raise ValueError("datetime needs a timezone; use UTC (Z or +00:00)")
    return _ts_ms(str(int(dt.timestamp() * 1000)), "ms")


def _is_buy(value, inverted=False):
    word = str(value).strip().lower()
    if word in ("buy", "b", "true", "1"):
        buy = True
    elif word in ("sell", "s", "false", "0"):
        buy = False
    else:
        raise ValueError(f"unknown aggressor side {value!r}; expected buy/sell or true/false")
    return not buy if inverted else buy


def load(path, time_unit="auto"):
    """Return stable timestamp-sorted (ms, price, base quantity, aggressor buy)."""
    path = Path(path)
    if path.stat().st_size > MAX_BYTES:
        raise ValueError("file exceeds 64 MiB example limit; export a smaller slice")
    if path.suffix.lower() == ".parquet":
        import pyarrow.parquet as pq
        source = pq.ParquetFile(path)
        if source.metadata.num_rows > MAX_ROWS:
            raise ValueError("file exceeds 250,000 rows; export a smaller slice")
        columns = [c for keys in (TS_KEYS, PRICE_KEYS, QTY_KEYS, SIDE_KEYS)
                   if (c := _column(dict.fromkeys(source.schema_arrow.names), keys))]
        rows = (r for batch in source.iter_batches(batch_size=4096, columns=columns) for r in batch.to_pylist())
        return _load_rows(rows, time_unit)
    with path.open(newline="", encoding="utf-8-sig") as source:
        return _load_rows(csv.DictReader(source), time_unit)


def _load_rows(rows, time_unit):
    out = []
    for index, row in enumerate(rows, 1):
        if index > MAX_ROWS:
            raise ValueError("file exceeds 250,000 rows; export a smaller slice")
        if index == 1:
            ts_c, px_c, qty_c, side_c = (_column(row, keys) for keys in (TS_KEYS, PRICE_KEYS, QTY_KEYS, SIDE_KEYS))
            missing = [name for name, col in zip(("time", "price", "quantity", "aggressor side"), (ts_c, px_c, qty_c, side_c)) if not col]
            if missing:
                raise ValueError("missing " + ", ".join(missing) + "; side is required, never inferred from price")
        try:
            ts = _ts_ms(row[ts_c], time_unit)
            price, qty = float(row[px_c]), float(row[qty_c])
            if not all(math.isfinite(v) and v > 0 for v in (price, qty)):
                raise ValueError("price and quantity must be finite and positive")
            buy = _is_buy(row[side_c], side_c.lower() in INVERTED_SIDE_KEYS)
        except (ValueError, TypeError, OverflowError) as exc:
            raise ValueError(f"row {index}: {exc}") from exc
        out.append((ts, price, qty, buy))
    if not out:
        raise ValueError("no trade rows")
    out.sort(key=lambda t: t[0])  # stable: preserve same-millisecond source order
    return out


def to_bars(trades, timeframe_s):
    step, bars = timeframe_s * 1000, []
    for ts, price, qty, _ in trades:
        bucket = ts // step * step
        if bars and bars[-1][5] == bucket:
            o, h, l, _, v, _ = bars[-1]
            bars[-1] = (o, max(h, price), min(l, price), price, v + qty, bucket)
        else:
            bars.append((price, price, price, price, qty, bucket))
    return bars


def trade_candles(trades, tf, as_of, count=1000):
    """Bounded candle messages, including supplied aggressor-side totals."""
    result = deque(maxlen=count)
    current = []
    buy = sell = 0.0
    nbuy = nsell = 0
    def encoded():
        return (candle_msg(current, tf, final=current[5]+tf*1000 <= as_of)
                + _f64(6,buy)+_f64(7,sell)+_int(8,nbuy)+_int(9,nsell))
    for ts, price, qty, side in trades:
        start = ts//(tf*1000)*(tf*1000)
        if current and current[5] != start:
            result.append(encoded())
            current = []
        if not current:
            current = [price,price,price,price,0.0,start]
            buy = sell = 0.0
            nbuy = nsell = 0
        current[1] = max(current[1],price)
        current[2] = min(current[2],price)
        current[3] = price
        current[4] += qty
        if side: buy += qty; nbuy += 1
        else: sell += qty; nsell += 1
    if current: result.append(encoded())
    return result


def minute_levels(trades, start, end):
    """Only whole minutes entirely inside [start,end); never include a future row."""
    result = {}
    for ts, price, qty, buy in trades:
        minute = ts // MINUTE * MINUTE
        if minute < start or minute + MINUTE > end:
            continue
        level = result.setdefault(minute, {}).setdefault(price, [0.0, 0.0, 0])
        level[0 if buy else 1] += qty
        level[2] += 1
    return result


def tick_volume_msg(start, levels):
    encoded = b""
    buy = sell = 0.0
    count = 0
    for price, (b, s, n) in sorted(levels.items()):
        encoded += _buf(1, _f64(1, price) + _f64(2, b) + _f64(3, s) + _f64(4, b+s) + _int(5, n))
        buy += b
        sell += s
        count += n
    # The decoder explicitly accepts raw inner protobuf as well as zstd.
    poc = max(sorted(levels), key=lambda p: sum(levels[p][:2]))
    return (_int(2, start) + _int(3, MINUTE) + _int(4, start) + _int(5, start+MINUTE)
            + _buf(6, encoded) + _f64(7, buy+sell) + _f64(8, buy) + _f64(9, sell)
            + _int(10, count) + _f64(11, max(levels)) + _f64(12, min(levels)) + _f64(13, poc))


def profile_msg(minutes, tick):
    if not minutes:
        return b""
    grouped = {}
    for levels in minutes.values():
        for price, (buy, sell, count) in levels.items():
            row = math.floor(price / tick + 1e-9) * tick
            dest = grouped.setdefault(row, [0.0, 0.0, 0])
            dest[0] += buy
            dest[1] += sell
            dest[2] += count
    rows = sorted(grouped.items())
    volumes = [v[0]+v[1] for _, v in rows]
    total = sum(volumes)
    poc = max(range(len(rows)), key=lambda i: volumes[i])
    lo = hi = poc
    va = volumes[poc]
    while va < total * .7 and (lo > 0 or hi < len(rows)-1):
        above = volumes[hi+1] if hi+1 < len(rows) else -1
        below = volumes[lo-1] if lo else -1
        if above >= below:
            hi += 1
            va += volumes[hi]
        else:
            lo -= 1
            va += volumes[lo]
    out = _int(1, min(minutes)) + _int(2, max(minutes)+MINUTE)
    for i, (price, (b, s, n)) in enumerate(rows):
        out += _buf(3, _f64(1, price)+_f64(2, b)+_f64(3, s)+_f64(4, b+s)+_int(5, n)
                    +_f64(6, (b+s)/total*100)+_int(7, i == poc)+_int(8, lo <= i <= hi))
    return (out + _f64(4, rows[poc][0])+_f64(5, rows[hi][0])+_f64(6, rows[lo][0])
            +_f64(7, total)+_f64(8, va)+_int(9, poc)+_int(10, hi)+_int(11, lo))


async def serve(ws, trades, cut, speed, exchange, symbol, resume=None):
    pair = pair_msg(exchange, symbol)
    subscriptions = set()
    started = asyncio.Event()
    if resume is not None:
        cut = max(cut, resume["cursor"])
    state = {"cursor": cut}
    timestamps = [t[0] for t in trades]
    # A partial opening minute cannot claim whole-minute coverage.
    coverage_start = (trades[0][0] + MINUTE - 1) // MINUTE * MINUTE

    async def send(stream, tf, ts, data):
        await ws.send(envelope(pair, stream, tf, ts, data))

    async def control():
        async for raw in ws:
            try:
                req = json.loads(raw)
                if not isinstance(req, dict):
                    raise ValueError("request must be an object")
                data = req.get("data") or {}
                if not isinstance(data, dict):
                    raise ValueError("data must be an object")
                target = data.get("pair") or {}
                if target != {"exchange": exchange, "symbol": symbol}:
                    continue  # Never relabel a recording as another instrument.
                method = req.get("method")
                tf = int(data.get("timeframe") or 60)
                key = (int(data.get("stream") or 0), tf if int(data.get("stream") or 0) == 2 else 0)
                if method == "subscribe":
                    if key[0] not in (1, 2, 17) or (key[0] == 2 and tf not in (1,5,15,30,60,300,900,1800,3600,14400,86400)):
                        continue
                    subscriptions.add(key)
                    if key == (STREAM_TRADES, 0):
                        started.set()
                elif method == "unsubscribe":
                    subscriptions.discard(key)
                else:
                    cursor = state["cursor"]
                    available_end = trades[cursor][0] if cursor < len(trades) else trades[-1][0] + 1
                    end = min(int(data.get("end_time") or available_end), available_end)
                    start = max(int(data.get("start_time") or coverage_start), coverage_start)
                    available = trades[:min(cursor, bisect.bisect_left(timestamps, end))]
                    if method == "get_historical_candles" and tf in (1,5,15,30,60,300,900,1800,3600,14400,86400):
                        count = max(1, min(int(data.get("count") or 500), 1000))
                        bars = trade_candles(available, tf, end, count)
                        await send(8, tf, end, _int(1,tf)+b"".join(_buf(2,bar) for bar in bars))
                    elif method == "get_footprint_history":
                        for minute, levels in sorted(minute_levels(available, start, end).items()):
                            await send(17, MINUTE, minute, tick_volume_msg(minute, levels))
                        await send(17, 0, 0, b"")  # End-of-history sentinel even when empty.
                    elif method == "get_volume_profile":
                        tick = float(data.get("tick_per_row") or .01)
                        if not math.isfinite(tick) or tick <= 0:
                            raise ValueError("tick_per_row must be positive and finite")
                        await send(26, 0, end, profile_msg(minute_levels(available, start, end), tick))
            except (ValueError, TypeError, KeyError, OverflowError) as exc:
                print(f"invalid request: {exc}", flush=True)

    async def playback():
        await started.wait()
        if cut >= len(trades):
            await asyncio.Future()
        prev = trades[cut][0]
        while state["cursor"] < len(trades):
            index = state["cursor"]
            ts, price, qty, buy = trades[index]
            await asyncio.sleep((ts-prev)/1000/speed)  # Preserve gaps and stated speed.
            while (STREAM_TRADES, 0) not in subscriptions:
                await asyncio.sleep(.05)
            state["cursor"] = index + 1
            if resume is not None:
                resume["cursor"] = max(resume["cursor"], index+1)
            await send(1, 0, ts, trade_msg(price, qty, buy, ts))
            for stream, tf in tuple(subscriptions):
                if stream == 2:
                    begin = bisect.bisect_left(timestamps, ts // (tf*1000) * (tf*1000))
                    bar = trade_candles(trades[begin:index+1],tf,ts,1)[-1]
                    await send(2, tf, ts, bar)
            if ts // MINUTE != prev // MINUTE:
                minute = prev // MINUTE * MINUTE
                levels = minute_levels(trades[:index], max(minute, coverage_start), ts // MINUTE * MINUTE)
                if minute in levels:
                    await send(17, MINUTE, minute, tick_volume_msg(minute, levels[minute]))
            prev = ts
        print(f"{exchange}/{symbol}: file exhausted; restart this feed and reload the page to replay again", flush=True)
        await asyncio.Future()

    tasks = [asyncio.create_task(control()), asyncio.create_task(playback())]
    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    except websockets.ConnectionClosed:
        pass
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def main(args):
    trades = load(args.file, args.time_unit)
    if trades[-1][0] > int(time.time() * 1000):
        raise ValueError("future timestamps cannot render closed-minute analysis in live transport; export a past slice")
    if not math.isfinite(args.speed) or not 0 < args.speed <= 1000 or not 0 <= args.history < 1:
        raise ValueError("use 0 < --speed <= 1000 and 0 <= --history < 1")
    cut = min(len(trades)-1, int(len(trades)*args.history))
    # Start playback at a minute boundary in the source, avoiding a split candle.
    boundary = trades[cut][0] // MINUTE * MINUTE
    cut = bisect.bisect_left([t[0] for t in trades], boundary)
    if not args.symbol.isalnum() or args.symbol != args.symbol.lower():
        raise ValueError("--symbol must be the terminal's lowercase market code")
    print(f"{args.file}: {len(trades)} trades; {cut} history, {len(trades)-cut} playback at {args.speed}x")
    print("Quantity is base-asset units. Side is supplied aggressor side. DOM/depth unavailable.")
    resume = {"cursor": cut}
    async with websockets.serve(lambda ws: serve(ws, trades, cut, args.speed, args.exchange, args.symbol, resume),
                                args.host, args.port, max_size=65536, max_queue=16):
        print(f"http://localhost:8080/terminal/{args.symbol}?exchange={args.exchange}&ws=ws://localhost:{args.port}", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", help="one instrument's CSV or Parquet trade recording")
    parser.add_argument("--symbol", required=True, help="explicit terminal symbol, e.g. btcusdt")
    parser.add_argument("--exchange", default="binancef", choices=("binancef", "hl"))
    parser.add_argument("--speed", type=float, default=1)
    parser.add_argument("--history", type=float, default=.8)
    parser.add_argument("--time-unit", choices=("auto", "s", "ms", "us", "ns"), default="auto")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    try:
        asyncio.run(main(parser.parse_args()))
    except (ValueError, OSError) as exc:
        parser.exit(2, f"file_feed: {exc}\n")
    except KeyboardInterrupt:
        pass
