// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_bg_effect.h — Background shader effect registry.
 *
 * Shader-mode effects render into a reduced-resolution composition target before
 * the UI pass and are linearly composed into the swapchain. This keeps animated
 * background work below native UI resolution while preserving sharp UI content.
 *
 * Two registration modes:
 *
 *   SHADER mode  — supply fragment_glsl; the system builds the Vulkan
 *                  pipeline automatically using Vulkan 1.3 dynamic rendering.
 *                  The fragment shader receives push constants
 *                  {time, width, height, opacity, primary, accent} and a v_uv
 *                  varying in [0,1]².
 *
 *   RAW mode     — fragment_glsl is NULL; supply init / render / destroy
 *                  callbacks for full Vulkan control.
 *
 * Fragment shader contract (shader mode):
 *
 *   #version 450
 *   layout(push_constant) uniform PC {
 *       float time; float width; float height; float opacity;
 *       vec3 primary; float _pad0;
 *       vec3 accent;  float _pad1;
 *   } pc;
 *   layout(location = 0) in  vec2 v_uv;
 *   layout(location = 0) out vec4 out_color;
 *   void main() { ... }
 *
 * Integration:
 *   Register sol_bg_effect_on_render with ca_window_set_bg_render.
 *   The swapchain calls it each frame; Causality composites CSS backdrop filters
 *   and UI content on top, letting translucent panels reveal the background.
 */

#ifndef SOL_BG_EFFECT_H
#define SOL_BG_EFFECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ca_Instance  Ca_Instance;
typedef struct Ca_Window    Ca_Window;
typedef struct SolBgEffectRegistry SolBgEffectRegistry;

#define SOL_BG_EFFECT_MAX_ANIMATION_FPS 240u

/* ================================================================== */
/* Raw-mode callback context                                           */
/* ================================================================== */

/*
 * Context passed to every raw-mode effect callback.
 *
 * instance         Causality instance; use ca_gpu_device(instance) for Vulkan.
 * time_sec         Seconds elapsed since the registry was created.
 * cmd              Current command buffer during render; null otherwise.
 * swapchain_image  Current swapchain image during render; null otherwise.
 * swapchain_view   Current swapchain view during render; null otherwise.
 * format           Current swapchain format during render.
 * width, height    Current swapchain dimensions during render.
 * frame_slot       Current in-flight frame slot during render.
 */
typedef struct SolBgEffectCtx {
    Ca_Instance *instance;
    float        time_sec;
    VkCommandBuffer cmd;
    VkImage         swapchain_image;
    VkImageView     swapchain_view;
    VkFormat        format;
    uint32_t        frame_slot;
    uint32_t        width;
    uint32_t        height;
} SolBgEffectCtx;

/* ================================================================== */
/* Raw-mode callbacks                                                  */
/* ================================================================== */

/*
 * Called once when the effect becomes active.  Allocate Vulkan resources here.
 * Returns false to signal init failure (effect will not be activated).
 */
typedef bool (*SolBgEffectInitFn)(const SolBgEffectCtx *ctx, void *user_data);

/*
 * Called when the effect is deactivated or the registry is destroyed.
 * Free Vulkan resources allocated in init.
 */
typedef void (*SolBgEffectDestroyFn)(const SolBgEffectCtx *ctx, void *user_data);

/*
 * Called every frame while the effect is active.
 * The callback receives the same parameters as Ca_BgRenderFn so it may record
 * rendering commands directly into the swapchain image.
 *
 * ctx            Effect context (instance, time).
 * user_data      User pointer from SolBgEffectDesc.
 */
typedef void (*SolBgEffectRenderFn)(const SolBgEffectCtx *ctx, void *user_data);

/*
 * Resize notification for raw-mode effects (optional).
 *
 * w  New pixel width.
 * h  New pixel height.
 */
typedef void (*SolBgEffectResizeFn)(const SolBgEffectCtx *ctx, void *user_data,
                                    uint32_t w, uint32_t h);

/* ================================================================== */
/* Effect descriptor                                                   */
/* ================================================================== */

/*
 * Descriptor passed to sol_bg_effect_register.
 *
 * id             Unique dotted identifier, e.g. "com.sol.bfx.plasma".
 * display_name   Human-readable name shown in Settings > Theme.
 * fragment_glsl  Shader mode: null-terminated GLSL fragment shader source.
 *                NULL to use raw-mode callbacks instead.
 * animation_fps  Plugin-selected animation rate. Zero means event-driven only.
 * init/destroy/render/resize  Raw-mode callbacks (ignored in shader mode).
 * user_data      Passed unchanged to every raw-mode callback.
 */
typedef struct SolBgEffectDesc {
    const char          *id;
    const char          *display_name;
    const char          *fragment_glsl;
    uint32_t             animation_fps;
    SolBgEffectInitFn    init;
    SolBgEffectDestroyFn destroy;
    SolBgEffectRenderFn  render;
    SolBgEffectResizeFn  resize;
    void                *user_data;
} SolBgEffectDesc;

/* ================================================================== */
/* Registry lifecycle                                                  */
/* ================================================================== */

/*
 * Create a background effect registry.
 *
 * instance  Causality instance used for GPU access.
 * Returns   Heap-allocated registry, or NULL on failure.
 */
SolBgEffectRegistry *sol_bg_effect_registry_create(Ca_Instance *instance);

/*
 * Destroy the registry, deactivating the current effect and freeing all
 * registered entries.  Safe to call with NULL.
 *
 * reg  Registry to destroy.
 */
void sol_bg_effect_registry_destroy(SolBgEffectRegistry *reg);

