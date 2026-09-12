# Causality splitter handle (2026-09-12)

Sol's workspace, sidebar and nested buffer splits use `ca_split_begin`.
Causality owns their paint and input lifecycle.

Root cause: `ca_widget_input_pass` cleared `Ca_Splitter.dragging` without setting
`CA_DIRTY_CONTENT`. Hover transitions only caused CSS re-resolution in `ui.c`,
which did not invalidate paint when resolved CSS stayed the same. Cached active
bar commands consequently survived release or pointer-leave.

Fix: invalidate splitter content on drag start/end and old/new splitter hover
transitions. Existing post-input dirty scans schedule painting in that frame;
no polling, forced continuous rendering or Sol redraw workaround is needed.

Handle appearance is built into Causality `paint.c`: two one-pixel line segments
around five 2.5-pixel circular dots, scaled by UI scale and bounded for short or
narrow gutters. Side-by-side panes use a vertical handle; stacked panes use its
horizontal rotation. The handle is hidden when neither hovered nor dragged. Release while still
hovering returns to the hover appearance; pointer-leave hides it.

Colors (2026-09-12 pass): line segments are white (idle #E8E8E8, dragging
#FFFFFF, both opaque); dots are gold (#FFD34E, opaque, same idle/dragging
tone — only the lines change shade on drag). Glow reduced from radius
6/8px + alpha 0x70/0xA0 down to radius 3/4px (idle/dragging) + alpha
0x40/0x60, same gold hue.

**Hover-visibility root cause (found via stderr instrumentation, not static
reading — first two static-trace attempts both wrongly concluded the logic
was fine):** `paint_splitter`'s visibility gate used
`win->hovered_node == node`. `hovered_node` is resolved once per frame by
scanning every node in the tree and picking the smallest-area hit at the
top z-index (`ca_widget_input_pass`'s Pass 1/Pass 2 + a splitter-ancestor
recovery loop). The splitter's own `Ca_Node` spans its *entire* container
(both panes, `flex_grow=1.0` from `ca_split_begin`), so its area is far
larger than the panes sharing that hover point — confirmed live: splitter
area 689472 vs a losing container's 717120 with the splitter's hit-test
(`point_in_splitter_handle`) reporting `hit=1` the whole time. The
recovery loop only walks upward from whatever won Pass 2, so it never
promotes the splitter when the winner isn't a descendant of it in the
frame it was captured. Net effect: `hovered_node` essentially never
becomes the splitter node during plain hover, even though the geometric
hit-zone is correct — confirmed by the user directly (hover: nothing;
press-and-hold drag: visible immediately, since drag-start hit-tests the
bar directly and bypasses `hovered_node` entirely).

Fix: decoupled splitter-handle visibility from `hovered_node`. Added
`Ca_Splitter.bar_hovered` (ca_internal.h), maintained every frame in
`ca_widget_input_pass` by calling `point_in_splitter_handle` directly
against each splitter (the same call drag-start already uses) and marking
`CA_DIRTY_CONTENT` only on enter/leave transitions. `paint_splitter` now
gates on `sp->dragging || sp->bar_hovered` instead of the node-arbitration
result. `win->hovered_node`'s existing splitter dirty-marking on hover
transition was left in place (harmless, still useful for `:hover` CSS) but
is no longer load-bearing for the handle itself.

**Drag-offset bug (found in the same pass, from the user noticing the bar
sits slightly right/below the cursor while dragging):** the ratio-update
math in `ca_widget_input_pass` used `local = mx - n->x` directly as the
bar's *leading edge* offset (`new_ratio = local / (total - bar_size)`),
matching `bar_x = node->x + pane_space * ratio` — i.e. it placed the bar's
left/top edge under the cursor instead of centering the bar (which is
`bar_size` ~8px wide) under it. Fixed by subtracting `bar_size * 0.5f`
from `local` before computing the ratio, so the bar now centers on the
cursor position, matching where the user visually grabbed it.

Debugging note: all instrumentation (`fprintf(stderr, ...)` in paint.c/
widget.c) used to find the hover root cause has been removed; the fix
itself needed no permanent diagnostics. Validated: full build clean, all
18 CTest tests pass (including `causality_splitter_tests`, which only
covers dragging/hover-paint transitions, not cursor-to-ratio math, so it
did not previously catch the offset bug and required no test changes for
either fix). Not yet re-verified live in the running app after this pass
(user reported the original bugs by running it themselves) — worth a
quick hover/drag check next time Sol is run.

Removed `Ca_SplitDesc.bar_color`, `.bar_hover_color` and their internal/CSS
mappings. `bar_size` remains a layout gutter setting. Sol's obsolete splitter
foreground CSS was removed; CSS may still style the container.

Validation: Causality's cached-paint/input regression covers hover enter/leave,
release without ratio movement, clean idle frames, seven handle shapes, both
orientations, scales 1/1.12/2, and tiny bounds. Removing release invalidation
makes the test fail; restoring it passes. Full build and all 18 CTest tests pass.
A preview generated from actual draw-command geometry was rendered and inspected
(`/tmp/sol-splitter-preview.svg`); this is not a live GPU/application screenshot.
Live Sol computer-use inspection remains unavailable as recorded in
`text-glyph-clipping.md`. No commits or pushes.
