# Menu / popup background — solid color

## Outcome

- Frosted-glass blur for menubar dropdowns, context menus, tooltips, and
  select popups was attempted and abandoned. Root cause of the invisible
  blur was never confirmed (deep investigation ruled out every C-level and
  CSS-level cause; the composite draw call recorded correctly in Vulkan but
  produced zero pixels even with a hardcoded solid-fill test — pointing to
  a GPU/driver defect in causality's `image_pipeline`, likely never
  exercised before this feature, on MoltenVK). Not worth chasing further.
- All causality engine plumbing for `backdrop-filter` (`CA_DRAW_BACKDROP_BLUR`,
  `blur.c`, `node.desc.backdrop_blur`) is untouched and still backs the
  Settings > Theme picker's buffer-pane/side-panel/titlebar blur — that is
  a separate, working feature and was deliberately left alone.
- Sol's own uncommitted WIP wiring for popup blur (`paint_overlay_backdrop`
  in causality's `paint.c`) was fully reverted.

## Actual fix — real bug found

- Popups are NOT styled by `sol/src/ui/style.h`'s `SOL_UI_DEFAULT_THEME_CSS`
  at runtime once a theme plugin is active — `sol-plugin-themes` generates
  its own `.ca-select-popup, .ca-tooltip, .ca-context-menu, .ca-menubar-popup`
  rule (`plugins/sol-plugin-themes/src/plugin.c`, `build_theme_css`) and that
  is what actually renders.
- That rule used the `raised` token — `theme->elevated` at 0.78 alpha
  (0.88 for light themes) — meant for subtle elevated-surface tinting
  elsewhere (`.fp-row:hover` etc). Using it for popup backgrounds made every
  popup translucent, letting background content bleed through even though
  `style.h` separately declared an opaque background (that CSS was simply
  overridden/never the active source).
- Fix: added a dedicated fully-opaque `popup_bg` variant
  (`color_with_alpha(theme->elevated, 1.0f, ...)`) and pointed the popup
  rule at it instead of `raised`, leaving `raised` untouched for its other
  (intentionally translucent) uses.
- `sol/src/ui/style.h`'s `.ca-select-popup...` rule was also updated to a
  solid `rgb(13, 17, 23)` background as the non-themed fallback, consistent
  with the plugin-theme fix.

## If backdrop-filter is revisited later

- Don't just re-add `backdrop-filter` to CSS — the underlying
  `image_pipeline` bug needs a GPU frame capture (Xcode Metal debugger) to
  actually diagnose first. See memory `backdrop-blur-removed` for the full
  investigation trail.
