# Visual composition

Sol's background is a depth layer, not the primary surface. The curated Glass
composition uses Aurora at 0.78 intensity, three blur passes for the chrome,
and one pass beneath the editor.

- Title bar, explorer, status bar, and overlays use cool near-black translucent
  materials so the effect remains visible but text retains contrast.
- Editor and gutter materials are lighter than the chrome and allow enough of
  the background through for blur to read without obscuring code.
- Blur regions are localized to material surfaces and begin below the custom
  title bar. Do not add blur to every layer; stacked blur reads as fog.
- Defaults live in `sol_settings.h`; the current user profile is intentionally
  aligned to the same Aurora preset.
