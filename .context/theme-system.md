# Sol Runtime Theme System

## Ownership

- `sol/include/sol_theme.h` and `sol/src/core/sol_theme.c` own the bounded theme registry.
- Registry entries copy ids, names, and CSS. A descriptor may provide `base_id`; registration snapshots `base CSS + override CSS`, so unloading a base cannot invalidate derived themes.
- The first registered theme is active. Removing the active theme selects the first remaining entry.
- `SolUISystem` owns the registry and parsed active `Ca_Stylesheet`.

## Built-in Themes

- `com.sol.theme.glass` is the only built-in theme. It is a complete baseline stylesheet defined in `sol/src/ui/style.h` as `SOL_UI_DEFAULT_THEME_CSS`.
- All other themes are shipped via `plugins/sol-plugin-themes` (plugin id: `com.sol.themes`).

## Plugin Themes (`plugins/sol-plugin-themes`)

All themes extend `com.sol.theme.glass` using the `THEME_CSS()` macro that generates
the full CSS template from palette tokens. 16 themes registered:

  Deep Ocean, Amethyst, Graphite, Ember Glass  — custom palettes
  Gruvbox Dark, Nord, Tokyo Night, Catppuccin   — popular neovim ports
  Dracula, One Dark, Rosé Pine, Everforest      — popular neovim ports
  Kanagawa, Carbonfox, Monokai Pro, Sonokai     — popular neovim ports

Each override covers all major visual surfaces: chrome, panels, editor, syntax, aux windows, terminal, settings/PM UI.

The `.buffer-caret { background: #xxxxxx; }` rule defines the accent color that
`sol_ui_push_theme_color()` (workspace.c) extracts and forwards to the bg_effect
registry as r/g/b push constants so shader-mode effects tint themselves.

## Background Effect Push Constants

`BgPushConst` in `sol/src/core/sol_bg_effect.c`:
```c
typedef struct { float time, width, height, opacity, r, g, b, _pad; } BgPushConst;
```
r/g/b are set via `sol_bg_effect_set_theme_color()`, called on every theme change.
Shaders must declare all 8 floats or omit trailing ones (Vulkan ignores extras past
what the shader reads).

## Background Effects

Only `com.sol.bfx.lava` is registered (others removed). The lava shader reads
`pc.r, pc.g, pc.b` to tint its hot palette toward the active theme accent color.
A saturation check falls back to classic orange if the theme is near-neutral.

## Runtime Application

- `sol_ui_system_set_active_theme()` changes the registry selection.
- The registry observer parses the selected complete CSS before replacing the old stylesheet.
- `ca_instance_refresh_styles()` re-resolves all CSS-owned visual and layout fields.
- After CSS parse, `sol_ui_push_theme_color()` scans the CSS for `.buffer-caret { background: #rrggbb }`
  and calls `sol_bg_effect_set_theme_color(reg, r, g, b)`.
- Selection is persisted under `theme.style` in `~/.sol/settings.json` and restored after plugins load.

## Settings UI (`sol/src/ui/settings_window.c`)

- Theme and background effect use `ca_select` dropdowns (NOT buttons anymore).
- `Ca_SelectDesc.on_hover` fires when the highlighted item changes → live preview.
- `Ca_SelectDesc.on_change` fires on commit click → persist + update snapshot.
- `on_hover(idx=-1)` means dropdown dismissed without commit → revert to `preview_theme_id`/`preview_effect_id` snapshot.
- The snapshot is saved at window open time and updated on each commit.

## Causality `Ca_Select` extension

`ca_internal.h` `Ca_Select` struct gained:
  `int hover_item` — index under cursor (-1 = none)
  `Ca_SelectFn on_hover` + `void *hover_data`

`paint.c` `paint_overlays()` updates `hover_item` and fires `on_hover` when it changes.
Both close-paths (click option / click outside) reset `hover_item = -1` and fire `on_hover`.

`ca_select_get_hover(const Ca_Select*)` exposed in `ca_components.h`.

## Plugin API

- `sol_plugin_register_theme(ctx, desc)` validates CSS, copies it into the UI registry.
- `sol_plugin_unregister_theme(ctx, id)` removes an owned theme early.
- Plugin cleanup removes all tracked themes before `dlclose`.

## Validation

- `sol_ui_system_register_theme` registers then validates via `ca_css_parse`. Rolls back on failure.
- `sol_ui_on_theme_change` bumps `sig_theme_rev` even on parse failure so the Settings UI stays coherent.
