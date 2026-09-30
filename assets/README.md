# README captures

These assets were captured on 1 October 2026 (Pacific/Auckland) from the OSS
terminal at source revision `72e61fa`, using its existing local Release build
and the Codex in-app browser. No hosted account or analytics feed was used.

All market data comes from the public TUT v2 replay pack in
[`replay-library/manifest.json`](../replay-library/manifest.json): Binance Futures,
9 August 2026, 06:50:00 to 07:17:25 UTC. The UI displays UTC+12.
Pack SHA-256: `f5959145721176c3a05ac67a56ceffefc682226e114f5be9be3e09989df88fe3`.

- `oss-realtime-20261001.png`: 1280 × 720, observed depth, trade bubbles and the attached DOM.
- `oss-footprint-20261001.png`: 1280 × 720, closed-minute footprints, paused at 07:01:28 UTC. The forming minute is explicitly awaiting its close.
- `oss-realtime-20261001.gif`: looping 20-second README preview, converted from the MP4 at 840 × 472 and 5 frames/second with a 128-color palette. The original MP4 remains available for full-quality playback.
- `oss-realtime-20261001.mp4`: 20-second real-time-view replay clip at 1280 × 720. Screen captures were sampled at roughly 6 frames/second and encoded as H.264 at 30 frames/second using capture timestamps. This is a workflow illustration, not a rendering benchmark.

No market data, gaps, values or UI labels were painted over. Status-bar FPS is
an observation from this setup, not a performance promise. These local captures
do not establish which image digest or hosted build a visitor is running.

Older assets remain for existing documentation links; the root README uses the
October captures above.

## Build and media checksums

- Local `build/index.wasm`: `171354494cc0147ed7f47dfe28c3f61fa21eeeb433cc0cd271fad5fd43f17d52`
- `oss-realtime-20261001.png`: `a12d1068149588758e17ee7e20d37ed723ecf2cbe25e25cffe2f092b5b94d366`
- `oss-footprint-20261001.png`: `123a9fbe70b062b0256e4c70f50262c0d7a4925a6af6b19980c52206fce67c55`
- `oss-realtime-20261001.mp4`: `b9d0b437f42f9122598f290cca726fa01825c9505cc0a51b32ef45b271c15753`

- `oss-realtime-20261001.gif`: `69797852d4a57fe38a615e679a2b182aa2bc753ebf8b8a38caf4decb8eed8615`
