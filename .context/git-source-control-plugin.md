# Git Source Control Plugin

## Goal

Implement Git as a production-ready Sol feature with a native source-control UI,
keyboard commands, asynchronous operations, and safe repository mutation flows.

## Current Architecture

- Plugins load through `SolPluginCtx` and can register commands, events, jobs,
  services, and status-bar segments.
- `SolUISystem` owns the reactive workspace layout. Its builders subscribe to
  Causality signals; worker threads must publish snapshots for UI-thread adoption
  and wake the Causality instance.
- The existing public plugin API can open custom editor buffers but cannot add a
  first-class workspace panel. A host-owned panel contribution API is required.
- `sol_file_tree_root()` is the canonical workspace root. Panel visibility must
  remain separate from repository discovery state.
- A stale, untracked build artifact contains an earlier Git plugin that rendered
  status/diff/log/blame as custom buffers. Its source is absent and it is not the
  implementation baseline.

## Implementation Plan

- Extend `SolUISystem` and `SolPluginCtx` with lifecycle-managed side-panel
  contributions and reactive invalidation.
- Add `Plugins/sol-plugin-git` with portable process execution, repository model,
  porcelain parsing, async refresh/mutation jobs, and native Causality rendering.
- Support status, stage/unstage, discard confirmation, commit, diff, history,
  blame, fetch, pull, push, branch checkout/create, refresh, and file opening.
- Register command-palette/leader actions and a branch status-bar segment.
- Add focused parser/process tests, then build and run the full test suite.

## Invariants

- Git subprocesses never block the UI thread.
- Worker threads never mutate Causality nodes or UI-owned collections directly.
- Repository snapshots have one owner and are adopted atomically on the UI thread.
- Paths are passed as argv entries with `--`; no shell command construction.
- Destructive discard/clean operations require explicit confirmation.
- Plugin unload waits for owned work and removes all UI contributions.
- No commits or pushes are made by this task.

## Progress

- Repository, plugin ABI, workspace reactivity, and stale Git artifact audited.
- Added lifecycle-managed `SolUISidePanel` contributions with reactive rebuild,
  UI-thread tick, wake, visibility, ownership checks, and plugin auto-cleanup.
- Added the `sol-plugin-git` source tree with portable non-interactive process
  execution, porcelain-v2 parsing, repository discovery, event-driven refresh, and
  serialized asynchronous task ownership.
- Implemented stage/unstage, stage all/unstage all, guarded discard, commit,
  fetch, fast-forward pull, push with first-upstream setup, repository init,
  branch creation/checkout, history, file diff, commit show, and blame.
- Added native source-control panel UI, branch status segment, keyboard command
  flows, progress/error states, clean-tree state, and colorized read-only views.
- Corrected application shutdown order so plugins unload before UI, syntax, and
  Causality resources are destroyed.
- Added parser/process tests and side-panel lifecycle tests. Top-level CTest now
  discovers all targets.
- Verification: full build passes; plugin sources pass `-Wall -Wextra
  -Wpedantic`; all 10 CTest targets pass with normal temp-file access; native
  Sol startup reports 11 plugins loaded and input ready.
- Replaced Unix fork/chdir execution with `posix_spawn` and explicit `git -C`
  repository selection. Windows uses the same `git -C` argument flow without a
  process current-directory dependency. This removes the recurring launch/setup
  exit 126 path and preserves useful Git errors for invalid repository paths.
- Removed the three-second visible status poll that repeatedly rebuilt the panel.
  Refresh now occurs on panel open, explicit refresh, workspace-root changes, and
  after Git mutations.
- Regression verification: strict subprocess compilation passes, the Git plugin
  tests execute real version and repository-status commands successfully, and all
  10 CTest targets pass with normal temporary-file access.

## Usage

- `Ctrl`, then `g s`: open Source Control.
- `Ctrl`, then `g r`: refresh.
- `Ctrl`, then `g d`: diff active file.
- `Ctrl`, then `g l`: history.
- `Ctrl`, then `g h`: branches.
- `Ctrl`, then `g b`: blame active file.
- `Ctrl`, then `g c`: focus commit input.
- `Ctrl`, then `g f/u/p`: fetch, pull, push.

## Verification Constraint

- Native launch and panel-open runtime paths completed without errors. macOS
  denied Accessibility keystroke injection and display capture, so pixel-level
  screenshot inspection was unavailable in this environment.
