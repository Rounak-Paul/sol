# Corner radius coverage (2026-10-10)

`sol_settings_build_appearance_css` (sol_settings.c) is the only place the
`corner_radius` setting reaches CSS; any class missing from its lists keeps
the static radius from style.h.

- Git UI: `.scm-submodule-row` added to the control-radius list; `.scm-tag`
  gets `min(control, 8px)` (16px-tall pill).
- `.term-header` was missing from the tab-strip top-radius rule, so at
  radius 20 the header kept 8px and cut through the focused panel's curve.
- Children of a focused panel (tab strips, buffer body, gutter) use
  `radius - SOL_UI_FOCUS_BORDER_WIDTH_PX` so they follow the border's inner curve.
- Not visually verified: VK screenshot layer produced no frames this session.

## Second pass
- `.term-panel`/`.buffer-pane` top padding 3px -> 0 so tab strip sits on the border's inner arc.
- Tab strips (`.buffer-tabs-row`, `.term-header`) get side padding
  `max(4, arc inset)` computed from the radius, so the first tab clears the corner arc.
- Overlay radius list gained scm icon/close/remote/branch-from buttons, scm file/commit rows,
  `.sess-item`, `.sw-btn`, `.sw-bind-input`, `.tree-root-row`.
- Themes plugin: `.scm-header-action/.scm-action/.scm-section-action/.scm-danger-action`
  now share the welcome-btn theme fill; file-picker primary/cancel buttons and
  `.sess-item` hover/selected are themed (they were hardcoded blues/greys).
- Overlay string is ~3KB of the 8KB `workspace.c` buffer; overflow silently drops the whole overlay.
