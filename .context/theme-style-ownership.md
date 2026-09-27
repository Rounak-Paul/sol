# Theme and widget-style ownership

## Boundary

- Themes own semantic background, panel, elevated, editor, text, accent, state, scrollbar-fill, and shadow colors.
- Theme base surfaces are opaque; appearance settings exclusively own panel opacity and backdrop blur.
- Styles own geometry and visual treatment: radii, border widths, spacing, blur shape, shadow shape, and relief arrangement.
- A style may derive presentation colors from `SolThemeColors`; it must not replace semantic fills or invent fixed palette colors.

## Style system

- `sol/src/ui/style_registry.h` is the built-in style catalog.
- `SolUIStyleDesc` contains the stable id, display name, registry CSS, and an optional `build_css` callback.
- `sol_ui_register_builtin_styles` registers descriptors in picker order.
- `sol_ui_build_style_css` resolves any active style generically against the active theme anchors.
- Adding a static style requires one descriptor. Adding a theme-derived style requires one descriptor plus a builder.
- `workspace.c` composes theme CSS, appearance CSS, then the resolved style CSS without style-id special cases.

## Retro

- `style_retro.h` contains static geometry CSS and `sol_retro_build_css`.
- Retro never emits `background`, text `color`, scrollbar fill colors, `shadow-color`, or fixed RGB/RGBA palette values.
- Its bevel highlight blends the theme background toward the theme primary color; its shadow derives from that highlight toward black, preserving one coherent bevel hue. Focus borders use the theme primary color.
- OLED remains black at maximum panel opacity because Retro no longer replaces panel/well/chrome fills.

## Validation

- `sol_style_tests` asserts Retro CSS contains no semantic fill or hard-coded color ownership.
- Full suite: 22/22 passed on 2026-09-27.

## Theme settings UI

- Theme settings are grouped into Color & Style, Background Effect, and Panel Appearance.
- Every slider shows a live formatted value and explains its ownership and effect.
- Panel controls remain available when background effects are unavailable.
- The scrollable panel includes a reset action that restores appearance defaults without changing theme, style, or effect.
