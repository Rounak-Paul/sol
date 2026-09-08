Task context: fix buffer pane sizing/scroll edge cases.

Root causes identified:
- `sol_text_view_render` was estimating visible rows from full window height instead of the actual buffer leaf height, so split panes could over-render and clamp scrollbars incorrectly.
- Buffer line rows were forced to `width: 100%`, which prevented horizontal overflow from producing a usable x-scroll region.
- The renderer had no pane geometry input, so input routing and post-edit cursor settling used approximations rather than the live leaf rectangle.

Planned fix:
- Extend buffer workspace traversal to carry leaf geometry.
- Compute the split-tree root rect from current UI geometry and pass it into traversal.
- Use exact leaf height for viewport line count.
- Let the text column scroll horizontally and size line rows to content.
- Update input-routing settle logic to use the same live pane geometry.

2026-06-07 horizontal-scroll task:
- Current vertical scrolling is owned by `SolTextBuffer.scroll_top_line` and custom-rendered in `sol/src/ui/text_view.c`.
- `style.h` has `overflow-x: scroll` on `.buffer-text-col`, but no persistent text-buffer x offset, no horizontal scrollbar, and cursor movement does not settle horizontally.
- Implement as logical visual columns, not raw pixels, so tabs and UTF-8 keep the same geometry as caret/selection rendering.
- Data API to add: horizontal scroll getter/setter plus `ensure_cursor_visible_2d(tb, viewport_lines, viewport_cols)`.
- Input router should use pane geometry and glyph width to compute visible columns after every edit/navigation, and use wheel `dx` for horizontal scroll.
- Text renderer should shift rows/caret/selection by `scroll_left_cols * glyph_advance`, clamp to visible content width, and draw a bottom horizontal scrollbar only when visible content overflows.

2026-06-07 follow-up:
- Horizontal wheel sign must feel like VS Code/content-following trackpad behavior: a rightward sweep should reduce `scroll_left` so the rendered content moves right, not left.
- Do not force every tiny `dx` into a whole column. Accumulate fractional horizontal wheel deltas in the input router and only apply whole-column changes once the accumulated movement crosses a column threshold.

2026-08-30 scrollbar edge-case audit (resize / tiny panes / mid-drag resize):
- `sol_ui_visit_render_leaf` (workspace.c) was clamping `args.rect.h` to `0.0f`
  after subtracting the tab-strip height. A rect of exactly 0 is what
  `sol_text_view_render` also uses to mean "no rect provided yet" (dynamic
  layout not ready), so a legitimately tiny pane (heavy splits, extreme
  resize) fell into the "unknown size" branch and got sized against the full
  window instead of its real tiny rect, producing an oversized/misplaced
  scrollbar. Fixed by flooring `args.rect.h`/`.w` to `1.0f` instead of `0.0f`
  so tiny-but-real panes stay distinguishable from "not laid out yet".
  Verified the rest of the geometry pipeline (sol_text_view_visible_lines_for_height,
  sol_text_view_visible_cols_for_width, text_track_w) already clamps safely
  down to a 1px pane — no follow-on div-by-zero.
- `on_scrollbar_drag_start`/`on_scrollbar_drag` (text_view.c) captured
  `grab_offset` as an absolute pixel distance from the thumb's top/left edge,
  computed against that frame's `track_len`/`thumb_len`. `ScrollbarDragCtx`
  is rebuilt fresh every frame from current pane geometry, so if the pane
  resizes (or ui_scale changes) mid-drag, the *next* frame's `on_scrollbar_drag`
  combines the *old* pixel offset with the *new* track/thumb length, causing
  the thumb to jump/snap relative to the pointer. Fixed by storing the grab
  point as a 0..1 fraction of thumb length instead, and re-scaling it by the
  current frame's `thumb_len` inside `on_scrollbar_drag` — invariant under
  mid-drag resizes.
