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

2026-06-07 scrollbar geometry/drag follow-up:
- Vertical editor scrollbar must use the actual pane-height track, not `viewport_lines * line_height`; otherwise its thumb cannot reach the bottom of tall panes.
- Horizontal editor scrollbar should be flush to the bottom edge of the text pane and span the text viewport width, not leave the old 8px padding gap underneath.
- Sol editor scrollbars are custom divs backed by `SolTextBuffer` line/column scroll state; give those tracks drag callbacks that update `scroll_top`/`scroll_left`.
- Causality native painted scrollbars already have drag plumbing, but the x-scrollbar hit-test/drag math uses a 14px edge bar while paint uses a 6px bar with 2px margin. Align widget hit-testing with paint geometry so native scrollbars can be clicked and dragged reliably.
- Custom editor scrollbar drag grab offsets must survive UI rebuilds during a drag. Store the active thumb grab offset outside the per-render ring context so pressing on a thumb keeps the same pointer-relative thumb position instead of snapping the pointer to the thumb's top/left.
