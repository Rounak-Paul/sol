// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-bfx — Built-in animated background shader effects.
 *
 * Registers five effects with the background effect registry:
 *
 *   com.sol.bfx.plasma    — classic sine-wave plasma in muted hues
 *   com.sol.bfx.aurora    — flowing aurora borealis bands
 *   com.sol.bfx.starfield — drifting star field with soft twinkle
 *   com.sol.bfx.wave      — deep ocean surface with foam highlights
 *   com.sol.bfx.nebula    — slow-drifting cosmic cloud layers
 *
 * All shaders are dark-themed and subtle so they sit behind the editor
 * UI without competing for attention.  The `opacity` push constant
 * (from the global intensity setting) controls overall brightness.
 *
 * Fragment shader contract:
 *
 *   #version 450
 *   layout(push_constant) uniform PC {
 *       float time; float width; float height; float opacity;
 *   } pc;
 *   layout(location = 0) in  vec2 v_uv;   // [0, 1]^2
 *   layout(location = 0) out vec4 out_color;
 */

#include "sol_bg_effect.h"
#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

/* ------------------------------------------------------------------ */
/* Fragment shaders                                                    */
/* ------------------------------------------------------------------ */

/* Aurora Borealis — multi-layer volumetric curtains with vertical ray shafts */
static const char k_aurora_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { float time; float width; float height; float opacity; } pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float noise(vec2 p) {\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    vec2 u = f*f*(3.0-2.0*f);\n"
    "    return mix(mix(hash21(i),hash21(i+vec2(1,0)),u.x),\n"
    "               mix(hash21(i+vec2(0,1)),hash21(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"
    "float fbm(vec2 p) {\n"
    "    float v=0.0,a=0.5;\n"
    "    for(int i=0;i<5;i++){v+=a*noise(p);p=p*2.1+vec2(1.3,1.7);a*=0.5;}\n"
    "    return v;\n"
    "}\n"
    "void main() {\n"
    "    float t = pc.time * 0.18;\n"
    "    vec2 uv = v_uv;\n"
    "    uv.x *= pc.width / pc.height;\n"
    /* Sky gradient: deep navy at top, near-black at bottom */
    "    vec3 sky = mix(vec3(0.02,0.04,0.12), vec3(0.005,0.008,0.03), uv.y);\n"
    /* Three aurora curtain layers at different heights */
    "    vec3 col = sky;\n"
    "    for(int li=0; li<3; li++) {\n"
    "        float lf = float(li);\n"
    "        float base_y = 0.30 + lf * 0.18;\n"
    /* Undulating baseline driven by fbm */
    "        float wx = fbm(vec2(uv.x * 0.8 + lf * 2.3 + t * (0.4 + lf * 0.15),\n"
    "                            t * 0.3 + lf));\n"
    "        float curve = base_y + (wx - 0.5) * 0.22;\n"
    /* Vertical ray shafts via fbm in x */
    "        float ray = fbm(vec2(uv.x * 2.5 - lf * 1.1 + t * 0.25, t * 0.1));\n"
    "        float band = exp(-pow((uv.y - curve) * (7.0 + ray * 4.0), 2.0));\n"
    "        band *= 0.7 + 0.3 * fbm(vec2(uv.x * 5.0 + t, uv.y * 3.0));\n"
    /* Per-layer hue: green → cyan → violet */
    "        vec3 hue;\n"
    "        if(li==0) hue = mix(vec3(0.05,0.80,0.40), vec3(0.10,0.95,0.60), ray);\n"
    "        else if(li==1) hue = mix(vec3(0.05,0.60,0.85), vec3(0.20,0.40,1.00), ray);\n"
    "        else           hue = mix(vec3(0.50,0.10,0.90), vec3(0.70,0.20,1.00), ray);\n"
    "        col += hue * band * (0.55 - lf * 0.08);\n"
    "    }\n"
    /* Stars in upper half */
    "    for(int si=0; si<80; si++) {\n"
    "        float fi = float(si);\n"
    "        vec2 sp = vec2(hash21(vec2(fi,1.0)), hash21(vec2(fi,2.0)) * 0.55);\n"
    "        sp.x *= pc.width / pc.height;\n"
    "        float d = length(uv - sp);\n"
    "        float b = smoothstep(0.004, 0.0, d) * (0.5 + 0.5*sin(fi + t*2.0));\n"
    "        col += b * vec3(0.8, 0.9, 1.0);\n"
    "    }\n"
    "    col = pow(clamp(col, 0.0, 1.0), vec3(0.85));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* Nebula — deep space gas cloud with star clusters and emission glow */
static const char k_nebula_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { float time; float width; float height; float opacity; } pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}\n"
    "float h11(float n){return fract(sin(n)*43758.5);}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    return mix(mix(h21(i),h21(i+vec2(1,0)),u.x),\n"
    "               mix(h21(i+vec2(0,1)),h21(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"
    "float fbm(vec2 p,int oct){\n"
    "    float v=0.0,a=0.5;\n"
    "    mat2 rot=mat2(0.8,-0.6,0.6,0.8);\n"
    "    for(int i=0;i<oct;i++){v+=a*noise(p);p=rot*p*2.1;a*=0.5;}\n"
    "    return v;\n"
    "}\n"
    "void main() {\n"
    "    float t  = pc.time * 0.05;\n"
    "    vec2  uv = (v_uv - 0.5) * vec2(pc.width/pc.height, 1.0);\n"
    /* Drift the whole field slowly */
    "    uv += vec2(t * 0.02, t * 0.01);\n"
    /* Deep void background */
    "    vec3 col = vec3(0.005, 0.004, 0.010);\n"
    /* Three overlapping nebula lobes with different hues */
    "    vec2 c1 = vec2(-0.35, 0.10), c2 = vec2(0.30,-0.15), c3 = vec2(-0.05,0.40);\n"
    "    float g1 = fbm(uv*1.8 + c1 + t*0.12, 6);\n"
    "    float g2 = fbm(uv*2.2 + c2 - t*0.09, 6);\n"
    "    float g3 = fbm(uv*1.5 + c3 + t*0.07, 5);\n"
    "    float d1 = exp(-length(uv-c1)*2.2) * g1 * 1.4;\n"
    "    float d2 = exp(-length(uv-c2)*2.5) * g2 * 1.2;\n"
    "    float d3 = exp(-length(uv-c3)*2.0) * g3 * 0.9;\n"
    "    col += vec3(0.18,0.04,0.35) * d1;\n"
    "    col += vec3(0.05,0.20,0.55) * d2;\n"
    "    col += vec3(0.55,0.08,0.18) * d3;\n"
    /* Emission filaments */
    "    float fil = fbm(uv*4.0 + t*0.08, 4);\n"
    "    col += vec3(0.30,0.50,1.00) * pow(fil,3.0) * 0.6;\n"
    "    col += vec3(1.00,0.35,0.20) * pow(fbm(uv*3.5-t*0.06,4),3.5)*0.5;\n"
    /* Stars: bright point clusters */
    "    for(int si=0;si<120;si++){\n"
    "        float fi=float(si);\n"
    "        vec2 sp=vec2(h11(fi*1.1)-0.5,h11(fi*2.3)-0.5)*vec2(pc.width/pc.height,1.0);\n"
    "        float mag=h11(fi*3.7);\n"
    "        float d=length(uv-sp);\n"
    "        float b=smoothstep(0.003+mag*0.003,0.0,d)*(0.6+0.4*sin(fi*7.3+t*3.0));\n"
    "        vec3 sc=mix(vec3(0.7,0.85,1.0),vec3(1.0,0.9,0.7),h11(fi*5.1));\n"
    "        col += b * sc * (0.5 + mag * 0.8);\n"
    "    }\n"
    "    col = col * 1.1;\n"
    "    col = pow(clamp(col,0.0,1.0),vec3(0.80));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* Hyperspace — warp tunnel with streaking stars flying toward viewer */
