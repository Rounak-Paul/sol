# Glyph edge clipping (2026-09-12)

Owner: `vendors/causality/causality/src/ui/paint.c`, `text_clip_for_node`.

Labels are laid out using glyph advances. A rasterized glyph's ink can extend
past that advance (or before its origin), so the advance box is not an ink clip.
Sol's syntax spans and terminal color/cursor runs are separate labels; automatic
self-clipping could therefore cut letters at each run boundary, not just the
viewport edge. The old helper also left `ClipRect.radius` uninitialized.

Causality now inherits ancestor clipping for visible overflow and intersects
self bounds only on explicitly hidden/scrolling axes. It no longer expands
ancestor clips by a fixed vertical padding. Text inputs explicitly retain their
own clipping boundary. Font advances, atlas data, layout and caret math are
unchanged. An explicit hidden/scrolling boundary still intentionally clips ink.

Validation: bundled Roboto Mono `w` glyphs at 8–32 px across scales 1, 1.12,
1.25, 1.5 and 2 produce eight cases whose raster bounds exceed their advance.
The new Causality regression fails against the original clipping helper and
passes against the fix. Tests also cover rounded ancestor inheritance,
independent overflow axes, zero width, and disjoint clips. Build, all 17 CTest
tests and both repository diff checks pass. Live screenshot verification was
unavailable: computer-use could resolve neither Sol nor its absolute binary
path, and Sol was absent from the available application inventory.

Regression: `vendors/causality/causality/tests/ca_text_clip_tests.c`.
