#!/usr/bin/env bash
set -euo pipefail
# Run after configuring the WASM build (provides the pinned JSON header).
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$repo/build-threaded"}
mkdir -p "$build/stream-test"
em++ -std=c++20 -O1 -include span -I"$repo/src" -I"$build/include" \
    "$repo/tests/wasm/stream_lifecycle_test.cpp" "$repo/src/stream_handler.cpp" "$repo/src/types/types.cpp" \
    -sENVIRONMENT=node -o "$build/stream-test/lifecycle.js"
node "$build/stream-test/lifecycle.js"
