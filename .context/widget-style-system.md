# Sol widget style system

A third appearance axis alongside Theme (colours) and Background (shader
effect). A **style** supplies the relief/shape language — bevel reliefs,
corner radii, edge treatments — leaving hues to the theme.

## Layering

`sol_ui_rebuild_stylesheet` (workspace.c) composes three layers in order:

    theme CSS  →  appearance overlay  →  style CSS

Style is **last on purpose**: a bevelled style must override the appearance
slider's `corner_radius` (a rounded bevel renders as a clipped, broken edge,
since Causality draws per-side edges as straight rects) and the theme's own
radii. Causality's cascade is specificity-then-source-order, so appending
gives the style final say without `!important`.

## Registry

Styles reuse `SolThemeRegistry` (`ui->styles`), not a parallel registry: the
data shape is identical (id / display name / CSS body) and only composition
order differs. Registered styles carry no `SolThemeColors`.

Both registries share `sol_ui_on_theme_change` as their observer — either one
changing needs the same full recomposition.

Gotcha: `sol_theme_register` rejects an empty CSS body, and a failed style
registration aborts `sol_ui_system_create` (hence `sol_project_create`
returning NULL, taking the whole app down). "Classic" therefore registers
`SOL_UI_STYLE_CLASSIC_CSS` — a bare comment — rather than `""`.

## Built-in styles

- `com.sol.style.classic` ("Classic") — identity style, no rules. Names the
  existing flat/soft-radius look in the picker. Registered first, so it is
  the default on a fresh config.
- `com.sol.style.retro` ("Retro") — `src/ui/style_retro.h`.

## Retro implementation

Built on Causality's per-side border colours. `paint_border` (paint.c) takes a
dedicated non-uniform path whenever the four side colours differ, emitting
disjoint per-side rects with correct diagonal corner joints — first-class
bevel support, explicitly documented in that function's comment.

Reliefs: light top/left + dark bottom/right = **raised** (buttons, tabs,
title/status bars, scrollbar thumbs, floating cards); inverted = **sunken**
(wells, inputs, scroll troughs, selected rows, `:active` buttons). 2px for
window-scale edges, 1px for small controls where 2px eats the interior.
A `*` reset zeroes every radius and backdrop blur first (specificity 0, so
every class rule still wins).

### Tones are generated per theme, not fixed

**This is the crux of the design.** The first implementation used fixed
translucent tones (`white@0.55` highlight, `black@0.55`/`0.72` shadow) and was
visibly broken: a bevel needs luminance room both above *and* below the
surface it sits on, and a fixed pair runs out of room at both ends of the
range. Measured contrast of the shadow edge against the surface:

| background | old shadow contrast | verdict |
|---|---|---|
| `#0d0d11` midnight | 1.04 | invisible |
| `#1e1e26` dark slate | 1.17 | invisible |
| `#808080` mid grey | 2.88 | fine |
| `#f5f5f0` paper | 4.67 | fine (but *highlight* → 1.05, invisible) |

Only mid-grey worked — which is exactly the surface the era's desktops used.
On Sol's dark themes widgets looked like they had a highlight on two sides
instead of a relief; on light themes the same bug mirrored.

Fix, in `style_retro.h`, applied in two steps:

1. `sol_retro_widget_surface` lifts a too-dark surface (or drops a too-bright
   one) into the band `[SOL_RETRO_SURFACE_FLOOR, SOL_RETRO_SURFACE_CEIL]`
   (58–200) where a two-sided bevel is physically representable, shifting all
   channels equally to preserve hue. This is why bevelled widgets read as
   slightly lighter panels on a dark theme rather than being flush with the
   desktop — the same reason real toolkits gave controls their own grey.
2. `sol_retro_bevel_tones` offsets `SOL_RETRO_EDGE_STEP` (52) in each
   direction from that surface, clamped to available channel room, with the
   shortfall from a clamped edge **redistributed to the opposite edge** so a
   surface pinned at either extreme still shows the same total relief through
   its one usable edge instead of silently losing half the bevel.

Wells drop `SOL_RETRO_WELL_DROP` below the surface and derive their own tone
pair, so a groove edge stays visible against the well rather than the frame.

Consequence: `SOL_UI_STYLE_RETRO_CSS` no longer exists. `sol_retro_build_css`
emits the whole overlay with concrete `#rrggbb` values, and
`sol_ui_rebuild_stylesheet` special-cases `SOL_UI_STYLE_RETRO_ID` to generate
it from `sol_theme_active_colors(ui->themes).background_rgb`. The registry
stores only `SOL_UI_STYLE_RETRO_CSS_PLACEHOLDER` so the style can be listed
and selected. **Because the CSS depends on the theme, it is regenerated on
every theme change too** — which already works, since both registries share
`sol_ui_on_theme_change` and that always recomposes from scratch.

## Persistence

- `SolSettings.style_id`, default `SOL_SETTINGS_STYLE_ID_DEFAULT`.
- JSON key `theme.widget_style`. **Not** `theme.style` — that legacy key
  already holds the colour *theme* id.
- Restored in `sol_run_deferred_init`, falling back to the default when the
  saved id names a style this build lacks.
- Live cross-instance reload via `sol_drain_settings_watcher`, same as theme.

## Settings UI

`settings_window.c`: a "Style" `ca_select` below Theme, with the same
hover-preview / revert-on-dismiss behaviour (`preview_style_id` snapshot
taken at window open).

## Tests

`sol_style_tests` (tests/test_style.c) links real Causality and, for a spread
of seven backgrounds from pure black to pure white, generates the retro CSS
and checks that it:

- parses standalone *and* in the full composed layering — a CSS syntax error
  is otherwise silent (`ca_css_parse` returns NULL,
  `sol_ui_rebuild_stylesheet` bails, the app keeps the previous stylesheet
  with no diagnostic);
- still contains the per-side colour properties and `border-radius: 0px`, so
  collapsing them into a uniform `border` — which Causality would paint as a
  flat outline, silently flattening the 3D effect — fails the build;
- has **both** bevel edges at least `0.04` luminance from the widget surface
  (`bevel_is_visible`). This is the direct regression guard for the
  fixed-alpha bug above; it fails for any background if the surface-lift or
  redistribution logic is broken.

It also asserts the surface lift actually fires at the extremes and is a
no-op for a surface already inside the band.

Links `stubs/sol_config_path_stub.c` so `sol_settings.c` can be linked for its
pure helpers without dragging in `sol_config.c` (which pulls the whole UI
layer) or touching the developer's real `~/.sol/settings.json`.
