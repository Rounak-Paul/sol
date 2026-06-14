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
- The left sidebar is a dynamic content slot. Showing the explorer deactivates
  the current plugin panel, while showing a plugin panel replaces the explorer
  without destroying either content provider's state.
- Mouse and keyboard access share command actions. Title-bar menu contributions
  reference registered command IDs, and plugin-owned items are removed before
  plugin commands and code are unloaded.
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
- Git leader-command labels now use their public dotted action identifiers
  (`git.status`, `git.refresh`, and related commands), matching the command popup
  convention used by configurable and third-party command registrations.
- File-row diff actions now preserve their source section: staged rows compare the
  index with `HEAD`, unstaged rows compare the worktree with the index, and
  untracked files render as additions against an empty file instead of showing
  "No diff is available".
- Fixed aliasing bug in `git_model_refresh`: when called with `task->snapshot.root`
  as both `root` and the snapshot pointer, `memset` zeroed the root string before
  copy, leaving `snapshot->root` empty and all diffs running from the wrong cwd.
- `git.status` (`L g g`) now toggles the panel; `git.diff`/`git.blame` show the
  panel on error so the message is visible; `git_relative_active_path` guards
  against an empty repo root.
- Explorer activation now explicitly selects the built-in file tree as the
  sidebar content, so `L e e` replaces an active Git or other contributed panel
  instead of treating any visible left panel as the explorer.
- The host owns a dynamic title-bar menu registry. Plugins can add items to
  `Plugins`, `View`, or any other existing menu, and can create a new top-level
  menu by supplying a new stable menu ID and label.
- Built-in menu layout keeps `View` focused on visible workspace surfaces,
  places editing/search actions under `Edit`, and groups plugin operations under
  `Plugins` submenus. Git contributes `View > Source Control` and `Plugins > Git`.
- The `Sol` menu owns New Buffer, Open File, and Open Folder first, followed by
  a separator and Settings. There is no separate `File` top-level menu.
- Causality menu overlays track hovered item indices explicitly and share
  viewport-aware dropdown/submenu geometry across paint, hit testing, clicks,
  and overlay suppression. This keeps highlights live and flips/clamps submenus
  that would otherwise render outside the window.

## Usage

- `Ctrl`, then `g g`: toggle Source Control panel.
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
