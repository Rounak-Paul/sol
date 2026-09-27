# Backdrop blur (CSS backdrop-filter) — current architecture

Rewritten 2026-09-27 to standard CSS semantics. Supersedes the 2026-09-19
per-band design (and everything in floating-glass-ui-overhaul-2026-08-29.md).

## Pipeline (vendors/causality/causality/src/renderer)
- `paint.c` emits `CA_DRAW_BACKDROP_BLUR` first for a node, then shadow, bg,
  children. `backdrop_blur` is scaled by ui_scale like other CSS lengths
  (widget.c `scale_resolved_style`, ui.c `rescale_desc`).
- `swapchain.c` `record_band`: walks paint order per band; content between
  backdrop commands is recorded by `record_range` (type-batched rects →
  glyphs → images → viewports). At each backdrop cmd `record_backdrop`
  ends dynamic rendering, calls `ca_blur_capture_region`, resumes with
  LOAD_OP_LOAD, draws one quad with `inst->backdrop_pipeline`.
  => an element's backdrop is exactly what painted before it; its own
  bg/text always paint over the blur. (Old bug: blur quad was drawn after
  the whole band's rects/text → covered own content, wrong colors.)
- `blur.c` `ca_blur_capture_region`: region-only (element bounds ∩ clip +
  3σ margin) blit to 1/2-res sRGB scratch, H pass → blur_temp, V pass →
  blur_image. True Gaussian σ = blur radius (CSS), σ_px = radius × content
  scale, merged bilinear taps, radius cap 96 texels. Composite samples
  `gl_FragCoord * uv_scale` (transform-correct), alpha = rounded-box
  coverage only. WAR barriers use FRAGMENT_SHADER src stage (multiple
  captures per frame).
- Shadow mode (`pipeline.c` MODE_SHADOW) is clipped out of the caster box
  (CSS outer box-shadow); paint.c passes shadow offset in gradient_cx/cy.

## Floating terminal (sol/src/ui/workspace.c sol_ui_term_float_builder)
- Frosted-glass card (user preference 2026-09-27): `.term-float-backdrop` is
  transparent with NO backdrop-filter (outside stays clear); the card blurs
  via the shared `.term-panel` panel_blur rule, and
  `.term-float-panel .term-viewport/.term-filler` are transparent so the
  frost shows in every widget style (Retro wells are otherwise opaque).
- Pure CSS geometry: `.term-float-backdrop` 100%/100% flex-centered,
  `.term-float-panel` width 82% height 78%. Never pass pixel sizes derived
  from `ui->window_w/h` into Ca_DivDesc — desc sizes are author units that
  Causality multiplies by ui_scale (was the off-center / overflowing card).
- `layout.c` flex placement: percentage children now get their resolved
  size as definite during `layout_node` (was double-applied: 82% → 67%).

## Verification method
No screen-recording permission, but the Vulkan SDK screenshot layer works:
`VK_LAYER_PATH=/usr/local/share/vulkan/explicit_layer.d
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation:VK_LAYER_LUNARG_screenshot
VK_SCREENSHOT_FRAMES=80 VK_SCREENSHOT_DIR=<dir> bin/Sol.app/Contents/MacOS/Sol`
→ PPM (convert with `sips -s format png`).

Menu/popup blur remains intentionally solid-color (see memory
backdrop_blur_removed).

## Command overlay (.cf-panel, sol/src/ui/command_panel.c)
- Already frosted by the per-element rewrite: `.cf-overlay` is transparent
  (no scrim), `.cf-panel` blurs via the shared panel_blur rule — same look
  as the floating terminal. Verified via screenshot-layer capture.
- Card height is content-sized (no explicit desc height): style overrides
  (glass padding 8/gap 3, Retro 2px borders) made the old hand-computed
  height clip the last row. Width stays 320 author px, clamped using
  window_w / ui_scale.
- Overlay inset: `.cf-overlay` padding uses SOL_UI_OVERLAY_INSET_RIGHT/BOTTOM
  (margin/0px) so the card sits flush with the pane's right AND bottom edges
  (user wants no gap on either side; workspace pads right by the margin but
  not bottom).
