#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$repo/build-threaded"}
mkdir -p "$build/heatmap-test"
em++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -I"$repo/src" -I"$build" -I"$build/_deps/imgui-src" \
  -I"$build/_deps/implot-src" -I"$build/_deps/protobuf-src/src" \
  "$repo/tests/wasm/heatmap_live_test.cpp" "$repo/src/rendering/shader_heatmap_renderer.cpp" \
  -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -o "$build/heatmap-test/live.js"
node "$build/heatmap-test/live.js"
