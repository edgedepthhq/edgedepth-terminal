# Replay packs (`.edpack`)

[Data sources](DATA_SOURCES.md) · [Replay Library manifest](../replay-library/README.md)

A pack is one recorded market episode, played locally without an account, live
feed or replay server. It contains only the streams actually recorded: candles,
trades, order book, liquidations or per-price volume. It is EdgeDepth's format,
not an industry standard.

## Getting a sample

Choose **Replay → Open Replay Library** in the terminal. Or, with the terminal
running locally, open this public TUT recording:

```text
http://localhost:8080/?pack=https%3A%2F%2Freplays.edgedepth.com%2Freplays%2Ftutusdt-short-squeeze%2Fv2.edpack&packsym=tutusdt
```

Pause, change speed or scrub with the replay controls. Footprints need completed
per-price volume minutes; older context candles may have none. See
[coverage](DATA_SOURCES.md#why-some-replay-candles-have-no-footprint).

## Hosting a pack

Pass `?pack=<URL-encoded HTTP(S) URL>&packsym=<symbol>` to the terminal.
For efficient loading, the host and CDN must:

- Support `Range` requests and return `206 Partial Content`.
- Allow cross-origin `GET` and the `Range` request header.
- Expose `Content-Range`, `Content-Length` and `ETag` to the browser.

If the server ignores ranges and returns `200`, the client downloads and retains
the **whole file** before playback. Cached/downloaded data can play without a
live feed; a remote pack still needs network access for blocks not yet fetched.

Use immutable pack URLs. The [library manifest](../replay-library/manifest.json)
records each file's `sha256` and `size_bytes`; verify downloads against them.

## Can I build one?

There is no public pack builder or CSV-to-pack exporter. The current builder
lives in EdgeDepth's private backend. To display your own trades today, use the
[CSV/Parquet adapter](DATAFRAME_WORKFLOW.md) or a [custom feed](DATA_SOURCES.md#bring-your-own-data).
Implementers can use the format below and the checked-in protobuf schema.

## File layout

| Offset | Size | Contents |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `EDPK` |
| 4 | 1 byte | Format version (`uint8`) |
| 5 | 4 bytes | Header length (`uint32`, little endian) |
| 9 | Header length | Protobuf `PackHeader` |
| 9 + header length | Remaining file | Optional v2 seed section, then compressed frame blocks |

All header offsets are relative to the **data section**. Absolute file offset
is `9 + header_length + offset`. Timestamps are Unix milliseconds, UTC.
The schema is [`protos/messages.proto`](../protos/messages.proto).

### Header

`PackHeader` carries event identity, exchange/symbol, start/end times, description,
tags, total frame count and the block index, plus these boot seeds:

| Field | Contents |
| --- | --- |
| `ob_seed` | Marshalled `BookUpdate` with `snapshot: true`. `ob_seed_src_ts_ms` identifies its source time. Deltas require this snapshot base. |
| `candle_seeds` | Marshalled `Candles`, one per timeframe; serves local candle-history requests. |
| `tick_size` (v2) | Instrument tick size; zero means unset. |
| `tick_volume_seed_ref` (v2) | Offset/length of a `TickVolumeUpdateBatch` for footprints and profiles. |
| `heatmap_seed_ref` (v2) | Offset/length of a `HeatmapSnapshotBatch` containing verbatim zstd frame payloads. `heatmap_seed_ts_ms` lists their column times for filtering before download. |

The large v2 seeds sit outside the header and are range-fetched on first use.

### Block index and framing

Each `PackBlockEntry` has `first_ts_ms`, `last_ts_ms`, `offset`, `length` and
`frame_count`. The builder targets 4 MiB of uncompressed data per block.
Each block is compressed independently with zstd; its frame checksum is disabled.
After decompression, read repeated records:

```text
uint32 little-endian length
PackFrame protobuf (exactly length bytes)
```

`PackFrame` contains `ts_ms`, `stream`, `timeframe` and `payload`. Stream numbers
come from the schema's `StreamType`; payload is the original marshalled stream
message. Supported archives can include analytics as well as raw market streams;
the format does not guarantee any particular stream is present.

The builder merges streams by timestamp, putting the order book first on ties.
Any disorder already inside a source stream is preserved, not repaired.

## How the terminal opens a pack

The reader fetches the 9-byte prefix, then the header, then blocks as needed.
Frames enter the same message router as live WebSocket data. The pack engine
supplies replay lifecycle messages and the playback clock.

**Seeking reconstructs from the opening seed.** Current packs have no intermediate
book checkpoints. The reader walks from block zero through the target, restoring
book events and trades; other streams start at the target. The clock waits while
this happens, so later seeks can take longer. Gaps never become valid depth.
See [`pack_replay_engine.cpp`](../src/replayer/pack_replay_engine.cpp).

## Integrity and determinism

- The container has **no internal checksum, hash or signature**. Verify the
  manifest SHA-256 and size; parseable corruption cannot be detected by parsing alone.
- The private builder verifies frame count, order, timestamps, stream,
  timeframe, payload and book seed against an independent source-archive read.
  Replaying the same sequence does not prove the source data was complete.
- A short prefix, wrong magic, unsupported version, invalid header or truncated
  block record causes an error. There is no repair or partial-record playback.

## Versioning and compatibility

This reader accepts **versions 1 and 2 only**. The prefix version byte is the gate;
optional protobuf fields do not make an unknown version safe to read. A future
v3 file is refused rather than partially interpreted.

A public pack covers one episode. Hosted [Research](https://edgedepth.com/features/research)
searches recorded history; its search and tick-replay coverage are separate.
Local pack playback requires no subscription.
