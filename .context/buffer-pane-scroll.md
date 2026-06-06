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