static const char k_hyperspace_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { float time; float width; float height; float opacity; } pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float h11(float n){return fract(sin(n*127.1)*43758.5);}\n"
    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}\n"
    "void main() {\n"
    "    vec2 uv = v_uv * 2.0 - 1.0;\n"
    "    uv.x *= pc.width / pc.height;\n"
    "    float t = pc.time * 0.7;\n"
    "    float ang = atan(uv.y, uv.x);\n"
    "    float rad = length(uv);\n"
    /* Tunnel rings: concentric bright rings rushing toward viewer */
    "    float tunnel = 0.0;\n"
    "    for(int ri=0;ri<6;ri++){\n"
    "        float rf = float(ri);\n"
    "        float ring_r = fract(0.15 + rf*0.18 - t * (0.5 + rf*0.08));\n"
    "        float ring = exp(-pow((rad - ring_r) * 18.0, 2.0));\n"
    "        tunnel += ring * (0.6 + 0.4 * sin(ang * (3.0+rf) + t*2.0));\n"
    "    }\n"
    /* Star streaks: points that zoom outward */
    "    vec3 col = vec3(0.0);\n"
    "    for(int si=0;si<90;si++){\n"
    "        float fi = float(si);\n"
    "        float a  = h11(fi * 1.3) * 6.2832;\n"
    "        float spd = 0.25 + h11(fi * 2.7) * 0.6;\n"
    "        float r0  = h11(fi * 3.1) * 0.1;\n"
    "        float r1  = r0 + spd * fract(t * spd + h11(fi * 4.9));\n"
    "        r1 = clamp(r1, 0.0, 2.0);\n"
    "        float px = cos(a), py = sin(a);\n"
    /* Streak from r0 to r1 along the angle */
    "        vec2 pa = vec2(px,py)*r0 - uv;\n"
    "        vec2 ba = vec2(px,py)*(r1-r0);\n"
    "        float h = clamp(dot(pa,ba)/max(dot(ba,ba),1e-6),0.0,1.0);\n"
    "        float d = length(pa - h*ba);\n"
    "        float streak = exp(-d*d*3000.0) * (r1 - r0) * 4.0;\n"
    "        float hue = h11(fi * 5.3);\n"
    "        col += streak * mix(vec3(0.5,0.8,1.0), vec3(1.0,0.7,0.4), hue);\n"
    "    }\n"
    /* Tunnel glow */
    "    col += vec3(0.2,0.4,1.0) * tunnel * 0.5;\n"
    "    col += vec3(0.4,0.2,0.8) * tunnel * tunnel * 0.3;\n"
    /* Central vortex glow */
    "    float vortex = exp(-rad * 5.0) * 0.6;\n"
    "    col += vec3(0.3,0.5,1.0) * vortex;\n"
    "    col = pow(clamp(col,0.0,1.0), vec3(0.82));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* Lava — organic blobs of molten rock with glowing fissures */
