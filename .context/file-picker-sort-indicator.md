Task context: file picker sort indicator rendered as a question mark.

Runtime facts:
- `file_picker.c` rendered active sort state by baking Unicode arrows U+2191/U+2193 into the header label string.
- Causality's dynamic font cache currently covers ASCII/Latin-1 plus the editor's Nerd Font private-use icon ranges. U+2191/U+2193 are outside those ranges, so missing glyph fallback renders as `?`.
- File picker toolbar/file icons already use private-use Font Awesome/Nerd Font glyphs that are supported by the renderer.

Fix:
- Use Font Awesome private-use sort glyphs for the active sort indicator: U+F0DE for ascending and U+F0DD for descending.
- Render the indicator as its own `ca_text` node using the existing `.fp-colhdr-sort-arrow` style instead of mixing unsupported arrows into the label text.
- Preserve column alignment by placing the icon before right-justified headers and after left-justified headers.
