// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_bg_effect.c — Background shader effect registry and Vulkan pipeline. */

#include "sol_bg_effect.h"

#include <causality.h>
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
#define SOL_BG_EFFECT_BLUR_SAMPLE_SPREAD 1.45f

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

/* Push constants layout shared by all shader-mode effects.
 * r,g,b carry the active theme accent color in [0,1] linear. */
typedef struct { float time, width, height, opacity, r, g, b, _pad; } BgPushConst;

/* Push constants for the blur passes. */
typedef struct { float inv_w, inv_h; } BlurPushConst;

/* ------------------------------------------------------------------ */
/* Shader-mode GPU state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;
    VkFormat         built_for_format;  /* swapchain format pipeline was compiled for */
    bool             initialized;
} BgShaderGPU;

/* ------------------------------------------------------------------ */
/* Blur GPU state — separable Gaussian, shared across all effects      */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Scratch image (same format/size as swapchain) */
    VkImage        scratch_image;
    VkDeviceMemory scratch_mem;
    VkImageView    scratch_view;

    /* Sampler for reading source image in blur passes */
    VkSampler      sampler;

    /* Descriptor layout + pool + two sets (one per pass) */
    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       desc_set_h;   /* H-pass reads swapchain */
    VkDescriptorSet       desc_set_v;   /* V-pass reads scratch   */

    /* Pipeline layout + two pipelines */
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline_h;
    VkPipeline       pipeline_v;

    /* Dimensions the scratch was allocated for */
    uint32_t    width;
    uint32_t    height;
    VkFormat    format;
    bool        initialized;
} BlurGPU;

typedef struct {
    Ca_Window *window;
    BlurGPU    frames[CA_FRAMES_IN_FLIGHT];
} AuxWindowBlurGPU;

static void blur_destroy(BlurGPU *blur, VkDevice device);

/* GLSL for horizontal blur pass — samples source bound at set 0, binding 0 */
static const char k_blur_h_frag[] =
    "#version 450\n"
    "layout(set=0,binding=0) uniform sampler2D src;\n"
    "layout(push_constant) uniform PC { float inv_w; float inv_h; } pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"
    /* 9-tap Gaussian kernel (sigma ~2.5), horizontal */
    "void main() {\n"
    "    float w[9] = float[](0.0542,0.0816,0.1065,0.1213,0.1283,0.1213,0.1065,0.0816,0.0542);\n"
    "    vec4 c = vec4(0.0);\n"
    "    for(int i=0;i<9;i++) c += w[i] * texture(src, v_uv + vec2((float(i)-4.0)*pc.inv_w, 0.0));\n"
    "    out_color = c;\n"
    "}\n";

static const char k_blur_v_frag[] =
    "#version 450\n"
    "layout(set=0,binding=0) uniform sampler2D src;\n"
    "layout(push_constant) uniform PC { float inv_w; float inv_h; } pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"
    /* 9-tap Gaussian kernel (sigma ~2.5), vertical */
    "void main() {\n"
    "    float w[9] = float[](0.0542,0.0816,0.1065,0.1213,0.1283,0.1213,0.1065,0.0816,0.0542);\n"
    "    vec4 c = vec4(0.0);\n"
    "    for(int i=0;i<9;i++) c += w[i] * texture(src, v_uv + vec2(0.0, (float(i)-4.0)*pc.inv_h));\n"
    "    out_color = c;\n"
    "}\n";

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
    float         theme_r, theme_g, theme_b;  /* accent color from active theme */
    int           blur_passes;  /* how many separable blur iterations (0 = off) */
    BlurGPU       blur[CA_FRAMES_IN_FLIGHT];
    AuxWindowBlurGPU aux_blur[CA_MAX_WINDOWS_TOTAL];
    SolBgEffectBlurRegion blur_regions[SOL_BG_EFFECT_MAX_BLUR_REGIONS];
    size_t        blur_region_count;
    void        (*on_change)(void *user_data);
    void         *on_change_data;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Return monotonic time in seconds for animation timing. */