static const char k_lava_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { float time; float width; float height; float opacity; } pc;\n"
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
    /* Domain-warp: distort uv with fbm twice for organic look */
    "    vec2 q  = vec2(fbm(uv + vec2(0.0,0.0) + t*0.3),\n"
    "                   fbm(uv + vec2(5.2,1.3) + t*0.25));\n"
    "    vec2 r  = vec2(fbm(uv + 3.0*q + vec2(1.7,9.2) + t*0.15),\n"
    "                   fbm(uv + 3.0*q + vec2(8.3,2.8) - t*0.12));\n"
    "    float f = fbm(uv + 2.8*r + t*0.08);\n"
    /* Color mapping: dark rock → hot orange → white-hot core */
    "    vec3 dark  = vec3(0.05, 0.015, 0.005);\n"
    "    vec3 warm  = vec3(0.55, 0.10,  0.01);\n"
    "    vec3 hot   = vec3(1.00, 0.45,  0.04);\n"
    "    vec3 white = vec3(1.00, 0.90,  0.70);\n"
    "    float fi = clamp(f * 1.3, 0.0, 1.0);\n"
    "    vec3 col;\n"
    "    if(fi < 0.33)      col = mix(dark,  warm,  fi / 0.33);\n"
    "    else if(fi < 0.66) col = mix(warm,  hot,   (fi-0.33)/0.33);\n"
    "    else               col = mix(hot,   white, (fi-0.66)/0.34);\n"
    /* Fissure glow: thin bright cracks along high-gradient ridges */
    "    float grad = length(vec2(dFdx(f), dFdy(f))) * 60.0;\n"
    "    float fissure = clamp(grad - 0.6, 0.0, 1.0);\n"
    "    col += vec3(1.0,0.6,0.1) * fissure * 0.8;\n"
    "    col = pow(clamp(col, 0.0, 1.0), vec3(0.88));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* Matrix Rain — cascading green digital rain columns */
