# EdgeDepth Pack (`.edpack`)

`.edpack` is the deterministic replay container used by the EdgeDepth
terminal. EdgeDepth designed the format for this repository's replay
engine; it is not an industry standard and nothing outside EdgeDepth
reads it today.

One file holds an entire recorded market episode: the order book, the
trade tape, liquidations, per-price tick volume, the depth heatmap and
the baked candle history. The terminal plays it start to finish, scrubs
and seeks inside it, and reproduces the same frames in the same order
every time, with no feed, no account and no server beyond static file
hosting.

## Why it exists

The terminal's live path is a WebSocket carrying zstd-compressed
protobuf. That is fine for watching a market and useless for anything
you need to do twice. A pack is the same frame sequence written down:

- **Portable deterministic replay.** The same file yields the same
  frames in the same order on every run and every machine.
- **Repeatable debugging.** A rendering bug that only shows up during a
  liquidation cascade is reproducible instead of anecdotal.
- **No live feed required.** Packs replay offline, in CI, and on a
  laptop with the network off.
- **Reproducible fixtures.** A pack is a market-data test vector.
- **Distribution.** The Replay Library ships curated packs as static
  files behind a CDN.

## File layout

```
offset  size          contents
0       4             magic, ASCII "EDPK"
4       1             format version, uint8
5       4             header length, uint32 little endian
9       header_len    PackHeader, protobuf
9+len   ...           data section
```

The data section holds, in order, the optional v2 seed section and then
the frame blocks. Every offset inside `PackHeader` is relative to the
data section start, so absolute file offset is `9 + header_len +
offset`.

### Header

`PackHeader` is defined in [`protos/messages.proto`](../protos/messages.proto).
It carries the episode identity (`event_id`, `exchange`, `symbol`,
`start_ts_ms`, `end_ts_ms`, `description`, `tags`), the boot seeds, and
the block index.

Boot seeds let the terminal render a populated screen at frame zero
rather than an empty one:

- `ob_seed` is a marshalled `BookUpdate` with `snapshot: true`, plus
  `ob_seed_src_ts_ms` recording which archived row it came from. This
  same seed is re-delivered after every seek, because order-book deltas
  are rejected without a snapshot base.
- `candle_seeds` are marshalled `Candles` messages, one per timeframe,
  serving historical candle requests locally.

Version 2 adds three optional fields and a lazily fetched seed section
at the start of the data area: `tick_size`, `tick_volume_seed_ref`
(a marshalled `TickVolumeUpdateBatch` that serves footprint and volume
profile) and `heatmap_seed_ref` with `heatmap_seed_ts_ms` (verbatim
zstd heatmap frame payloads plus their column timestamps, so the header
alone can decide which columns are wanted). These are large, so they
sit outside the header and are range-fetched on first use rather than
taxing the boot fetch.

### Block index and framing

`PackHeader.blocks` is a `repeated PackBlockEntry`, each carrying
`first_ts_ms`, `last_ts_ms`, `offset`, `length` and `frame_count`.
Blocks are the seek granularity: one range GET plus one zstd decode per
block. Blocks are cut at a 4 MiB uncompressed target.

Each block decompresses to a sequence of length-prefixed records:

```
uint32 little endian record length
PackFrame protobuf of exactly that length
```

`PackFrame` is `{ ts_ms, stream, timeframe, payload }`. The `payload`
is a verbatim marshalled stream message, byte for byte what the live
WebSocket path would have carried, and `stream` is the `StreamType`
enum from the same proto file. Packs built from event archives carry
order book, trades, ticker, volumes, stats, liquidations, tick volume,
the depth and liquidation heatmaps, analytics, VPIN, positioning,
patterns, contagion, alerts and liquidation levels.

Timestamps are milliseconds since the Unix epoch, UTC. Frames are
ordered by `ts_ms` with the order book first at equal timestamps.
Source archives can carry internal disorder within a single stream and
that disorder is preserved rather than corrected, because the goal is
to reproduce what the live path would have delivered, not to improve on
it.

### Compression

zstd, per block, whole-block `EncodeAll`. The zstd frame checksum is
deliberately disabled.

