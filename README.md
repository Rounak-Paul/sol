# Sol

A modal-flow code editor written in C11, built on the [Causality](vendors/causality/) reactive UI library.

> **Status:** Early development — core editing, split-pane workspace, command panel, and file tree are functional. File I/O from the UI is in progress; plugin and job systems are dormant.

---

## Features

- **Rope-backed text buffers** — O(log n) insert/delete via a B-tree rope
- **Split pane workspace** — vertical/horizontal splits, cycle focus, previous-buffer toggle
- **Command panel** — NvChad-style which-key floating panel with prefix matching and fuzzy suggestions
- **Event bus** — priority-ordered pub/sub with queued posting and unsubscribe-safe dispatch
- **Key bindings** — loaded from `~/.sol/bindings.conf`, auto-seeded on first launch
- **File tree** — directory browser wired to the causality UI
- **Reactive UI** — fine-grained signal/effect system (Solid.js-style) via Causality

---

## Requirements

| Requirement | Version |
|---|---|
| CMake | ≥ 3.20 |
| C compiler | C11 (clang or gcc) |
| C++ compiler | C++17 (for Causality / VMA) |
| Vulkan SDK | ≥ 1.3 (LunarG) |
| Platform | macOS (primary), Linux (builds, untested) |

> The Vulkan SDK must be installed and `VULKAN_SDK` must be set in your environment. On macOS, install from [vulkan.lunarg.com](https://vulkan.lunarg.com).

---

## Building

```sh
git clone --recurse-submodules https://github.com/your-org/sol.git
cd sol
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target sol
```

The `sol` binary is written to `bin/sol`.

### Release build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sol
```

Tests are automatically excluded from Release builds.

---

## Running

```sh
./bin/sol                        # open current directory
./bin/sol path/to/file.c         # open a specific file
./bin/sol path/to/project/       # open a directory in the file tree
```

---

## Tests

Tests are only compiled in Debug (or when `BUILD_TESTING=ON`). They are excluded at compile time in Release builds via a CMake guard and a `#ifdef NDEBUG` compile error in the test harness.

### Run all tests

```sh
cmake --build build               # ensure everything is built
ctest --test-dir build/sol -V
```

### Run a single suite

```sh
./bin/sol_event_tests
./bin/sol_buffer_tests
./bin/sol_text_buffer_tests
./bin/sol_rope_tests
./bin/sol_integration_tests
./bin/sol_command_flow_tests
```

### Test suites

| Binary | What it covers | Tests |
|---|---|---|
| `sol_rope_tests` | B-tree rope: insert, remove, line indexing, iteration | 10 |
| `sol_event_tests` | Event bus: subscribe, publish, priority, queue drain | 15 |
| `sol_buffer_tests` | Split-pane system: create, close, splits, cycling, layout hit-test | 17 |
| `sol_text_buffer_tests` | Text editing: cursor, UTF-8, scroll, line count, file I/O | 23 |
| `sol_integration_tests` | Cross-subsystem: lifecycle events, focus history, edit events | 9 |
| `sol_command_flow_tests` | Command registry: normalisation, prefix matching, suggestions | 33 |

All test binaries run headless (no window, no Vulkan). Tests that touch `sol_buffer` use a lightweight signal stub (`tests/stubs/sol_causality_signals_stub.c`) instead of linking the full Causality dylib, so they work in CI without a display.

### Performance benchmarks

Each suite prints a `[BENCH]` line to stderr. These run automatically as part of the suite and report min/avg/max latency per operation.

---

## Project layout

```
sol/
├── sol/
│   ├── include/         Public headers (sol_*.h)
│   ├── src/
│   │   ├── core/        Event bus, rope, buffer system, text buffer, job, platform
│   │   └── ui/          Workspace, text view, command panel, file picker, status bar
│   ├── tests/
│   │   ├── test_harness.h   Lightweight test framework (SOL_CHECK_*, SOL_RUN, sol_bench)
│   │   ├── sol_validation.h Runtime invariant assertions (compiled out in Release)
│   │   ├── stubs/           Causality signal stubs for headless test builds
│   │   └── test_*.c         Test suites
│   └── CMakeLists.txt
├── vendors/
│   └── causality/       Reactive UI framework (vendored)
├── bin/                 Build output (gitignored)
├── CMakeLists.txt
├── LICENSE
└── NOTICE
```

---

## Key bindings

On first launch Sol writes `~/.sol/bindings.conf`. Edit this file to customise keymaps. The default bindings follow a leader-key model — press the leader to open the command panel, then a sequence to invoke an action.

---

## Architecture

Sol is layered bottom-up:

```
Causality (window, renderer, signals)
    └── SolSystemManager (service locator)
            ├── SolEventBus  — pub/sub backbone
            ├── SolBufferSystem — split-pane layout + buffer registry
            ├── SolInputRouter  — Causality key events → Sol commands
            ├── SolJobSystem    — worker-thread pool (dormant)
            └── SolPlugin       — dynamic plugin loader (dormant)
                    └── UI modules (workspace, text view, command panel, ...)
```

The UI layer reads and writes Sol state exclusively through the public `sol_*.h` APIs. Causality internals are never reached directly.

---

## License

Apache-2.0 — see [LICENSE](LICENSE).

Third-party components and their licenses are listed in [NOTICE](NOTICE).
