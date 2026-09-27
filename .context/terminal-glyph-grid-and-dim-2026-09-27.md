# Terminal glyph grid, missing symbols, dim text (2026-09-27)

User report (Claude Code running in Sol's terminal): diff lines shift after
message dots, auto-mode icon shows `??`, prompt suggestions not dim and seem
not to go away while typing.

## Evidence (captured, not guessed)
- Captured Claude Code's real PTY output (python pty, 120x40) and replayed it
  through Sol's VT parser (scratch harness including sol_terminal.c): the
  grid state was CORRECT — suggestion `Try "..."` carries SGR 2 and is erased
  by `CSI K` on the first keystroke. So the bugs were render-side.
- FreeType probe of the embedded faces @13px (cell = 8px):
  ⏺ U+23FA / ⏸ only in Noto Emoji (adv 17px); ● ✳ ⚠ ✓ ◐ from DejaVu Sans
  (proportional, 11-12px); ⏵ U+23F5 (auto mode) and ⎿ U+23BF (tool-result
  elbow) in NO face → `?`.
- terminal_panel.c renders attribute runs as text, so glyph advances ≠ cell
  width shifted the rest of the row. SOL_TERM_ATTR_DIM was parsed but never
  rendered → suggestions looked like typed text.
- VT parser gave zero-width codepoints (VS16, ZWJ, combining) a full cell.

## Fix
Causality (vendors/causality):
- `ca_codepoint_cell_width(cp)` (causality.h, src/core/ca_unicode_width.c):
  0/1/2 from generated UCD tables `src/core/ca_unicode_width_table.h`
  (tools/gen_unicode_width.py; UnicodeData + EastAsianWidth 18.0). 0 = Mn/Me/
  Cf/Cc (not U+00AD) + Hangul medial jamo; 2 = EAW W/F + unassigned-W ranges.
- New face "Causality Mono Symbols" = renamed JuliaMono subset (OFL RFN
  requires rename), 2865 symbol-block codepoints minus Roboto-covered and
  minus Emoji_Presentation=Yes. tools/subset_mono_symbols.py + embed_font.py
  → src/renderer/embedded_mono_symbols_font.c (693 KB). NOTICE updated.
- font_render_glyph order: styled → regular → mono symbols → emoji → DejaVu
  (emoji moved before DejaVu). Face loads deduplicated via
  font_load_embedded_face.
- Grid snap: glyphs from fallback layers (and the `?` stand-in) get advance
  = cell × ca_codepoint_cell_width when the primary face is FT_IS_FIXED_WIDTH;
  wider glyphs rendered at fractional size (font_set_face_size_exact) to fit,
  then centred. Cell = '0' advance measured with the tier's text flags,
  cached in Ca_FontTier.cell_advance. Missing zero-width cp → empty glyph.
- Test: tests/ca_font_grid_tests.c (width table + snapping at scale 1/1.5/2,
  10–20px, regular/bold). Connector glyphs (⎿, box drawing) ink into the next
  cell by design — only scaled glyphs are ink-bounded.

Sol:
- sol_terminal.c: term_put_char uses ca_codepoint_cell_width; width 0 is
  dropped. Removed the hand-kept term_codepoint_is_wide/bmp emoji lists
  (they missed 🟠 1F7E0.., 🀄, squared letters).
- terminal_panel.c: SGR 2 → foreground alpha × 3/5 (term_dim_rgba).
- test_terminal.c: cell widths for Claude glyphs/emoji/zero-width; SGR dim.

## Underline / strikethrough (follow-up, same day)
- Causality: `text-decoration` now parses to CA_TEXT_DECORATION_* flags
  (css.h) with combinations ("underline line-through"), stored as a NUMBER
  value (css.c custom decl handler; style.c resolves it). Unknown tokens drop
  the declaration.
- Font tiers carry underline_position/thickness (post table) and
  strikeout_position/thickness (OS/2), logical px, positive = below baseline.
- paint.c `paint_text_decoration` emits pixel-snapped rects; wired into
  paint_text, paint_text_left, paint_text_wrapped (per line) and
  paint_inline_run (per run).
- Side effect (intended): existing Sol CSS now renders — markdown links /
  strikethrough / done tasks, `.scm-file-name-old`.
- Sol: classes `.term-underline/.term-strike/.term-underline-strike`;
  terminal_panel.c `term_cell_style` picks static class-list literals
  (pointer-compared for run splitting).
- VT: CSI now parses ':' sub-parameters (vt_param_sub). SGR 4:0 off, 4:1-5 on,
  21 on, 38/48/58 accept colon and semicolon forms (58 consumed, not drawn),
  colour components clamped to 255, params saturate at 65535, leading ';'
  keeps param 0 default. Before, "4:3" became 43 (yellow bg).
- Tests: ca_text_clip_tests (parse + rect placement), ca_font_grid_tests
  (real metrics sane), test_terminal.c test_sgr_subparameters.

## Underline styles (follow-up, same day)
- Causality CSS: new `text-decoration-style: solid|double|dotted|dashed|wavy`
  (CA_CSS_PROP_TEXT_DECORATION_STYLE → Ca_NodeDesc.text_decoration_style,
  CA_TEXT_DECORATION_STYLE_*); the `text-decoration` shorthand also accepts a
  style keyword ("underline wavy"). Style applies to every line of the node,
  per CSS.
- Rect shader (pipeline.c FRAG_GLSL) gained CA_DRAW_MODE_WAVE (5: blur_radius
  = wavelength, gradient_cx = stroke) and CA_DRAW_MODE_DASH (6: blur_radius =
  period, gradient_cx = dash length). Both phase on absolute x
  (v_node_pos.x), so adjacent terminal runs continue one pattern. One draw cmd
  per line regardless of length.
- paint.c: paint_decoration_rect + style switch in paint_text_decoration.
  double = two t-thick rects 2t apart; dotted = period 2t/dash t; dashed =
  period 5t/dash 3t; wavy = height max(4t+1, 0.3em), wavelength max(4t,
  0.6em ≈ one mono cell), stroke t+0.5 (curve at arbitrary pixel phase looked
  faint at 1x with stroke t — checked with a CPU replica of the GLSL rendered
  to PNG).
- Sol: attrs bits 10-12 = SolTermUnderlineStyle (sol_terminal.h,
  sol_term_underline_style()). SGR 4:1-5 / 21 / 4 / 24 / 4:0; out-of-range
  4:n → single. terminal_panel.c builds a static class-list table
  (base × decoration × ul-style) once; classes .term-ul-double/curly/dotted/
  dashed in style.h.
- Verified: glslc compiles the shaders; 22/22 tests incl. parse, draw
  mode/geometry, and terminal SGR style tests; isolated live launch clean.

## Underline colour (follow-up, same day)
- Causality: CSS `text-decoration-color` (CA_CSS_PROP_TEXT_DECORATION_COLOR →
  Ca_NodeDesc.text_decoration_color, 0 = text colour/currentColor); the
  `text-decoration` shorthand accepts a colour token too ("underline wavy
  red"). Ca_TextDesc.decoration_color overrides CSS like `.color` does;
  ca_text compares against the pre-CSS snapshot for dirtying.
  paint_text_decoration takes the text colour and substitutes the node's
  decoration colour when set.
- BUG FIXED: content_desc_changed() (node.c) ignored text_decoration /
  _style / _color, so a reused text node whose only change was its
  decoration kept stale cached paint. Now compared.
- Sol: SolTermColor.mode is uint8_t (struct 8 → 4 bytes); SolTermCell gained
  `ul` (SGR 58, DEFAULT = follow fg) and still shrank 24 → 20 bytes. SGR 58
  (colon and semicolon forms) sets, 59 / 0 reset. terminal_panel.c splits
  runs on the resolved underline RGBA and passes .decoration_color; removed
  the dead cells_same_run().
- Tests: test_sgr_underline_color (incl. sizeof guards), text_clip
  colour parse/paint + content_desc_changed checks. 22/22 pass, clean
  isolated launch, installed.

## Not done
- Underline style also applies to a strikethrough on the same cell (CSS
  semantics: one style per node); same for decoration colour.
- Combining marks are dropped (cell holds one codepoint).
- Not visually verified in a live window (no screen capture here).

## "Everything underlined" under Claude Code (follow-up, same day)
- Cause: Claude Code sends `CSI > 4 m` (XTMODKEYS reset) at startup.
  vt_csi_dispatch ran every final 'm' as SGR regardless of the private
  marker, so it set SOL_TERM_ATTR_UNDERLINE on the pen, and all text after it
  was underlined. The bug was already there; it only became visible once
  underline was rendered.
- Evidence: replayed a real Claude PTY capture through the VT parser. Before
  the fix most rows had the underline attr; after it, only the markdown
  heading does (Claude really sends SGR 4 there).
- Fix: `case 'm'` runs vt_sgr only when vt_csi_marker == 0.
  test_marked_csi_m_is_not_sgr covers `>4m`, `>4;2m` and `?4m`.
- Other finals (e.g. 'r') still ignore the marker; not changed.
