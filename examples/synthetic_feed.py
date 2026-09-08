#!/usr/bin/env python3
"""
A synthetic EdgeDepth feed in one file, for driving the terminal with your own data.

    pip install websockets
    python3 examples/synthetic_feed.py

Then open the terminal against it:

    http://localhost:8000/?ws=ws://localhost:8765

The terminal accepts any ws:// or wss:// URL in the ?ws= query parameter, so
you can point it at a strategy, a simulator, or a replay of your own capture
without touching the C++ or running the EdgeDepth backend.

The wire format is two things:

  1. A control plane of JSON TEXT frames, keyed on "method" (not "type"):
     {"method":"subscribe","data":{"pair":{"exchange":..,"symbol":..},
                                   "stream":<int>,"timeframe":<int>}}
  2. Market data as ONE protobuf WSPayload per binary frame. No length prefix,
     no stream-id header byte. One frame, one payload.

Compression is optional: the client sniffs the zstd magic (28 B5 2F FD) and
passes anything else straight through, so this sends plain protobuf.

The protobuf is hand-encoded below, which is why this example needs no protoc
step and no protobuf package. Field numbers come from protos/messages.proto and
must stay in step with it. For a real venue adapter, generate the bindings
instead: see the Go gateway at github.com/edgedepthhq/edgedepth-gateway.

What shows up: the chart, the tape and the DOM. Candles are built client-side
from the trade stream, so trades alone drive the chart. Panels fed by streams
this example does not send (VPIN, positioning, scanner, heatmaps) stay empty,
exactly as they do on any feed that does not carry them.
"""

import asyncio
import json
import random
import struct
import time

import websockets

HOST, PORT = "0.0.0.0", 8765

# Stream ids from protos/messages.proto.
STREAM_TRADES = 1
STREAM_ORDERBOOK = 3
STREAM_HISTORICAL_CANDLES = 8

BASE_PRICE = 100.0
TICK = 0.01
BOOK_DEPTH = 150   # deep enough to fill the DOM ladder at its default grouping
HISTORY_BARS = 240

# The terminal subscribes to ticker24h on the sentinel pair <exchange>/global,
# which is a watchlist channel rather than a market. It arrives FIRST, before
# any real symbol, so anything that just latches onto the first subscription
# ends up addressing every frame to a pair no widget listens on.
SENTINEL_SYMBOL = "global"


# ---------------------------------------------------------------------------
# Minimal proto3 encoding: just the wire types these messages use.
# ---------------------------------------------------------------------------

def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b, n = n & 0x7F, n >> 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _tag(field: int, wire: int) -> bytes:
    return _varint((field << 3) | wire)


def _f64(field: int, value: float) -> bytes:
    return _tag(field, 1) + struct.pack("<d", value)


def _int(field: int, value: int) -> bytes:
    return _tag(field, 0) + _varint(value)


def _buf(field: int, raw: bytes) -> bytes:
    return _tag(field, 2) + _varint(len(raw)) + raw


def _str(field: int, value: str) -> bytes:
    return _buf(field, value.encode())


# ---------------------------------------------------------------------------
# Messages
# ---------------------------------------------------------------------------

def pair_msg(exchange: str, symbol: str) -> bytes:
    return _str(1, exchange) + _str(2, symbol)


def level_msg(price: float, size: float) -> bytes:
    return _f64(1, price) + _f64(2, size)


def trade_msg(price: float, qty: float, is_buy: bool, ts_ms: int) -> bytes:
    return _f64(1, price) + _f64(2, qty) + _int(3, int(is_buy)) + _int(4, ts_ms)


def book_msg(ts_ms: int, bids, asks, last_price: float) -> bytes:
    out = _int(1, ts_ms)
    for price, size in asks:
        out += _buf(2, level_msg(price, size))
    for price, size in bids:
        out += _buf(3, level_msg(price, size))
    return out + _int(4, 1) + _f64(5, last_price)   # field 4 = snapshot


