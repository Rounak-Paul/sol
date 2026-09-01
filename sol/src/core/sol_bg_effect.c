// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_bg_effect.c — Background shader effect registry and Vulkan pipeline. */

#include "sol_bg_effect.h"

#include "sol_platform.h"

#include <causality.h>
#include <ca_gpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define SOL_BG_EFFECT_MAX     32
#define SOL_BG_EFFECT_ID_MAX  63
#define SOL_BG_EFFECT_NAME_MAX 127
#define SOL_BG_EFFECT_RENDER_SCALE_DIVISOR 2u

/* ------------------------------------------------------------------ */
/* Vertex shader — generates a full-screen triangle from gl_VertexIndex */
/* ------------------------------------------------------------------ */

static const char k_fullscreen_vert[] =
    "#version 450\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Push constants shared by every shader-mode effect. */
typedef struct {
    float time, width, height, opacity;
    float primary_r, primary_g, primary_b, _pad0;
    float accent_r, accent_g, accent_b, _pad1;
} BgPushConst;


typedef struct {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       width;
    uint32_t       height;
    VkFormat       format;
    VkImageLayout  layout;
} BgRenderTarget;

typedef struct {
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;
    VkFormat         built_for_format;  /* swapchain format pipeline was compiled for */
    BgRenderTarget   targets[CA_FRAMES_IN_FLIGHT];
    bool             initialized;
} BgShaderGPU;

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char                  id[SOL_BG_EFFECT_ID_MAX + 1];
    char                  display_name[SOL_BG_EFFECT_NAME_MAX + 1];
    char                 *fragment_glsl;    /* heap-allocated; NULL = raw mode */
    uint32_t              animation_fps;
    SolBgEffectInitFn     init;
    SolBgEffectDestroyFn  destroy;
    SolBgEffectRenderFn   render;
    SolBgEffectResizeFn   resize;
    void                 *user_data;
    BgShaderGPU           gpu;              /* only used in shader mode */
    bool                  raw_initialized;  /* raw mode: init has been called */
    uint32_t              last_width;
    uint32_t              last_height;
    bool                  in_use;
} BgEntry;

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

