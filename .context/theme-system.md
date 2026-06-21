# Sol Runtime Theme System

## Ownership

- `sol/include/sol_theme.h` and `sol/src/core/sol_theme.c` own the bounded theme registry.
- Registry entries copy ids, names, and CSS. A descriptor may provide `base_id`; registration snapshots `base CSS + override CSS`, so unloading a base cannot invalidate derived themes.
- The first registered theme is active. Removing the active theme selects the first remaining entry.
- `SolUISystem` owns the registry and parsed active `Ca_Stylesheet`.

## Built-in Themes

- `com.sol.theme.glass` is the only built-in theme. It is a complete baseline stylesheet defined in `sol/src/ui/style.h` as `SOL_UI_DEFAULT_THEME_CSS`.
- All other themes are shipped via `plugins/sol-plugin-themes` (plugin id: `com.sol.themes`).
- Theme CSS owns: chrome (titlebar, status bar, splitters), panels (file tree, side panel, PM, settings), editor (buffer body, gutter, tabs, tab text, scrollbars, selection, caret), syntax tokens, aux windows (file picker, search, welcome, terminal, command flow popup).
- Terminal ANSI indexed/RGB colors remain process content. Terminal surfaces, default text, and typography are CSS-owned.

## Plugin Themes (`plugins/sol-plugin-themes`)

- Registers four themes via `sol_plugin_register_theme`, each with `base_id = "com.sol.theme.glass"`.
- `com.sol.theme.ocean` — teal/cyan (Deep Ocean)
- `com.sol.theme.amethyst` — purple/violet (Amethyst)
- `com.sol.theme.graphite` — neutral monochrome (Graphite)
- `com.sol.theme.ember` — amber/rust (Ember Glass)
- Each override covers all major visual surfaces: chrome, panels, editor, syntax, aux windows, terminal.
- The composed CSS (base + override) is validated by `sol_ui_system_register_theme` before acceptance.

## Runtime Application

- `sol_ui_system_set_active_theme()` changes the registry selection.
- The registry observer parses the selected complete CSS before replacing the old stylesheet.
- `ca_instance_refresh_styles()` re-resolves all CSS-owned visual and layout fields for every live window, rebuilds system title bars, invalidates layout/paint, and wakes the event loop.
- Widget-owned foreground/fill state is synchronized during CSS resolution for labels, buttons, inputs, checks, radios, tree nodes, progress bars, and splitters.
- Selection is persisted under `theme.style` in `~/.sol/settings.json` and restored after plugins load.

## Plugin API

- `sol_plugin_register_theme(ctx, desc)` validates CSS, copies it into the UI registry, and tracks the id on the plugin context.
- `sol_plugin_unregister_theme(ctx, id)` removes an owned theme early.
- Plugin cleanup removes all tracked themes before `dlclose`; removing an active plugin theme applies and persists the built-in fallback.
- Use a complete stylesheet by leaving `base_id` null, or set `base_id = "com.sol.theme.glass"` and provide a CSS override document.

## Settings UI

- Settings > Theme enumerates the live registry and applies selections immediately.
- Registry revisions rebuild an already-open Settings window when plugins add or remove themes.

## Validation

- `sol_ui_system_register_theme` registers first, then validates the **composed** CSS (base + override as stored) via `ca_css_parse`. Rolls back via `sol_theme_unregister` on failure.
- `sol_ui_on_theme_change` bumps `sig_theme_rev` even when `ca_css_parse` fails so the Settings UI stays coherent; it does not replace the old stylesheet.
- Settings > Theme click does not double-save; persistence flows exclusively through the registry change callback in `sol_ui_on_theme_change`.

## Verification

- `sol_theme_tests` covers ownership, copied CSS, inheritance, duplicate rejection, selection, and active-theme fallback.
- `sol_plugin_tests` covers manual theme removal and automatic unload cleanup.
