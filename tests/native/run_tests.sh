#!/usr/bin/env bash
# Native unit tests for the pure, Emscripten-free modules.
# CMake/CTest is the portable entrypoint; this Bash file is a Linux/WSL
# convenience wrapper around the same build Windows CI runs.
set -euo pipefail
TEST_SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_BUILD_DIR"' EXIT

cmake -S "$TEST_SOURCE_DIR" -B "$TEST_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$TEST_BUILD_DIR" --config Release --parallel
cmake -E chdir "$TEST_BUILD_DIR" ctest -C Release --output-on-failure
