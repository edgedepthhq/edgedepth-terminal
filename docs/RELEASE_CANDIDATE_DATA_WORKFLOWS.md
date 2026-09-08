# Terminal data-workflow release candidate

Unreleased local changes, 8 September 2026. No version tag or image publication
is implied.

- Add deterministic dataframe CSV/Parquet export and a strict one-instrument display adapter.
- Correct profile market keys; refresh bounded history; label empty/partial coverage.
- Fix chart callback ownership across live/replay swaps, preventing a late stats callback from writing through a destroyed chart after exit.
- Cap stored footprint history and describe TPO as a candle-range approximation.
- Require Python tests to decode responses with the terminal schema in Ubuntu CI.

Before announcing: publish reviewed terminal and gateway revisions, pin both
image digests, and rerun the cold-start, file, live and pack-replay checks against
those published images. The repos version independently. Keep the previous
digests for rollback. Do not claim Windows, macOS, Safari, all venues, a fixed
FPS figure, historical completeness, or broker execution from this Linux test.