## Integrity and determinism

**A pack carries no checksum of its own.** There is no CRC, no hash and
no signature anywhere in the container, and the zstd frame CRC is off.
Integrity is pinned one layer up: every entry in
[`replay-library/manifest.json`](../replay-library/manifest.json) carries
a `sha256` and a `size_bytes` for its pack, so a downloaded pack can be
verified against the manifest that advertised it.

Determinism is a property of how packs are built, not something the
file asserts about itself. EdgeDepth's builder re-reads the source event
archive with an independent iterator and requires the pack's frame
sequence to be byte-identical: same count, same order, same
`(ts_ms, stream, timeframe, payload)` for every frame, and an
independently converted order-book seed matching the header byte for
byte. A pack that fails that check is not published.

## Versioning and compatibility

The version byte is the gate. A reader accepts a closed range and
refuses anything outside it; this repository's engine accepts versions
1 and 2. Because every v2 addition is an optional protobuf field, a v1
reader would happily parse a v2 header and then miss the seeds it does
not know about, which is exactly why the byte exists and why the check
is a range rather than a minimum.

Forward compatibility is therefore explicit, not accidental: a future
v3 pack is refused by today's terminal rather than half-played.

## How the terminal opens a pack

```
?pack=<url-encoded pack URL>&packsym=<symbol>
```

The engine fetches the 9-byte prefix, learns the header length, fetches
the header, emits a synthetic `replay_joined`, then range-fetches
blocks as playback and seeking need them. Seeks binary-search
`blocks[].first_ts_ms`, range-GET the containing block, and re-deliver
the header's order-book seed.

Nothing about this path is special-cased for packs downstream: each
`PackFrame` is wrapped in a `WSPayload` and handed to the same message
router the live WebSocket path uses, and the replay lifecycle messages
are synthesized so the existing replay state machine is untouched.

**Hosting requirement.** Packs are read with HTTP range requests, so the
server and any CDN in front of it must allow the `Range` header and
answer `206 Partial Content`. A server that ignores `Range` and answers
`200` with the whole body still works, because the engine falls back to
keeping one whole-file copy, but it will pull the entire pack to play
the first second of it.

## Failure behavior

Packs fail closed, not silently:

- prefix shorter than 9 bytes, wrong magic, or a version outside the
  supported range puts the engine in its error phase before any frame
  is delivered;
- a header that does not parse as `PackHeader` is an error;
- a block whose final record is cut short reports a truncated record
  length at its offset rather than delivering partial frames.

There is no repair path and no partial-play mode. Because the container
has no internal checksum, a pack whose bytes were corrupted in a way
that still parses is not detectable from the file alone. Verify against
the manifest `sha256` when one exists.

## Getting a sample

The Replay Library ships curated packs as static files. Every entry in
[`replay-library/manifest.json`](../replay-library/manifest.json) has a
`pack_url`, a `sha256` and a `size_bytes`. To play one directly:

```
http://localhost:8080/?pack=https%3A%2F%2Freplays.edgedepth.com%2Freplays%2Ftutusdt-short-squeeze%2Fv1.edpack&packsym=tutusdt
```

Packs are free and need no account. Nothing in this format requires a
subscription.

## Can I build one?

Not yet, and this is the honest answer rather than a policy. The pack
builder lives in EdgeDepth's private backend alongside the event
archive it reads, so there is no public `edpack build` today. The
format above is complete enough to write a reader against, and the
proto file in this repository is the whole contract.

If you want to drive the terminal from your own data right now, the
supported path is the WebSocket contract rather than a hand-built pack:
see the "Bring your own data" section of the [README](../README.md) and
[`examples/synthetic_feed.py`](../examples/synthetic_feed.py).

## Relationship to hosted EdgeDepth

A pack is a single recorded episode someone chose to publish. The
hosted [EdgeDepth Research](https://edgedepth.com/features/research)
surface is the other end of the same idea: instead of replaying one
episode you already know about, you search the continuously recorded
history for every minute that matched a condition, and open any match
in tick replay. The container here is free and standalone; the
searchable record is the hosted product.
