# smallagent.cpp — Marmel in C++

Commit-by-commit C++ translation of the Rust agent
[Na1w/marmel](https://github.com/Na1w/marmel) (*marmennill — autonomous
coding harness*: Manager + Specialist subagents over any OpenAI-compatible
backend).

## Layout

Each upstream commit gets its own self-contained folder so the author's chain
of thought stays reviewable step by step:

- [`commits/01-marmel-v0.1`](commits/01-marmel-v0.1) — upstream `b741f9e`
  (`Marmel v0.1`, 2026-08-30): full C++20 port, CMake, 32 tests green.
- `commits/02-…` and on — future commits, one folder each, in upstream order.

## Build (per commit folder)

```sh
cmake -S commits/01-marmel-v0.1 -B build && cmake --build build -j
./build/marmel_tests && ./build/marmel --help
```

## Method

- One translation per upstream commit, oldest first; no squashing, so every
  design decision (`plan` lifecycle, validation loop, resilience harness,
  context engine, …) can be followed as the author made it.
- Fully corresponding: identical module structure, tool names, literals,
  thresholds and state machines; documented deviations only where a Rust
  crate has no std-only C++ counterpart (see each commit's README §4).
- Upstream `prompts/*.md` and configs are carried verbatim.

Remote: `github.com/MaurerAnton/marmel-cpp`.