static double get_monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Return the frame-local blur resources reserved for an auxiliary window. */
static BlurGPU *aux_blur_for_window(SolBgEffectRegistry *reg,
                                    Ca_Window *window,
                                    uint32_t frame_slot)
{
    if (!reg || !window || frame_slot >= CA_FRAMES_IN_FLIGHT) return NULL;
    AuxWindowBlurGPU *available = NULL;
    for (size_t i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
        AuxWindowBlurGPU *entry = &reg->aux_blur[i];
        if (entry->window == window) return &entry->frames[frame_slot];
        if (!entry->window && !available) available = entry;
    }
    if (!available) return NULL;
    available->window = window;
    return &available->frames[frame_slot];
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

static void gpu_destroy(BgShaderGPU *gpu, VkDevice device)
{
    if (!gpu) return;
    vkDeviceWaitIdle(device);
    if (gpu->pipeline        != VK_NULL_HANDLE)
        vkDestroyPipeline(device, gpu->pipeline, NULL);
    if (gpu->pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, gpu->pipeline_layout, NULL);
    memset(gpu, 0, sizeof(*gpu));
}

/* ------------------------------------------------------------------ */
/* Blur GPU helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Build a single blur pipeline (horizontal or vertical) using dynamic rendering.
 * Reads from a combined-image-sampler at (set=0, binding=0).
 *
 * layout     Pre-built pipeline layout (shared between H and V pipelines).
 * device     Vulkan device.
 * instance   Causality instance (for ca_shader_compile).
 * format     Target image format.
 * frag_glsl  Blur fragment shader source.
 * out        Output pipeline handle.
 * Returns    true on success.
 */
static bool blur_build_pipeline(VkPipelineLayout layout, VkDevice device,
                                Ca_Instance *instance, VkFormat format,
                                const char *frag_glsl, VkPipeline *out)
{
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
    VkPipelineVertexInputStateCreateInfo   vi  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo      vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rst = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo   ms  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState    bla = { .blendEnable = VK_FALSE, .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo    bl  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &bla };
    VkDynamicState                         dyn_s[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo       dyn = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn_s };
    VkPipelineRenderingCreateInfo          dri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &dri,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rst,
        .pMultisampleState = &ms, .pColorBlendState = &bl,
        .pDynamicState = &dyn, .layout = layout, .renderPass = VK_NULL_HANDLE,
    };
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, NULL, out);
    vkDestroyShaderModule(device, vert, NULL);
    vkDestroyShaderModule(device, frag, NULL);
    return r == VK_SUCCESS;
}

/*
 * Allocate scratch image, sampler, descriptor sets and blur pipelines.
 * Called lazily on first render when blur_passes > 0.
 *
 * blur      BlurGPU state to populate.
 * instance  Causality instance.
 * format    Swapchain format.
 * width     Swapchain pixel width.
 * height    Swapchain pixel height.
 * Returns   true on success.
 */
static bool blur_build(BlurGPU *blur, Ca_Instance *instance,
                       VkFormat format, uint32_t width, uint32_t height)
{
    VkDevice device = ca_gpu_device(instance);

    /* Scratch image */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(device, &img_ci, NULL, &blur->scratch_image) != VK_SUCCESS)
        goto fail;

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, blur->scratch_image, &mem_req);
    uint32_t mem_type = ca_gpu_find_memory_type(instance, mem_req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = mem_req.size,
                                   .memoryTypeIndex = mem_type };
    if (vkAllocateMemory(device, &alloc, NULL, &blur->scratch_mem) != VK_SUCCESS)
        goto fail;
    if (vkBindImageMemory(device, blur->scratch_image, blur->scratch_mem, 0) != VK_SUCCESS)
        goto fail;

    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = blur->scratch_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(device, &view_ci, NULL, &blur->scratch_view) != VK_SUCCESS)
        goto fail;

    /* Sampler — linear filter, clamp to edge */
    VkSamplerCreateInfo smp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(device, &smp_ci, NULL, &blur->sampler) != VK_SUCCESS)
        goto fail;

    /* Descriptor layout — one combined-image-sampler at binding 0 */
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dsl_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    if (vkCreateDescriptorSetLayout(device, &dsl_ci, NULL, &blur->desc_layout) != VK_SUCCESS)
        goto fail;

    /* Descriptor pool — 2 sets (one per pass) */
    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    if (vkCreateDescriptorPool(device, &pool_ci, NULL, &blur->desc_pool) != VK_SUCCESS)
        goto fail;

    VkDescriptorSetLayout layouts[2] = { blur->desc_layout, blur->desc_layout };
    VkDescriptorSetAllocateInfo alloc_ds = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = blur->desc_pool, .descriptorSetCount = 2, .pSetLayouts = layouts,
    };
    VkDescriptorSet sets[2];
    if (vkAllocateDescriptorSets(device, &alloc_ds, sets) != VK_SUCCESS)
        goto fail;
    blur->desc_set_h = sets[0];
    blur->desc_set_v = sets[1];

    /* Pipeline layout — descriptor set + push constant */
    VkPushConstantRange pc_range = { .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                     .size = sizeof(BlurPushConst) };
    VkPipelineLayoutCreateInfo pl_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &blur->desc_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pc_range,
    };
    if (vkCreatePipelineLayout(device, &pl_ci, NULL, &blur->pipeline_layout) != VK_SUCCESS)
        goto fail;

    if (!blur_build_pipeline(blur->pipeline_layout, device, instance, format,
                              k_blur_h_frag, &blur->pipeline_h))
        goto fail;
    if (!blur_build_pipeline(blur->pipeline_layout, device, instance, format,
                              k_blur_v_frag, &blur->pipeline_v))
        goto fail;

    blur->width  = width;
    blur->height = height;
    blur->format = format;
    blur->initialized = true;
    return true;

