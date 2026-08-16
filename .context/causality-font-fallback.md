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
- Restore Roboto Mono Nerd Font Mono regular/bold as the embedded text layer.
- Add Symbols Nerd Font Mono regular from `/Users/duke/Downloads/NerdFontsSymbolsOnly` as the embedded icon layer.
- Use named Symbols-layer icon constants for UI sort indicators instead of generated arrows or Unicode arrows missing from Roboto.
- Shared icon names are defined in `vendors/causality/causality/include/ca_icons.h`.
- `ca_icons.h` is generated from upstream Nerd Fonts `glyphnames.json` version 3.4.0 and filtered against the embedded Roboto/Symbols cmaps. It exposes 10,764 canonical `CA_ICON_NF_*` macros plus compatibility aliases for shorter existing Sol/Causality names.
- `causality.h` publicly includes `ca_icons.h`, so Causality users can use these names directly after including the main public header.
- `font_path` / `bold_font_path` override only the regular/bold text faces. The embedded Symbols icon layer remains active for `CA_ICON_*` glyphs when text faces are overridden.
- The embedded font bytes remain internal renderer assets, not a public raw-font-data API.
- Symbols Nerd Font Mono icon glyphs use full-em advances while Roboto Nerd Font Mono icons use about 0.60em. The renderer normalizes Symbols icons with a 0.78 scale and slight baseline raise, giving a middle-ground visual size that is smaller than raw Symbols but not as small as Roboto-patched icons.

Unicode fallback layer (2026-08-16):
- A Nerd Font patch injects icons into the private-use area only; it does not extend
  the base font's coverage of ordinary Unicode text blocks. RobotoMono NF covers
  Latin/Greek/Cyrillic/box-drawing/blocks; Symbols NF is private-use only. Neither
  face had arrows (U+2190-21FF), Braille (U+2800-28FF), geometric shapes,
  dingbats, or most math operators — all rendered as `?`.
- Fixed by embedding DejaVu Sans 2.37 as a third face: `fallback_face` /
  `fallback_data` in `Ca_Font`, blob at `src/renderer/embedded_fallback_font.c`
  (757 KB), symbols `ca_embedded_fallback_font_{data,size}`.
- Resolution order in `font_render_glyph`: styled face -> regular face ->
  fallback face -> `?`. The fallback is consulted last so a codepoint present in
  the chosen font always renders from that font; `is_icon_range` is cleared when
  the fallback resolves so Symbols-layer scale/baseline tweaks are not applied.
- Licensing: Bitstream Vera / Arev, permissive with notice. Recorded in
  `vendors/causality/NOTICE`. Still self-contained — no OS font lookup.

Boundary:
- CJK and most emoji remain uncovered (DejaVu Sans has neither). Closing that
  needs a Noto CJK asset, tens of MB — deliberately out of scope.
- The renderer architecture can cache arbitrary codepoints, but it can only rasterize glyphs available in bundled faces.

Verification:
- `cmake --build build -j 8` passes.
- The named private-use icons in `ca_icons.h` were checked against the embedded Roboto/Symbols layer; all listed icon codepoints are present.
- Focused Sol suites pass: file tree, buffer, text buffer, integration, command flow, and plugin tests.
- Sol uses named icons for file tree entries, file picker entries/toolbar/sort/footer actions, buffer-tab close, welcome actions, plugin actions, Causality title-bar controls, tree disclosure, and menu submenu chevrons.
- Additional event, fuzzy, and search tests pass. `sol_rope_tests` is blocked in this sandbox because its `tmpnam` path cannot be opened for writing.
