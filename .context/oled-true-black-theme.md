# OLED true-black theme

## Ownership

- The OLED palette is registered by `plugins/sol-plugin-themes/src/plugin.c`.
- `build_theme_css` normally derives translucent Glass surfaces from every palette.
- `sol_settings_build_appearance_css` is appended after theme CSS and applies the user panel-opacity setting.

## Required contract

- OLED background, panel background, and elevated palette colors are `#000000`.
- OLED's base surfaces are emitted at alpha `1`, so the panel is black when the appearance panel-opacity setting is at its maximum.
- The separate appearance panel-opacity and backdrop-blur settings remain active, allowing frosted glass below maximum opacity.
- Accent-derived hover, selection, syntax, status, and focus colors remain visible and theme-owned.
- Other palettes retain the existing Glass alpha and appearance behavior.

## Implementation

- `ThemePalette.panel_background` explicitly owns the panel fill color; it was previously named the ambiguous `surface`.
- Every theme emits opaque background, panel, editor, raised, and popup base surfaces. The Theme settings panel-opacity slider is their sole opacity owner.
- OLED sets `background`, `panel_background`, and `elevated` to black.
- Retro's static layer is geometry-only and its derived layer emits only theme-derived relief borders, so OLED needs no style-specific cascade workaround.
- Retro may still derive bevel and focus border colors from the active theme through the generic style descriptor builder; it does not replace OLED fills.