fail:
    blur_destroy(blur, device);
    return false;
}

/*
 * Destroy all blur GPU resources.
 *
 * blur    BlurGPU state to destroy.
 * device  Vulkan device.
 */
static void blur_destroy(BlurGPU *blur, VkDevice device)
{
    if (!blur) return;
    vkDeviceWaitIdle(device);
    if (blur->pipeline_h    != VK_NULL_HANDLE) vkDestroyPipeline(device, blur->pipeline_h, NULL);
    if (blur->pipeline_v    != VK_NULL_HANDLE) vkDestroyPipeline(device, blur->pipeline_v, NULL);
    if (blur->pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, blur->pipeline_layout, NULL);
    if (blur->desc_pool     != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, blur->desc_pool, NULL);
    if (blur->desc_layout   != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, blur->desc_layout, NULL);
    if (blur->sampler       != VK_NULL_HANDLE) vkDestroySampler(device, blur->sampler, NULL);
    if (blur->scratch_view  != VK_NULL_HANDLE) vkDestroyImageView(device, blur->scratch_view, NULL);
    if (blur->scratch_image != VK_NULL_HANDLE) vkDestroyImage(device, blur->scratch_image, NULL);
    if (blur->scratch_mem   != VK_NULL_HANDLE) vkFreeMemory(device, blur->scratch_mem, NULL);
    memset(blur, 0, sizeof(*blur));
}

/*
 * Execute one separable Gaussian blur iteration on the swapchain image.
 * On entry:  swapchain is COLOR_ATTACHMENT_OPTIMAL, scratch is any layout.
 * On exit:   swapchain is COLOR_ATTACHMENT_OPTIMAL (ready for UI pass).
 *
 * blur           Initialized BlurGPU state.
 * device         Vulkan device (needed for descriptor writes).
 * cmd            Recording command buffer.
 * swapchain_img  The VkImage backing the swapchain view (for barriers).
 * swapchain_view VkImageView for the swapchain.
 * first_pass     true on the very first call (scratch layout is UNDEFINED).
 */