struct SolBgEffectRegistry {
    Ca_Instance  *instance;
    BgEntry       entries[SOL_BG_EFFECT_MAX];
    int           count;
    int           active_idx;   /* -1 = none */
    double        start_sec;    /* CLOCK_MONOTONIC base */
    float         opacity;      /* [0.0, 1.0] */
    float         background_r, background_g, background_b;
    float         primary_r, primary_g, primary_b;
    float         accent_r, accent_g, accent_b;
    void        (*on_change)(void *user_data);
    void         *on_change_data;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Return monotonic time in seconds for animation timing. */
static double get_monotonic_sec(void)
{
    return (double)sol_platform_now_monotonic_ns() * 1e-9;
}

/* Notify the registry observer after externally visible state changes. */
static void fire_change(SolBgEffectRegistry *reg)
{
    if (reg->on_change) reg->on_change(reg->on_change_data);
}

/* Return the registered entry matching id, or NULL when absent. */
static BgEntry *find_entry(SolBgEffectRegistry *reg, const char *id)
{
    if (!id || !*id) return NULL;
    for (int i = 0; i < reg->count; ++i) {
        if (reg->entries[i].in_use &&
            strcmp(reg->entries[i].id, id) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

/* Return the array index matching id, or -1 when absent. */
static int find_entry_idx(SolBgEffectRegistry *reg, const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < reg->count; ++i) {
        if (reg->entries[i].in_use &&
            strcmp(reg->entries[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Shader-mode GPU pipeline (Vulkan 1.3 dynamic rendering)             */
/* ------------------------------------------------------------------ */

/*
 * Build the Vulkan graphics pipeline for a fragment shader effect.
 * Uses dynamic rendering so no VkRenderPass or VkFramebuffer is needed.
 * The pipeline targets the given swapchain format directly.
 *
 * gpu       GPU state to populate.
 * instance  Causality instance (owns the VkDevice).
 * format    Swapchain image format (VkFormat).
 * frag_glsl GLSL fragment shader source.
 * Returns   true on success.
 */
static bool gpu_build_pipeline(BgShaderGPU *gpu, Ca_Instance *instance,
                               VkFormat format, const char *frag_glsl)
{
    VkDevice device = ca_gpu_device(instance);

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(BgPushConst),
    };
    VkPipelineLayoutCreateInfo pl_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
    };
    if (vkCreatePipelineLayout(device, &pl_ci, NULL, &gpu->pipeline_layout) != VK_SUCCESS)
        return false;

    VkShaderModule vert = ca_shader_compile(device, k_fullscreen_vert,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(device, frag_glsl,
                                            VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device, vert, NULL);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device, frag, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_att,
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states,
    };
    /* Dynamic rendering — no VkRenderPass needed. */
    VkPipelineRenderingCreateInfo dyn_render = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &dyn_render,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp_state,
        .pRasterizationState = &rast,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = gpu->pipeline_layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci,
                                           NULL, &gpu->pipeline);
    vkDestroyShaderModule(device, vert, NULL);
    vkDestroyShaderModule(device, frag, NULL);

    if (r != VK_SUCCESS) return false;
    gpu->built_for_format = format;
    return true;
}

/* Release the reduced-resolution render target owned by one in-flight frame. */
static void bg_target_destroy(BgRenderTarget *target, VkDevice device)
{
    if (!target) return;
    if (target->view != VK_NULL_HANDLE)
        vkDestroyImageView(device, target->view, NULL);
    if (target->image != VK_NULL_HANDLE)
        vkDestroyImage(device, target->image, NULL);
    if (target->memory != VK_NULL_HANDLE)
        vkFreeMemory(device, target->memory, NULL);
    memset(target, 0, sizeof(*target));
}

/* Allocate one device-local target used to shade a background below native resolution. */
static bool bg_target_build(BgRenderTarget *target, Ca_Instance *instance,
                            VkFormat format, uint32_t width, uint32_t height)
{
    if (!target || !instance || width == 0u || height == 0u) return false;
    VkDevice device = ca_gpu_device(instance);
    VkImageCreateInfo image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1u },
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(device, &image_ci, NULL, &target->image) != VK_SUCCESS)
        goto fail;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, target->image, &requirements);
    uint32_t memory_type = ca_gpu_find_memory_type(
        instance, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    if (vkAllocateMemory(device, &allocation, NULL, &target->memory) != VK_SUCCESS)
        goto fail;
    if (vkBindImageMemory(device, target->image, target->memory, 0) != VK_SUCCESS)
        goto fail;

    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1u,
            .layerCount = 1u,
        },
    };
    if (vkCreateImageView(device, &view_ci, NULL, &target->view) != VK_SUCCESS)
        goto fail;
    target->width = width;
    target->height = height;
    target->format = format;
    target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;

fail:
    bg_target_destroy(target, device);
    return false;
}

/* Record a precise image-layout transition for the background composition target. */
static void bg_image_barrier(VkCommandBuffer cmd, VkImage image,
                             VkImageLayout old_layout, VkImageLayout new_layout,
                             VkPipelineStageFlags2 src_stage,
                             VkAccessFlags2 src_access,
                             VkPipelineStageFlags2 dst_stage,
                             VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1u,
            .layerCount = 1u,
        },
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

static void gpu_destroy(BgShaderGPU *gpu, VkDevice device)
{
    if (!gpu) return;
    vkDeviceWaitIdle(device);
    for (uint32_t i = 0u; i < CA_FRAMES_IN_FLIGHT; ++i)
        bg_target_destroy(&gpu->targets[i], device);
    if (gpu->pipeline        != VK_NULL_HANDLE)
        vkDestroyPipeline(device, gpu->pipeline, NULL);
    if (gpu->pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, gpu->pipeline_layout, NULL);
    memset(gpu, 0, sizeof(*gpu));
}

/* Activate an entry and initialize raw-mode effects when required. */
static bool entry_activate(SolBgEffectRegistry *reg, BgEntry *entry)
{
    if (entry->fragment_glsl) return true;
    SolBgEffectCtx context = {
        .instance = reg->instance,
        .time_sec = get_monotonic_sec() - reg->start_sec,
    };
    if (entry->init && !entry->init(&context, entry->user_data)) return false;
    entry->raw_initialized = true;
    return true;
}

/* Release shader or raw-mode resources when an entry stops being active. */
static void entry_deactivate(SolBgEffectRegistry *reg, BgEntry *entry)
{
    if (entry->fragment_glsl) {
        if (entry->gpu.initialized)
            gpu_destroy(&entry->gpu, ca_gpu_device(reg->instance));
        return;
    }
    if (!entry->raw_initialized) return;
    SolBgEffectCtx context = {
        .instance = reg->instance,
        .time_sec = get_monotonic_sec() - reg->start_sec,
    };
    if (entry->destroy) entry->destroy(&context, entry->user_data);
    entry->raw_initialized = false;
}


/* ------------------------------------------------------------------ */
/* Public API — lifecycle                                              */
/* ------------------------------------------------------------------ */

/*
 * Create a background effect registry.
 *
 * instance  Causality instance.
 * Returns   Heap-allocated registry, or NULL on failure.
 */
SolBgEffectRegistry *sol_bg_effect_registry_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    SolBgEffectRegistry *reg = (SolBgEffectRegistry *)calloc(1, sizeof(*reg));
    if (!reg) return NULL;
    reg->instance   = instance;
    reg->active_idx = -1;
    reg->opacity    = 1.0f;
    reg->background_r = 0.024f;
    reg->background_g = 0.031f;
    reg->background_b = 0.059f;
    reg->primary_r  = 0.376f;
    reg->primary_g  = 0.647f;
    reg->primary_b  = 0.980f;
    reg->accent_r   = 0.655f;
    reg->accent_g   = 0.545f;
    reg->accent_b   = 0.980f;
    reg->start_sec  = get_monotonic_sec();
    return reg;
}

