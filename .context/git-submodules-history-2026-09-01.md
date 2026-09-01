# Git submodules and history

## 2026-09-01

The source-control plugin discovers and refreshes only the workspace's top-level
repository. `git status --porcelain=v2` exposes a submodule only as a gitlink
file record, so the UI cannot show clean, uninitialized, nested, or dirty
submodules as repositories. The refresh model must additionally collect
`git submodule status --recursive --cached` and render those entries explicitly.

History used record-separator output without `-z`. Git inserts a newline after
each record separator, so every parsed commit after the first starts with a
newline in its hash. That breaks lane matching and produces incoherent graph
rails. Use NUL-delimited, six-field `git log -z` output instead, and parse
complete records without treating line endings as structure.

The previous graph only painted vertical lanes. The model should retain each
commit's assigned parent lanes so rendering can draw the merge/branch
connectors rather than implying every history is a set of disconnected rails.

Implemented in `plugins/sol-plugin-git/src/git_model.c`, `git_plugin.h`, and
`plugin.c`. `GitSnapshot` owns recursive submodule entries and preserves
porcelain content-modified/untracked flags; the Changes tab renders the
registered submodules independently of whether their gitlink changed. History
now uses six NUL-delimited fields per commit and records parent lanes for
diagonal merge connectors. Tests cover NUL history parsing, porcelain
submodule flags, parent-lane assignment, and live refresh against this
repository's `vendors/causality` submodule.

Object identifiers are sized for both SHA-1 and SHA-256 repositories.

Follow-up: the initial UI integration was hidden by `git_render_changes()`'
clean-tree early return. A clean parent repository with registered submodules
therefore rendered only `No changes`; the empty state must apply only when both
the file-change and submodule lists are empty.

Submodules are separate repositories, so a parent-repository action cannot
fetch, pull, push, commit, or switch a submodule branch. Clicking a submodule
now discovers it as the active repository; all existing source-control actions
therefore run at that submodule root. When selected, the panel shows the active
repository path and offers a Workspace Repository control to return to the
parent repository.

The Git row Open File action uses `sol_plugin_open_file()`. That shared API was
loading disk content but registered a NULL render callback, causing all files
opened by plugins to appear blank. It now uses the standard `sol_text_view_render`
callback, matching the main application's file-open path.

Changed-file rows reserve a fixed 56px right action rail. The filename column
is explicitly shrinkable and clipped, so long paths cannot move the open,
stage/unstage, or discard buttons off-screen.

Remote operations are repository-level actions. Fetch, Pull, and Push use
three equal-sized icon controls with tooltips, aligned as a compact toolbar.
Causality scales stylesheet dimensions and text with the UI scale; the History
graph was the Git-panel exception because it uses inline geometry, so its row,
lane, line, and dot measurements now apply the current UI scale explicitly.
## 2026-09-02 — custom Git buffers and Git action strip

- Git Diff output uses `git_view_open()` with plugin-owned render/destroy callbacks. System teardown unloads plugins before the buffer system, so those callbacks could point into an unloaded Git module when a Diff buffer remained open. `SolPluginCtx` now tracks custom-buffer ids and closes them during plugin cleanup, before `dlclose`.
- `sol_plugin_open_custom()` closes the newly-created buffer and returns failure if tracking allocation fails, so no untracked callback-bearing buffer can escape.
- `test_buf_custom_closed_on_plugin_unload` checks that plugin unloading closes the custom buffer and invokes destruction exactly once; later system teardown does not invoke it again.
- Git Diff line styles now match normal editor `SOL_UI_BOOT_FONT_SIZE_PX_CSS` and 20px line height, allowing the existing Causality CSS scale to apply identically.
- Remote Fetch/Pull/Push/Refresh controls are four equal 30x26 logical-pixel icon actions, right-aligned as a compact toolbar with tooltips. Pull is warning-colored for incoming commits; Push is success-colored for outgoing commits. The header reserves its former Refresh space for a persistent status icon that reports running work, errors, conflicts, worktree changes, sync state, or a clean tree.
- The status slot is 20x20 logical pixels with a 12px glyph so scaled theme icons cannot be clipped. Local changes use the compact Edit glyph rather than the taller Diff Modified glyph.
- The header Close action has its own 20x20 logical-pixel control box; file-row actions retain their denser 16px geometry.
