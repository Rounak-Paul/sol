# Background effect system

## Architecture

`SolBgEffectRegistry` renders a theme-aware fullscreen shader into the swapchain
before the Causality UI pass. UI surfaces then composite on top through localized
per-frame blur resources. Auxiliary windows share the active shader pipeline but
retain isolated frame-local blur resources.

Shader push constants are:

```c
float time, width, height, opacity;
float primary_r, primary_g, primary_b, _pad0;
float accent_r, accent_g, accent_b, _pad1;
```

The colors come from `SolThemeColors`, not parsed CSS. Plugin shaders are copied
at registration, compiled lazily for the active swapchain format, and paced by
their declared animation rate through Causality's event-driven frame requests.

## Built-in effects

`plugins/sol-plugin-bfx` is a data-driven set adapted from LocalDocsMD:

| ID | Name | FPS |
| --- | --- | --- |
| `com.sol.bfx.particles` | Particles | 30 |
| `com.sol.bfx.waves` | Waves | 30 |
| `com.sol.bfx.matrix` | Matrix Rain | 24 |
| `com.sol.bfx.aurora` | Aurora | 30 |
| `com.sol.bfx.starfield` | Starfield | 30 |
| `com.sol.bfx.metaballs` | Metaballs | 30 |
| `com.sol.bfx.flowfield` | Flow Field | 24 |
| `com.sol.bfx.fireflies` | Fireflies | 30 |
| `com.sol.bfx.circuit` | Circuit | 24 |
| `com.sol.bfx.voronoi` | Voronoi | 24 |

The former Lava, Nebula, Wave, and sin-ribbon Aurora implementations were
removed. Unknown saved effect IDs migrate in memory to the new Aurora.

## GPU and lifecycle contracts

- Shader pipelines use Vulkan 1.3 dynamic rendering and alpha blending.
- Theme hex colors are converted from sRGB to linear in the shader before the
  sRGB swapchain conversion.
- Blur descriptor sets are frame-local and updated before recording blur draws.
- Causality requests `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` when the surface supports
  it because backdrop blur snapshots the swapchain. Unsupported surfaces skip
  capture instead of recording an invalid layout transition or blit.
- Background GPU resources are destroyed before Causality instance teardown.
- Each effect is capped at 24 or 30 FPS; no effect forces a continuous busy
  render loop.

## Validation

- Build and unit tests validate the C/API integration.
- All embedded shaders must pass `glslangValidator -V`.
- Runtime startup with the Vulkan validation layer proves active shader pipeline,
  swapchain composition, and generated theme registration.
- Visual inspection is still required for final motion density and contrast;
  build and shader compilation do not prove aesthetics.