/*
 * Destroy the registry, deactivating all effects.
 *
 * reg  Registry to destroy (safe to call with NULL).
 */
void sol_bg_effect_registry_destroy(SolBgEffectRegistry *reg)
{
    if (!reg) return;
    if (reg->active_idx >= 0)
        entry_deactivate(reg, &reg->entries[reg->active_idx]);
    for (int i = 0; i < reg->count; ++i)
        if (reg->entries[i].in_use)
            free(reg->entries[i].fragment_glsl);
    free(reg);
}

/* ------------------------------------------------------------------ */
/* Public API — registration                                           */
/* ------------------------------------------------------------------ */

/*
 * Register an effect.  Strings are copied internally.
 *
 * reg   The registry.
 * desc  Effect descriptor.
 * Returns  true on success.
 */
bool sol_bg_effect_register(SolBgEffectRegistry *reg, const SolBgEffectDesc *desc)
{
    if (!reg || !desc || !desc->id || !*desc->id || !desc->display_name) return false;
    if (strlen(desc->id) > SOL_BG_EFFECT_ID_MAX ||
        strlen(desc->display_name) > SOL_BG_EFFECT_NAME_MAX)
        return false;
    if (!desc->fragment_glsl && !desc->render) return false;
    if (desc->animation_fps > SOL_BG_EFFECT_MAX_ANIMATION_FPS) return false;
    if (reg->count >= SOL_BG_EFFECT_MAX) return false;
    if (find_entry(reg, desc->id)) return false;  /* duplicate */

    BgEntry *e = &reg->entries[reg->count];
    memset(e, 0, sizeof(*e));

    strncpy(e->id,           desc->id,           SOL_BG_EFFECT_ID_MAX);
    strncpy(e->display_name, desc->display_name, SOL_BG_EFFECT_NAME_MAX);
    e->id[SOL_BG_EFFECT_ID_MAX]           = '\0';
    e->display_name[SOL_BG_EFFECT_NAME_MAX] = '\0';

    if (desc->fragment_glsl) {
        e->fragment_glsl = strdup(desc->fragment_glsl);
        if (!e->fragment_glsl) return false;
    }
    e->init       = desc->init;
    e->destroy    = desc->destroy;
    e->render     = desc->render;
    e->resize     = desc->resize;
    e->user_data  = desc->user_data;
    e->animation_fps = desc->animation_fps;
    e->in_use     = true;
    ++reg->count;
    fire_change(reg);
    return true;
}

/*
 * Unregister an effect by id.  Deactivates first if active.
 *
 * reg  The registry.
 * id   Effect id.
 */
