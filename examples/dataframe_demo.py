#!/usr/bin/env python3
"""Create identical, reproducible CSV and Parquet fixtures from a dataframe."""
import argparse
from pathlib import Path
import pandas as pd


def demo_frame():
    # Generated values, not a market capture or backtest result. Exact source
    # times make two exports comparable; rows cover 2 hours at one trade/second.
    i = pd.Series(range(7200), dtype="int64")
    return pd.DataFrame({
        "timestamp": 1788739200000 + i * 1000,  # 2026-09-07 00:00 UTC
        "price": (600000 + (i % 240 - 120) + (i // 240 % 5) * 50) / 10,
        "qty": (1 + i % 7) / 100,
        "side": i.map(lambda n: "buy" if n % 5 < 3 else "sell"),
    })


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("demo-data"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frame = demo_frame()
    frame.to_csv(args.output / "synthetic-btcusdt.csv", index=False)
    frame.to_parquet(args.output / "synthetic-btcusdt.parquet", index=False)
    print(f"Wrote {len(frame)} GENERATED trades to {args.output}; price in USDT, quantity in BTC.")
