#!/usr/bin/env bash
# Native (host g++) unit tests for the pure, Emscripten-free modules.
# No CMake target on purpose: the project CMake is an Emscripten build, and
# these tests must stay runnable in seconds with nothing but a host compiler.
set -euo pipefail
cd "$(dirname "$0")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -o "$OUT/research_url_test" research_url_test.cpp
"$OUT/research_url_test"