void sol_bg_effect_unregister(SolBgEffectRegistry *reg, const char *id)
{
    if (!reg || !id) return;
    int idx = find_entry_idx(reg, id);
    if (idx < 0) return;

    if (reg->active_idx == idx) {
        entry_deactivate(reg, &reg->entries[idx]);
        reg->active_idx = -1;
        fire_change(reg);
    }

    free(reg->entries[idx].fragment_glsl);
    /* Compact the entries array. */
    if (idx < reg->count - 1)
        memmove(&reg->entries[idx], &reg->entries[idx + 1],
                (size_t)(reg->count - idx - 1) * sizeof(BgEntry));
    memset(&reg->entries[reg->count - 1], 0, sizeof(BgEntry));
    --reg->count;

    /* Adjust active_idx if it was above the removed slot. */
    if (reg->active_idx > idx)
        --reg->active_idx;
    fire_change(reg);
}

/* ------------------------------------------------------------------ */
/* Public API — active effect                                          */
/* ------------------------------------------------------------------ */

/*
 * Activate an effect by id.  NULL or "" deactivates all effects.
 *
 * reg  The registry.
 * id   Effect id or NULL/"" for none.
 * Returns  true on success.
 */
bool sol_bg_effect_set_active(SolBgEffectRegistry *reg, const char *id)
{
    if (!reg) return false;

    const bool want_none = (!id || !*id);
    int new_idx = want_none ? -1 : find_entry_idx(reg, id);
    if (!want_none && new_idx < 0) return false;
    if (new_idx == reg->active_idx) return true;

    /* Deactivate old. */
    if (reg->active_idx >= 0)
        entry_deactivate(reg, &reg->entries[reg->active_idx]);

    reg->active_idx = new_idx;

    /* Activate new. */
    if (new_idx >= 0) {
        if (!entry_activate(reg, &reg->entries[new_idx])) {
            reg->active_idx = -1;
            fire_change(reg);
            return false;
        }
    }

    fire_change(reg);
    return true;
}

/*
 * Return the id of the currently active effect, or NULL.
 *
 * reg  The registry.
 */
const char *sol_bg_effect_active_id(const SolBgEffectRegistry *reg)
{
    if (!reg || reg->active_idx < 0) return NULL;
    return reg->entries[reg->active_idx].id;
}

/**
 * Return the plugin-selected interval for the active animated effect.
 *
 * @param reg Background effect registry.
 * @return Seconds between frames, or zero when no timed animation is active.
 */
double sol_bg_effect_active_frame_interval(const SolBgEffectRegistry *reg)
{
    if (!reg || reg->active_idx < 0) return 0.0;
    const uint32_t fps = reg->entries[reg->active_idx].animation_fps;
    return fps > 0u ? 1.0 / (double)fps : 0.0;
}

/* ------------------------------------------------------------------ */
/* Public API — enumeration                                            */
/* ------------------------------------------------------------------ */

/*
 * Return the number of registered effects.
 *
 * reg  The registry.
 */
size_t sol_bg_effect_count(const SolBgEffectRegistry *reg)
{
    if (!reg) return 0;
    return (size_t)reg->count;
}

/*
 * Get id and display name of effect at index.
 *
 * reg       The registry.
 * index     Zero-based index.
 * id_out    Written with pointer to effect id.
 * name_out  Written with pointer to display name.
 * Returns   true if index is in range.
 */
