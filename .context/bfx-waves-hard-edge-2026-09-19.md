# Waves background effect — hard edges instead of soft glow (2026-09-19)

## Root cause

The native-resolution fix earlier the same day
([[bg_effect_native_resolution]]) removed the shared render-pipeline
softness affecting all background effects. The waves effect
(`k_waves_frag`, `plugins/sol-plugin-bfx/src/plugin.c`) still read as
blurry afterward because its own shader math bakes in a wide soft edge,
independent of render resolution:

```
float fill=smoothstep(wave-0.035,wave+0.01,uv.y);
float edge=exp(-abs(uv.y-wave)*75.0);
```

`uv.y` is normalized [0,1] across the window height, so the `smoothstep`
transition band (0.045 wide) is 36-45 physical pixels on a typical
window — a large, deliberately soft gradient per wave band. The
`exp(-abs(...)*75.0)` term added a second, even softer glow halo on top.
Other effects (starfield, matrix) use `smoothstep` bands of a few
thousandths, reading as crisp by comparison — waves was the outlier.

## Fix

User chose the hard-edge option (no glow at all) over just tightening
the band. Changed:
- `fill` smoothstep band from `(wave-0.035, wave+0.01)` to
  `(wave-0.002, wave+0.001)` — a ~3px transition instead of ~40px.
- Removed the `edge`/glow term and its contribution to both `strength`
  (color accumulation) and `alpha` entirely.

Each of the 4 wave layers now renders as a flat-filled band with a
crisp cutoff instead of a soft glowing gradient. Motion/layout (the sine
composition, per-layer color/opacity falloff) is unchanged.

## Validation

- `cmake --build build -j 6`: clean.
- `ctest --test-dir build`: 20/20 passed.
- `git diff --check`: clean.
- Live launch under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
  with the effect switched to `com.sol.bfx.waves` (GLSL is compiled at
  runtime via `ca_shader_compile`, not by the C compiler, so this is the
  only way to confirm the shader source itself is valid): ran ~8 seconds
  with zero stderr output, clean shutdown. Confirms no GLSL compile
  error was introduced.
- **Not yet visually confirmed by the user** —
  [[screencapture_unavailable]] still applies.

## Incidental note

While testing, briefly edited `~/.sol/settings.json` to switch the
active effect for validation and introduced a stray top-level `"effect"`
key (the real field is nested under `"theme"`). The user had the app
open live at the same time, and its own atomic-save round-trip
overwrote my edits and silently dropped the malformed key before I
could revert it manually — settings.json is unaffected. No corruption,
but confirms: don't hand-edit `~/.sol/settings.json` while the app might
be running; prefer reading the target key's real nesting first (checked
against `sol_settings.c`'s parser) rather than assuming a flat schema.
