// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-bfx — Built-in animated background shader effects. */

#include "sol_bg_effect.h"
#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

/* ------------------------------------------------------------------ */
/* Shared vertex shader (fullscreen triangle)                          */
/* Push constant layout for all effects:                               */
/*   float time, width, height, opacity, r, g, b, _pad                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Lava                                                                 */
/* Low-frequency domain-warped FBM only — no aliasing on 1080p.        */
/* ------------------------------------------------------------------ */

static const char k_lava_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time,width,height,opacity,r,g,b,_pad;\n"
    "} pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"

    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    return mix(mix(h21(i),h21(i+vec2(1,0)),u.x),\n"
    "               mix(h21(i+vec2(0,1)),h21(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"
    /* 4-octave FBM — stops before the octave that would alias at 1080p */
    "float fbm(vec2 p){\n"
    "    float v=0.0,a=0.5;\n"
    "    mat2 m=mat2(0.8,-0.6,0.6,0.8);\n"
    "    for(int i=0;i<4;i++){v+=a*noise(p);p=m*p*2.0+vec2(3.1,1.7);a*=0.5;}\n"
    "    return v;\n"
    "}\n"
    "void main(){\n"
    "    float t  = pc.time*0.12;\n"
    "    vec2  uv = v_uv*vec2(pc.width/pc.height,1.0)*1.4;\n"
    "    vec2 q = vec2(fbm(uv+t*0.30), fbm(uv+vec2(5.2,1.3)+t*0.25));\n"
    "    vec2 rr= vec2(fbm(uv+2.8*q+vec2(1.7,9.2)+t*0.14),\n"
    "                  fbm(uv+2.8*q+vec2(8.3,2.8)-t*0.11));\n"
    "    float f = fbm(uv+2.5*rr+t*0.07);\n"
    "    vec3 accent = vec3(pc.r,pc.g,pc.b);\n"
    "    float sat = length(accent-vec3(dot(accent,vec3(0.333))));\n"
    "    vec3 hc = sat>0.08 ? accent : vec3(1.0,0.42,0.04);\n"
    "    float fi = smoothstep(0.0,1.0,clamp(f*1.6-0.08,0.0,1.0));\n"
    "    vec3 col = mix(hc*0.06, mix(hc*0.38, mix(hc*0.90,\n"
    "               mix(hc,vec3(1.0,0.95,0.85),0.60), smoothstep(0.66,1.0,fi)),\n"
    "               smoothstep(0.33,0.66,fi)), smoothstep(0.0,0.33,fi));\n"
    /* Reinhard + gamma 2.2 */
    "    col = col/(col+vec3(0.30));\n"
    "    col = pow(max(col,vec3(0.0)),vec3(1.0/2.2));\n"
    "    out_color = vec4(clamp(col,0.0,1.0),pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Nebula — deep-space gas cloud, clearly distinct from Lava.          */
/* Uses radial falloff + dust lanes + stars. Very dark background.     */
/* ------------------------------------------------------------------ */

static const char k_nebula_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time,width,height,opacity,r,g,b,_pad;\n"
    "} pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"

    "vec2 hash2(vec2 p){\n"
    "    p=vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3)));\n"
    "    return fract(sin(p)*43758.5);\n"
    "}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    vec2 a=hash2(i),b=hash2(i+vec2(1,0)),\n"
    "         c=hash2(i+vec2(0,1)),d=hash2(i+vec2(1,1));\n"
    "    return mix(mix(a.x,b.x,u.x),mix(c.x,d.x,u.x),u.y);\n"
    "}\n"
    /* 3-octave FBM — fast and alias-free at any resolution */
    "float fbm(vec2 p){\n"
    "    float v=0.0,a=0.5;\n"
    "    mat2 m=mat2(1.6,-1.2,1.2,1.6);\n"
    "    for(int i=0;i<3;i++){v+=a*noise(p);p=m*p;a*=0.5;}\n"
    "    return v;\n"
    "}\n"
    "void main(){\n"
    "    float t  = pc.time*0.05;\n"
    "    float ar = pc.width/pc.height;\n"
    "    vec2  uv = (v_uv-0.5)*vec2(ar,1.0);\n"

    "    vec3 accent = vec3(pc.r,pc.g,pc.b);\n"
    "    float lum = dot(accent,vec3(0.299,0.587,0.114));\n"
    "    float sat = length(accent-vec3(lum));\n"
    /* Primary nebula hue from accent, fallback cool purple */
    "    vec3 h1 = sat>0.07 ? accent : vec3(0.45,0.05,0.80);\n"
    /* Complementary hue: rotate by shifting channels */
    "    vec3 h2 = clamp(vec3(h1.b*0.7,h1.r*0.5,h1.g+0.25),0.0,1.0);\n"
    /* Blue-white for bright emission cores */
    "    vec3 h3 = mix(h1,vec3(0.15,0.55,1.0),0.45);\n"

    "    vec2 q = vec2(fbm(uv*1.6+vec2(0.0,t*0.35)),\n"
    "                  fbm(uv*1.6+vec2(4.8,t*0.30)));\n"
    "    vec2 rr= vec2(fbm(uv*1.3+3.5*q+vec2(1.7,9.2)+t*0.18),\n"
    "                  fbm(uv*1.3+3.5*q+vec2(8.3,2.8)-t*0.16));\n"
    "    float f = fbm(uv*1.0+3.0*rr+t*0.09);\n"

    /* Nebula blob: strong radial falloff keeps void dark */
    "    float dist = length(uv*vec2(0.75,1.0));\n"
    "    float veil = exp(-dist*dist*2.8)*1.4;\n"
    "    float dens = clamp((f-0.30)*2.6,0.0,1.0)*veil;\n"

    "    vec3 nebcol = mix(h1*0.12, h2*0.55, smoothstep(0.2,0.65,f));\n"
    "    nebcol      = mix(nebcol,  h3*1.0,  smoothstep(0.60,0.90,f));\n"
    "    nebcol     += h1*0.30*smoothstep(0.82,1.0,f);\n"

    /* Very dark void — almost black */
    "    vec3 bg  = vec3(0.002,0.003,0.010)+h1*0.018;\n"
    "    vec3 col = mix(bg, nebcol, dens);\n"

    /* Dust lane: darker streak, tightened threshold */
    "    float lane = fbm(uv*3.5+vec2(t*0.25,0.0));\n"
    "    col = mix(col, bg*0.25, smoothstep(0.52,0.46,lane)*dens*0.5);\n"

    /* Stars: cell size = ~36 logical px so density is resolution-independent */
    "    float sc0 = min(pc.width,pc.height)/36.0;\n"
    "    for(int li=0;li<3;li++){\n"
    "        float sc = sc0*(1.0+float(li)*1.5);\n"
    "        vec2  sg = floor(uv*sc+float(li)*vec2(31.3,17.7));\n"
    "        vec2  hh = hash2(sg);\n"
    "        float br = step(0.930+float(li)*0.022,hh.x);\n"
    "        float tw = 0.6+0.4*sin(t*25.0*(3.5+hh.y*5.0)+hh.y*6.28);\n"
    /* Radius in UV: 1.2 logical pixels */
    "        float sz = 1.2/pc.width;\n"
    "        vec2  fc = fract(uv*sc+float(li)*vec2(31.3,17.7))-0.5;\n"
    "        float sd = length(fc)/sc;\n"
    "        float st = br*tw*exp(-sd*sd/(sz*sz)*9.0);\n"
    "        vec3 sc2 = mix(vec3(0.75,0.87,1.0),vec3(1.0,0.92,0.65),hh.y);\n"
    "        col += sc2*st*(0.85-dens*0.65);\n"
    "    }\n"

    /* Core glow */
    "    col += h1*exp(-dist*dist*9.0)*0.40;\n"

    "    col = col/(col+vec3(0.18));\n"
    "    col = pow(max(col,vec3(0.0)),vec3(1.0/2.2));\n"
    "    out_color = vec4(clamp(col,0.0,1.0),pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Wave — dark deep ocean, subtle Gerstner swell.                      */
/* Minimal specular, no caustics, reads well at any resolution.        */
/* ------------------------------------------------------------------ */

static const char k_wave_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time,width,height,opacity,r,g,b,_pad;\n"
    "} pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"

    "vec3 gerstner(vec2 p, vec2 dir, float wl, float steep, float t){\n"
    "    float k=6.28318/wl, c=sqrt(9.8/k);\n"
    "    float f=k*(dot(dir,p)-c*t), a=steep/k;\n"
    "    return vec3(-dir*k*a*sin(f), a*cos(f));\n"
    "}\n"
    "vec4 ocean(vec2 p, float t){\n"
    "    vec3 n=vec3(0.0);\n"
    "    n+=gerstner(p,normalize(vec2( 1.0, 0.4)),1.80,0.22,t);\n"
    "    n+=gerstner(p,normalize(vec2(-0.6, 1.0)),1.20,0.18,t*0.91);\n"
    "    n+=gerstner(p,normalize(vec2( 0.3,-1.0)),0.80,0.14,t*1.07);\n"
    "    n+=gerstner(p,normalize(vec2( 1.0, 1.0)),0.55,0.10,t*1.23);\n"
    "    n+=gerstner(p,normalize(vec2(-1.0, 0.3)),0.38,0.07,t*0.85);\n"
    "    return vec4(normalize(vec3(-n.x,-n.y,1.0)),n.z);\n"
    "}\n"
    "void main(){\n"
    "    float t  = pc.time*0.45;\n"
    "    float ar = pc.width/pc.height;\n"
    "    vec2  uv = (v_uv-0.5)*vec2(ar,1.0)*3.2;\n"

    "    vec3 accent = vec3(pc.r,pc.g,pc.b);\n"
    "    float lum = dot(accent,vec3(0.299,0.587,0.114));\n"
    "    float sat = length(accent-vec3(lum));\n"
    /* Very dark ocean floor, mid-tone swell, subtle crest highlight */
    "    vec3 deep  = sat>0.07 ? accent*0.08 : vec3(0.003,0.018,0.060);\n"
    "    vec3 mid   = sat>0.07 ? accent*0.22 : vec3(0.008,0.060,0.160);\n"
    "    vec3 crest = sat>0.07 ? mix(accent,vec3(0.6,0.8,1.0),0.5)*0.55\n"
    "                          : vec3(0.12,0.30,0.55);\n"

    "    vec4 surf = ocean(uv,t);\n"
    "    vec3 N    = surf.xyz;\n"
    "    float wh  = surf.w;\n"

    "    vec3 L = normalize(vec3(0.3,-0.7,1.0));\n"
    "    vec3 V = vec3(0.0,0.0,1.0);\n"
    "    vec3 H = normalize(L+V);\n"

    /* Diffuse: wide range but stays dark */
    "    float diff = max(0.0,dot(N,L))*0.55+0.45;\n"
    /* Single tight specular highlight, intentionally dim */
    "    float spec = pow(max(0.0,dot(N,H)),80.0)*0.35;\n"

    "    float h01 = wh*0.5+0.5;\n"
    "    vec3 water = mix(deep, mid,   smoothstep(0.0,0.55,h01));\n"
    "    water      = mix(water,crest, smoothstep(0.60,0.90,h01));\n"

    "    vec3 col = water*diff + crest*spec;\n"

    /* Subtle edge vignette to anchor the scene */
    "    float vig = 1.0-smoothstep(0.4,1.2,length(v_uv-0.5)*2.2);\n"
    "    col *= 0.45+0.55*vig;\n"

    "    col = col/(col+vec3(0.18));\n"
    "    col = pow(max(col,vec3(0.0)),vec3(1.0/2.2));\n"
    "    out_color = vec4(clamp(col,0.0,1.0),pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Aurora — horizontal light ribbons drifting across dark sky.         */
/* No noise aliasing: uses only smooth sin/cos curves.                 */
/* ------------------------------------------------------------------ */

static const char k_aurora_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time,width,height,opacity,r,g,b,_pad;\n"
    "} pc;\n"
    "layout(location=0) in  vec2 v_uv;\n"
    "layout(location=0) out vec4 out_color;\n"

    "void main(){\n"
    "    float t  = pc.time*0.18;\n"
    "    float ar = pc.width/pc.height;\n"
    "    vec2  uv = (v_uv-0.5)*vec2(ar,1.0);\n"

    "    vec3 accent = vec3(pc.r,pc.g,pc.b);\n"
    "    float lum = dot(accent,vec3(0.299,0.587,0.114));\n"
    "    float sat = length(accent-vec3(lum));\n"
    /* Three aurora band hues, derived from accent */
    "    vec3 h1 = sat>0.07 ? accent            : vec3(0.05,0.90,0.55);\n"
    "    vec3 h2 = sat>0.07 ? vec3(h1.b,h1.g*0.5,h1.r*0.8) : vec3(0.05,0.55,0.90);\n"
    "    vec3 h3 = sat>0.07 ? vec3(h1.g,h1.b,h1.r)          : vec3(0.60,0.20,0.95);\n"

    /* Very dark sky — almost black at the bottom, slightly less at top */
    "    float sky = smoothstep(-0.5,0.5,uv.y)*0.03;\n"
    "    vec3 col  = vec3(0.003,0.004,0.012)+vec3(sky*0.5,sky*0.6,sky);\n"

    /* Five ribbon bands, each a thin sine-modulated horizontal strip */
    "    for(int i=0;i<5;i++){\n"
    "        float fi = float(i);\n"
    /* Band centre Y: spread in upper half of screen */
    "        float cy  = 0.05+fi*0.08;\n"
    /* Slow horizontal drift and gentle vertical oscillation */
    "        float wave = sin(uv.x*1.2+fi*1.7+t*(0.6+fi*0.15))*0.06\n"
    "                   + sin(uv.x*2.5+fi*3.1+t*(0.4+fi*0.20))*0.03;\n"
    "        float dy   = uv.y-cy-wave;\n"
    /* Band thickness: ~0.06 UV units = not resolution dependent */
    "        float thick = 0.028+fi*0.006;\n"
    "        float band = exp(-dy*dy/(thick*thick));\n"
    /* Brightness modulates along X — gives curtain-like columns */
    "        float bri = 0.55+0.45*sin(uv.x*(3.0+fi*1.1)+t*(1.2+fi*0.3));\n"
    "        bri *= 0.40+0.60*sin(uv.x*(0.8+fi*0.5)-t*(0.7+fi*0.2)+fi*2.1);\n"
    "        bri  = max(bri,0.0);\n"
    /* Mix band hue; alternate h1/h2/h3 across bands */
    "        vec3 hue = (i==0||i==3) ? h1 : (i==1||i==4) ? h2 : h3;\n"
    /* Upper bands slightly bluish-white at the top edge */
    "        float top = smoothstep(0.0,-0.015,dy)*0.4;\n"
    "        hue = mix(hue, mix(hue,vec3(0.7,0.9,1.0),0.6), top);\n"
    "        col += hue*band*bri*(0.18-fi*0.015);\n"
    "    }\n"

    /* Faint star layer: purely hash-based point lookup, no loop over noise */
    "    float sc = min(pc.width,pc.height)/32.0;\n"
    "    vec2  sg  = floor(uv*sc);\n"
    "    vec2  hh  = fract(sin(vec2(dot(sg,vec2(127.1,311.7)),\n"
    "                              dot(sg,vec2(269.5,183.3))))*43758.5);\n"
    "    float br  = step(0.94,hh.x);\n"
    "    float tw  = 0.5+0.5*sin(t*20.0*(3.0+hh.y*4.0)+hh.y*6.28);\n"
    "    float sz  = 1.0/pc.width;\n"
    "    vec2  fc  = fract(uv*sc)-0.5;\n"
    "    float sd  = length(fc)/sc;\n"
    "    col += vec3(0.75,0.88,1.0)*br*tw*exp(-sd*sd/(sz*sz)*8.0)*0.7;\n"

    "    col = col/(col+vec3(0.15));\n"
    "    col = pow(max(col,vec3(0.0)),vec3(1.0/2.2));\n"
    "    out_color = vec4(clamp(col,0.0,1.0),pc.opacity);\n"
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
            .id            = "com.sol.bfx.lava",
            .display_name  = "Lava",
            .fragment_glsl = k_lava_frag,
            .animation_fps = 30u,
        },
        {
            .id            = "com.sol.bfx.nebula",
            .display_name  = "Nebula",
            .fragment_glsl = k_nebula_frag,
            .animation_fps = 30u,
        },
        {
            .id            = "com.sol.bfx.wave",
            .display_name  = "Wave",
            .fragment_glsl = k_wave_frag,
            .animation_fps = 30u,
        },
        {
            .id            = "com.sol.bfx.aurora",
            .display_name  = "Aurora",
            .fragment_glsl = k_aurora_frag,
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

    static const char *effect_ids[] = {
        "com.sol.bfx.lava",
        "com.sol.bfx.nebula",
        "com.sol.bfx.wave",
        "com.sol.bfx.aurora",
    };
    for (size_t i = 0; i < sizeof(effect_ids) / sizeof(effect_ids[0]); ++i)
        sol_bg_effect_unregister(reg, effect_ids[i]);
}

/* ------------------------------------------------------------------ */
/* Plugin entry point                                                   */
/* ------------------------------------------------------------------ */

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
