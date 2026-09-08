# Contributing

Thanks for your interest. A few ground rules keep this maintainable by a very small team.

## Before you open a PR

- **Open an issue first for anything non-trivial.** Agreeing on the direction before you write code protects your time and ours.
- **Keep PRs small and single-purpose.** One widget, one fix, one behavior. Large or mixed PRs will be asked to split.
- **CI must pass.** Every PR is built with Emscripten on Ubuntu and native Windows, and the host-side tests run on both platforms. A red build won't be reviewed.

## Code conventions (enforced in review)

- `snake_case` for functions and variables, C++20, no exceptions in hot paths.
- **No heap allocation in the render loop.** No `std::string` construction, no vector growth, no per-frame `new`. Pre-size, reuse, or cache.
- All price text goes through `PriceFormatter`. Never `printf("%f")` a price.
- All colors come from `Theme::Tokens::*`, never a hardcoded `ImVec4` in a widget. New colors start life in [`design/tokens.json`](design/README.md), then get mirrored into the theme.
- Match the surrounding style. This codebase favors explicitness over cleverness.

## What we're most interested in

- Feed adapters and gateway implementations for other exchanges and venues
- Indicators and widget improvements
- Build and tooling portability fixes
- Performance work, with before/after frame-time numbers please

## What will likely be declined

- Large refactors of working systems ("modernization" PRs)
- New dependencies. The dependency set is deliberately small and pinned
- Features that require EdgeDepth's closed backend to test

## Native tests

The host-side test suite is CMake/CTest based and does not require Emscripten,
protoc. The workspace test uses the terminal's existing nlohmann JSON 3.12.0
dependency, fetched on first configuration (or supplied through CMake's
`FETCHCONTENT_SOURCE_DIR_WORKSPACE_JSON` override for offline builds). It covers the parts of the terminal that are pure
enough to run off the browser: the Renko and TPO chart transforms, the order
book's sorted container and its publish-on-frame double buffer, the ingest
queue's time-budgeted drain, the indicator series cache, replay entitlements,
route parsing, and the liquidation heatmap's reach maths.

There is no test framework. Each test is a single
source file with its own `main()`, a file-static failure counter, and small
`expect_*` helpers. Follow the shape of an existing one; please do not introduce
gtest, Catch2, or doctest. Tests must be deterministic: no sleeps, no wall-clock
reads, and no unseeded randomness. Where a module needs time, inject it.

These commands work from Bash, PowerShell, or CMD:

```text
cmake -S tests/native -B build-native-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-tests --config Release --parallel
cmake -E chdir build-native-tests ctest -C Release --output-on-failure
```

Linux and WSL contributors can also run `bash tests/native/run_tests.sh`.

A new test needs one line in `tests/native/CMakeLists.txt`:
`add_terminal_native_test(<target> <source>.cpp)`. CI picks it up from there.
Everything is built with `-Wall -Wextra -Werror` on GCC/Clang and `/W4 /WX` on
MSVC, so watch signed/unsigned comparisons.

## Expectations

This project is maintained alongside a running product. Triage happens in batches, typically weekly, so a quiet few days doesn't mean your PR is ignored. Issues are welcome, but this is not a support contract: "doesn't build" reports need the info in the issue template or they'll be closed.
