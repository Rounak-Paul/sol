Task context: file picker sort indicator rendered as a question mark.

Runtime facts:
- `file_picker.c` rendered active sort state by baking Unicode arrows U+2191/U+2193 into the header label string.
- Causality's dynamic font cache covers ASCII/Latin-1 plus the editor's Nerd Font private-use icon ranges, with an extra-glyph hash path for codepoints outside those hot ranges.
- File picker toolbar/file icons already use private-use Font Awesome/Nerd Font glyphs that are supported by the renderer.

Self-contained font fix:
- The atlas already has an extra-glyph hash path for codepoints outside the hot fixed ranges, so the correct fix is not to expand the eager range table indefinitely.
- The embedded default text font is Roboto Mono Nerd Font Mono, with Symbols Nerd Font Mono as the icon layer.
- Roboto/Symbols do not provide the U+2191/U+2193 text arrows needed by the previous label string path, so the file picker uses named Symbols-layer sort icons (`CA_ICON_FA_SORT_ASC` / `CA_ICON_FA_SORT_DESC`) for sort state.
- Shared glyph names live in `vendors/causality/causality/include/ca_icons.h`; UI code should use those names rather than raw private-use byte strings.
- `ca_icons.h` includes 10,764 generated canonical `CA_ICON_NF_*` names from Nerd Fonts metadata, filtered to glyphs available in the bundled font layer.

Verification:
- `cmake --build build -j 8` passes.
- The Symbols font contains both sort icon codepoints used by the file picker.
- File picker keeps icons for file types, navigation toolbar controls, and sort state. Dialog-style actions such as Create, Select Folder, and Cancel stay text-only for a cleaner professional look.
