Task context: fonts looked "weird"/bad on an external monitor after the 2026-09-04 causality "opt" commit (680800a).

That commit itself (LCD->grayscale in font.c, fwidth-based AA in pipeline.c
shape shaders) is DPI-uniform and not the cause — it doesn't touch per-monitor
scale. The actual bug is pre-existing and unmasked by using an external
monitor with a different scale factor than the built-in display.

Root cause:
- `Ca_Font::display_scale` / `content_scale` are captured ONCE in
  `font_create_internal()` (font.c) via `glfwGetWindowContentScale`, at
  whichever window/monitor is active when the font is first created.
- `tier->baked_px = tier->logical_px * font->content_scale` in
  `font_init_page()` (font.c:651) — this is the actual FreeType rasterisation
  pixel size, not just a display heuristic.
- Atlas pages are keyed only by logical size (`font_size_key`, ui_scale-based),
  never by content_scale, so a page baked at the wrong DPI is never evicted
  or invalidated on its own.
- Everywhere else in the codebase (viewport.c:307-309, widget.c:3482-3484,
  paint.c:2672) already re-queries `glfwGetWindowContentScale` live per
  frame — only the font atlas's cached scale was frozen at creation time.
- No `glfwSetWindowContentScaleCallback` was ever registered anywhere in the
  codebase (confirmed via grep) — DPI changes were simply never observed.

Fix (2026-09-05):
- Added `ca_font_refresh_content_scale(Ca_Font *font, GLFWwindow *glfw_win)`
  in `causality/src/renderer/font.c` (declared in `font.h`). Re-queries
  content scale; if changed, updates `display_scale`/`content_scale`, wipes
  every packed dynamic atlas page (same teardown as page eviction in
  `font_alloc_page`) so they re-bake at the new `baked_px`, calls
  `font_invalidate_paint_caches()` so already-painted nodes repaint with
  fresh UVs, and re-primes the default page.
- Called once per frame from `ca_renderer_frame()` in `renderer.c`, against
  the first live window's `glfw` handle (font is a single instance-wide
  atlas shared across all windows in this codebase, not per-window).

Verification: `cmake --build build -j 11` clean, full `ctest` 14/15 -> 15/15
(added no new test — none of the existing 15 exercise multi-monitor DPI,
this is a live-hardware-only repro). Could not visually verify on an actual
external monitor in this sandbox (no screen-recording permission, no
attached second display). User should drag the Sol window to the external
monitor (or launch with it as primary) and confirm text renders crisp;
watch stderr for the new `[font] content scale changed to %.2fx; atlas
pages reset` log line to confirm the refresh actually fires.

Boundary: `inst->font` remains one atlas per `Ca_Instance`, not per-window.
If a future window ever spans two windows on two different-DPI monitors
simultaneously, the shared atlas would still thrash between the two scales
every frame (last-checked window wins). Out of scope here — Sol is
single-window today.
