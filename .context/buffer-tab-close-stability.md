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
