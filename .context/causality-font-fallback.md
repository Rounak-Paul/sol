Task context: support missing UI glyphs through bundled Causality font data without OS font dependencies.

Existing architecture:
- Font rendering uses FreeType and a 4096x4096 RGBA atlas split into LRU pages keyed by visual size and style.
- Hot codepoint ranges are pre-indexed for ASCII/Latin-1 and editor icon ranges.
- Codepoints outside those ranges already use a per-page extra-glyph hash table, so the atlas can cache arbitrary codepoints without eagerly reserving the full Unicode space.

Problem:
- When a codepoint is absent from the bundled regular/bold/icon faces, `font_render_glyph` falls back to `?`.
- System-font fallback was rejected because Sol/Causality should stay self-contained.
- Procedural glyph generation was rejected for UI arrows; glyphs should come from the selected font pack when possible.

Fix:
- Do not scan or load OS/system fonts.
- Remove procedural arrow generation.
- Swap the embedded default to Departure Mono Nerd Font Mono, which contains the U+2190-U+2193 arrows and the Nerd Font icon ranges needed by Sol UI.
- The downloaded package only contains regular faces; the Mono regular face is embedded into both regular and bold slots until a real bold Departure Mono face is supplied.

Boundary:
- True "all possible Unicode" coverage cannot be achieved self-contained unless the project embeds a broad fallback font asset such as a Noto family font.
- The renderer architecture can cache arbitrary codepoints, but it can only rasterize glyphs available in bundled faces.
