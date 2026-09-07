# Real-time depth and trade bubbles

Select **RT** in the chart's timeframe menu. The default view combines observed
resting liquidity, historical best bid/ask steps and received trade records.
It opens without candles. The right gutter shows the current fresh book.

![Live IOST RT depth and trade bubbles](../assets/terminal-rt-live.png)

*Real IOST/USDT market data through the community gateway, captured from a local
browser build. The gateway requests Binance depth at 100ms; this is not a
measurement of the hosted feed's cadence or performance.*

## Reading a bubble outside the spread

The **center** is the received trade's timestamp and execution price. Its radius
represents size, so its edge can cross either quote even when its center is at
that quote. Green means buy aggressor; red means sell aggressor. This does not
identify a trader or say whether they opened or closed a position.

The quote lines come from synchronized depth observations, keeping the first
actual event in each 100ms display bin. Trades retain their own source timestamps.
These are separate streams, not an atomic quote-and-trade record. Prices can move
between depth observations, and an aggressive order can execute at several prices.
A trade center outside the *displayed sampled* spread is therefore possible.
It is not proof of an erroneous fill, nor proof that the sampled quote was the
executable quote at that exact instant. Never clamp trades to the drawn lines.
[Binance documents the separate depth and trade streams](https://developers.binance.com/en/docs/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/public).

Each bubble is one received record. A source may aggregate fills, and this wire
format has no trade IDs for reliable deduplication. The renderer preserves the
received price, time, quantity and side; it cannot verify every exchange fill
from a screenshot.

## Settings

| Location | Control | Effect |
| --- | --- | --- |
| Timeframe > RT | Pause display | Live only: freeze the displayed book, clock and trades while collection continues. Replay uses its transport pause. |
| Timeframe > RT | 1s observed candles | Show partial one-second OHLC from received trades. These are not historical candle backfill. |
| Timeframe > RT | Trade-price line | Connect eligible observed trade prices. |
| Timeframe > RT | Trade bubbles | Show or hide trade markers. |
| Timeframe > RT | Auto market size | Default on. Minimum is based on the 75th percentile of received quote notionals over 60 seconds. After 32 records, changes are bounded to 25% every five seconds. |
| Timeframe > RT | Minimum trade value | With auto off, set price times quantity in quote units. For a USDT pair, the threshold is in USDT. Default manual value: 10,000. |
| Layers > Depth settings | Fidelity | UHD/HD/SD/LD/ULD group 1/2/5/10/20 native price ticks. RT keeps the chosen grouping fixed; candle-mode zoom adaptation is separate. |
| Layers > Depth settings | Recalibrate colors | Explicitly recalibrate brightness from the currently visible liquidity. This deliberately recolors history; normal feed updates do not. |
| Chart navigation | Time zoom / Follow | Show five seconds to two minutes; Follow returns to the advancing edge. Price auto-fits eligible visible trades and quote steps. |

Bubbles use square-root radius scaling from 7px to a 28px cap. Up to 20,000 trade
records/two minutes are retained, and the newest 1,500 qualifying records in view
are drawn. Auto size is independent of chart zoom. A changing auto threshold can
change which historical trade markers qualify; it does not change heatmap cells.

## Why historical depth stays fixed

An observation owns its original time, prices and quantities. New columns and
GPU rebuilds use the same absolute price grid, including after the oldest samples
expire. RT does not automatically regroup historical rows as the price axis fits.
The color scale calibrates from grouped visible liquidity at the 98th percentile,
then stays fixed. A large new order cannot recolor all earlier observations.

Changing fidelity explicitly recalibrates the scale and redraws price groups;
Recalibrate colors updates brightness without changing the price grouping;
replay resets start a new traversal. The fixed scale can saturate unusually large
new orders or make thinner new liquidity look dim. Recalibrate only when you
want a new reference for the visible market. Scrolling and price auto-fit still change screen coordinates for the
whole chart. Those axis transformations are distinct from rewriting a past price
level or quantity. The history remains anchored to market coordinates.

## Replay: verified workflow and current limitation

![TUT recorded RT depth paused with the replay transport](../assets/terminal-rt-replay.png)

*Actual TUT v2 pack playback, paused on 9 August 2026. The screenshot uses the
Pacific/Auckland display timezone (UTC+12). This is a local recording, not live
market data or proof of hosted deployment.*

1. Open the TUT recording from the Replay Library at its beginning.
2. Select **RT** in the chart timeframe menu.
3. Play forward to build observed depth. Use the transport speed controls to slow
   the tape, then pause to inspect it. All RT evidence is bounded by the playhead.
4. Use the same bubble and fidelity controls as live. Replay's transport replaces
   the live Pause display checkbox.

Continuous playback and pause were checked with the TUT v2 pack. Two paused chart
captures were pixel-identical. **Arbitrary seek/rewind is not yet a complete RT
workflow.** Seeking clears the previous traversal; retained future data cannot
paint backward. RT requires a valid seed followed by continuous depth deltas.
The current pack seek implementation reuses the opening seed while skipping to
the target, which can break that sequence. An in-buffer DOM restore also does not
certify sequence integrity. RT reports that it is waiting for synchronized depth
instead of presenting the restored DOM as verified history.

To study RT depth reliably in these packs, reopen at the start and play through
the move. Correct arbitrary seeking requires replaying the intervening orderbook
deltas or adding verified checkpoints. A trades-only recording can show bubbles
but cannot supply an orderbook heatmap. Hosted replay likewise depends on the
actual seed, continuity and stream coverage delivered by the replay service.

## Coverage

Depth stores up to 1,200 observations/two minutes and 512 levels per side. It
samples the first actual event per 100ms bin; it does not claim an exchange event
occurred at every bin boundary. Quiet intervals hold the last synchronized book.
Sequence breaks and transport interruptions wait for a fresh seed. The initial
history boundary is marked; no current book is painted into pre-join history.
Other candle/model overlays are omitted in RT. Feed cadence, recorded coverage
and display sampling are separate limits.