/* ================================================================== */
/* Effect registration                                                 */
/* ================================================================== */

/*
 * Register an effect.  Duplicate ids are rejected.
 *
 * reg   The registry.
 * desc  Effect descriptor.  Strings are copied internally.
 * Returns  true on success.
 */
bool sol_bg_effect_register(SolBgEffectRegistry *reg, const SolBgEffectDesc *desc);

/*
 * Unregister an effect by id.  If the effect is currently active it is
 * deactivated first.
 *
 * reg  The registry.
 * id   Effect id to remove.
 */
void sol_bg_effect_unregister(SolBgEffectRegistry *reg, const char *id);

/* ================================================================== */
/* Active-effect control                                               */
/* ================================================================== */

/*
 * Activate an effect by id.  Pass NULL or "" to deactivate all effects.
 * Triggers the on_change callback.
 *
 * reg  The registry.
 * id   Effect id, or NULL/"" for none.
 * Returns  true on success, false if id is not registered.
 */
bool sol_bg_effect_set_active(SolBgEffectRegistry *reg, const char *id);

/*
 * Return the id of the currently active effect, or NULL if none.
 *
 * reg  The registry.
 */
const char *sol_bg_effect_active_id(const SolBgEffectRegistry *reg);

/**
 * Return the active effect's requested animation interval.
 *
 * @param reg Background effect registry.
 * @return Seconds between frames, or zero for event-driven-only effects.
 */
double sol_bg_effect_active_frame_interval(const SolBgEffectRegistry *reg);

/* ================================================================== */
/* Enumeration                                                         */
/* ================================================================== */

/*
 * Return the number of registered effects.
 *
 * reg  The registry.
 */
size_t sol_bg_effect_count(const SolBgEffectRegistry *reg);

/*
 * Get the id and display name of the effect at index.
 *
 * reg       The registry.
 * index     Zero-based index.
 * id_out    Written with a pointer to the effect's id string.
 * name_out  Written with a pointer to the display name string.
 * Returns   true if index is in range.
 */
bool sol_bg_effect_get_info(const SolBgEffectRegistry *reg, size_t index,
                            const char **id_out, const char **name_out);

/* ================================================================== */
/* Global opacity                                                      */
/* ================================================================== */

/*
 * Set the global effect opacity [0.0, 1.0].
 * Passed as the opacity push constant to shader-mode effects.
 *
 * reg      The registry.
 * opacity  Opacity value, clamped to [0.0, 1.0].
 */
void sol_bg_effect_set_opacity(SolBgEffectRegistry *reg, float opacity);

/*
 * Return the current global opacity.
 *
 * reg  The registry.
 */
float sol_bg_effect_opacity(const SolBgEffectRegistry *reg);

/* ================================================================== */
/* Theme colors                                                        */
/* ================================================================== */

/*
 * Set the semantic theme colors pushed to shader-mode effects.
 * Each channel is in [0.0, 1.0].
 *
 * reg                 The registry.
 * background_r/g/b    Background RGB channels.
 * primary_r/g/b       Primary RGB channels.
 * accent_r/g/b        Accent RGB channels.
 */
void sol_bg_effect_set_theme_colors(SolBgEffectRegistry *reg,
                                    float background_r, float background_g,
                                    float background_b,
                                    float primary_r, float primary_g, float primary_b,
                                    float accent_r, float accent_g, float accent_b);

/* ================================================================== */
/* Change notification                                                 */
/* ================================================================== */

/*
 * Register a callback invoked whenever the active effect or opacity changes.
 * Only one callback is supported; calling again replaces the previous one.
 *
 * reg        The registry.
 * fn         Callback function (may be NULL to clear).
 * user_data  Passed unchanged to fn.
 */
void sol_bg_effect_set_change_callback(SolBgEffectRegistry *reg,
                                       void (*fn)(void *user_data),
                                       void *user_data);

/* ================================================================== */
/* Render hook — matches Ca_BgRenderFn                                 */
/* ================================================================== */

/*
 * Per-frame background render callback; matches Ca_BgRenderFn signature.
 * Register with ca_window_set_bg_render(window, sol_bg_effect_on_render, registry).
 *
 * cmd              Command buffer recording (outside any render pass).
 * window           Window owning the target swapchain.
 * swapchain_image  VkImage for the swapchain image (needed for blur barriers).
 * swapchain_view   VkImageView for the swapchain image (COLOR_ATTACHMENT_OPTIMAL).
 * format           Swapchain image format.
 * image_usage      Usage flags enabled for the swapchain image.
 * frame_slot       In-flight frame slot safe for per-frame GPU resources.
 * width            Swapchain image pixel width.
 * height           Swapchain image pixel height.
 * user_data        Pointer to SolBgEffectRegistry.
 */
bool sol_bg_effect_on_render(VkCommandBuffer cmd,
                             Ca_Window       *window,
                             VkImage         swapchain_image,
                             VkImageView     swapchain_view,
                             VkFormat        format,
                             VkImageUsageFlags image_usage,
                             uint32_t        frame_slot,
                             uint32_t        width,
                             uint32_t        height,
                             void           *user_data);

/* Render the active shader with title-bar-only blur into an auxiliary window. */
bool sol_bg_effect_on_render_aux(VkCommandBuffer cmd,
                                 Ca_Window *window,
                                 VkImage swapchain_image,
                                 VkImageView swapchain_view,
                                 VkFormat format,
                                 VkImageUsageFlags image_usage,
                                 uint32_t frame_slot,
                                 uint32_t width,
                                 uint32_t height,
                                 void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* SOL_BG_EFFECT_H */
