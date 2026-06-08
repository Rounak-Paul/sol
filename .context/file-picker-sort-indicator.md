Task context: file picker sort indicator rendered as a question mark.

Runtime facts:
- `file_picker.c` rendered active sort state by baking Unicode arrows U+2191/U+2193 into the header label string.
- Causality's dynamic font cache covers ASCII/Latin-1 plus the editor's Nerd Font private-use icon ranges, with an extra-glyph hash path for codepoints outside those hot ranges.
- File picker toolbar/file icons already use private-use Font Awesome/Nerd Font glyphs that are supported by the renderer.

Self-contained font fix:
- The atlas already has an extra-glyph hash path for codepoints outside the hot fixed ranges, so the correct fix is not to expand the eager range table indefinitely.
- The embedded default font is now Departure Mono Nerd Font Mono, which contains U+2191/U+2193.
- The file picker keeps using normal Unicode arrows for sort state; the glyphs come from the bundled font.