static const char k_matrix_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC { float time; float width; float height; float opacity; } pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float h11(float n){return fract(sin(n)*43758.5453);}\n"
    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}\n"
    "void main() {\n"
    "    float cols = 48.0;\n"
    "    float rows = cols * (pc.height / pc.width);\n"
    "    vec2  cell = floor(v_uv * vec2(cols, rows));\n"
    "    vec2  f    = fract(v_uv * vec2(cols, rows));\n"
    /* Per-column properties */
    "    float col_id  = cell.x;\n"
    "    float speed   = 0.4 + h11(col_id * 1.3) * 1.2;\n"
    "    float offset  = h11(col_id * 2.7) * rows;\n"
    /* Drop head position scrolls downward */
    "    float head_y  = fract(pc.time * speed * 0.22 + offset / rows) * rows;\n"
    /* Distance from head (wrapping) */
    "    float dist    = mod(head_y - cell.y, rows);\n"
    /* Trail length varies per column */
    "    float trail   = 6.0 + h11(col_id * 3.9) * 10.0;\n"
    /* Brightness falls off behind the head */
    "    float bright  = clamp(1.0 - dist / trail, 0.0, 1.0);\n"
    "    bright = pow(bright, 1.6);\n"
    /* Head pixel is white, rest is green */
    "    float is_head = step(dist, 0.9);\n"
    /* Glitch: random chars flicker */
    "    float ch = h21(cell + vec2(floor(pc.time * 8.0 * speed)));\n"
    "    float glyph = smoothstep(0.55, 0.70, ch) * bright;\n"
    /* Pixel rounded corners to fake a glyph shape */
    "    vec2 fc = abs(f - 0.5);\n"
    "    float pixel = (1.0 - smoothstep(0.30, 0.42, max(fc.x, fc.y))) * glyph;\n"
    /* Glow around active cells */
    "    float glow = exp(-dist * 0.5) * h11(col_id * 4.1 + floor(pc.time)) * 0.3;\n"
    "    vec3  green  = vec3(0.05, 0.90, 0.30);\n"
    "    vec3  bright_col = mix(green, vec3(0.8, 1.0, 0.9), is_head);\n"
    "    vec3  col = bright_col * pixel + green * glow * 0.4;\n"
    /* Dark void background with very subtle grid */
    "    col += vec3(0.0, 0.03, 0.01);\n"
    "    col = pow(clamp(col, 0.0, 1.0), vec3(0.90));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Plugin callbacks                                                    */
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
        { .id = "com.sol.bfx.aurora",      .display_name = "Aurora",      .fragment_glsl = k_aurora_frag      },
        { .id = "com.sol.bfx.nebula",      .display_name = "Nebula",      .fragment_glsl = k_nebula_frag      },
        { .id = "com.sol.bfx.hyperspace",  .display_name = "Hyperspace",  .fragment_glsl = k_hyperspace_frag  },
        { .id = "com.sol.bfx.lava",        .display_name = "Lava",        .fragment_glsl = k_lava_frag        },
        { .id = "com.sol.bfx.matrix",      .display_name = "Matrix",      .fragment_glsl = k_matrix_frag      },
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

    static const char *effect_ids[] = {
        "com.sol.bfx.aurora",
        "com.sol.bfx.nebula",
        "com.sol.bfx.hyperspace",
        "com.sol.bfx.lava",
        "com.sol.bfx.matrix",
    };
    for (size_t i = 0; i < sizeof(effect_ids) / sizeof(effect_ids[0]); ++i)
        sol_bg_effect_unregister(reg, effect_ids[i]);
}

/* ------------------------------------------------------------------ */
/* Plugin entry point                                                  */
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
