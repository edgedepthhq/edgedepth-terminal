# Contributing

Thanks for your interest. A few ground rules keep this maintainable by a very small team.

## Before you open a PR

- **Open an issue first for anything non-trivial.** Agreeing on the direction before you write code protects your time and ours.
- **Keep PRs small and single-purpose.** One widget, one fix, one behavior. Large or mixed PRs will be asked to split.
- **CI must pass.** Every PR is built with Emscripten; a red build won't be reviewed.

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

## Expectations

This project is maintained alongside a running product. Triage happens in batches, typically weekly, so a quiet few days doesn't mean your PR is ignored. Issues are welcome, but this is not a support contract: "doesn't build" reports need the info in the issue template or they'll be closed.
