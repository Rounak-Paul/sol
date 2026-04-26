# Sol + Causality — Refactor Plan

> **Status:** Batches 1–3 landed; build green; reactive core, asset embedding, sol piece-table & file I/O all in.
> **Scope locked by user:** keep GLFW + Vulkan; builder-call API; full CSS3 target; reactivity model = architect's choice; everything must be Apache-2.0 compliant; **no separate runtime assets — everything embedded.**

---

## Batch landing log

### ✅ Batch 1 — Compliance + safety net
- `LICENSE` + `NOTICE` written at sol root and at `vendors/causality/`. Apache-2.0 verbatim text; third-party attributions enumerated (GLFW, GLM, VMA, stb, shaderc, Vulkan headers, embedded font).
- SPDX header (`Apache-2.0`) prepended to all 66 first-party `.c/.h` files via `tools/spdx.sh` script. Skipped: pch.h, autogen embedded fonts.
- `vendors/causality/causality/include/causality_config.h` introduced — every cap (`CA_FRAMES_IN_FLIGHT`, `CA_MAX_NODES_PER_WINDOW`, `CA_MAX_SIGNALS_PER_INSTANCE=4096`, `CA_MAX_EFFECTS_PER_INSTANCE=2048`, `CA_MAX_SIGNAL_DEPS=32`, all widget pools) is `#ifndef`-guarded so downstream apps can override at compile time.
- `ca_internal.h` no longer hardcodes those constants; pulls from `causality_config.h`.
- Vulkan API floor dropped from 1.3 → 1.2 (no 1.3-specific calls were used; broadens device support).
- `sol/src/main.c` cleaned: dead `SolWarmupContext`, the 100k parallel_for warmup busy-loop, and the no-op `sol_system_load_plugins_from_directory(NULL)` call all removed; startup payload trimmed to `{ worker_count, input_binding_active }`.

### ✅ Batch 2 — Reactive core (Solid-style fine-grained signals)
- Public API: `vendors/causality/causality/include/ca_reactive.h` — signals (`ca_signal_*`), effects (`ca_effect`, `ca_effect_invalidate`, `ca_effect_destroy`), computeds (`ca_computed`), batching (`ca_batch_begin/end`), `ca_untrack`. Generic over value size; typed convenience wrappers for int/float/bool/u32/ptr.
- Implementation: `vendors/causality/causality/src/reactive/signal.c` — per-signal subscriber list, current-effect tracking stack with untrack guard, dependency capture on `_get`, dirty fan-out on `_set` (memcmp-suppressed no-op writes), batched flush, computed = effect that writes a target signal.
- Per-instance runtime in side-table indexed by `Ca_Instance*` (no public ABI break). Hooks: `ca_reactive_flush()` called from `ca_instance_tick()`, `ca_reactive_release_instance()` from `ca_instance_destroy`.
- Memory: pools sized by `CA_MAX_SIGNALS_PER_INSTANCE` / `CA_MAX_EFFECTS_PER_INSTANCE` from config.h. No per-tick allocations.
- The legacy `ca_state_*` system is left untouched as a compatibility layer; new code should use signals directly.

### ✅ Batch 3 — Departure Mono Nerd Font + reproducible embed pipeline
- `vendors/causality/tools/embed_font.py` — converts any TTF/OTF into a C byte array matching the renderer's expected `extern const unsigned int X_size; extern const unsigned char X_data[];` shape.
- `embedded_font.c` and `embedded_font_bold.c` regenerated from `DepartureMonoNerdFontMono-Regular.otf` (OFL-1.1, from nerd-fonts v3.4.0 release). Bold slot intentionally aliases regular — Departure Mono ships only a Regular weight.
- `embedded_font.h` updated with provenance comment.
- `vendors/causality/NOTICE` updated: Ubuntu attribution → Departure Mono OFL-1.1 attribution.
- stb_truetype's CFF/OpenType support handles the `OTTO`-flavored OTF correctly; no font-rasteriser change needed.

