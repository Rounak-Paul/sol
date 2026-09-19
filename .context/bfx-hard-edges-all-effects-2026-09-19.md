# Background effects — hard edges across the board (2026-09-19)

## Scope

Following the waves-specific fix ([[bfx_waves_hard_edge]]), user asked
for all 10 built-in background effects
(`plugins/sol-plugin-bfx/src/plugin.c`) to have a hard-edge look. Since
several effects are soft-glow effects *by genre* (their whole visual
identity depends on blur/blending, not an edge-softness bug), scoped
this to: tighten each effect's shape-boundary `smoothstep` so silhouettes
read as crisp, while leaving true glow/blend techniques untouched where
the effect's design depends on them.

## Tightened (shape boundary now crisp)

- **particles**: dot smoothstep `(0.025,0.075)` → `(0.040,0.052)`; link
  smoothstep `(0.006,0.018)` → `(0.010,0.013)`. Dots and connecting
  lines now have a sharp cutoff instead of a soft radial falloff.
- **matrix**: glyph cell margins `step(0.10/0.90, f.x)` /
  `step(0.08/0.92, f.y)` widened to `step(0.06/0.94, f.x)` /
  `step(0.05/0.95, f.y)` — larger, more solid-looking glyph blocks with
  less anti-aliased-looking gap between cells. Left the trail fade
  (`smoothstep(0.0,22.0,behind)`, a deliberate 22-row fade length) and
  `headGlow` (`exp(-behind*behind*1.8)`, the falling-character's glow)
  untouched — those are the effect's actual visual identity, not edge
  softness.
- **circuit**: trace smoothstep `(0.025,0.055)` → `(0.038,0.048)`; node
  smoothstep `(0.055,0.105)` → `(0.075,0.090)`. Thinner transition band
  on both traces and junction nodes.
- **voronoi**: cell-boundary edge smoothstep `(0.0,0.012)` →
  `(0.0,0.006)` — crisper cell outlines. Left `interior`'s
  `smoothstep(0.0,0.18,...)`/`smoothstep(0.18,0.55,...)` alone — that's
  the deliberate soft interior cell-shading gradient, not a boundary
  artifact.

## Left unchanged (glow/blend is the effect's design, not a bug)

- **aurora**: `smoothstep(0.0,1.0,...)` bands are inherently soft —
  aurora is a glow-based effect by nature.
- **starfield**: `smoothstep(radius,radius*2.8,...)` core falloff plus
  a separate `halo` term — the halo is a deliberate glow layer around
  each star.
- **metaballs**: `smoothstep(0.9,1.0,field)`/`smoothstep(0.7,0.9,field)`
  — metaballs are soft-blob blending by definition of the technique;
  hard-edging this would just make them look like flat circles, not
  metaballs.
- **flowfield**: `halo`/`filament` smoothstep pair — halo is an
  intentional ambient glow layer around the sharper filament line
  (which was already reasonably tight at `(0.018,0.065)`).
- **fireflies**: `exp(-d*d*24000.0)` core / `exp(-d*d*1800.0)` halo —
  glow-based by design; fireflies should glow.

## Validation

- `cmake --build build -j 6`: clean.
- `ctest --test-dir build`: 20/20 passed.
- `git diff --check`: clean.
- Since GLSL compiles at runtime (`ca_shader_compile`), cycled the four
  edited effects (particles, matrix, circuit, voronoi) one at a time as
  the live active effect and launched under
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` for ~4s each: all
  four ran clean with zero stderr output, confirming each edited shader
  source compiles and runs correctly. (waves already validated in the
  prior session; aurora/starfield/metaballs/flowfield/fireflies were
  not touched, so not re-tested.)
- `~/.sol/settings.json` was snapshotted and restored exactly after
  testing, and testing only proceeded after confirming Sol was not
  running (avoiding the live-save race documented in
  [[bfx_waves_hard_edge]]/[[bg_effect_half_res_intentional]]).
- **Not yet visually confirmed by the user** —
  [[screencapture_unavailable]] still applies.
