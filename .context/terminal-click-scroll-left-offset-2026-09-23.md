# Terminal Click/Scroll Target Left-Offset (fixed 2026-09-23)

## Symptom

User-reported: terminal click targets (column resolution) and scroll
routing felt "left-shifted" — not affecting buttons, "feels like the
terminal somehow." No exact repro steps given ("not sure exact behaviour,
i know there is offset towards left").

## Root cause: three disagreeing clamp ranges on the same ratio

The terminal/buffer split ratio (`SolTerminalManager.ratio`, fraction of
the pane the *terminal* occupies for docked BOTTOM/RIGHT) was clamped to
**three different ranges** across the codebase:

1. `sol_terminal_manager_set_ratio` (`sol_terminal.c`) — stored value
   clamped to `[0.10, 0.80]`.
2. `sol_ui_buffer_area_rect_internal` (`workspace.c:179-180`) — clamps
   `term_ratio` to `[0.20, 0.80]` before deriving the buffer area rect.
3. `ca_split_begin`'s `min_ratio`/`max_ratio` in
   `sol_ui_render_buffer_and_terminal` (`workspace.c:997-998`) — the
   *actual rendered* split also clamps to `[0.20, 0.80]`.

(2) and (3) agree — the real on-screen split never lets the terminal pane
go below 20%. But (1) let the *stored* ratio go down to 10%. Whenever the
user dragged the terminal pane thinner than 20% of the split, the stored
`mgr->ratio` (e.g. 0.12) no longer matched what was actually rendered
(clamped to 0.20).

`input_router.c`'s `terminal_cell_at_point` (used by both click-to-cell
and scroll-over-terminal detection) recovers the terminal's on-screen rect
by **inverting** the buffer rect's split math: `available = bw /
buffer_ratio`. It read the raw, unclamped `sol_terminal_manager_ratio()`
for this inversion — a different ratio than the one `bw` was actually
computed with (which used the clamped 0.20 floor). This produced a
systematically wrong `vx`/`vw` (and `vh` for BOTTOM), offsetting every
terminal click/scroll target — while buttons were unaffected because they
hit-test against Causality's real laid-out `node->x/y`, not this
manually re-derived rect.

## Fix (two-part, in `sol/src/core/sol_terminal.c` and `sol/src/ui/input_router.c`)

1. **Root fix**: `sol_terminal_manager_set_ratio` now clamps to
   `[0.20, 0.80]`, matching the two render-side consumers — the stored
   value can no longer enter the disagreeing zone at all.
2. **Defense in depth**: `terminal_cell_at_point` also clamps the ratio it
   reads to `[0.20, 0.80]` before inverting, so it stays correct even if a
   future caller writes `mgr->ratio` directly instead of through the
   setter (belt-and-suspenders, not required if (1) is respected
   everywhere, but cheap and makes the function self-contained/correct
   regardless of caller discipline).

Both changes documented in-place with comments explaining why the ranges
must match.

## Verification

- Clean build (`cmake --build . --target sol`).
- Full test suite: 20/20 pass, including `sol_terminal_tests`.
- **NOT interactively/visually confirmed** — [[screencapture_unavailable]],
  no screen-recording permission in this shell. The fix is confirmed
  correct by static trace of the exact mismatched formulas (traced from
  the user's symptom description, which lacked precise repro steps — this
  was found by auditing every consumer of the ratio for consistency, not
  by runtime instrumentation). User has not yet re-tested.

## Note on scope

This only explains the docked BOTTOM/RIGHT case (the only place a
split-ratio inversion happens). FLOAT position hit-tests via
`ca_div_screen_rect` directly (exact, no ratio math) and was never
affected. If the user still sees offset after this fix at default/normal
split ratios (not deliberately dragged thin), or in FLOAT mode, the cause
is elsewhere and needs runtime instrumentation
([[feedback_verify_dont_rationalize]]) rather than another static pass —
this fix was reachable via code audit precisely because it was a
clamp-range inconsistency, not a general guarantee that no other
click/scroll bug exists.

See also [[terminal-architecture]] for the full terminal subsystem map.
