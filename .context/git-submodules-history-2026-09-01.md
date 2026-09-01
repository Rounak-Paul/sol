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
