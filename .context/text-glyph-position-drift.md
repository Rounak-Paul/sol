# Text/Cursor Position Drift at Non-1.0 UI Scale (2026-08-27 – 2026-08-29)

## Actual root cause (found 2026-08-29, after two other real-but-partial fixes below)

The caret rendering visibly mid-glyph (not at a cell boundary) at any
`ui_scale != 1.0` — reproduced on this Retina MacBook itself (not
Linux/DPI-specific at all, since Retina's `display_scale > 1.25` means
`snap_text_position` is a complete no-op here, ruling out everything in
the two fixes below as the cause of *this* symptom) — was
`scale_resolved_style()` in
`vendors/causality/causality/src/ui/widget.c` applying `ui_scale` to
`style->font_size`:

```c
style->font_size *= scale;   /* scale == g_ctx.window->ui_scale */
```

This directly contradicts the explicit, already-written contract comment
on `rescale_desc()` in `ui.c`: *"font_size is intentionally author-space:
paint/layout use ui_scale when selecting the atlas tier, so scaling it
here would double-count zoom."* Every actual consumer of
`node->desc.font_size` (`layout.c`'s text measurement, all three
`paint.c` glyph-drawing functions, Sol's `glyph_advance_px_for` in
`text_view.c`) already multiplies by `ui_scale` itself
(`desired_size * ui_s`) when selecting the atlas tier — they all assume
`font_size` arrives as raw CSS px. `scale_resolved_style` was the one
place doing it a second time.

Effect: for `.buffer-line` (`font-size: 12px`), `node->desc.font_size`
ended up `13.44` (`12 * 1.12`) instead of `12`. `paint_text` then
computed the atlas tier at `13.44 * 1.12 ≈ 15.05` instead of the correct
`13.44`, silently shifting every character's rasterized width — while
Sol's caret math (`glyph_advance_px_for` → `ca_measure_text_px(win, "M",
SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)`) passes the hardcoded, never-CSS-touched
constant `12.0f` directly, correctly landing on tier `13.44`. Two
different tiers → two different per-character advances → the caret
(correct) and the painted text (silently shifted) disagree by a fixed,
non-compounding ratio, which is exactly "caret sits inside a glyph
instead of between two glyphs," visible immediately (not something that
grows with line length, unlike the two bugs below).

Confirmed with temporary instrumentation (`fprintf` in `paint_text` and
the caret's `text_view.c` computation, launching `./bin/sol
CMakeLists.txt` and reading stderr — this machine has no Screen
Recording/Accessibility permission granted to the shell, so a live
screenshot/click-driven repro wasn't possible; numeric stderr capture was
the only way to get hard evidence instead of guessing further after two
wrong turns) — before the fix: buffer-line `desired_size=13.4400`, caret
`adv=7.144186`. After removing `style->font_size *= scale`: buffer-line
`desired_size=12.0000` (matches the caret's internal `12 * ui_scale`
computation exactly).

**Fix**: deleted `style->font_size *= scale;` from `scale_resolved_style`
in `widget.c`, with a comment cross-referencing the `ui.c` contract this
restores. Audited every other reader of `node->desc.font_size` across
Causality first (`layout.c`, `paint.c`, tooltip/menu font-size fallbacks)
— all treat it as author-space, confirming this was the sole outlier and
safe to remove outright. Left `letter_spacing`/`line_height`/
`word_spacing`/`flex_basis` untouched — `letter_spacing` has the same
theoretical class of bug (its sole consumer in `paint.c` also expects
author-space) but is unexercised (`.buffer-line` never sets it) so was
left alone rather than risk an unverified second change.

This is the bug that actually explains what the user saw and reported —
the two entries below were real, legitimate bugs found and fixed along
the way (both still correct, both still needed), but neither was
sufficient on its own, and this session's investigation should have
checked "does font_size survive CSS resolution unscaled" before diving
into `paint.c`'s accumulation math both previous times.

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

## Follow-up regression (2026-08-29) — wrong raster-space unit for the initial snap

The first version of this fix introduced a second, more severe bug: the
one-time snap that establishes `glyph_raster_xpos` at the start of each
line/word used `cs` (`content_scale/ui_scale`) as the raster-space
conversion factor:

```c
float glyph_raster_xpos = snap_text_position(left_logical * cs, cs, font->display_scale);
```

But every glyph's actual draw math inside the loop operates in units of
`glyph_cs_eff = cs / font_scale` (`font_scale = desired_size /
tier->logical_px`), not plain `cs`. The old (pre-refactor) code was safe
here because it converted the one-time snap result back down to logical
space (`/ cs`) before the loop, then correctly re-entered raster space via
`xpos * glyph_cs_eff` on first use. The new code skipped that round trip
and treated the `cs`-scaled snap result as if it were already in
`glyph_cs_eff` units — off by a factor of `font_scale`.

At `font_scale == 1` (tier's quantized `logical_px` happens to equal
`desired_size` exactly) this is a no-op and invisible — which is why it
passed the earlier numerical verification, which used inputs where that
coincidentally held. At any `ui_scale` where the tier doesn't land exactly
on the requested size (the normal case — confirmed with a real screenshot
showing window title text, menu labels, file tree entries, and the
title-bar min/max/close icons all rendering many pixels off-node —
clipped, or entirely outside their button's visible bounds while still
being clickable, since layout/hit-testing were never touched, only
paint position).

Fix: compute `line_cs_eff = ca_font_glyph_cs_eff(tier, desired_size, cs)`
once per function (using the line's primary tier — safe, since all
glyphs on one line share one tier; see the earlier fallback-face
investigation above) and use it — not `cs` — for every
`snap_text_position` call and every raster-space delta (`space_adv *
line_cs_eff`, `letter_spacing * line_cs_eff`) in all three functions.

**Lesson**: when splitting a value into "logical" and "raster" space
variants, double-check *which* raster-space conversion factor is correct
at each call site — `cs` and `glyph_cs_eff` differ by `font_scale`, and
that factor is silently 1.0 (making the bug invisible) exactly when the
requested font size happens to already be a multiple of the atlas tier's
1/64px quantization grid, which is common enough at the default 1.0 scale
to pass a superficial check but wrong in general.
