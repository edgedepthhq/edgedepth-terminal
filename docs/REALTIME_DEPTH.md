# Real-time depth

[Data sources](DATA_SOURCES.md) · [Replay packs](EDPACK.md)

Choose **Real-time** in the chart's timeframe menu to see the order book over
time, with trades on top. It works with a compatible live feed or a recording
containing a book snapshot and continuous deltas. A trade-only file has no depth.

![Real-time depth and trade bubbles in the OSS terminal](../assets/oss-realtime-20261001.png)

*Public TUT recording from 9 August 2026, replayed locally in the OSS build.
Captured 1 October 2026. [Try the recording](EDPACK.md#getting-a-sample).*

## Read the chart

| Element | Meaning |
| --- | --- |
| Heatmap | Observed resting bid/ask quantity. Brighter rows contain more quantity; they are not predictions or executed trades. |
| Bubbles | Received trades grouped into fixed 100 ms windows at their exact price. Up to eight busiest price groups per window are selected once. Green/red shows buy/sell aggression; mixed groups can contain both. |
| Price line / 1s candles | Prices from received trades. Trades and book updates arrive separately, so the line can briefly sit outside the displayed spread. |
| Depth ladder | Quantities on the chart's price grid. Prices label row centers, not executable quotes; both sides can share a grouped row. The header shows the separate best bid/ask. |
| CVD | Received buy minus sell base quantity in five-minute market-time windows. It starts when RT opens, not at the beginning of the exchange session. |

Bubbles settle after a short delay and retain their selected price, time and
quantity when you zoom. They do not show every fill or identify traders,
positions, entries or exits. Late/approximate records may be omitted; the RT
status reports these limits.

## Controls

Open the **Real-time** settings to:

- Show trade bubbles, the price line, observed 1s candles or the depth ladder.
- **Pause display** to inspect live history while collection continues. In replay,
  use the replay transport to pause.
- **Return to live** for the latest 30 seconds, or **Whole session** for retained history.
- Enable **Follow price** or **Auto fit visible history**. Manual price-axis
  navigation takes control until you enable following again.
- Use **Auto size** / **Recalibrate** for bubble sizing. The scale settles from
  received trades; it does not continually resize earlier bubbles.

**Layers** controls depth grouping and brightness. Grouped intensity uses mean
quantity per native tick; time bins average observed samples. Changing grouping
can recalibrate colors without changing the stored quantities.

**Extend current depth** holds a fresh book into the right margin for context.
It does not predict future liquidity or fill missing history.

## Coverage

| Source | Available history |
| --- | --- |
| Live working window | Up to five minutes of depth, sampled at 100 ms, with at most 512 levels per side. Recent trades are capped at five minutes / 20,000 records. |
| Browser archive | Best-effort 30-minute target, within a shared 256 MiB compressed storage budget and four active archives. Capture continues while paused or hidden. Browser storage limits can shorten coverage. |
| Startup history | A compatible feed may supply up to 30 seconds of recent observations at coarser resolution. The community gateway does not supply this backfill. |
| Replay pack | Only recorded coverage. Seeking restores book and trades from the opening seed; the replay clock waits for reconstruction. Later seeks can take longer. |

The first real observation in each 100 ms bin is retained. A synchronized book
can be held between updates while fresh; gaps, invalid chains and data older
than the 15-second freshness limit remain unavailable. The current book is never
painted backward into missing time.

The browser archive is temporary, not a recording or a cross-reload guarantee.
Source changes, replay seeks and closing the chart reset it. Switching back to
candles keeps collection running. **Clear history**
clears archived data; a recent working window can remain. Large paused views can
use substantial memory even though storage and visible columns are bounded.

## When something looks wrong

- **Empty depth:** check for a valid snapshot and continuous deltas. Candles and
  trades alone are insufficient. Wait for fresh coverage; reopening a gap does
  not recover it.
- **Sparse bubbles:** each window keeps at most eight price groups. Check the
  displayed coverage/counters before treating silence as no trading.
- **DOM moves while RT is paused:** an independent DOM can still be live. A
  linked depth ladder follows the RT display clock; check its mode label.
- **CVD restarts:** windows reset every five minutes. An overflow during a long
  pause also resets after a gap rather than presenting an unbroken total.
- **Buffered seek:** keep replay open while the recorded book is reconstructed.
  Missing/corrupt chains remain an error; close replay to return to live.

Startup backfill does not alter live DOM, CVD or alerts. Hosted history and
analytics require their own source coverage and access; changing the renderer
cannot recover data that was never recorded.

For implementation details, see [depth sampling](../src/core/realtime_history.h),
[bubble grouping](../src/core/realtime_bubbles.h), and the
[native regression tests](../CONTRIBUTING.md#native-tests).
