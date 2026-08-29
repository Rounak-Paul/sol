# Visual composition

Sol follows the LocalDocsMD composition model: a theme-colored animated canvas
under semantic frosted surfaces.

- The active theme owns its canvas background, primary and accent effect colors,
  and surface, elevated, text, muted, success, warning, and danger roles.
- Editor surfaces reveal more motion than navigation and floating chrome.
  Localized Causality blur stays at three passes for chrome and one beneath the
  buffer by default.
- Animations provide transparent colored content rather than opaque scene
  backgrounds. Theme surfaces supply depth and readability.
- Midnight + Aurora is the fresh-install composition. Unknown removed theme or
  effect IDs fall back to the current defaults without retaining compatibility
  aliases.
- Theme and effect selection remain independent, but every effect consumes the
  active theme's semantic primary and accent colors.
