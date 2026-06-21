// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-bfx — Built-in animated background shader effects.
 *
 * Registers one effect with the background effect registry:
 *
 *   com.sol.bfx.lava  — organic molten-rock blobs, theme-color tinted
 *
 * The lava shader blends its palette with the active theme accent color
 * (r/g/b push constants) so every theme produces a distinct background mood.
 *
 * Fragment shader push constant contract:
 *
 *   #version 450
 *   layout(push_constant) uniform PC {
 *       float time; float width; float height; float opacity;
 *       float r; float g; float b; float _pad;
 *   } pc;
 *   layout(location = 0) in  vec2 v_uv;   // [0, 1]^2
 *   layout(location = 0) out vec4 out_color;
 */

#include "sol_bg_effect.h"
#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

/* ------------------------------------------------------------------ */
/* Fragment shader                                                      */
/* ------------------------------------------------------------------ */

/*
 * Lava — organic domain-warped blobs of molten rock with glowing fissures.
 * The hot palette mixes toward the theme accent color (pc.r, pc.g, pc.b)
 * so the animation reflects the active theme's hue.
 */
static const char k_lava_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time; float width; float height; float opacity;\n"
    "    float r; float g; float b; float _pad;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    return mix(mix(h21(i),h21(i+vec2(1,0)),u.x),\n"
    "               mix(h21(i+vec2(0,1)),h21(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"
    "float fbm(vec2 p){\n"
    "    float v=0.0,a=0.5;\n"
    "    mat2 m=mat2(0.8,-0.6,0.6,0.8);\n"
    "    for(int i=0;i<6;i++){v+=a*noise(p);p=m*p*2.0+vec2(3.1,1.7);a*=0.5;}\n"
    "    return v;\n"
    "}\n"
    "void main() {\n"
    "    float t  = pc.time * 0.14;\n"
    "    vec2  uv = v_uv * vec2(pc.width/pc.height, 1.0) * 1.6;\n"
    "\n"
    "    vec2 q  = vec2(fbm(uv + vec2(0.0,0.0) + t*0.3),\n"
    "                   fbm(uv + vec2(5.2,1.3) + t*0.25));\n"
    "    vec2 r  = vec2(fbm(uv + 3.0*q + vec2(1.7,9.2) + t*0.15),\n"
    "                   fbm(uv + 3.0*q + vec2(8.3,2.8) - t*0.12));\n"
    "    float f = fbm(uv + 2.8*r + t*0.08);\n"
    "\n"
    "    vec3 accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float sat = length(accent - vec3(dot(accent, vec3(0.333))));\n"
    "    vec3 hot_col = sat > 0.08 ? accent : vec3(1.0, 0.45, 0.04);\n"
    "\n"
    "    vec3 dark  = hot_col * 0.045;\n"
    "    vec3 warm  = hot_col * 0.35;\n"
    "    vec3 hot   = hot_col * 1.05;\n"
    "    vec3 white = mix(hot_col, vec3(1.0, 0.96, 0.88), 0.72);\n"
    "\n"
    "    float fi = clamp((f - 0.24) * 1.55, 0.0, 1.0);\n"
    "    fi = smoothstep(0.0, 1.0, fi);\n"
    "\n"
    "    vec3 col;\n"
    "    if(fi < 0.33)\n"
    "        col = mix(dark, warm, fi / 0.33);\n"
    "    else if(fi < 0.66)\n"
    "        col = mix(warm, hot, (fi - 0.33) / 0.33);\n"
    "    else\n"
    "        col = mix(hot, white, (fi - 0.66) / 0.34);\n"
    "\n"
    "    float grad = length(vec2(dFdx(f), dFdy(f))) * 60.0;\n"
    "    float fissure = smoothstep(0.65, 1.15, grad);\n"
    "    col += white * fissure * 0.45;\n"
    "\n"
    "    col = pow(clamp(col, 0.0, 1.0), vec3(0.82));\n"
    "\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Plugin callbacks                                                     */
/* ------------------------------------------------------------------ */

static bool bfx_on_load(SolPluginCtx *ctx)
{
    SolBgEffectRegistry *reg = (SolBgEffectRegistry *)
        sol_plugin_get_service(ctx, "sol.bg_effect_registry", 0);
    if (!reg) {
        sol_plugin_log(ctx, "sol.bg_effect_registry not available; skipping effect registration");
        return true;
    }

    static const SolBgEffectDesc effects[] = {
        {
            .id = "com.sol.bfx.lava",
            .display_name = "Lava",
            .fragment_glsl = k_lava_frag,
            .animation_fps = 30u,
        },
    };

    for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); ++i) {
        if (!sol_bg_effect_register(reg, &effects[i]))
            sol_plugin_log(ctx, "failed to register effect '%s'", effects[i].id);
    }

    return true;
}

static void bfx_on_unload(SolPluginCtx *ctx)
{
    SolBgEffectRegistry *reg = (SolBgEffectRegistry *)
        sol_plugin_get_service(ctx, "sol.bg_effect_registry", 0);
    if (!reg) return;

    static const char *effect_ids[] = { "com.sol.bfx.lava" };
    for (size_t i = 0; i < sizeof(effect_ids) / sizeof(effect_ids[0]); ++i)
        sol_bg_effect_unregister(reg, effect_ids[i]);
}

/* ------------------------------------------------------------------ */
/* Plugin entry point                                                   */
/* ------------------------------------------------------------------ */

/*
 * Query function exported by the plugin; called by the plugin manager to
 * discover capabilities and verify API compatibility.
 *
 * requested_api_version  API version the manager was compiled against.
 * out_api                Filled with this plugin's descriptor.
 * Returns  true if the plugin supports the requested API version.
 */
bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION) return false;

    *out_api = (SolPluginAPI){
        .api_version  = SOL_PLUGIN_API_VERSION,
        .id           = "com.sol.bfx",
        .display_name = "Sol Background Effects",
        .version      = "1.0.0",
        .after        = { NULL },
        .on_load      = bfx_on_load,
        .on_unload    = bfx_on_unload,
    };
    return true;
}
