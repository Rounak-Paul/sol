# Background Effect System

## Architecture Overview

Background shader effects render directly into the swapchain image **before** the Causality UI pass using the `Ca_BgRenderFn` hook. The UI composites on top with `LOAD_OP_LOAD`, allowing semi-transparent panel backgrounds to reveal the shader art.

### Why not Ca_Viewport?

Causality draws in per-phase order: rects → text → viewports. A `Ca_Viewport` at z=0 always composites after all z=0 rects — it cannot sit behind UI panels. The `Ca_BgRenderFn` hook bypasses this by rendering before the UI render pass entirely.

---

## Data Flow

```
ca_swapchain_frame()
  → transition_image(UNDEFINED → COLOR_ATTACHMENT_OPTIMAL)
  → win->bg_render_fn(cmd, swapchain_view, format, w, h, user_data)
       └─ sol_bg_effect_on_render(...)
            └─ vkCmdBeginRendering / draw full-screen triangle / vkCmdEndRendering
  → vkCmdBeginRendering(loadOp=LOAD)   ← preserves bg content
       └─ Causality UI (rect, text, viewport pipelines)
  → vkCmdEndRendering
  → PRESENT_SRC_KHR transition
```

---

## Files

### Causality vendor

| File | Change |
|------|--------|
| `include/causality.h` | `Ca_BgRenderFn` typedef; `ca_window_set_bg_render` declaration |
| `src/core/ca_internal.h` | `bg_render_fn` + `bg_render_data` fields on `Ca_Window` |
| `src/platform/window.c` | `ca_window_set_bg_render` implementation |
| `src/renderer/swapchain.c` | Calls `bg_render_fn` after image transition; switches `loadOp` to `LOAD` when set |

### Sol

| File | Purpose |
|------|---------|
| `sol/include/sol_bg_effect.h` | Public API; `sol_bg_effect_on_render` signature matches `Ca_BgRenderFn` |
| `sol/src/core/sol_bg_effect.c` | Registry + Vulkan dynamic rendering pipeline (no VkRenderPass/Framebuffer) |
| `sol/src/ui/workspace.c` | `sol_ui_system_set_bg_effects` calls `ca_window_set_bg_render` |
| `sol/src/ui/sol_ui_internal.h` | `bg_effects`, `sig_bg_effect_rev` on `SolUISystem`; `bg_host` removed |
| `sol/src/ui/style.h` | Semi-transparent panel backgrounds (rgba ~0.82 alpha) |

### Plugin

| File | Purpose |
|------|---------|
| `plugins/sol-plugin-bfx/src/plugin.c` | 5 built-in effects; registered via `sol.bg_effect_registry` service |
| `plugins/CMakeLists.txt` | Target: `sol-plugin-bfx` |

---

## Key Types

```c
/* causality.h */
typedef void (*Ca_BgRenderFn)(VkCommandBuffer cmd,
                              VkImageView     swapchain_view,
                              VkFormat        format,
                              uint32_t        width,
                              uint32_t        height,
                              void           *user_data);

void ca_window_set_bg_render(Ca_Window *window, Ca_BgRenderFn fn, void *user_data);
```

```c
/* sol_bg_effect.h */
void sol_bg_effect_on_render(VkCommandBuffer cmd,
                             VkImageView     swapchain_view,
                             VkFormat        format,
                             uint32_t        width,
                             uint32_t        height,
                             void           *user_data);
```

---

## Shader Pipeline (BgShaderGPU)

- Uses `VkPipelineRenderingCreateInfo` (Vulkan 1.3 dynamic rendering — no VkRenderPass)
- Pipeline is built lazily on first `sol_bg_effect_on_render` call
- Rebuilt automatically if swapchain format changes (`built_for_format` field)
- Full-screen triangle trick: vertex shader generates positions from `gl_VertexIndex`
- Push constants: `{float time, width, height, opacity}`
- No VkFramebuffer — renders directly to the `swapchain_view` argument
- Normalized glass regions carry independent blur-pass counts; the current Sol theme uses four passes for title/editor/welcome regions and three for navigation, tabs, and status chrome so primary content remains readable without flattening the whole background
- Sol synchronizes blur regions for the editor, title bar, tab strip, status bar, and active left panel across resize, panel visibility, and splitter changes
- Blur scratch images and descriptor sets are isolated per in-flight frame slot, preventing cross-frame GPU read/write races
- Animation timing stores the monotonic epoch as double precision and only converts the elapsed delta to float, preserving smooth sub-second motion during long sessions
- The render callback reports whether it produced content so Causality clears normally when no effect is active or shader setup fails

## BgShaderGPU struct

```c
typedef struct {
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;
    VkFormat         built_for_format;
    bool             initialized;
} BgShaderGPU;
```

