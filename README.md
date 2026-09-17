<h1 align="center">my_agent</h1>

<p align="center">
  <b>English</b> | <a href="README.zh-CN.md">简体中文</a>
</p>

<p align="center">
  <b>A terminal coding agent in C++26 — rebuilt from scratch, one TDD slice at a time.</b><br>
  A from-scratch reconstruction of <a href="https://github.com/1ay1/agentty">agentty</a>: own event loop,
  own transports, own tools, own retrieval — only the renderer is borrowed.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-26-blue?style=flat-square" alt="C++26" />
  <img src="https://img.shields.io/badge/CMake-%E2%89%A54.4-064F8C?style=flat-square" alt="CMake 4.4" />
  <img src="https://img.shields.io/badge/renders-via%20maya-8A2BE2?style=flat-square" alt="maya" />
  <img src="https://img.shields.io/badge/provider-localhost%20Ollama-green?style=flat-square" alt="Ollama" />
</p>

## Why my_agent?

This project exists to learn how a terminal coding agent actually works — by rebuilding one, not reading one. Every subsystem lands as a slice with its own red → green cycle, and the commits are the curriculum.

- **The event loop is the point.** A hand-built Elm-style core — `(Model, Msg) → Model` on top of `AsyncHost`, a worker pool, and wake signals. [maya](https://github.com/1ay1/maya) is deliberately used **for rendering only**; its `run<Program>` loop is banned in this repo, precisely because owning the loop is what the project is for.
- **The terminal is treated as an adversary.** Raw mode is restored via RAII *and* a signal handler (RAII doesn't run under SIGSEGV); the driver tracks a `Synced`/`Divergent` coherence state so a resize or a partial write can never leave ghost cells on screen. Every claim is verified against a real pty plus a virtual terminal model, using official Unicode 16 width data.
- **Local-first.** The model is a localhost Ollama server, retrieval runs fully offline (keyword-only when no embedding server is up), and memory/skills live in plain files under `.my_agent/` — Claude Code's `.claude/` directories are read as-is.
- **One binary, zero runtime deps.** Everything — SSE parsing, BM25, RRF fusion, the TUI — is in-house C++/STL on top of three small pinned libraries.

## Getting Started

Requirements: a C++26 toolchain, CMake ≥ 4.4, OpenSSL, and [Ollama](https://ollama.com) running locally.

> **The maya checkout is not in this repo.** The current branch needs a local
> [maya](https://github.com/1ay1/maya) working copy containing the
> `Composer::CaretMode::SolidCell` extension (not yet upstream) — place it at
> `./maya`, or point `FETCHCONTENT_SOURCE_DIR_MAYA` at it. Without it, CMake
> fails by design rather than silently pinning an old upstream.

```bash
git clone git@github.com:TCWW0/Agent.git my_agent && cd my_agent
# place your maya checkout at ./maya (see note above)
cmake -B build && cmake --build build -j
ollama pull qwen3.5:latest        # or any model you have
./build/my_agent_repl
```

Everything is configured through the environment:

| Variable | Default | Meaning |
|----------|---------|---------|
| `MY_AGENT_OLLAMA_HOST` | `localhost` | Ollama server host |
| `MY_AGENT_OLLAMA_PORT` | `11434` | Ollama server port |
| `MY_AGENT_MODEL` | `qwen3.5:latest` | Model to run |
| `MY_AGENT_PROFILE` | `write` | Permission profile: `minimal` · `ask` · `write` |
| `MY_AGENT_CONTEXT_LIMIT` | `8192` | Context budget shown in the status bar |
| `MY_AGENT_DOCS_DIR` | `./docs` | Corpus root for `search_docs` |

Not on a tty (pipe, CI, `| tee`)? The TUI refuses to start and the binary falls
back to a line-mode REPL with `[y/N]` permission prompts — a terminal-free
end-to-end path that stays green in tests.

## Features

<table>
<tr>
<td width="50%">

### 🧠 Remembers your facts
`remember` / `forget` tools write plain JSONL to `~/.my_agent/memory.jsonl` and `<project>/.my_agent/memory.jsonl`. Both scopes are re-injected into the system prompt **every turn** — a fact remembered mid-session is in the next request.

### 📖 Agent Skills
Drop a `SKILL.md` under `.my_agent/skills/` or `.claude/skills/` — project root overrides `~/.`, and skills already installed for Claude Code work without copying. Only the name + description tier enters the prompt; the body is loaded on demand by the `skill` tool, and resources are listed, not read.

### 📚 Local retrieval
`search_docs` over your docs folder: hybrid BM25 + dense embeddings fused with RRF, incremental corpus cache, graceful BM25-only fallback when no embedding server answers. Fully offline. [How it works ↓](#retrieval-rag)

</td>
<td width="50%">

### ⚡ Own event loop, no framework
`AsyncHost` — `dispatch` / `run_until_quiescent` / `model()` — driving a `(Model, Msg) → Model` reducer. Tools execute on a worker pool and wake the loop through a pollable signal; repaints coalesce into frame windows. The rendering seam is exactly two calls: `FrameBuffer::render()` + `commit()`.

### 🛡️ Permission profiles
Every tool declares an effect set (`ReadFs` · `WriteFs` · `Net` · `Exec`). `minimal` prompts on reads, `ask` prompts on writes, `write` allows the workspace; the `read` tool refuses paths outside your workspace, period.

### 🖥️ A TUI that survives reality
Fullscreen alt-screen, raw mode restored by RAII *and* by a crash handler (SIGSEGV leaves your shell usable), resize re-syncs the whole screen instead of leaving ghost cells, and the composer edits UTF-8 by character, not by byte.

</td>
</tr>
</table>

## Providers

The shipped entry point talks to a **local Ollama** server — no key, no cloud. The wire formats for **Anthropic** and **OpenAI** (request expansion, SSE stream decoding, interleaved tool-call accumulation) are implemented and covered by transport tests, but are not yet wired into `main`; they currently live one seam away at the library level.

```bash
MY_AGENT_MODEL=qwen3.5:latest ./build/my_agent_repl    # default
MY_AGENT_OLLAMA_HOST=192.168.1.10 MY_AGENT_OLLAMA_PORT=11434 ./build/my_agent_repl
```

## Retrieval (RAG)

`search_docs` retrieves a small, source-tagged set of passages instead of
dumping whole documents into the prompt. The engine is fully local; the only
optional network hop is a *localhost* Ollama server for embeddings, and without
one it degrades to BM25-only and keeps working.

```bash
export MY_AGENT_DOCS_DIR=~/my-project/docs   # or just create ./docs
ollama pull nomic-embed-text && ollama serve # optional: enables the dense half
```

<details>
<summary><b>The pipeline</b></summary>

1. **Corpus walk** — fingerprint every file (size + mtime) under the docs
   root, then chunk each document. First call builds the index; later calls
   only re-embed files whose fingerprints drifted, so the second build is
   seconds, not minutes.
2. **BM25** — keyword ranking over tokenized chunks (the always-on path;
   zero setup, zero network).
3. **Dense embeddings** — chunks and queries are embedded via localhost
   Ollama with instruction-tuned roles (document side vs. query side are
   prefixed differently, per the nomic/e5 convention).
4. **RRF fusion** — the two ranked lists are fused with Reciprocal Rank
   Fusion into one hybrid ranking; with no embedding server reachable the
   result is reported as BM25-only instead of silently missing half.

The index is built lazily on the first `search_docs` call — registering the
tool costs zero I/O. Retrieval quality is tracked by `rag_bench`, a
three-ladder benchmark (synthetic queries where BM25 should win, LLM-rewritten
probes where dense must rescue, cached per gold chunk):

```bash
./build/rag_bench ~/my-project/docs 5 100 nomic-embed-text:latest
```

</details>

## Keys

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `Enter` | Send (empty line is a no-op) | `y` / `n` | Approve / reject a pending tool |
| `Backspace` | Delete a whole UTF-8 character | `←` `→` `Home` `End` | Move within the input |
| `↑` / `↓` | Move across wrapped input lines | `^U` / `^C` | Clear the draft (never quits) |
| `Ctrl-D` | Quit — the only exit path | | |

## More

<details>
<summary><b>Architecture</b></summary>

- **Update loop**: `AsyncHost` (dispatch / `run_until_quiescent` / `model`)
  over a `(Model, Msg) → Model` reducer in `runtime/agent.cpp`. Streaming
  deltas, tool results, and permission responses all arrive as `Msg`s through
  one inbox; background work lands on a worker pool and pokes the loop through
  a pollable wake signal.
- **View**: `Model → Element` (`ui/view.cpp`, `ui/screen.cpp`), rendered by
  maya. The project uses exactly one maya seam — `FrameBuffer::render()` +
  `commit()` — and deliberately not `maya::run<App>`: the loop is the
  deliverable, not a detail to delegate.
- **Terminal driver**: the only place that touches `termios` or writes bytes.
  Alt-screen + raw mode restored in reverse order; a crash handler
  (async-signal-safe `write` + `tcsetattr`) covers the paths where destructors
  don't run. Screen coherence is a `std::variant`: `Synced` (front buffer ==
  terminal pixels, diffable) or `Divergent` (size changed or a write failed —
  full re-serialization, row-by-row with trailing erases, in a single write).
- **Transcripts**: markdown is scanned line-by-line with fence awareness, so a
  half-streamed code block renders as live text instead of flickering.
- **Headless**: a `headless_runner` drives the same loop without a terminal,
  pausing at permission gates for an external answer.

</details>

<details>
<summary><b>Testing</b></summary>

36 test binaries, 343 tests, all green. The suite goes beyond unit tests:

- **Real ptys** — raw mode, resize, and byte-level output are verified on
  actual pseudo-terminals, not mocks.
- **A virtual terminal** — a from-scratch ECMA-48/DEC model that decodes what
  the app *actually emitted* onto a grid, so assertions read "the screen
  shows X", not "the output contains byte sequence Y". It preserves old
  content across resize, which is how ghost cells get caught.
- **An official width oracle** — Unicode 16.0.0 East-Asian-Width data drives
  the same width table the renderer uses, so an emoji can never be
  mis-measured identically on both sides of the assertion.

Development itself is test-first: every slice lands as a failing test commit
(`red N`) followed by its implementation (`green N`), each referencing its
GitHub issue.

</details>

<details>
<summary><b>Repository layout</b></summary>

```
src/
  domain/     conversation model, profiles
  provider/   anthropic · openai · ollama transports + SSE framing
  http/       HTTPS client (cpp-httplib, OpenSSL pinned on)
  prompt/     system prompt assembly (environment, memory, skills)
  runtime/    agent loop · async host · worker pool · wake signals · headless runner
  tool/       registry, effects & permission policy, read, memory, skills, search_docs
  rag/        corpus · chunker · BM25 · embeddings · RRF hybrid · bench
  ui/         event loop, terminal driver, view/screen, markdown scanner,
              input editor & layout, status bar, repaint clock, resize watch
tests/        36 gtest binaries + virtual terminal + width oracle
```

Dependencies are pinned via FetchContent: googletest, nlohmann_json,
cpp-httplib — and the local maya checkout described in
[Getting Started](#getting-started).

</details>

<details>
<summary><b>Status</b></summary>

Done: the full agent loop with streaming and tool calls, permission
gates, memory, skills, local retrieval with benchmark, and the TUI (composer,
status bar, resize-safe rendering, crash restore).

Open (tracked as slice issues): wiring the Anthropic/OpenAI transports into
the entry point, resize hysteresis, and the layout height-budget contract for
very small terminals.

</details>

## About

A personal learning project — a reconstruction of
[agentty](https://github.com/1ay1/agentty), built slice by slice to understand
every layer of a terminal coding agent from the bytes up. No license yet;
the engineering notes under `docs/` are kept locally and not part of the repo.
