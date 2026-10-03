# Git changes list performance

## Owner and cause

`plugins/sol-plugin-git/src/plugin.c::git_render_file_group` previously emitted every changed-file row, tooltip, and action button during each reactive workspace rebuild. The snapshot stores up to 512 files; partially staged files appear in both groups, creating up to 1024 heavyweight rows. Hover and scroll therefore incurred work proportional to the whole list.

## Implementation

- `src/git_list.h` calculates a half-open visible range from viewport height, group offset, scroll position, and scaled row height. Eight rows of overscan on either side; an unmeasured viewport uses a bounded 96-row initial window.
- The Changes tab reads Causality's existing `scm-content-scroll` reactive signal, including wheel, scrollbar drag, and programmatic scrolling. Signals are acquired from the live container rather than cached across window/project lifetimes.
- Group offsets use measured commit/confirmation heights and current group counts. File rows are fixed at 28 author pixels and headers at 27, with no shrinking. Hidden rows become keyed spacers preserving the exact scroll extent. Only visible rows allocate action contexts and child widgets.
- Counts use the same staged/unstaged predicates as row selection, including conflicts. File keys use snapshot indices and independent group containers; long paths cannot collide through truncated node IDs.
- Snapshot limits, Git operations, and worker ownership are unchanged.

## Validation (2026-10-04)

- Debug app/plugin/tests and full Release build passed. All 23 CTests passed; `git diff --check` passed.
- Range regression tests cover every row of a 512-file group at scales 0.75, 1, 1.5, and 2, fractional scroll positions, offscreen/empty groups, and unmeasured/invalid geometry. A 560-author-pixel viewport emits at most 37 rows per group.
- Native UI verified with an isolated 512-file repository: wheel scrolling to intermediate rows and the final `changed-0511.txt`; staging visible `changed-0312.txt` confirmed by `git diff --cached --name-only`.
- Expanded the fixture to 512 staged plus 512 unstaged rows; native rendering verified across the group boundary with the last staged rows and first unstaged rows visible together.
- No timing percentage is claimed; verified bounded widget construction and rendered behavior rather than an averaged frame-time comparison.
- Installed the Release component to `/Applications`; installed `Sol` and `sol-plugin-git.dylib` byte-match `bin/Sol.app`. Existing processes require a restart. Stopped the isolated verification instance and removed its temporary MRU entry.
