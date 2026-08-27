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
- CJK remains uncovered (needs a Noto CJK asset, tens of MB — deliberately out of scope).
- The renderer architecture can cache arbitrary codepoints, but it can only rasterize glyphs available in bundled faces.

## Emoji fallback layer (2026-08-25)

Symptom: Claude Code's CLI running in Sol's terminal panel showed `??` for its
status-indicator icon (e.g. the "auto mode" toggle glyph, in the 1F300+
pictograph range) — DejaVu Sans covers text/symbol blocks (arrows, Braille,
geometric shapes) but has almost no emoji pictograph coverage (Misc Symbols &
Pictographs, Transport & Map, Supplemental Symbols & Pictographs were >90%
missing across every embedded face). This was the known boundary noted above,
now closed for emoji specifically.

Fixed by embedding a 4th face: `emoji_face` / `emoji_data` in `Ca_Font`, blob
at `src/renderer/embedded_emoji_font.c` (887 KB), symbols
`ca_embedded_emoji_font_{data,size}`.

- Source: Noto Emoji (monochrome outline variable font), static-instanced to
  its Regular (wght=400) weight via `fonttools varLib.instancer` — a plain
  outline font FreeType rasterizes like any other face, unlike Noto Color
  Emoji's CBDT/COLR tables which would need different rendering code.
- License: SIL Open Font License 1.1 — permissive, no royalty, attribution
  via LICENSE text only. Recorded in `vendors/causality/NOTICE`. Chosen over
  the color variant specifically so no new rendering path was needed and
  size stayed small (~870 KB vs 10s of MB).
- Resolution order in `font_render_glyph` (`font.c`): styled face -> regular
  face -> DejaVu fallback face -> **emoji face** -> `?`. Consulted last so a
  codepoint present in an earlier face always renders from that face.
- Coverage after the fix: Emoticons 80/80, Misc Symbols & Pictographs
  637/768, Transport & Map 105/128, Supplemental Symbols & Pictographs
  242/256 (verified via FreeType `FT_Get_Char_Index` against the embedded
  blob, not just fontTools cmap parsing).

Related terminal-side fix: `term_codepoint_is_wide()` in `sol_terminal.c` was
missing the `0x1F680-0x1F6FF` (Transport and Map) and `0x1FA00-0x1FAFF`
(Symbols/Pictographs Ext-A) ranges, so those emoji were measured as
single-width cells even though real terminals render them double-width —
this desynced column position for any text following such an emoji. Added
both ranges alongside the existing emoji entries. See
`.context/terminal-architecture.md` for the terminal-side write-up.

Verification: `cmake --build build -j 11` passes; full ctest suite (14/14)
passes; standalone FreeType probes against the extracted embedded blobs
confirmed glyph indices resolve for previously-`?` codepoints (e.g. U+1F501
cycle arrows: gi 760, U+1F916 robot: gi 1103).

Verification:
- `cmake --build build -j 8` passes.
- The named private-use icons in `ca_icons.h` were checked against the embedded Roboto/Symbols layer; all listed icon codepoints are present.
- Focused Sol suites pass: file tree, buffer, text buffer, integration, command flow, and plugin tests.
- Sol uses named icons for file tree entries, file picker entries/toolbar/sort/footer actions, buffer-tab close, welcome actions, plugin actions, Causality title-bar controls, tree disclosure, and menu submenu chevrons.
- Additional event, fuzzy, and search tests pass. `sol_rope_tests` is blocked in this sandbox because its `tmpnam` path cannot be opened for writing.

## BMP legacy-emoji column desync (2026-08-27)

Symptom: Claude Code's `>>` auto-mode indicator (U+23E9 fast-forward) rendered
as `??` in Sol's terminal panel, even though the embedded emoji face has the
glyph (confirmed via a standalone FreeType probe against the extracted blob:
`FT_Get_Char_Index` returns a valid glyph index for U+23E9 in
`ca_embedded_emoji_font_data`).

Root cause was terminal-side, not font-side: `term_codepoint_is_wide()` in
`sol/src/core/sol_terminal.c` only classified the SMP emoji blocks
(U+1F300+) as double-width. U+23E9 lives in Miscellaneous Technical
(U+2300-23FF), a BMP block, so it was measured as a single-width cell. The
renderer then drew one double-width emoji glyph into a cell budgeted for
one column, desyncing the following cell — the classic wide-char-measured-
narrow bug, same failure mode as the prior Transport & Map / Symbols Ext-A
fix below, just in a different Unicode block.

Fix: added `term_codepoint_is_bmp_wide_emoji()` — a precise, non-contiguous
set of BMP codepoints with Unicode's `Emoji_Presentation=Yes` property
(emoji-data.txt), the same property real terminals (Alacritty's
unicode-width, kitty, wcwidth9) key off for BMP legacy emoji width.
Deliberately **not** the whole 2600-26FF/2700-27BF/2B00-2BFF blocks —
those are sparse; most codepoints in them (suit symbols, plain arrows,
dingbat punctuation) are `Emoji_Presentation=No` and must stay
single-width. Widening the whole block would have desynced columns for
plain-text uses of those characters. Codepoints that are emoji-capable but
default to text presentation (need explicit VS16/U+FE0F to become wide,
e.g. U+2194 arrows, U+2764 heart, U+2934/2935, U+3030, U+303D, U+3297,
U+3299) are intentionally excluded — Sol's VT parser does not track
variation selectors as a separate combining step.

Verification: standalone FreeType probe (compiled against the vendored
FreeType `.o`s + all 5 embedded font blobs, linking real `hvf.c.o` +
system `libhvf` to satisfy the optional HVF module FreeType's `ftinit.c`
references unconditionally) confirmed glyph coverage for all newly-added
ranges in the emoji face before writing the fix. `cmake --build` and full
`ctest` (14/14) pass after the change.