static void blur_execute(BlurGPU *blur, VkDevice device, VkCommandBuffer cmd,
                         VkImage swapchain_img, VkImageView swapchain_view,
                         const SolBgEffectBlurRegion *regions,
                         size_t region_count, uint32_t pass_index,
                         bool first_pass)
{
    VkImageSubresourceRange subres = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1
    };
    VkViewport    vp = { 0.0f, 0.0f, (float)blur->width, (float)blur->height, 0.0f, 1.0f };
    VkRect2D      sc = { {0,0}, {blur->width, blur->height} };
    BlurPushConst pc = {
        SOL_BG_EFFECT_BLUR_SAMPLE_SPREAD / (float)blur->width,
        SOL_BG_EFFECT_BLUR_SAMPLE_SPREAD / (float)blur->height,
    };

    /* Update descriptors: H reads swapchain, V reads scratch */
    VkDescriptorImageInfo h_img = { .sampler = blur->sampler,
                                    .imageView = swapchain_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo v_img = { .sampler = blur->sampler,
                                    .imageView = blur->scratch_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = blur->desc_set_h,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &h_img },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = blur->desc_set_v,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &v_img },
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    /* --- H-pass barriers --- */
    /* Swapchain: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL */
    VkImageMemoryBarrier2 b0 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = swapchain_img, .subresourceRange = subres,
    };
    /* Scratch: UNDEFINED (first) or SHADER_READ (subsequent) → COLOR_ATTACHMENT */
    VkImageMemoryBarrier2 b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = first_pass ? VK_IMAGE_LAYOUT_UNDEFINED
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = blur->scratch_image, .subresourceRange = subres,
    };
    VkImageMemoryBarrier2 barr_h[2] = { b0, b1 };
    VkDependencyInfo dep_h = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barr_h };
    vkCmdPipelineBarrier2(cmd, &dep_h);

    /* H-pass draw */
    VkRenderingAttachmentInfo h_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = blur->scratch_view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo h_ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                             .renderArea = { {0,0}, {blur->width, blur->height} },
                             .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &h_att };
    vkCmdBeginRendering(cmd, &h_ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blur->pipeline_h);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            blur->pipeline_layout, 0, 1, &blur->desc_set_h, 0, NULL);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, blur->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    /* --- V-pass barriers --- */
    /* Scratch: COLOR_ATTACHMENT → SHADER_READ */
    VkImageMemoryBarrier2 b2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = blur->scratch_image, .subresourceRange = subres,
    };
    /* Swapchain: SHADER_READ → COLOR_ATTACHMENT */
    VkImageMemoryBarrier2 b3 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = swapchain_img, .subresourceRange = subres,
    };
    VkImageMemoryBarrier2 barr_v[2] = { b2, b3 };
    VkDependencyInfo dep_v = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barr_v };
    vkCmdPipelineBarrier2(cmd, &dep_v);

    /* V-pass draw */
    VkRenderingAttachmentInfo v_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchain_view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo v_ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                             .renderArea = { {0,0}, {blur->width, blur->height} },
                             .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &v_att };
    vkCmdBeginRendering(cmd, &v_ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blur->pipeline_v);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            blur->pipeline_layout, 0, 1, &blur->desc_set_v, 0, NULL);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdPushConstants(cmd, blur->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    for (size_t i = 0; i < region_count; ++i) {
        const SolBgEffectBlurRegion *region = &regions[i];
        if (region->passes <= pass_index) continue;
        uint32_t x = (uint32_t)(region->x * (float)blur->width);
        uint32_t y = (uint32_t)(region->y * (float)blur->height);
        uint32_t right = (uint32_t)((region->x + region->width) * (float)blur->width + 0.999f);
        uint32_t bottom = (uint32_t)((region->y + region->height) * (float)blur->height + 0.999f);
        if (right > blur->width) right = blur->width;
        if (bottom > blur->height) bottom = blur->height;
        if (right <= x || bottom <= y) continue;
        VkRect2D region_scissor = {
            .offset = { (int32_t)x, (int32_t)y },
            .extent = { right - x, bottom - y },
        };
        vkCmdSetScissor(cmd, 0, 1, &region_scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    /* Swapchain is COLOR_ATTACHMENT_OPTIMAL — ready for the UI pass. */
}

/* ------------------------------------------------------------------ */
/* Entry activate / deactivate                                         */
/* ------------------------------------------------------------------ */

static bool entry_activate(SolBgEffectRegistry *reg, BgEntry *e)
{
    if (e->fragment_glsl) {
        /* Shader mode: GPU pipeline built lazily on first render call. */
        return true;
    }

    SolBgEffectCtx ctx = {
        .instance = reg->instance,
        .time_sec = get_monotonic_sec() - reg->start_sec,
    };
    if (e->init && !e->init(&ctx, e->user_data)) return false;
    e->raw_initialized = true;
    return true;
}

static void entry_deactivate(SolBgEffectRegistry *reg, BgEntry *e)
{
    if (e->fragment_glsl) {
        if (e->gpu.initialized)
            gpu_destroy(&e->gpu, ca_gpu_device(reg->instance));
    } else {
        if (e->raw_initialized) {
            SolBgEffectCtx ctx = {
                .instance = reg->instance,
                .time_sec = get_monotonic_sec() - reg->start_sec,
            };
            if (e->destroy) e->destroy(&ctx, e->user_data);
            e->raw_initialized = false;
        }
    }
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
    reg->theme_r    = 1.0f;
    reg->theme_g    = 1.0f;
    reg->theme_b    = 1.0f;
    reg->blur_passes = 4;
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
    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i)
        blur_destroy(&reg->blur[i], ca_gpu_device(reg->instance));
    for (size_t w = 0; w < CA_MAX_WINDOWS_TOTAL; ++w)
        for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i)
            blur_destroy(&reg->aux_blur[w].frames[i],
                         ca_gpu_device(reg->instance));
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
 * Set the theme accent color forwarded to shader-mode effects as r/g/b push constants.
 *
 * reg      The registry.
 * r,g,b    Linear RGB channels, each clamped to [0.0, 1.0].
 */
void sol_bg_effect_set_theme_color(SolBgEffectRegistry *reg, float r, float g, float b)
{
    if (!reg) return;
    reg->theme_r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    reg->theme_g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
    reg->theme_b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
}

/* Return the configured maximum number of localized blur iterations. */
uint32_t sol_bg_effect_blur_passes(const SolBgEffectRegistry *reg)
{
    return reg && reg->blur_passes > 0 ? (uint32_t)reg->blur_passes : 0u;
}

/* Replace the normalized regions receiving backdrop blur. */
void sol_bg_effect_set_blur_regions(SolBgEffectRegistry *reg,
                                    const SolBgEffectBlurRegion *regions,
                                    size_t count)
{
    if (!reg) return;
    if (!regions) count = 0;
    if (count > SOL_BG_EFFECT_MAX_BLUR_REGIONS)
        count = SOL_BG_EFFECT_MAX_BLUR_REGIONS;

    reg->blur_region_count = 0;
    for (size_t i = 0; i < count; ++i) {
        float left = regions[i].x;
        float top = regions[i].y;
        float right = regions[i].x + regions[i].width;
        float bottom = regions[i].y + regions[i].height;
        if (!isfinite(left) || !isfinite(top) ||
            !isfinite(right) || !isfinite(bottom))
            continue;
        if (left < 0.0f) left = 0.0f;
        if (top < 0.0f) top = 0.0f;
        if (right > 1.0f) right = 1.0f;
        if (bottom > 1.0f) bottom = 1.0f;
        if (right <= left || bottom <= top) continue;
        reg->blur_regions[reg->blur_region_count++] = (SolBgEffectBlurRegion){
            .x = left,
            .y = top,
            .width = right - left,
            .height = bottom - top,
            .passes = regions[i].passes > (uint32_t)reg->blur_passes
                          ? (uint32_t)reg->blur_passes
                          : regions[i].passes,
        };
    }
    fire_change(reg);
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
 * Renders directly into the swapchain image before the UI pass.
 * If blur_passes > 0, applies separable Gaussian blur for a frosted-glass base.
 * Registered via ca_window_set_bg_render; called by Causality each frame.
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
                             bool apply_blur)
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

        /* Lazy-build or rebuild blur GPU resources when dimensions change. */
        SolBgEffectBlurRegion aux_region = {0};
        const SolBgEffectBlurRegion *blur_regions = reg->blur_regions;
        size_t blur_region_count = reg->blur_region_count;
        BlurGPU *blur = NULL;
        if (apply_blur && frame_slot < CA_FRAMES_IN_FLIGHT) {
            blur = &reg->blur[frame_slot];
        } else if (!apply_blur && window) {
            blur_region_count = 0;
            blur = aux_blur_for_window(reg, window, frame_slot);
        }
        if (blur && reg->blur_passes > 0 && blur_region_count > 0 &&
            (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
            bool needs_blur_init = !blur->initialized;
            bool dims_changed    = blur->initialized &&
                                   (blur->width != width ||
                                    blur->height != height ||
                                    blur->format != format);
            if (needs_blur_init || dims_changed) {
                VkDevice dev = ca_gpu_device(reg->instance);
                if (blur->initialized) blur_destroy(blur, dev);
                if (!blur_build(blur, reg->instance, format, width, height))
                    reg->blur_passes = 0; /* Disable blur on failure — degrade gracefully */
            }
        }

        /* Render bg shader directly into swapchain. */
        VkRenderingAttachmentInfo color_att = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = swapchain_view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue  = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
        };
        VkRenderingInfo ri = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = { {0, 0}, {width, height} },
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color_att,
        };
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->gpu.pipeline);

        VkViewport vk_vp  = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        VkRect2D   scissor = { {0, 0}, {width, height} };
        vkCmdSetViewport(cmd, 0, 1, &vk_vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        BgPushConst pc = { t, (float)width, (float)height, reg->opacity,
                           reg->theme_r, reg->theme_g, reg->theme_b, 0.0f };
        vkCmdPushConstants(cmd, e->gpu.pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        /* Apply separable Gaussian blur for frosted-glass. */
        if (blur && reg->blur_passes > 0 && blur->initialized &&
            blur_region_count > 0 &&
            (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
            VkDevice dev = ca_gpu_device(reg->instance);
            for (int i = 0; i < reg->blur_passes; ++i)
                blur_execute(blur, dev, cmd, swapchain_image, swapchain_view,
                             blur_regions, blur_region_count,
                             (uint32_t)i, i == 0);
        }
    } else {
        if (!apply_blur) return false;
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

/* Render the active effect with the primary window's localized blur regions. */
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

/* Render the same shader state into an auxiliary window without rebuilding blur resources. */
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
