# Buffer Tab Close Stability — 2026-06-20

## Current Issue
- A file opened by pressing Enter in the search window creates a visible tab whose close button does not receive clicks.
- Causality flushes signal subscribers synchronously. Opening the file mutates the buffer signal while the search window's frame callback owns the global build context.
- The primary window's reactive tab-strip builder therefore allocated the new button widget from the search window's button pool. The node rendered in the primary window, but its input pass could not find the button.
- Independently, buffer-tab callbacks could become unreliable when the pane context pool grew because `realloc` invalidated addresses already attached to buttons.

## Relevant Files
- `sol/src/ui/workspace.c`
- `sol/src/ui/sol_ui_internal.h`
- `vendors/causality/causality/src/ui/widget.c`

## Fix
- Run every reactive div builder in an isolated build context bound to the div's owning window, then restore any interrupted window build context.
- Preserve the pending CSS reconciliation snapshot across nested reactive builds.
- Store pane click contexts as stable heap pointers instead of raw structs inside the growable slot array.
- Keep the per-frame reset semantics, but ensure button callbacks keep valid addresses even if the pool expands.
- Reject capacity and allocation-size overflow before growing the pool.

## Review Notes
- The screenshot demonstrated the failure with one tab, ruling out pool growth as the immediate cause.
- Search result opening is not a separate buffer type or close path; the difference is that Enter opens it during another window's active frame callback.
- The fix belongs in Causality because any reactive builder invalidated from another window could otherwise create visually present but non-interactive widgets.
- No z-index override is needed: the close button already has explicit stacking and the failure is widget-pool ownership, not hit testing.

## Verification
- `cmake --build build -j2` completed successfully.
- `ctest --test-dir build --output-on-failure` passed all 10 tests.
- Manual UI verification remains: press Enter on a search result and click its tab close button; also open at least 17 buffers and close early, middle, and late tabs.

## 2026-09-08 sibling bugs found by broad codebase sweep

A full-codebase bug sweep (parallel forks over sol/src/core, sol/src/ui, and
plugins/sol-plugin-git) turned up two more instances of patterns from this
file's own history, plus a graph-layout bug. All fixed:

**Shared global caret-blink state corrupted across split panes**
(`sol/src/ui/text_view.c`): `caret_blink_visible()` used a single set of
module-level globals (`g_caret_prev_line/col`, `g_caret_last_move_ms`) to
track "did the cursor just move" for the blink timer, but it's called once
per visible pane per frame. With two split panes on different cursor
positions, each pane's render call overwrote the shared state the other
pane just wrote, so both panes perpetually looked like "cursor just moved"
relative to each other — carets never blinked correctly in any multi-pane
layout with differing cursor positions. Fixed by keying blink state per
`SolBufferNodeId leaf_id` in a small fixed-size linear-scan cache
(`g_caret_states[SOL_CARET_STATE_SLOTS]`, capacity 64, matching
`SOL_UI_MAX_SPLIT_CALLBACKS`); on cache exhaustion it falls back to slot 0
rather than growing or crashing — degrades to the old single-pane-ish
behavior only in the extreme case of 65+ simultaneous panes.

**`SOL_UI_MAX_SPLIT_CALLBACKS` (64) hard-`assert(false)`s on overflow**
(`sol/src/ui/workspace.c` `sol_ui_visit_begin_split`,
`sol_ui_internal.h`): opening more than 64 simultaneous split panes hit a
debug-build crash for a condition a user can genuinely trigger (not a
programming error) — release builds already degraded gracefully
(`on_resize = NULL`, drag just doesn't persist). Raised the cap to 256 and
removed the `assert`, leaving only the existing graceful no-op path. Same
class of "fixed-size pool used for content that outlives one frame, no
graceful growth" as the click-context pool bug earlier in this file — the
click-context pool was already fixed correctly (pointer indirection so
`realloc` can't invalidate handed-out pointers); this one just needed the
crash removed since growth isn't actually needed at this scale.

**Side-panel unregister leaves stale `SOL_UI_FOCUSED_PANEL_TREE`**
(`sol/src/ui/workspace.c` `sol_ui_system_unregister_side_panel`):
unregistering the currently-active side panel cleared `active_side_panel`
but not `focused_panel`, so if that panel had keyboard focus
(`SOL_UI_FOCUSED_PANEL_TREE`) when a plugin unloaded/unregistered it, the
"-focused" CSS styling stayed stranded on the sidebar with nothing there to
own it. Cosmetic only — `FOCUSED_PANEL_TREE` only drives CSS, not input
routing (confirmed before fixing). Fixed by falling back to
`SOL_UI_FOCUSED_PANEL_BUFFER` via the `sol_ui_system_set_focused_panel`
funnel when the unregistered panel held tree-focus.

**Git commit-graph lane clobbering at 32+ concurrent lanes**
(`plugins/sol-plugin-git/src/git_model.c` `git_model_layout_graph`): when
all `GIT_MAX_GRAPH_LANES` (32) lanes were already claimed by distinct
still-open branches and a new lane was needed, the code forced the new
commit onto the *last* lane regardless, clobbering whatever hash that lane
was legitimately tracking — that branch's line then silently
mis-terminates or reconnects to the wrong commit later in the graph.
Needs 32+ simultaneously-open unmerged branch tips within the visible
history window to trigger (large repo, many long-lived feature branches).
Not a crash. Fixed to match the exact pattern already used one code path
below for overflowing merge parents: leave `my_lane` as `-1` (sentinel)
instead of clobbering, which the renderer (`plugin.c`) already treats as
"don't draw this" via existing `entry->lane < 0` guards — no renderer
changes needed, the sentinel path already existed and was just
inconsistently applied.

Found via 3 parallel review forks (sol/src/core, sol/src/ui,
sol-plugin-git) plus one follow-up fork for core files the first pass
didn't reach (sol_syntax_highlight.c, sol_file_watcher.c, sol_search.c,
sol_event.c, and others) — all came back clean, with sol_file_watcher.c
and sol_syntax_highlight.c specifically noted as already well-hardened
against the exact bug classes being hunted (self-pipe wakeup avoiding a
close()-during-poll() race, bounded-depth iterative stack walks against
crafted-file stack overflow). One additional finding (git plugin's
per-frame `GitActionContext` array reusing raw pointers into snapshot data
across rebuilds) was flagged as a fragile pattern matching this file's own
history but left unconfirmed/unfixed — click dispatch is believed
synchronous against the current frame's tree, making it likely
unreachable in practice; worth a second look only if a related crash is
ever actually observed.

Verification: `sol_plugin_git` + `sol` build clean (no new warnings),
full ctest 16/16 pass, `./bin/sol` launches and stays alive. Caret-blink
and lane-clobbering fixes are not visually/scenario re-confirmed by the
user yet (need a real multi-pane session / a 32+-branch repo respectively)
— see [[screencapture_unavailable]].
