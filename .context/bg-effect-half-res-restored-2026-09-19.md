# Background effect resolution — half-res restored, blur was a shader bug (2026-09-19)

## What happened

Earlier the same day, misdiagnosed "background looks blurry" as caused
by `sol_bg_effect.c`'s half-resolution render target + linear upscale,
and removed that entirely (rendering shader-mode effects at native
resolution directly into the swapchain). User corrected this: half-res
is intentional and correct — **a code editor should not spend GPU/power
budget rendering its animated background at full resolution**, since
the UI on top of it is what actually needs to be sharp. Restored the
original `BgRenderTarget`/`bg_target_build`/`bg_target_destroy`/
`bg_image_barrier` machinery and the half-res render + `VK_FILTER_LINEAR`
blit-upscale path exactly as it was (`SOL_BG_EFFECT_RENDER_SCALE_DIVISOR
= 2`).

## The actual bug (still fixed, unaffected by this revert)

The real cause of "waves looks blurry" was the waves shader's own edge
math — see [[bfx_waves_hard_edge]]. `k_waves_frag` used a `smoothstep`
transition band ~0.045 wide in UV space (36-45px on a typical window)
plus an `exp()` glow term, producing a soft gradient per wave band
regardless of render resolution. That fix (tightened smoothstep to
~3px, removed the glow term) stands and was the correct, sufficient fix
on its own — the resolution change was an unnecessary, incorrect side
change made in the same investigation and has now been reverted.

## Current state (both correct)

- `sol_bg_effect.c`: shader-mode effects render at half resolution per
  axis (1/4 the pixels) into a per-frame-slot offscreen target, then
  `vkCmdBlitImage2` with `VK_FILTER_LINEAR` upscales into the swapchain.
  This is deliberate: keeps a continuously-animating background cheap
  so GPU/power budget goes to the editor UI, not decorative canvas.
- `plugins/sol-plugin-bfx/src/plugin.c`'s `k_waves_frag`: hard-edged wave
  bands (tight smoothstep, no glow) — this is what actually fixes the
  "waves looks blurry" complaint; resolution was never the cause for
  this specific effect.

## Validation

- `cmake --build build -j 6`: clean, matches original object code path.
- `ctest --test-dir build`: 20/20 passed.
- `git diff --check`: clean. Diff against session start for
  `sol_bg_effect.c`/`.h` is now comment-only (logic fully restored).
- Live launch under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`:
  zero stderr output, clean shutdown.
- **Not yet visually confirmed by the user** —
  [[screencapture_unavailable]] still applies.

## Lesson for future sessions

Don't fix a symptom ("background looks blurry") by removing the first
plausible-looking mechanism found ("this renders at reduced
resolution") without confirming that mechanism is actually the cause of
the SPECIFIC complaint, especially when the mechanism is documented as
a deliberate resource tradeoff (the original header doc comment already
said as much: "keeps animated background work below native UI
resolution"). The user's follow-up correction here was the second time
in two related sessions the initially-suspected cause turned out to be
a different effect-specific shader bug instead
([[backdrop-blur-colorspace-fix]] was the correctly-diagnosed one;
this resolution change was the incorrectly-diagnosed one). When in
doubt about whether a perf/quality tradeoff is intentional, ask before
removing it.