def candle_msg(bar, timeframe_s: int) -> bytes:
    o, h, l, c, volume, ts_ms = bar
    return (_f64(1, o) + _f64(2, h) + _f64(3, l) + _f64(4, c) + _f64(5, volume)
            + _int(10, ts_ms) + _int(11, timeframe_s) + _int(12, 1))  # 12 = final


def candles_msg(timeframe_s: int, bars) -> bytes:
    out = _int(1, timeframe_s)
    for bar in bars:
        out += _buf(2, candle_msg(bar, timeframe_s))
    return out


def envelope(pair: bytes, stream: int, timeframe: int, ts_ms: int, inner: bytes) -> bytes:
    """WSPayload: pair, stream, timeframe, data, event_time_ms."""
    return (_buf(1, pair) + _int(2, stream) + _int(3, timeframe)
            + _buf(4, inner) + _int(5, ts_ms))


# ---------------------------------------------------------------------------
# Synthetic market
# ---------------------------------------------------------------------------

def make_history(timeframe_s: int, end_ms: int, count: int):
    """A random walk backwards from BASE_PRICE, oldest first."""
    bars, price = [], BASE_PRICE
    start = end_ms - count * timeframe_s * 1000
    for i in range(count):
        o = price
        c = max(TICK, o + random.gauss(0, BASE_PRICE * 0.0015))
        h, l = max(o, c) * 1.0008, min(o, c) * 0.9992
        bars.append((round(o, 2), round(h, 2), round(l, 2), round(c, 2),
                     round(random.uniform(5, 60), 3), start + i * timeframe_s * 1000))
        price = c
    return bars


def book_around(price: float):
    bids = [(round(price - TICK * (i + 1), 2), round(random.uniform(0.5, 25), 3))
            for i in range(BOOK_DEPTH)]
    asks = [(round(price + TICK * (i + 1), 2), round(random.uniform(0.5, 25), 3))
            for i in range(BOOK_DEPTH)]
    return bids, asks


async def serve(ws):
    """One connection: read subscriptions, stream trades and book back."""
    state = {"pair": None, "price": BASE_PRICE}

    async def control():
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            method, data = msg.get("method"), msg.get("data") or {}
            pair = data.get("pair") or {}
            symbol = pair.get("symbol")
            if not symbol:
                continue
            encoded = pair_msg(pair.get("exchange", ""), symbol)
            if state["pair"] is None and symbol != SENTINEL_SYMBOL:
                state["pair"] = encoded
                print(f"  streaming {pair.get('exchange')}/{symbol}", flush=True)

            if method == "get_historical_candles":
                tf = int(data.get("timeframe") or 60)
                count = min(int(data.get("count") or HISTORY_BARS), 1000)
                now = int(time.time() * 1000)
                bars = make_history(tf, now, count)
                state["price"] = bars[-1][3]
                await ws.send(envelope(encoded, STREAM_HISTORICAL_CANDLES, tf, now,
                                       candles_msg(tf, bars)))
                print(f"  sent {len(bars)} historical candles at {tf}s", flush=True)

    reader = asyncio.create_task(control())
    try:
        while True:
            await asyncio.sleep(0.1)
            if state["pair"] is None:
                continue
            ts = int(time.time() * 1000)
            pair = state["pair"]

            for _ in range(random.randint(1, 4)):
                state["price"] = max(TICK, state["price"] + random.gauss(0, TICK * 2))
                price = round(state["price"], 2)
                qty = round(random.expovariate(0.5) + 0.001, 3)
                is_buy = random.random() < 0.5
                await ws.send(envelope(pair, STREAM_TRADES, 0, ts,
                                       trade_msg(price, qty, is_buy, ts)))

            bids, asks = book_around(round(state["price"], 2))
            await ws.send(envelope(pair, STREAM_ORDERBOOK, 0, ts,
                                   book_msg(ts, bids, asks, round(state["price"], 2))))
    except websockets.ConnectionClosed:
        pass
    finally:
        reader.cancel()


async def main():
    async with websockets.serve(serve, HOST, PORT, max_size=None):
        print(f"synthetic feed listening on ws://localhost:{PORT}")
        print(f"open the terminal with  ?ws=ws://localhost:{PORT}")
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
