# Text/Cursor Position Drift at Non-1.0 UI Scale (2026-08-27)

## Symptom

On a Linux machine with Sol's UI-scale setting at a non-1.0 value (user
reported ~1.12), the editor text buffer's cursor progressively fell behind
(left of) where it visually should be as a line got longer, and some
glyphs appeared clipped. Not reproducible on macOS at the default 1.0 UI
scale or at typical Retina 2x display scale.

## Root Cause

`vendors/causality/causality/src/ui/paint.c` — three text-drawing functions
(`paint_text`, `paint_text_left`, `paint_text_wrapped`) positioned each
glyph by repeatedly round-tripping an accumulated **logical**-space `xpos`
through `snap_text_position()`:

```c
float glyph_xpos = snap_text_position(xpos * glyph_cs_eff, glyph_cs_eff, font->display_scale);
...
xpos += pc->xadvance / glyph_cs_eff;   /* convert raster advance -> logical, accumulate */
```

`snap_text_position` rounds to the nearest pixel only when
`display_scale <= 1.25` (the low-DPI path; HiDPI returns the value
unsnapped/fractional, which is why this never surfaced on Retina).

`glyph_cs_eff = (content_scale/ui_scale) / (desired_size/tier->logical_px)`.
`tier->logical_px` is quantized to 1/64px (`font_size_key`), so whenever
`ui_scale` isn't 1.0 (or another value that happens to make
`desired_size * ui_scale` land on a clean tier boundary), `glyph_cs_eff`
is *not exactly* 1.0/`content_scale` — off by a tiny residual (e.g.
1.0004783 instead of 1.0 at ui_scale=1.12, content_scale=1.0).

Each glyph's *rasterized* advance (`pc->xadvance`, from FreeType, honors
hinting) is essentially always an exact whole raster pixel for a genuinely
monospace hinted face — confirmed by direct FreeType probe: Roboto Mono
Nerd Font Mono has `FT_FACE_FLAG_FIXED_WIDTH` set and every glyph's
`advance.x` is bit-identical, hinted or unhinted, at every tested pixel
size. So per-glyph advance variance was **ruled out** as a cause.

The actual mechanism: converting that exact raster advance back to logical
space (`/ glyph_cs_eff`) with a `glyph_cs_eff` that carries a tiny residual
produces a logical advance that is *not quite* an integer number of raster
pixels (e.g. 9.9952 instead of 10 at the example scale above). Snapping
the **cumulative** logical position each glyph
(`floor(xpos_cumulative * display_scale + 0.5)`) discards the fractional
remainder independently every time instead of carrying it forward — the
discarded remainder does not average out, it compounds directionally
(`~0.0048px` per glyph in the example, reaching ~0.48px drift by character
100). This is a quantization/aliasing effect, not float32 summation error
(verified separately: pure iterative-add error over 120 terms is
~0.001px, negligible — confirmed via standalone probes before concluding
this was the real mechanism).

The caret position in `sol/src/ui/text_view.c` (`caret_x = cp_count * adv`,
a single multiply, never compounding) was correct the whole time — it's
the *painted glyphs* that drifted away from it, not the other way around.

## Fix

Changed all three paint functions to accumulate glyph position in
**raster space** using `pc->xadvance` directly (exact, from FreeType) via
a `glyph_raster_xpos` accumulator, instead of repeatedly deriving each
glyph's position from a compounding logical-space multiply-then-resnap.
The one-time line/word-start snap via `snap_text_position` is unaffected
(non-compounding — only the per-glyph loop changed). `paint_text_wrapped`
keeps its separate logical-space `xpos` for word-wrap width comparisons
against `max_w` (unrelated to draw position) and mirrors every `xpos`
reset (line break, space, wrapped word start) onto the new
`glyph_raster_xpos` accumulator so the two never desync.

Verified via standalone probes (compiled against the vendored FreeType +
extracted embedded font blobs — same technique as the earlier
[[causality-font-fallback]] BMP-emoji investigation) that the new
accumulation produces exactly `N * raw_advance` with zero drift at any N,
and that it now agrees with the caret's independent `N * adv` formula to
float32 epsilon (~0.0001px) rather than diverging.

## Why this was hard to find

Several plausible-looking theories were tested and ruled out before the
real mechanism was confirmed numerically:
- Per-glyph hinting variance across a "monospace" font — ruled out,
  FreeType probe showed zero variance.
- Wrong codepoint falling through to the non-monospace DejaVu fallback
  face — ruled out, full printable-ASCII + common punctuation coverage
  check showed no gaps in the primary Roboto Mono face.
- Plain float32 summation error from repeated `+=` — ruled out
  numerically (negligible, ~1000x smaller than observed drift).
- Content-scale (OS DPI) alone — ruled out by sweeping realistic Linux
  scale factors (1.0–1.25) with `ui_scale` fixed at 1.0; no drift.

The missing variable was Sol's own `ui_scale` setting (user-controlled,
independent of OS DPI) — confirmed by the user mid-investigation
("ui scaling set to something approx 1.12"), then reproduced exactly via
a standalone simulation before writing the fix.

## Files

- `vendors/causality/causality/src/ui/paint.c` — the fix (all three
  glyph-drawing loops).
- `vendors/causality/causality/src/renderer/font.h` — `ca_font_get_quad`
  (unchanged; confirmed it internally mutates `*xpos` but callers already
  discarded that mutation before this fix, so no double-advance risk).
- `.context/windows-font-rendering.md` — prior platform-specific font
  work (`display_scale`, low-DPI snapping) that motivated checking this
  code path first.
