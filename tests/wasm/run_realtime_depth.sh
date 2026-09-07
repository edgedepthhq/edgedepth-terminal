#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$repo/build-threaded"}
cmake --build "$build" --target realtime_depth_regression --parallel 4
node "$build/realtime_depth_regression.js"
