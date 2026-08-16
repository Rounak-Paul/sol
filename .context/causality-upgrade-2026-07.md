# Causality upgrade compatibility

## Scope

Sol's bundled Causality submodule advanced from `aa74b8a` to `c0877b2` on 2026-07-25.

## Verified integration boundary

- `causality.h` is now backend-agnostic and no longer includes Vulkan declarations.
- Native rendering integration moved to the opt-in `ca_gpu.h` header.
- Sol's background-effect renderer uses `Ca_BgRenderFn`, `ca_window_set_bg_render`, and GPU accessors; its implementation and workspace registration unit must include `ca_gpu.h` alongside `causality.h`.
- The background blur uses frame-local descriptor sets. Write them once before the first blur draw; rewriting a bound set during a later blur iteration invalidates the recording command buffer unless the layout explicitly enables update-after-bind.
- Causality `c0877b2` adds `ca_gpu_set_predestroy_callback` for external renderers with deferred Vulkan destruction. Sol destroys its background-effect registry before `ca_instance_destroy`, so it has no deferred resources to flush and does not register this optional hook.
- Build and CTest are the compatibility checks after future Causality pulls.

## Upgrade to `104e0a6` (2026-08-16)

Causality replaced its compile-time fixed pools with `Ca_DynArray`-backed
dynamic storage and dropped the per-window scale aliases. Two breaking changes
reached Sol:

- **`CA_MAX_WINDOWS` / `CA_MAX_WINDOWS_TOTAL` removed.** Window count is now
  unbounded. `SolBgEffectRegistry.aux_blur` became a `Ca_DynArray` of
  `AuxWindowBlurGPU`, grown on first sight of each auxiliary window.
  `aux_blur_for_window` returns a pointer into that array, so it is valid only
  until the next call — safe because a render callback reserves at most one
  window before using the result.
- **`ca_window_set_scale` / `ca_window_get_scale` removed.** Scale was always
  instance-wide; the window-handle aliases are gone. Sol added
  `sol_ui_system_scale(const SolUISystem *)`, forwarding to
  `ca_instance_get_scale`. Call sites that needed the window only for scale no
  longer resolve `sol_ui_system_primary_window`; those that also call
  `glyph_advance_px_for` / `router_glyph_advance_px` still do.

Other new surface is additive and unused by Sol so far: `ca_css_retain` and
subtree-scoped `Ca_DivDesc.stylesheet`, 2D transforms
(`ca_div_set_transform`, painted and hit-tested but layout-neutral),
`ca_window_input_capture` / `ca_window_key_consumed`, `no_hover` on the widget
descriptors, tree drag/drop indicators, and `ca_div_screen_rect` /
`ca_div_set_absolute_rect`.

Causality's own CMake now defines `BUILD_TESTING` targets, so Sol's `ctest`
run includes three upstream suites (`causality_array_tests`,
`causality_storage_tests`, `causality_input_capture_tests`).

Verified: full build clean, 14/14 ctest pass, `bin/sol` runs a live render
loop and shuts down cleanly.
