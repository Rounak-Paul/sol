Task context: borders/edges/lines looked jagged after the 2026-09-04 causality
"opt" commit (680800a) — same commit as the font LCD-disable change, but the
font half was unrelated (see causality-multimonitor-dpi-fix.md); this is the
shape-shader half of that same commit.

Root cause:
- That commit replaced a fixed `smoothstep(-0.5, 0.5, d)` AA ramp (in
  FRAG_GLSL/IMAGE_FRAG_GLSL, causality/src/renderer/pipeline.c) with a
  `fwidth(d)`-derived ramp, on the theory that a fixed logical-pixel
  constant "collapses to a hard step" at scale 1.0. That theory was wrong —
  `p`/`v_local`/`d_outer` are logical-pixel node-space coordinates
  (see VERT_GLSL's `vec2 ndc = (pixel / d.viewport) * 2.0 - 1.0`, where
  d.viewport is log_w/log_h from swapchain.c), so a fixed `+/-0.5` ramp was
  already exactly 1 logical pixel wide at any scale — never a hard step.
- The real bug: rasterization happens once per *physical* fragment (the
  Vulkan viewport spans the full physical swapchain extent, swapchain.c
  ~line 650), but `d_outer` only changes in logical-pixel units. At
  content_scale > 1x (2.0x on this dev Mac per `[font] ... display_scale=2.0x`
  in stdout), `fwidth(d_outer)` per physical fragment is 1/content_scale
  logical-units — i.e. the AA ramp became half as wide as before at 2x DPI,
  a quarter as wide at 4x, etc. Narrower ramp = harder/more jagged edges,
  worse at higher DPI. This matches the user's report exactly (fonts use a
  completely different rendering path — MSDF/bitmap atlas, not this SDF
  shader — hence "not related to fonts, fonts looks good").

Fix (2026-09-05):
- Reverted to a fixed logical-space ramp half-width, but computed as
  `0.5 / content_scale` (not the old hardcoded `0.5`) so the ramp is exactly
  one *physical* pixel wide at any DPI — crisper than the original 0.5-at-
  any-scale behavior, and correct at high DPI where fwidth() broke.
- FRAG_GLSL (rect pipeline): added `edge_aa_scale` field to the existing
  `ClipPC` fragment push constant (renamed from an unused `_clip_pad0` float
  in `Ca_ClipPushConst`, causality/src/core/ca_internal.h — no size/layout
  change). Populated from `scale_x` (already computed in
  causality/src/renderer/swapchain.c as `sc->extent.width / log_w`) at both
  `Ca_ClipPushConst` construction sites in `ca_swapchain_frame`.
- IMAGE_FRAG_GLSL (image/viewport-composite/blur pipeline): no existing
  per-batch fragment push constant, so the scale rides as a new
  vertex-to-fragment varying (`v_edge_aa_scale`) sourced from a previously
  unused padding slot in the shared 128-byte instance struct
  (`ImageData.edge_aa_scale_pad.x` in GLSL == `Ca_TextInstance._pad1[4]` in C
  — _pad1[0..3] were already corner_01/corner_23 per-corner radii).
  `image_instance_pack_corner_radii()` gained an `aa_scale` parameter, called
  with `scale_x` at all 4 call sites (image band, viewport-composite band,
  blur-composite band).
- TEXT_FRAG_GLSL (glyph pipeline) was untouched — it never used fwidth()/SDF
  AA; text anti-aliasing goes through FreeType's own rasterization (see
  causality-font-fallback.md), a separate path entirely.

Verification:
- `cmake --build build -j 11` clean, full `ctest` 15/15.
- Launched `./bin/sol` directly (no project run-skill existed; none
  written since nothing needed installing/patching to get it running).
  Confirmed via stdout: `[pipeline] rect pipeline created` and
  `[pipeline] image pipeline created` both succeeded — the edited GLSL
  strings are syntactically valid and compiled by `ca_shader_compile` at
  runtime. No Vulkan validation errors, no crash, clean shutdown.
  `display_scale=2.0x` confirms this dev Mac itself runs at the exact DPI
  regime (2x Retina) where the fwidth() bug was most visible — this
  reproduces the underlying condition even without an external monitor.
- Could NOT visually compare edge crispness before/after — no
  screen-recording permission in this sandbox (see screencapture_unavailable
  memory). User should eyeball a rounded panel border or button edge at
  normal zoom to confirm edges look crisp/smooth again, not hard-jagged.

Boundary: TEXT_FRAG_GLSL's own alpha blending (single-scalar max(r,g,b))
and the earlier LCD-disable change are unaffected by and unrelated to this
fix — confirmed separately in causality-multimonitor-dpi-fix.md.
