# Background Effects GPU and Power Scope

## Observed render path

- `plugins/sol-plugin-bfx/src/plugin.c` schedules every built-in animated effect at 24 or 30 FPS. `sol_ui_on_frame()` requests the next frame at that interval, including when the workspace is otherwise idle.
- The default Aurora shader is full-resolution and evaluates two five-octave procedural noise fields per pixel. Several selectable effects are substantially heavier (`Particles`, `Starfield`, `Flow Field`, and `Fireflies`).
- `sol/src/core/sol_bg_effect.c` renders the background then applies the Sol localized, 9-tap separable blur per configured region and iteration. The maximum is four iterations; normal settings use three chrome iterations and one buffer iteration.
- The final appearance CSS also assigns `backdrop-filter` to panel classes and the title bar. Causality sees those draw commands and runs `ca_blur_capture_and_blur()` every rendered frame: a full-swapchain copy plus full-window horizontal and vertical blur. This is independent of Sol's localized blur and is the dominant avoidable duplication.

## Recommended implementation boundary

`Causality` should be the sole blur owner. `backdrop-filter` is CSS and its capture, blur, cache, and composition belong in Causality; Sol must remove its custom localized blur regions and GPU blur resources. Sol retains only responsibility for producing the animated background source.

The background itself is normally viewed through translucent panels or in narrow gutters, so Sol should render the effect to a reduced-resolution offscreen source and upscale it before Causality composites the UI. Causality should capture and blur at a matching reduced scale, then sample the cached result beneath CSS backdrop-filter nodes. This leaves all text, icons, borders, and controls native-resolution while reducing both procedural shading and blur bandwidth. Start Balanced at 0.5x linear resolution (one quarter of the shaded pixels); add a 0.375x power-saver mode and retain 1.0x only as an explicit quality mode. Do not use 0.25x as the default without visual review of exposed background gutters.

## Power policy

- Default to 15 FPS background animation on battery, 24 FPS on AC, and pause it when the app is unfocused, minimized, occluded, or the user enables reduced motion.
- Re-render only on the animation cadence or a visual invalidation (resize, effect/theme/settings change); do not blur on every unrelated editor or terminal repaint.
- Expose an explicit quality preset. Balanced should use a 0.5x background/blur target, one chrome blur iteration, no buffer blur, and a moderate opacity. High quality is opt-in.
- Benchmark matched idle scenes at the same resolution and presentation mode before/after. Capture GPU timestamps for background draw, Sol blur, CSS backdrop blur, UI render, and present; measure package/GPU power separately.

## Acceptance criteria

- Idle focused and unfocused windows do not sustain the discrete GPU or continuous high-rate rendering unnecessarily.
- There is no Sol-owned primary-window blur path; every glass surface is blurred through Causality CSS composition.
- Background quality remains stable at the selected cadence and has no resize, scene, or window-lifetime regressions.
- Runtime validation covers primary plus auxiliary windows, minimize/restore, focus changes, settings changes, and clean shutdown.

## Implemented 2026-09-01

- Removed Sol's localized backdrop-blur API, per-frame region synchronization, GPU resources, settings, and controls. CSS `backdrop-filter` is now the only primary-window blur authority.
- Shader-mode effects render into one half-resolution, frame-local Vulkan target before a linear upscale into the primary swapchain. Raw-mode plugins and auxiliary windows retain direct rendering because their callbacks own their rendering contract.
- Causality now captures and runs CSS backdrop blur at half linear resolution, scaling the blur radius so the final CSS blur size is unchanged when sampled back at native resolution.
- All bundled background effects use a 15 FPS cadence instead of mixed 24/30 FPS rates.
- Validation: full build, 14/14 CTests, whitespace checks for both repositories, and an eight-second native startup smoke completed without output or a crash.

## Visual refinement 2026-09-01

- Replaced the Starfield tunnel-streak shader with three sparse, parallax-scrolling hash-grid layers. It now has stable points, controlled halos, palette-aware color variation, and low-cost twinkle without a per-pixel list of 52 line segments.
- Replaced the Flow Field's 56 per-pixel particle segments with four backward vector-field advection steps and continuous, animated stream filaments. The result is coherent motion rather than a noisy field of disconnected marks while materially lowering ALU work.
- Visual proof requires selecting both effects in the running app. No desktop capture is available in this session.
