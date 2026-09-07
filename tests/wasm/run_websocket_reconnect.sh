#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$repo/build-threaded"}
mkdir -p "$build/websocket-test"
em++ -std=c++20 -O1 -include span -I"$repo/src" -I"$build/include" \
    "$repo/tests/wasm/websocket_reconnect_test.cpp" "$repo/src/core/websocket.cpp" \
    "$repo/src/stream_handler.cpp" "$repo/src/types/types.cpp" \
    -sENVIRONMENT=node -o "$build/websocket-test/reconnect.js"
node "$build/websocket-test/reconnect.js"
