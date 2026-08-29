# Sol runtime theme system

## Ownership

- `sol_theme.h` and `sol_theme.c` own the bounded registry, copied CSS, and
  semantic background / primary / accent metadata.
- A derived theme snapshots its base CSS and inherits both semantic colors
  unless its descriptor explicitly marks a replacement palette.
- `workspace.c` forwards the active registry colors directly to the background
  effect registry. Theme-aware rendering no longer scrapes CSS declarations.
- `style.h` remains the complete `com.sol.theme.glass` fallback and structural
  baseline. Fresh settings select `com.sol.theme.midnight` after plugins load.

## Curated themes

`plugins/sol-plugin-themes` uses one data-driven palette model and one semantic
CSS generator. It carries all 66 palettes from LocalDocsMD's theme registry:

- Midnight, Daylight, Catppuccin, Obsidian, OLED
- Dracula, Nord, Gruvbox, Solarized Light/Dark, Tokyo Night, Monokai
- GitHub Light/Dark, Forest, Rose, Sunset, Ocean, Aurora, Slate
- Copper, Sakura, Terminal, Coffee, Arctic, High Contrast Light/Dark
- Cyberpunk, Neon, Synthwave, Retro
- Amber, Mint, Lavender, Peach, Sky, Lemon
- Moonlight, Kanagawa, Everforest, Rosé Pine, Ayu Light/Dark
- One Light/Dark, Material Light/Dark, Palenight, Panda, Horizon
- Pitch Black, Paper, Newspaper, Ink, Dusk, Pastel, Teal
- Woodland, Desert, Volcano, Deep Sea, Grape, Ash, Crimson, Ice, Coral

Each palette defines background, layered surfaces, primary/accent, text levels,
and status colors. The generator consistently themes system chrome, workspace,
editor and syntax, auxiliary windows, overlays, terminal, and plugin surfaces.
Dark editor glass uses a 54% surface and dark navigation uses 64%; light themes
use 68% and 74% respectively so animation remains present without losing text
contrast.

## Runtime application

- Plugin themes extend `com.sol.theme.glass` and are copied by the registry.
- A plugin can track up to the registry's `SOL_THEME_MAX` owned theme IDs, so
  ownership cleanup cannot become a smaller silent limit. The bounded capacity
  is 96, leaving room beyond the 66 plugin themes and Glass fallback.
- Theme changes parse the complete CSS before replacing the live stylesheet,
  refresh every Causality window, then push semantic colors to the active
  background.
- Unknown saved theme IDs migrate in memory to Midnight, then Glass if the
  themes plugin is unavailable.
- Settings preview/commit behavior remains observer-driven through the theme
  registry and `sig_theme_rev`.

## Validation boundary

- `sol_theme_tests` covers copied CSS, derived CSS, semantic-color inheritance,
  active selection, removal, invalid descriptors, and the 96-entry boundary.
- Runtime startup is required to prove every generated stylesheet registers;
  a successful C build alone cannot validate Causality CSS parsing.
