# 01 — Marmel v0.1 (`b741f9e`, 2026-08-30): C++ translation

Faithful C++20 port of the **first** upstream commit of
[Na1w/marmel](https://github.com/Na1w/marmel) — `Marmel v0.1`
(`b741f9ef96d73a0c62fecb8b2745e62a3de93200`, +22 870 lines, 63 files).
Every Rust module is translated; prompts and the example config are copied
verbatim. Build: CMake 3.20+, g++/clang++ with C++20, pthreads; optional
libcurl for the live LLM backend.

```sh
cmake -S . -B build && cmake --build build -j
./build/marmel_tests        # 32 tests, no external framework
ctest --test-dir build
./build/marmel --help
```

---

## 1. Author's chain of thought (reconstructed from v0.1)

Reading the first commit as a whole, the author's design intent is:

1. **Manager + isolated Specialists, never mixed.** The Manager
   (`OrchestratorManager`) only plans, delegates and synthesizes; domain work
   happens exclusively in `delegate_task` calls that build an
   `IsolatedContext` of *exactly two messages* (role prompt + brief). No
   manager transcript ever leaks into a specialist. The code even guards
   (`guard_no_domain_work`) against the manager module doing domain work.
   *Why:* local 8–30B models lose instruction discipline with long contexts;
   isolation keeps each inference short, role-pure and retryable.
2. **The disk is the source of truth.** The execution plan
   (`.marmel/execution_plan.md`, `- [ ] [t-xxx]` checkboxes), the crash
   journal (`.session_frozen.json` + `.session_journal.json`) and the forced
   phase file survive crashes; `recover_frozen()` and plan auto-resume make
   restarts seamless. *Why:* autonomous runs are long and local backends die;
   crash-only persistence beats in-memory state.
3. **Distrust the model, verify everything.** Terminal markers
   (`MISSION COMPLETE` / `FAILED` / `REPLAN REQUIRED`, strict precedence),
   validator subagents with mandatory `leave_verdict`, unique-match `replace`,
   repetition/cycle detectors, XML rescue, empty-production nudges — v0.1
   already assumes the model will loop, hallucinate tools and emit malformed
   XML, and every one of those behaviors has a dedicated countermeasure.
4. **Deterministic core, swappable edges.** Pure logic (plan parsing, marker
   precedence, detectors, compaction ratios, dispatch policy) is fully unit-
   and integration-tested with canned doubles (`#[cfg(test)]` compiles the
   network out); the LLM backend is just an OpenAI-compatible SSE endpoint
   (Ollama/vLLM/OpenRouter). *Why:* iterate on the harness without spending
   GPU tokens.
5. **Tolerant input, strict state transitions.** Tool names have generous
   aliases (`run`, `exec`, `bash` → `run_command`) and argument keys have
   fallbacks (`cmd`, `script` → `command`), because weak models paraphrase —
   but plan check-off only fires on a genuine `MISSION COMPLETE` marker, and
   the double-gate defeats validator-`REVOKED` replays.

## 2. File correspondence (Rust → C++)

| Upstream (`/tmp/marmel-v01`) | This translation | Notes |
|---|---|---|
| `Cargo.toml` / `Cargo.lock` | `CMakeLists.txt` | std-only + optional libcurl |
| `.gitignore` | `gitignore-reference.txt` | kept for reference |
| `marmel.toml`, `marmel.toml.example` | `marmel.toml.example` (verbatim) | live LAN config not shipped |
| `prompts/*.md` (12) | `prompts/*.md` (verbatim) + `include/marmel/prompts.hpp` | generated embed, mirrors `include_str!` |
| `src/lib.rs` | `include/` layout itself | one dir/namespace per module |
| `src/main.rs` | `apps/marmel_main.cpp` | CLI, log rotation, MCP boot, raw/TUI |
| `src/types.rs` | `include/marmel/types.hpp`, `src/types.cpp` | 16-tool catalogue + order preserved |
| `src/tool_names.rs` | `include/marmel/tool_names.hpp` | header-only constants |
| `src/config.rs` | `include/marmel/config.hpp`, `src/config.cpp` | lookup order, merge rules, `~/` expansion |
| `src/prompts.rs` | `include/marmel/prompts.hpp` | generated from `prompts/` |
| `src/agent/context.rs` | `agent_context.hpp` + `src/agent.cpp` | 90→70 % compaction, 4-msg rebirth, 300 s prefill |
| `src/agent/phase.rs` | `agent_phase.hpp` + `src/agent.cpp` | checkbox regexes, marker precedence |
| `src/agent/loop.rs` | `agent_loop.hpp` + `src/agent.cpp` (+ `ManagerLoop`) | read-parallel/write-sequential, `"ok"` check-off literal |
| `src/agents/mod.rs` | `agents.hpp` + `src/agents.cpp` | live loop, validation loop, `REVOKED` rewrite |
| `src/agents/{coder,debugger,researcher,validator,generalist}.rs` | same files, `Specialist` subclasses | Coder+Researcher 7 tools, Debugger +PTY, Validator +verdict, Generalist `*` |
| `src/orchestrator/mod.rs` | `orchestrator.hpp` + `src/orchestrator.cpp` | unconditional depth gate, double-gated check-off |
| `src/orchestrator/registry.rs` | `agents.hpp` (`SpecialistRegistry`) | `terminal__` stripping, `*`/`prefix*` globs |
| `src/orchestrator/steer.rs` | `orchestrator.hpp` + `src/orchestrator.cpp` | §5.3 fallback, streaming `response` extractor |
| `src/orchestrator/freeze.rs` | `orchestrator.hpp` (`CrashJournal`) | frozen JSON + JSONL journal |
| `src/harness/mod.rs` | `harness.hpp` + `src/harness.cpp` | alias tables, Manager allowlist, 10 000-char truncation |
| `src/harness/fs.rs` | same files | char pagination, unique-match atomic `replace` |
| `src/harness/monitor.rs` | same files + `src/harness_monitor.cpp` | 50/5/1000/5 detectors, `call_text_` rescue |
| `src/harness/pty.rs` | same files | forkpty, 300 s one-shot, SIGKILL groups, cursor manager |
| `src/harness/search.rs` | same files | `path:line:` grep, sorted capped glob, `.gitignore` |
| `src/harness/workspace.rs` | `harness.hpp` (`Workspace`) | probe-write handshake |
| `src/llm/client.rs` | `llm.hpp` + `src/llm.cpp` (`ChatClient`) | 60 s watchdog, 503/429 ×3 backoff, index-merged tool calls |
| `src/llm/stream.rs` | same files (`StreamSink`, drivers) | `VecSink` test double, `?` nudge ×3 |
| `src/llm/thinking.rs` | same files (`ThinkingDemuxer`) | split-tag holdback, `+0.5/+0.1` recovery |
| `src/mcp/mod.rs`, `client.rs` | `mcp.hpp`, `src/mcp.cpp` | stdio JSON-RPC, 2024-11-05 handshake, tolerant boot |
| `src/ui/mod.rs` | `ui.hpp` + `src/ui.cpp` (`run_session`) | goal/turn loops, steer queue, parallel fan-out rule |
| `src/ui/raw.rs` | same files (`RawRenderer`) | `[assistant]…[done]` labels, 512-char chunks |
| `src/ui/tui.rs` | same files (`TuiRenderer`, ANSI) | same panels, keybindings, slash commands |
| `src/ui/tui_click_impl.rs` | `TuiRenderer::click_to_cursor` | grapheme cursor |
| `src/widget.rs` | `widget.hpp`, `src/widget.cpp` | `line L: msg` errors |
| `tests/*.rs` (8 files) | `tests/test_marmel.cpp` (32 cases) | same counts, markers and literals asserted |

## 3. Rust → C++ dependency mapping

| Rust crate | C++ equivalent used |
|---|---|
| `tokio`, `futures` | `std::thread` / `std::async` / `std::future` (sequential-equivalent loops) |
| `reqwest`, `eventsource-stream` | libcurl multi + hand-rolled SSE parser (`MARMEL_HAVE_CURL`; stub otherwise) |
| `serde`, `serde_json` | vendored `marmel::Json` (`include/marmel/json.hpp`, sorted objects) |
| `toml` | small built-in TOML subset parser (`src/config.cpp`) |
| `ratatui`, `crossterm` | ANSI renderer with identical state machine/keybindings (no TUI lib) |
| `portable-pty` | `forkpty`/`openpty` + `sh -c` wrappers (POSIX; stub elsewhere) |
| `tiktoken-rs` (cl100k) | `≈ chars/4` approximation — same thresholds/ratios, not exact BPE |
| `regex`, `ignore` | `std::regex` + minimal `.gitignore` walker |
| `unicode-width/segmentation` | UTF-8 codepoint iteration + boundary-safe slicing |
| `uuid` | `std::random_device`/`mt19937_64` v4 |
| `anyhow`/`thiserror` | `ToolError`/`ToolResult` + `std::runtime_error` |
| `tracing` | `stderr` diagnostics + `.marmel/marmel.log` rotation |
| `chrono`, `libc` | `std::chrono`, POSIX (`kill`, `chmod`, `getpwuid`) |
| `tempfile`, `wiremock` | `mkdtemp` + scripted `ChatFn` / `ScriptedRenderer` |

## 4. Intentional deviations (all documented in code)

- Token counts are approximate (`(codepoints+3)/4` + 3 framing tokens); all
  ratios, literals and invariants are exact.
- `handle_delegate_task`'s re-delegation guard is always on (Rust gates it to
  non-test builds); the test-suite is unaffected.
- `OrchestratorManager::delegate` reports unknown-agent/depth errors by
  throwing (Rust returns `Err`); tests assert the same message substrings.
- Mid-stream abort is honored between SSE events rather than via a 50 ms
  heartbeat thread; the 60 s/1800 s watchdogs and retry policy are exact.
- The steer arbitrator runs synchronously at turn boundaries (no background
  task); the JSON schema, extractor and §5.3 fallback are fully ported.
- `grep_search`/`glob` skip `.git/` and honor a practical `.gitignore`
  subset (no full gitignore-spec engine).
- `TuiRenderer` uses ANSI escapes instead of Ratatui; panels, keybindings
  (`Tab`, double-`Esc`, `Ctrl+D/P/A`, history, mouse-click stub) and slash
  commands match v0.1.

## 5. Config

Copy `marmel.toml.example` to `./marmel.toml` (or use `--config`), point
`backend_url` at an OpenAI-compatible endpoint (Ollama default
`http://localhost:11434/v1`), then `./build/marmel --raw "explain src"`.