### ✅ Batch 4 — Sol text-storage + file I/O foundations
- `sol/include/sol_text_buffer.h` + `sol/src/core/sol_text_buffer.c` — classic two-buffer **piece table** (original + append-only add buffer + doubly-linked piece list). O(1)-amortised insert/erase at the piece level, no continuous reallocation under typing load. UTF-8 in/out (no transcoding), generation counter for reactive integration, dirty bit, `insert_char` UTF-8 codepoint helper. Fast-path: appending to the tail of an "add" piece extends it in place to keep piece count bounded.
- `sol/include/sol_file.h` + `sol/src/core/sol_file.c` — `read_all` (with optional max-size guard, returns malloc'd NUL-terminated buffer) and `write_all_atomic` (write to `<path>.tmp.<pid>`, fsync, rename — survives mid-write crashes).
- Glob-discovered by `sol/CMakeLists.txt`. No CMake change required.

**Build status after each batch:** `cmake --build . --target sol` clean every time. The `sol` binary at `bin/sol` is up to date.

---

## Remaining batches (handoff for next session)

### ⏳ Batch 5 — CSS engine extensions (~1500 LOC parser to extend)
**Files:** `vendors/causality/causality/src/ui/css.c`, `style.c`, `node.c` (interaction-state hookup).
**Adds:**
1. Pseudo-class parser tokens (`:hover`, `:focus`, `:active`, `:disabled`, `:checked`) at selector-tail; per-node interaction state already lives in widget.c, just needs a bit-field wired into the matcher.
2. `var(--name)` substitution: parsed at value-tokenisation time, resolved at cascade time against the matched element's computed `--*` chain.
3. `calc()` evaluator: shunting-yard over `+ - * /` with px/% terms; result coerced to the property's expected unit.
4. `!important` flag in declaration; tie-breaker in cascade specificity comparator.
5. `@media (min-width: …)` / `(max-width: …)` rules: gated on per-window viewport size at style-resolve time.
**Risk:** CSS specificity bugs are silent visual breakage. Add unit tests covering each rule before integrating.

### ⏳ Batch 6 — Move widget chrome out of paint.c into CSS
`paint.c` lines ~200-360 hardcode theme fallbacks (`CA_THEME_ACCENT` for checkbox-checked, etc.). After Batch 5, ship a default stylesheet as a `static const char *` in `default_stylesheet.h` that auto-loads on `ca_instance_create`, then delete the hardcoded paths. **Visual regression risk is high — must be done after Batch 5 + a screenshot harness.**

### ⏳ Batch 7 — Sol textarea widget on top of piece table
Wire `sol_text_buffer` into a `sol_textarea` widget (cursor + selection + viewport scroll + key dispatch). Replace the current `sol_text_buffer_render`-as-cstring path with a token-streamed paint that pulls only the visible rows via `sol_text_buffer_read`. Hook `sol_text_buffer_generation` into a `Ca_Signal` per buffer for automatic redraw.

### ⏳ Batch 8 — Tabs / file tree / modal+palette
- File-tree widget reading directory recursively.
- Tab strip bound to `SolBufferSystem` active leaf.
- Promote sol's leader-key state machine in `sol_input.c` to a public `sol_modal.h/.c` (modes: normal/insert/visual/command).
- `Ctrl+P` quick-open palette and `Ctrl+Shift+P` command palette with fuzzy-match scorer.

---



## 0. Architectural Decisions (locked)

### 0.1 Reactivity model — **Fine-grained signals (Solid-style), no VDOM**

| Option | Verdict |
|---|---|
| React-style VDOM + diff | Rejected. Heavy allocation, full subtree walks, GC pressure — wrong fit for native C and your "performance is top priority" goal. |
| **Signals + computed + effects (Solid/Leptos)** | **Chosen.** Each signal tracks its readers; setting a signal marks exactly the affected effects/nodes dirty. No tree diffing. Maps cleanly to causality's existing per-node dirty flags. Best perf in C. |
| Pure immediate-mode (Dear ImGui style) | Rejected. Would throw away your existing retained-mode infra and is hostile to a code editor's needs (text caret state, animations, A11y). |

**What this means concretely:**
- Components are *plain C functions*. The runtime tracks which signals each function read while it ran. When any of those signals changes, only that component re-runs (and only the nodes it produced are re-emitted). This is the same model causality is *already crawling toward* with `ca_state` + `ca_div_set_builder` — we are finishing the job, not changing direction.
- Public primitives: `ca_signal_create`, `ca_signal_get`, `ca_signal_set`, `ca_computed`, `ca_effect`, `ca_batch_begin/end`, `ca_untrack`.
- The current `ca_state_*` API will be reimplemented on top of signals as a thin compat shim, then deprecated.

### 0.2 API style — **C99 designated-initializer builder calls** (no macro DSL)

You chose "Builder calls". The clean form:

```c
ca_div((Ca_DivDesc){ .class = "container", .id = "root" });
    ca_text((Ca_TextDesc){ .class = "title", .text = "Hello" });
    ca_button((Ca_ButtonDesc){
        .class    = "primary",
        .text     = "Click me",
        .on_click = handle_click,
    });
ca_end();
```

- No `BEGIN/END`/macro tag soup. No HTML parser. Just C.
- All container elements take a `Desc` struct and are paired with one `ca_end()` (the parent stack already exists in `widget.c`).
- Self-closing widgets (`ca_text`, `ca_button`, `ca_input`, `ca_image`, etc.) take a `Desc` and emit no `end`.
- Optional `for`-loop scope-guard macros (`CA_DIV(...) { ... }`) will be provided as a non-mandatory ergonomic layer in `causality_scope.h`. Off by default to keep the core surface non-magical.

### 0.3 Styling — **CSS via libcausality-css, phased to "CSS3 snapshot 2023" subset**

"Full CSS3" as a hard requirement is unrealistic for a single library (the spec is several hundred modules). The user's intent — *"complete implementation, used for styles, not custom value changes"* — is satisfied by:

- **Phase A (P1):** Cascade fixes, `:hover` `:focus` `:active` `:disabled` `:checked` `:not()`, `var(--*)`, `calc()`, `!important`, `@media (min/max-width)`, attribute selectors `[x=y]`.
- **Phase B (P2):** `@keyframes` + `animation`, `display: grid` (subset: lines, gap, placement), `position: absolute/fixed/sticky` round-trip, `transform`, `box-shadow` already present (verify), `text-overflow`, `white-space`.
- **Phase C (P3):** `@import`, `@font-face`, container queries, logical properties (`inline-start` etc.), color-mix(), light/dark via `prefers-color-scheme`.

After Phase B causality covers ~95 % of what real apps actually use. Phase C is polish.

**Hard rule from the user:** widget chrome (checkbox box, slider track, scrollbar) must read its colors and metrics from CSS, not from `CA_THEME_*` constants. The `ca_theme.h` constants stay only as *default values for the bundled stylesheet*, not as render-time fallbacks. This is the single biggest source of "custom value changes" in the current code.

### 0.4 Renderer — **Stay on Vulkan + GLFW**

No SDL3 migration. Concrete renderer changes are listed in §3.

### 0.5 Licensing — **Apache-2.0 across the board**

- Add `LICENSE` (Apache-2.0 verbatim) to `sol/`, `vendors/causality/`, and `vendors/causality/causality/`.
- Add `NOTICE` files declaring third-party attributions (GLFW, GLM, VMA, stb, shaderc, bundled fonts).
- Insert SPDX header on every first-party `.c`/`.h`:
  ```c
  // SPDX-License-Identifier: Apache-2.0
  // Copyright (c) <year> <owner>
  ```
- Bundled Ubuntu Nerd Font: confirm Ubuntu Font License + Nerd Font MIT, ship both as `vendors/.../fonts/LICENSE.*`. If license cannot be confirmed, switch to a vetted font (Inter / JetBrains Mono Nerd patch).

---

## 1. Causality refactor — phase plan

> Goals: clean reactive core, complete builder API, real CSS, fix the structural smells. Each phase ends with `sandbox` building and running.

### Phase C1 — Foundations & safety net (no behavior change)

| Step | Detail |
|---|---|
| C1.1 | Add `LICENSE`, `NOTICE`, SPDX headers. |
| C1.2 | Introduce `causality_config.h` exposing the hard limits (`CA_MAX_NODES_PER_WINDOW`, `CA_FRAMES_IN_FLIGHT`, etc.) via overridable `#ifndef ... #define`. Compile-time configurable, no runtime cost. |
| C1.3 | Convert silent pool overflows in `node.c`, `widget.c`, `state.c`, `paint.c` to a single `ca_panic_oom(category)` that logs + (in debug) traps. Document main-thread-only contract in `causality.h`. |
| C1.4 | Add `tests/` with a minimal CTest harness: pure-C unit tests for css parser, layout flexbox math, signal/effect tracking. No GPU. CI-ready. |
| C1.5 | Fix `ca_color()` endian bug (`paint as little-endian RGBA8 always`). Audit all call sites with `grep`. |
| C1.6 | Drop `Vulkan 1.3` hard requirement to `1.2` unless a 1.3-only feature is actually used. Confirm with validation layers. |
| **Exit** | `sandbox` runs identically to today; CI green; SPDX coverage 100 %. |

### Phase C2 — Reactive core rewrite

| Step | Detail |
|---|---|
| C2.1 | New `src/reactive/` module: `signal.h/.c`, `effect.h/.c`, `batch.h/.c`. Implementation: per-signal subscriber list, current-effect TLS pointer, dependency capture on `ca_signal_get`, dirty-flag fan-out on `ca_signal_set`, `ca_batch_begin/end` defers fan-out. |
| C2.2 | `ca_computed(Ca_ComputeFn, void* user)` returns a read-only signal whose value is recomputed lazily on first read after any dep changed. Memoized. |
| C2.3 | Wire builder rebuilds onto effects: `ca_div_set_builder(div, fn, user)` registers an effect whose body calls `fn`. Any signal read in `fn` auto-subscribes the div. `ca_div_invalidate` is no longer needed for state-driven changes (kept as escape hatch). |
| C2.4 | Reimplement `ca_state_*` as a thin shim on signals. Mark deprecated in header. Remove the 512-state / 64-subscriber caps. |
| C2.5 | Lock-free single-writer + multi-reader signal store (signals are main-thread by contract; *value* may be a struct that points to thread-shared data). Worker→UI updates go through `ca_post_update(fn, data)` which queues onto the main-thread effect runner. |
| C2.6 | Update sandbox to use signals. Add a "counter / todo list / file-tree-stress" demo proving fine-grained updates only repaint affected nodes (validate via paint-cmd counter overlay). |
| **Exit** | Sandbox demonstrates fine-grained reactivity; perf overlay shows zero re-renders for unrelated subtrees on a counter increment. |

### Phase C3 — Builder API cleanup

| Step | Detail |
|---|---|
| C3.1 | Unify all elements onto `ca_<tag>(Ca_<Tag>Desc)` for self-closing and `ca_<tag>(Ca_<Tag>Desc) … ca_end()` for containers. Remove the `_begin/_end` suffix style except where genuinely required (kept names: `ca_ui_begin/end` is the frame, not an element). Macros for the *deprecated* names live in `causality_compat.h` for one minor version. |
| C3.2 | Make every container accept `.children = (Ca_ChildrenFn){ .fn=…, .user=… }` as an alternative to imperative push/end — useful for components. |
| C3.3 | Move all widget chrome rendering out of `paint.c` into per-widget builder code. `paint.c` only emits `RECT` / `GLYPH` / `IMAGE` / `VIEWPORT` primitives. This is the change that makes CSS truly authoritative for styles (decision §0.3). |
| C3.4 | Implement the missing/broken APIs: `ca_gpu_shader_compile/destroy` (real shaderc call), `ca_radio_group_set`, viewport `on_resize`, tree node depth/indent. |
| C3.5 | Replace per-widget pools (label, button, input, …) with a single `Ca_Widget` arena keyed by widget kind. Removes the cross-type fragmentation problem. |
| **Exit** | Public header diff is small and consistent; sandbox compiles unchanged after a sed-style rename pass. |

### Phase C4 — CSS engine (Phase A then B then C as scoped in §0.3)

| Step | Detail |
|---|---|
| C4.1 | Tokenizer: rewrite as a proper state-machine matching CSS Syntax Module 3 (`<ident-token>`, `<dimension-token>`, `<function-token>`, …). Export a tokenizer test suite. |
| C4.2 | Parser: produce a typed AST of selectors + declarations. Replace value parsing in `css.c` with a per-property dispatch table keyed by `Ca_CssPropId`. |
| C4.3 | Computed-value engine: variables (`var()`), `calc()`, units (`px`, `%`, `em`, `rem`, `vw`, `vh`, `ch`), color functions (`rgb`, `rgba`, `hsl`, `hsla`, `#hex`, named). |
| C4.4 | Pseudo-class engine: per-node interaction state (`:hover`, `:focus`, `:active`, `:disabled`, `:checked`, `:focus-visible`, `:not()`, `:nth-child`). |
| C4.5 | `@media (min-width)`, `(prefers-color-scheme)`. Hook into existing window resize. |
| C4.6 | `@keyframes` + `animation` (interpolated with the existing transition engine, expanded to N tracks per node — drop the 4-slot cap). |
| C4.7 | `display: grid` (track-list / placement / gap), `position: absolute|fixed|sticky` (verify), `transform` (2D + opacity baseline). |
| C4.8 | Move widget chrome styling into `default.css` (bundled stylesheet auto-loaded; userland CSS cascades over it). Validate: deleting the bundled CSS → unstyled but functional UI. |
| **Exit** | Sandbox uses pseudo-class hover states, CSS variables, a media query, one keyframe animation, and a grid layout. |

### Phase C5 — Renderer hardening

| Step | Detail |
|---|---|
| C5.1 | Make `CA_FRAMES_IN_FLIGHT` configurable (2 or 3). |
| C5.2 | Replace the per-frame whole-list re-sort by z-index with a stable sort *only when* z-order or tree changed. |
| C5.3 | One SSBO per pipeline grew unbounded by node count — switch to chunked SSBO ring with explicit capacity probe. |
| C5.4 | Async font/atlas pack: move `stbtt_pack_*` off the main thread on first use, fence into render. Configurable Unicode ranges via `Ca_InstanceDesc.font_ranges` (no more hardcoded Nerd Font assumption). |
| C5.5 | Implement `ca_gpu_shader_compile` properly through shaderc. Cache compiled SPIR-V on disk by source-hash. |
| C5.6 | GLFW refcount: replace `static int g_glfw_refcount` with `_Atomic int` and a `pthread_once_t` initializer. |
| **Exit** | Frame-time histogram on sandbox stable; no main-thread font stalls; multi-instance create/destroy stress test passes. |

### Phase C6 — A11y + ergonomics (optional, defer if scope tight)

Tab/focus order, ARIA-equivalent role attribute, keyboard activation for all widgets, `prefers-reduced-motion` honored by transitions/animations.

---

## 2. Sol refactor — phase plan

> Sol is in good shape architecturally (clean module boundaries, no internal coupling to causality, no live TODO/HACK markers). The plan is mostly *additive*: license, decoupling glue, and pulling the editor toward a real workflow.

### Phase S1 — Compliance & hygiene

| Step | Detail |
|---|---|
| S1.1 | Add `LICENSE` (Apache-2.0), `NOTICE`, SPDX headers. |
| S1.2 | Move the embedded CSS literal in `workspace.c` to `assets/sol.css`, loaded at startup. Lets you tune the editor look without rebuilding. |
| S1.3 | Remove the `parallel_for(100k checksum)` warmup. It's diagnostic dead weight. Replace with an opt-in `--bench` flag in `main.c`. |
| S1.4 | Remove the `sol_plugin_manager_load_directory(NULL)` stub call until a real plugin path exists. Keep the subsystem; just don't pretend to use it. |
| S1.5 | Make `sol_ui_constants.h` data-driven: read font size from settings (a `Ca_Signal`, naturally) so it can be live-edited. |

### Phase S2 — Real editor primitives

| Step | Detail |
|---|---|
| S2.1 | New `sol/include/sol_text_buffer.h` + `core/sol_text_buffer.c`: a piece-table or rope (piece-table is simpler and fast enough for the editor's expected file sizes). Operations: insert/delete/replace, line index, undo/redo via change journal, line-ending normalization. Backed by the existing generic buffer kind. |
| S2.2 | `sol_file.h/.c`: file I/O (`open`, `save`, `save_as`, atomic write via tmp+rename, encoding detection — start with UTF-8 + BOM detect). |
| S2.3 | `ui/textarea.c`: the actual code-editing widget on top of `sol_text_buffer` and causality. Caret rendering, selection, copy/paste (hook causality clipboard once exposed; otherwise GLFW), virtual scrolling, soft wrap toggle. |
| S2.4 | Tab strip + buffer list UI: real causality `ca_tabs` consumer driven by `SolBufferSystem`. |
| S2.5 | File tree sidebar (causality `ca_tree_*`) showing CWD, toggle hidden, open-on-Enter. |

### Phase S3 — Modal command flow refinements

| Step | Detail |
|---|---|
| S3.1 | Promote the leader-key state machine into its own module `sol_modal.h/.c`. Decouple from `workspace.c`. |
| S3.2 | Allow >8-key sequences and >64 flows: replace fixed arrays with `SolDynArray`. |
| S3.3 | Add a real command palette (`Ctrl+P`/`Ctrl+Shift+P`) — fuzzy search over registered actions. Fold the existing settings menu into it. |
| S3.4 | Lift binding storage to a `~/.config/sol/keys.toml` (or json) so customizations persist. |

### Phase S4 — LSP / syntax highlighting hooks (optional this round)

Tree-sitter as a vendored optional dep (MIT, Apache-2.0 compatible). Highlight queries → `ca_text` color spans.

---

## 3. Cross-cutting tasks

| Task | Where |
|---|---|
| `compile_commands.json` regeneration baked into a `make ide` target | top-level `CMakeLists.txt` |
| `clang-format` config + git-hook wiring | repo root |
| `clang-tidy` baseline run; fix or suppress with rationale | new `.clang-tidy` |
| AddressSanitizer + UBSan CMake preset | `CMakePresets.json` |
| GitHub-Actions-equivalent (or just a Makefile target) for lint + unit tests | new `ci/` |

---

## 4. What I am explicitly **not** doing

- No SDL3 migration.
- No HTML/JSX text format — the API is C builder calls per your decision.
- No VDOM. Reactivity is signals, period.
- No silent rewrites of working sol code (event bus, job system, plugin mgr stay; they're sound).
- No "full CSS3 spec compliance" promise — the achievable target is the snapshot defined in §0.3, which delivers what real apps need.

---

## 5. Recommended execution order & checkpoints

1. **C1 + S1** (compliance & safety net) — small, mergeable, unblocks everything.
2. **C2** (signals) — biggest architectural win for causality; everything else benefits.
3. **C3** (builder cleanup) — short, high signal-to-noise.
4. **C4 phase A** (CSS pseudo-classes/vars/calc/media) — most visible improvement to consumers.
5. **S2** (editor primitives) — sol becomes a real editor.
6. **C5** (renderer hardening) — quality-of-life perf work.
7. **C4 phase B/C, C6, S3, S4** — opportunistic.

After each phase, both repos build, sandbox runs, sol runs, no regressions.

---

## 6. Open questions for you (not blocking)

1. Is the bundled Ubuntu Nerd Font definitely the font you want to ship? If not, I'll swap during C1.
2. Plugin ABI: should `sol_plugin` go ahead of file I/O (S2.2) or after? (I default to *after* — file I/O lands sooner.)
3. Settings/keys persistence file format: TOML, JSON, or RON-like? (Default: TOML via a tiny vendored parser, MIT/Apache-2.0 only.)

Reply "go" and I'll start with **C1 + S1** (license + safety-net pass, no behavior change) as the first PR-sized chunk.
