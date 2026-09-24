#!/usr/bin/env bash
set -euo pipefail
# Run after configuring the WASM build (provides the pinned JSON header).
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$repo/build-threaded"}
mkdir -p "$build/candle-seam-test"
em++ -std=c++20 -O1 -include span -I"$repo/src" -I"$build/include" \
    "$repo/tests/wasm/candle_seam_test.cpp" "$repo/src/core/candle_manager.cpp" \
    "$repo/src/stream_handler.cpp" "$repo/src/types/types.cpp" \
    -sENVIRONMENT=node -o "$build/candle-seam-test/candle_seam.js"
node "$build/candle-seam-test/candle_seam.js"
