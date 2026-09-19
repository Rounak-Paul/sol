# Backdrop blur — colorspace bug fix (2026-09-19)

## Current architecture (single implementation, confirmed via full sweep)

There is exactly one blur mechanism in the whole codebase:
`vendors/causality/causality/src/renderer/blur.c` implements CSS
`backdrop-filter: blur()` as a per-window, whole-swapchain, two-pass
separable Gaussian blur at 1/4 resolution per axis. Every panel that
declares `backdrop-filter` (`.tree-panel`, `.buffer-pane`, `.term-panel`
— which the floating terminal card also carries — `.cf-panel` command
overlay, `.term-float-backdrop` scrim, the four dialog-window roots,
`.ca-titlebar`) samples the *same* shared blurred image, cropped to its
own rect with its own corner radius via `IMAGE_FRAG_GLSL`'s rounded-box
SDF. `sol/src/core/sol_bg_effect.c` is unrelated — it only renders the
animated shader background canvas and has zero blur logic of its own
(confirmed by grep).

The much longer `floating-glass-ui-overhaul-2026-08-29.md` (rounds 1-17)
describes an older, since-rewritten region-based blur system (per-panel
H/V-pass scissoring, `sample_pad`, etc.) that no longer exists in the
current source — that file's early rounds are stale and should not be
used to reason about current behavior. Rounds 16-17's per-band real
capture (blurring actual painted UI, not a stale/background-only
snapshot) is the one piece of that history that does describe the
current `swapchain.c` band-loop logic and is still accurate.

## Root cause found this session

`blur.c` created its two intermediate images (`blur_image`, `blur_temp`)
as `VK_FORMAT_R8G8B8A8_UNORM`, while the swapchain (blit source) and
every other sampled texture in the renderer (`image.c`'s images, fonts
via a separate path) are `VK_FORMAT_R8G8B8A8_SRGB`. `IMAGE_FRAG_GLSL`
(the shared composite shader used for both normal images and backdrop-
blur quads) assumes whatever it samples is already linear — true for
sRGB-viewed textures (hardware auto-decodes on sample) but false for the
blur images:

- `vkCmdBlitImage2` from the sRGB swapchain into the UNORM blur image is
  a raw byte copy with no colorspace conversion (format-incompatible
  blits do get conversion, but sRGB↔UNORM of the same numeric layout is
  treated as same-layout — bytes carried straight across).
- The 9-tap Gaussian in `BLUR_FRAG_GLSL` then averaged still-sRGB-encoded
  values as if they were linear — blurring in gamma space, which is a
  well-known source of muddy/incorrect blur (edges skew dark, falloff
  looks wrong).
- The composite shader (`IMAGE_FRAG_GLSL`) sampled that result and
  treated it as already-linear (per its own doc comment), so the
  mis-blurred bytes went straight through without a second wrong
  conversion — the visible symptom is a flat, muddy, low-contrast blur
  that doesn't match the correctly gamma-managed content around it.

This explains the user's "bad blur"/"weird effect" report across every
`backdrop-filter` consumer at once (command overlay, floating terminal,
regular panels) — they all share this one shader/pipeline path, so a
single fix in `blur.c` corrects all of them simultaneously.

## Fix

Changed `blur.c`'s blur image format (both `create_blur_image` calls'
`VkImageCreateInfo.format`/`VkImageViewCreateInfo.format`) and the blur
pipeline's `VkPipelineRenderingCreateInfo` target format
(`blur_fmt`) from `VK_FORMAT_R8G8B8A8_UNORM` to `VK_FORMAT_R8G8B8A8_SRGB`.
No shader changes needed — this makes the blit-in, both blur render
passes, and the final composite sample all decode/encode through the
same sRGB↔linear convention every other sampled surface in the renderer
already uses, so the Gaussian averaging now happens in linear light and
the composite shader's "already linear" assumption is true again.

## Validation

- `cmake --build build -j 6`: clean, zero warnings from the change (one
  pre-existing unrelated `unused-includes` clangd lint on `blur.c`'s
  `pipeline.h` include, predates this session).
- `ctest --test-dir build`: 20/20 passed.
- `git diff --check` / `git -C vendors/causality diff --check`: clean.
- Live launch under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
  with this user's actual `~/.sol/settings.json` (`panel_blur: 15.22`,
  `titlebar_blur: 20.89` — blur actively enabled and non-trivial): zero
  stderr output across ~9 seconds of runtime, clean process exit on
  SIGTERM. No VUID errors from the format change (sRGB is a valid
  color-attachment + sampled-image format on every target this renderer
  supports; the swapchain surface-format query already requires exactly
  this format, so support is guaranteed).
- **Not yet visually confirmed by the user** — [[screencapture_unavailable]]
  still applies this session. The colorspace mismatch is a structural,
  provable bug independent of visual confirmation (verified by reading
  the exact format constants passed at each stage), but the user should
  compare panel/command-overlay/floating-terminal blur before/after to
  confirm the fix reads as "proper" per their original complaint.

## If revisiting

- The blur is intentionally cheap: 1/4-resolution cache per axis, 9 taps,
  `spread = blur_radius/4 * 0.4`. This is a real quality/perf tradeoff
  independent of the colorspace bug — if the fix above doesn't fully
  satisfy "looks bad", the next lever is more taps or a smaller
  downsample divisor (`CA_BACKDROP_BLUR_SCALE_DIVISOR`, currently 4),
  not another colorspace investigation.
- Menu/popup blur (`.ca-select-popup`/`.ca-tooltip`/`.ca-context-menu`/
  `.ca-menubar-popup`) remains intentionally solid-color, unrelated to
  this fix — see [[backdrop_blur_removed]]. Not revisited this session;
  the user's report was specifically about command overlay, floating
  terminal, and panel frosted-glass, none of which are popups.