bool sol_bg_effect_get_info(const SolBgEffectRegistry *reg, size_t index,
                            const char **id_out, const char **name_out)
{
    if (!reg || index >= (size_t)reg->count || !reg->entries[index].in_use) return false;
    if (id_out)   *id_out   = reg->entries[index].id;
    if (name_out) *name_out = reg->entries[index].display_name;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API — opacity                                                */
/* ------------------------------------------------------------------ */

/*
 * Set global effect opacity [0.0, 1.0].
 *
 * reg      The registry.
 * opacity  Target opacity, clamped to [0.0, 1.0].
 */
void sol_bg_effect_set_opacity(SolBgEffectRegistry *reg, float opacity)
{
    if (!reg || !isfinite(opacity)) return;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    reg->opacity = opacity;
    fire_change(reg);
}

/*
 * Return the current global opacity.
 *
 * reg  The registry.
 */
float sol_bg_effect_opacity(const SolBgEffectRegistry *reg)
{
    return reg ? reg->opacity : 1.0f;
}

/*
 * Clamp one normalized color channel.
 *
 * value  Candidate channel value.
 * Returns  The channel clamped to [0,1].
 */
static float clamp_color_channel(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/* Set the semantic colors forwarded to shader-mode effects. */
void sol_bg_effect_set_theme_colors(SolBgEffectRegistry *reg,
                                    float background_r, float background_g,
                                    float background_b,
                                    float primary_r, float primary_g, float primary_b,
                                    float accent_r, float accent_g, float accent_b)
{
    if (!reg) return;
    reg->background_r = clamp_color_channel(background_r);
    reg->background_g = clamp_color_channel(background_g);
    reg->background_b = clamp_color_channel(background_b);
    reg->primary_r = clamp_color_channel(primary_r);
    reg->primary_g = clamp_color_channel(primary_g);
    reg->primary_b = clamp_color_channel(primary_b);
    reg->accent_r = clamp_color_channel(accent_r);
    reg->accent_g = clamp_color_channel(accent_g);
    reg->accent_b = clamp_color_channel(accent_b);
}

/* ------------------------------------------------------------------ */
/* Public API — change callback                                        */
/* ------------------------------------------------------------------ */

/*
 * Set the change callback.  NULL clears it.
 *
 * reg        The registry.
 * fn         Callback function.
 * user_data  Passed unchanged to fn.
 */
void sol_bg_effect_set_change_callback(SolBgEffectRegistry *reg,
                                       void (*fn)(void *user_data),
                                       void *user_data)
{
    if (!reg) return;
    reg->on_change      = fn;
    reg->on_change_data = user_data;
}


/*
 * Per-frame background render callback matching Ca_BgRenderFn.
 * Renders before the UI pass. The primary shader-mode window is shaded into a
 * half-resolution target and linearly composed into the swapchain; Causality
 * owns all CSS backdrop filtering after this callback returns.
 *
 * cmd              Command buffer already recording (outside any render pass).
 * window           Window owning the target swapchain.
 * swapchain_image  VkImage for the swapchain image (needed for blur barriers).
 * swapchain_view   VkImageView for the swapchain image (COLOR_ATTACHMENT_OPTIMAL).
 * format           Swapchain image format.
 * image_usage      Usage flags enabled for the swapchain image.
 * width            Swapchain image width in pixels.
 * height           Swapchain image height in pixels.
 * user_data        Pointer to SolBgEffectRegistry.
 */
static bool bg_effect_render(VkCommandBuffer cmd,
                             Ca_Window *window,
                             VkImage swapchain_image,
                             VkImageView swapchain_view,
                             VkFormat format,
                             VkImageUsageFlags image_usage,
                             uint32_t frame_slot,
                             uint32_t width,
                             uint32_t height,
                             void *user_data,
                             bool is_primary_window)
{
    SolBgEffectRegistry *reg = (SolBgEffectRegistry *)user_data;
    if (!reg || reg->active_idx < 0 || width == 0 || height == 0) return false;

    BgEntry *e = &reg->entries[reg->active_idx];
    float    t = (float)(get_monotonic_sec() - reg->start_sec);

    if (e->fragment_glsl) {
        /* Lazy-build bg pipeline on first call or if swapchain format changed. */
        if (!e->gpu.initialized || e->gpu.built_for_format != format) {
            if (e->gpu.pipeline != VK_NULL_HANDLE ||
                e->gpu.pipeline_layout != VK_NULL_HANDLE)
                gpu_destroy(&e->gpu, ca_gpu_device(reg->instance));
            if (!gpu_build_pipeline(&e->gpu, reg->instance, format, e->fragment_glsl))
                return false;
            e->gpu.initialized = true;
        }

        const bool use_reduced_target = is_primary_window &&
            frame_slot < CA_FRAMES_IN_FLIGHT &&
            (image_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u;
        const uint32_t render_width = use_reduced_target
            ? (width + SOL_BG_EFFECT_RENDER_SCALE_DIVISOR - 1u) /
                  SOL_BG_EFFECT_RENDER_SCALE_DIVISOR
            : width;
        const uint32_t render_height = use_reduced_target
            ? (height + SOL_BG_EFFECT_RENDER_SCALE_DIVISOR - 1u) /
                  SOL_BG_EFFECT_RENDER_SCALE_DIVISOR
            : height;
        BgRenderTarget *target = use_reduced_target
            ? &e->gpu.targets[frame_slot]
            : NULL;
        if (target && (target->image == VK_NULL_HANDLE ||
                       target->width != render_width ||
                       target->height != render_height ||
                       target->format != format)) {
            VkDevice device = ca_gpu_device(reg->instance);
            bg_target_destroy(target, device);
            if (!bg_target_build(target, reg->instance, format,
                                 render_width, render_height))
                return false;
        }
        if (target && target->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            bg_image_barrier(cmd, target->image, target->layout,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkImageView output_view = target ? target->view : swapchain_view;
        VkRenderingAttachmentInfo color_att = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = output_view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue  = { .color = { .float32 = {
                powf(reg->background_r, 2.2f),
                powf(reg->background_g, 2.2f),
                powf(reg->background_b, 2.2f),
                1.0f,
            } } },
        };
        VkRenderingInfo ri = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = { {0, 0}, {render_width, render_height} },
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color_att,
        };
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->gpu.pipeline);

        VkViewport vk_vp  = { 0.0f, 0.0f, (float)render_width, (float)render_height, 0.0f, 1.0f };
        VkRect2D   scissor = { {0, 0}, {render_width, render_height} };
        vkCmdSetViewport(cmd, 0, 1, &vk_vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        BgPushConst pc = {
            t, (float)render_width, (float)render_height, reg->opacity,
            reg->primary_r, reg->primary_g, reg->primary_b, 0.0f,
            reg->accent_r, reg->accent_g, reg->accent_b, 0.0f,
        };
        vkCmdPushConstants(cmd, e->gpu.pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        if (target) {
            bg_image_barrier(cmd, target->image,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT);
            bg_image_barrier(cmd, swapchain_image,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageBlit2 region = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
                .srcOffsets = { {0, 0, 0}, {(int32_t)render_width, (int32_t)render_height, 1} },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
                .dstOffsets = { {0, 0, 0}, {(int32_t)width, (int32_t)height, 1} },
            };
            VkBlitImageInfo2 blit = {
                .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                .srcImage = target->image,
                .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .dstImage = swapchain_image,
                .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .regionCount = 1u,
                .pRegions = &region,
                .filter = VK_FILTER_LINEAR,
            };
            vkCmdBlitImage2(cmd, &blit);
            bg_image_barrier(cmd, swapchain_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            bg_image_barrier(cmd, target->image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

    } else {
        if (!is_primary_window) return false;
        if (!e->raw_initialized || !e->render) return false;
        SolBgEffectCtx ctx = {
            .instance = reg->instance,
            .time_sec = t,
            .cmd = cmd,
            .swapchain_image = swapchain_image,
            .swapchain_view = swapchain_view,
            .format = format,
            .frame_slot = frame_slot,
            .width = width,
            .height = height,
        };
        if (e->resize && (e->last_width != width || e->last_height != height)) {
            e->resize(&ctx, e->user_data, width, height);
            e->last_width = width;
            e->last_height = height;
        }
        e->render(&ctx, e->user_data);
    }
    return true;
}

/* Render the active effect for the primary workspace window. */
bool sol_bg_effect_on_render(VkCommandBuffer cmd,
                             Ca_Window *window,
                             VkImage swapchain_image,
                             VkImageView swapchain_view,
                             VkFormat format,
                             VkImageUsageFlags image_usage,
                             uint32_t frame_slot,
                             uint32_t width,
                             uint32_t height,
                             void *user_data)
{
    return bg_effect_render(cmd, window, swapchain_image, swapchain_view, format,
                            image_usage, frame_slot, width, height, user_data,
                            true);
}

/* Render the active shader directly into an auxiliary window. */
bool sol_bg_effect_on_render_aux(VkCommandBuffer cmd,
                                 Ca_Window *window,
                                 VkImage swapchain_image,
                                 VkImageView swapchain_view,
                                 VkFormat format,
                                 VkImageUsageFlags image_usage,
                                 uint32_t frame_slot,
                                 uint32_t width,
                                 uint32_t height,
                                 void *user_data)
{
    return bg_effect_render(cmd, window, swapchain_image, swapchain_view, format,
                            image_usage, frame_slot, width, height, user_data,
                            false);
}
