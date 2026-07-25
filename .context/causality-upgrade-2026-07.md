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