- Confirmed NOT a bug (initially suspected, ruled out after checking git
  history): `viewport = rendered - 2` vs `rendered` in `sol_text_view_render`.
  `rendered` deliberately over-paints by 2 rows (parent clips via
  overflow:hidden); `viewport` is the precise fitting count and is correctly
  the one used for `max_top`/thumb-size math.
- Confirmed NOT a bug: the `g_scrollbar_drag_leaf_id == ctx->leaf_id ||
  g_scrollbar_drag_tb == ctx->tb` OR-match. Causality's `on_drag` only fires
  per-frame on the element actually being dragged (drag_data is bound to
  that div), so this can never cross-match two different panes/split leaves
  even when they share the same underlying SolTextBuffer (e.g. same file
  open in two split panes).

2026-09-08 terminal hit-rect double-subtraction (wrong-pane scroll bug):
- User report: terminal docked right, buffer in the middle; scrolling with the mouse clearly
  over the buffer pane instead scrolled the terminal.
- Root cause in `terminal_cell_at_point()` (`sol/src/ui/input_router.c`), used by
  `on_mouse_scroll` and mouse click/drag routing to decide "is the pointer over the terminal
  grid": it read `bx,by,bw,bh` from `sol_ui_system_buffer_area_rect()` — but that rect is
  already the buffer's share AFTER `sol_ui_buffer_area_rect_internal()`
  (`sol/src/ui/workspace.c`) subtracted the terminal's split. `terminal_cell_at_point` then
  applied `(1-ratio)`/`ratio` a SECOND time on top of that already-shrunk rect, computing a
  phantom terminal hit-box that landed partway INSIDE the real buffer pane instead of at the
  terminal's true screen position (concretely, with a 30% terminal ratio, ~the rightmost 70% of
  the buffer's own width was misclassified as "over the terminal").
- Fix: invert the same panel_gap/ratio split `sol_ui_buffer_area_rect_internal` applied, to
  recover the true pre-split extent from the already-split buffer rect (`available = bw /
  buffer_ratio` for the RIGHT case, `bh / buffer_ratio` for BOTTOM), then place the terminal
  rect directly after `buffer + panel_gap`. Guards added for `buffer_ratio <= 0` and
  non-positive resulting `vw`/`vh`.
- This function backs 3 call sites (scroll routing, and two mouse click/drag sites) — fixing it
  once fixes wrong-pane scroll AND any click/drag misrouting into a mouse-aware terminal app
  (htop/less/etc.) in that same phantom zone.
- Lesson: `sol_ui_system_buffer_area_rect()` is POST-terminal-split. Anything that needs the
  terminal's own on-screen rect must not re-apply the split ratio on top of it — either invert
  the subtraction (as done here) or add a dedicated pre-split accessor if a second call site
  ever needs this.
- Verification: `sol_plugin_git` + `sol` build clean, full ctest 16/16 pass, `./bin/sol`
  launches and stays alive. Not visually re-confirmed by the user yet — see
  [[screencapture_unavailable]].

2026-06-07 scrollbar geometry/drag follow-up:
- Vertical editor scrollbar must use the actual pane-height track, not `viewport_lines * line_height`; otherwise its thumb cannot reach the bottom of tall panes.
- Horizontal editor scrollbar should be flush to the bottom edge of the text pane and span the text viewport width, not leave the old 8px padding gap underneath.
- Sol editor scrollbars are custom divs backed by `SolTextBuffer` line/column scroll state; give those tracks drag callbacks that update `scroll_top`/`scroll_left`.
- Causality native painted scrollbars already have drag plumbing, but the x-scrollbar hit-test/drag math uses a 14px edge bar while paint uses a 6px bar with 2px margin. Align widget hit-testing with paint geometry so native scrollbars can be clicked and dragged reliably.
- Custom editor scrollbar drag grab offsets must survive UI rebuilds during a drag. Store the active thumb grab offset outside the per-render ring context so pressing on a thumb keeps the same pointer-relative thumb position instead of snapping the pointer to the thumb's top/left.