---

## Effects (com.sol.bfx plugin)

| ID | Name |
|----|------|
| `com.sol.bfx.aurora` | Aurora |
| `com.sol.bfx.nebula` | Nebula |
| `com.sol.bfx.hyperspace` | Hyperspace |
| `com.sol.bfx.lava` | Lava |
| `com.sol.bfx.matrix` | Matrix |

All shaders: dark-themed, subtle, suitable as editor backgrounds.

---

## Integration Points

- **Plugin manager** discovers `sol-plugin-bfx.dylib` via `sol_plugin_query` entry point
- Plugin calls `sol_plugin_get_service(ctx, "sol.bg_effect_registry", 0)` to get the registry
- `sol_ui_system_set_bg_effects(ui, reg)` wires the registry to the window's bg render hook
- Settings > Theme picker calls `sol_bg_effect_set_active(reg, id)` to change effects
- `sig_bg_effect_rev` signal is bumped on change so settings UI updates reactively
- Registry registration and removal publish change notifications, so runtime-loaded effects appear in an already-open Settings window
- Plugins unregister their effects on unload, allowing clean reloads without stale registry entries

## Minimal Glass Theme

- Navigation uses the registry's strongest localized blur with a thicker cool-black material; editor and welcome content use dark neutral scrims, and the wider Gaussian sampling spread increases frost diffusion without adding another full blur iteration
- Explorer section and project-root rows remain borderless and transparent so hierarchy comes from spacing and typography instead of stacked boxes
- The welcome composition is centered inside a bounded 1120px content column, preserving deliberate negative space on wide windows and preventing separators from spanning the entire editor surface
- The tab strip uses a dark thick material; the active buffer uses a brighter cool fill, high-contrast label, and one bottom indicator while inactive tabs remain visually subordinate
- The bottom status bar uses the registry's strongest localized blur beneath a 97% almost-black tint
- Status layout reserves a shrinkable left message region and an intrinsic non-shrinking right plugin region, preventing right-edge segments from being placed beyond the bar
- The command suggestion popup uses a compact near-black floating material, edge breathing room, restrained shadow, borderless key chips, and non-interactive row styling
- Sol's stylesheet contains no visual borders or outlines; hierarchy is expressed through material tint, blur, spacing, typography, hover fills, and active-surface contrast
- Resize handles remain fully interactive but transparent at rest, revealing a restrained translucent accent only while hovered or dragged
- Overflow scrollbars are CSS-resolved Causality node properties (width, radius, track, thumb, and active-thumb colors); Sol applies a universal slim sharp scrollbar theme, while Causality retains neutral defaults when CSS omits those properties
- Scrollbar CSS state tracks declaration presence separately from packed values, so transparent colors, zero radius, and zero width are not confused with an omitted property
- Retro raised/inset bevels are superseded by sharp edges, borderless controls, tonal surface changes, slimmer scrollbars, and restrained selection fills
- Causality exposes semantic classes for its system title-bar nodes and retains neutral fallback styling only when no stylesheet is installed
- Sol's stylesheet owns the title-bar surface, menu items, title, controls, hover states, and colors; no renderer-wide rule forces Sol's presentation onto other Causality applications
- Causality's renderer-owned select, tooltip, context-menu, and menubar overlays omit fallback borders and separator strokes whenever an author stylesheet is installed; unstyled applications retain the neutral fallback chrome
- Causality supports an instance-level background fallback inherited by every window unless that window installs an override; background callbacks receive the target window so window-local GPU resources and display scaling remain correct
- Auxiliary windows reuse the active effect registry, compiled pipeline, animation epoch, and parameters. They issue one size-adapted fullscreen draw and use isolated per-window/per-frame blur resources only for the title-bar strip; raw callback effects remain primary-window-only because their resize state is not window-local
- Sol does not duplicate Causality's title-bar height: workspace geometry and auxiliary blur query the resolved system-bar height, account for the target window's framebuffer ratio, and inherit the registry's configured blur-pass count
- File picker, search, plugin manager, settings, and Causality popup roots use dark translucent CSS surfaces so the inherited effect remains visible without reducing text contrast
- The Sol title bar is 26px at 1x with a translucent surface, compact menu spacing, and sharp borderless window controls

---

## Removed (from previous Ca_Viewport approach)

- `bg_host` div (CA_POSITION_FIXED) — no longer in layout tree
- `sol_ui_bg_builder` — removed
- `sol_bg_effect_attach_viewport` — removed
- `sol_bg_effect_on_resize` — removed (no framebuffer to rebuild)
- `VkRenderPass`, `VkFramebuffer` in `BgShaderGPU` — replaced by dynamic rendering
