// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-bfx — Built-in animated background shader effects. */

#include "sol_bg_effect.h"
#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

/* ------------------------------------------------------------------ */
/* Lava                                                                 */
/* ------------------------------------------------------------------ */

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
    "    vec2 q  = vec2(fbm(uv + vec2(0.0,0.0) + t*0.3),\n"
    "                   fbm(uv + vec2(5.2,1.3) + t*0.25));\n"
    "    vec2 r  = vec2(fbm(uv + 3.0*q + vec2(1.7,9.2) + t*0.15),\n"
    "                   fbm(uv + 3.0*q + vec2(8.3,2.8) - t*0.12));\n"
    "    float f = fbm(uv + 2.8*r + t*0.08);\n"
    "    vec3 accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float sat = length(accent - vec3(dot(accent, vec3(0.333))));\n"
    "    vec3 hot_col = sat > 0.08 ? accent : vec3(1.0, 0.45, 0.04);\n"
    "    vec3 dark  = hot_col * 0.045;\n"
    "    vec3 warm  = hot_col * 0.35;\n"
    "    vec3 hot   = hot_col * 1.05;\n"
    "    vec3 white = mix(hot_col, vec3(1.0, 0.96, 0.88), 0.72);\n"
    "    float fi = clamp((f - 0.24) * 1.55, 0.0, 1.0);\n"
    "    fi = smoothstep(0.0, 1.0, fi);\n"
    "    vec3 col;\n"
    "    if(fi < 0.33)\n"
    "        col = mix(dark, warm, fi / 0.33);\n"
    "    else if(fi < 0.66)\n"
    "        col = mix(warm, hot, (fi - 0.33) / 0.33);\n"
    "    else\n"
    "        col = mix(hot, white, (fi - 0.66) / 0.34);\n"
    "    float grad = length(vec2(dFdx(f), dFdy(f))) * 60.0;\n"
    "    float fissure = smoothstep(0.65, 1.15, grad);\n"
    "    col += white * fissure * 0.45;\n"
    "    col = pow(clamp(col, 0.0, 1.0), vec3(0.82));\n"
    "    out_color = vec4(col, pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Wave — deep ocean viewed from above, complex Gerstner + caustics    */
/* ------------------------------------------------------------------ */

static const char k_wave_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time; float width; float height; float opacity;\n"
    "    float r; float g; float b; float _pad;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"

    "float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p);\n"
    "    vec2 u=f*f*(3.0-2.0*f);\n"
    "    return mix(mix(hash(i),hash(i+vec2(1,0)),u.x),\n"
    "               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"

    /* Gerstner wave: returns height + horizontal displacement gradient */
    "vec3 gerstner(vec2 p, vec2 dir, float wavelength, float steepness, float t){\n"
    "    float k = 6.28318 / wavelength;\n"
    "    float c = sqrt(9.8 / k);\n"
    "    float f = k * (dot(dir, p) - c * t);\n"
    "    float a = steepness / k;\n"
    /* returns (dheight/dx, dheight/dy, height) */
    "    return vec3(-dir * k * a * sin(f), a * cos(f));\n"
    "}\n"

    /* accumulate several Gerstner waves, return surface normal and height */
    "vec4 ocean(vec2 p, float t){\n"
    "    vec3 n = vec3(0.0);\n"
    "    n += gerstner(p, normalize(vec2( 1.0, 0.4)), 1.80, 0.28, t);\n"
    "    n += gerstner(p, normalize(vec2(-0.6, 1.0)), 1.20, 0.22, t * 0.91);\n"
    "    n += gerstner(p, normalize(vec2( 0.3,-1.0)), 0.80, 0.18, t * 1.07);\n"
    "    n += gerstner(p, normalize(vec2( 1.0, 1.0)), 0.55, 0.14, t * 1.23);\n"
    "    n += gerstner(p, normalize(vec2(-1.0, 0.3)), 0.38, 0.10, t * 0.85);\n"
    "    n += gerstner(p, normalize(vec2( 0.7,-0.7)), 0.28, 0.07, t * 1.40);\n"
    "    n += gerstner(p, normalize(vec2(-0.4, 0.9)), 0.22, 0.06, t * 1.55);\n"
    "    n += gerstner(p, normalize(vec2( 1.0,-0.2)), 0.18, 0.05, t * 1.80);\n"
    /* surface normal from displacement gradients: N = normalize((-dx,-dy,1)) */
    "    vec3 normal = normalize(vec3(-n.x, -n.y, 1.0));\n"
    "    return vec4(normal, n.z);\n"
    "}\n"

    "void main() {\n"
    "    float t   = pc.time * 0.55;\n"
    "    float ar  = pc.width / pc.height;\n"
    "    vec2  uv  = (v_uv - 0.5) * vec2(ar, 1.0) * 3.5;\n"

    "    vec3 accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float lum   = dot(accent, vec3(0.299,0.587,0.114));\n"
    "    float sat   = length(accent - vec3(lum));\n"
    "    vec3 deep    = sat > 0.07 ? accent * 0.10 : vec3(0.004, 0.030, 0.090);\n"
    "    vec3 mid     = sat > 0.07 ? accent * 0.30 : vec3(0.010, 0.095, 0.220);\n"
    "    vec3 crest   = sat > 0.07 ? mix(accent,vec3(1.0),0.65) : vec3(0.38, 0.75, 0.98);\n"
    "    vec3 foam_c  = vec3(0.88, 0.95, 1.00);\n"

    /* evaluate ocean surface */
    "    vec4  surf  = ocean(uv, t);\n"
    "    vec3  N     = surf.xyz;\n"
    "    float height= surf.w;\n"

    /* sun direction — high and slightly to the side */
    "    vec3 L = normalize(vec3(0.4, -0.8, 1.0));\n"
    "    vec3 V = vec3(0.0, 0.0, 1.0);\n"
    "    vec3 H = normalize(L + V);\n"

    /* diffuse + specular from surface normal */
    "    float diff  = max(0.0, dot(N, L)) * 0.7 + 0.3;\n"
    "    float spec  = pow(max(0.0, dot(N, H)), 180.0) * 2.5;\n"
    "    float spec2 = pow(max(0.0, dot(N, H)),  30.0) * 0.4;\n"

    /* subsurface scatter approximation: brighter where wave is thin/cresting */
    "    float sss   = pow(max(0.0, 1.0 - height * 0.6), 3.0) * 0.55;\n"

    /* depth tint by height */
    "    float h01   = height * 0.5 + 0.5;\n"
    "    vec3  water = mix(deep, mid, smoothstep(0.0, 0.5, h01));\n"
    "    water       = mix(water, crest, smoothstep(0.55, 0.9, h01));\n"

    "    vec3 col = water * diff;\n"
    "    col += crest * sss;\n"
    "    col += vec3(1.0, 0.97, 0.90) * spec;\n"
    "    col += crest * spec2;\n"

    /* caustics: second-derivative sharpness of wave height = bright lines */
    "    vec4  surf2 = ocean(uv + vec2(0.012, 0.0), t);\n"
    "    vec4  surf3 = ocean(uv - vec2(0.012, 0.0), t);\n"
    "    vec4  surf4 = ocean(uv + vec2(0.0, 0.012), t);\n"
    "    vec4  surf5 = ocean(uv - vec2(0.0, 0.012), t);\n"
    "    float laplacian = abs(surf2.w + surf3.w + surf4.w + surf5.w - 4.0*height);\n"
    "    float caustic   = pow(max(0.0, 1.0 - laplacian * 22.0), 6.0);\n"
    "    col += crest * caustic * 0.7;\n"

    /* foam on sharp crests */
    "    float foam = smoothstep(0.72, 0.95, h01);\n"
    "    foam      *= 0.5 + 0.5 * noise(uv * 6.0 + vec2(t * 0.4, 0.0));\n"
    "    col        = mix(col, foam_c * diff, foam * 0.55);\n"

    /* horizon vignette */
    "    float vig = 1.0 - smoothstep(0.5, 1.5, length(v_uv - 0.5) * 2.0);\n"
    "    col *= 0.5 + 0.5 * vig;\n"

    "    col = col / (col + vec3(0.22));\n"
    "    col = pow(max(col, vec3(0.0)), vec3(0.80));\n"
    "    out_color = vec4(clamp(col, 0.0, 1.0), pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Nebula — deep space gas cloud with star particles                   */
/* ------------------------------------------------------------------ */

static const char k_nebula_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time; float width; float height; float opacity;\n"
    "    float r; float g; float b; float _pad;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"

    "vec2 hash2(vec2 p){\n"
    "    p=vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3)));\n"
    "    return fract(sin(p)*43758.5);\n"
    "}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    vec2 a=hash2(i),b=hash2(i+vec2(1,0)),c=hash2(i+vec2(0,1)),d=hash2(i+vec2(1,1));\n"
    "    return mix(mix(a.x,b.x,u.x),mix(c.x,d.x,u.x),u.y);\n"
    "}\n"
    "float fbm(vec2 p, int oct){\n"
    "    float v=0.0,a=0.5; mat2 m=mat2(1.6,-1.2,1.2,1.6);\n"
    "    for(int i=0;i<oct;i++){v+=a*noise(p);p=m*p;a*=0.5;}\n"
    "    return v;\n"
    "}\n"

    "void main() {\n"
    "    float t  = pc.time * 0.06;\n"
    "    float ar = pc.width / pc.height;\n"
    "    vec2  uv = (v_uv - 0.5) * vec2(ar, 1.0);\n"

    /* derive two nebula hues from accent */
    "    vec3 accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float lum   = dot(accent, vec3(0.299, 0.587, 0.114));\n"
    "    float sat   = length(accent - vec3(lum));\n"
    "    vec3 col1   = sat > 0.07 ? accent : vec3(0.55, 0.08, 0.9);\n"
    "    vec3 col2   = vec3(col1.b, col1.r * 0.6, col1.g + 0.3);\n"
    "    col2        = clamp(col2, 0.0, 1.0);\n"
    "    vec3 col3   = mix(col1, vec3(0.1, 0.5, 1.0), 0.4);\n"

    /* domain warp for the main gas cloud */
    "    vec2 q = vec2(fbm(uv * 1.8 + vec2(0.0, t * 0.4), 5),\n"
    "                  fbm(uv * 1.8 + vec2(5.2, t * 0.35), 5));\n"
    "    vec2 rr= vec2(fbm(uv * 1.5 + 4.0*q + vec2(1.7, 9.2) + t*0.2, 5),\n"
    "                  fbm(uv * 1.5 + 4.0*q + vec2(8.3, 2.8) - t*0.18, 5));\n"
    "    float f = fbm(uv * 1.2 + 3.5*rr + t*0.1, 6);\n"

    /* falloff from centre — nebula is a blob */
    "    float dist  = length(uv * vec2(0.8, 1.0));\n"
    "    float veil  = exp(-dist * dist * 2.2) * 1.6;\n"

    /* cloud density */
    "    float dens  = clamp((f - 0.28) * 2.2, 0.0, 1.0) * veil;\n"

    /* emission: bright regions glow hot-white toward their hue */
    "    vec3 nebcol = mix(col1 * 0.15, col2 * 0.6, smoothstep(0.2, 0.7, f));\n"
    "    nebcol      = mix(nebcol, col3 * 1.2, smoothstep(0.6, 0.95, f));\n"
    "    nebcol     += col1 * 0.4 * smoothstep(0.8, 1.0, f);\n"

    /* dark void background with faint blue haze */
    "    vec3 bg     = vec3(0.003, 0.005, 0.012) + col1 * 0.02;\n"
    "    vec3 col    = mix(bg, nebcol, dens);\n"

    /* dust lanes: darker streaks across the cloud */
    "    float lane  = fbm(uv * 4.0 + vec2(t * 0.3, 0.0), 3);\n"
    "    float dark  = smoothstep(0.52, 0.48, lane) * dens * 0.6;\n"
    "    col         = mix(col, bg * 0.3, dark);\n"

    /* scattered stars: 3 layers of density */
    "    for(int li = 0; li < 3; li++) {\n"
    "        float scale = 60.0 + float(li) * 90.0;\n"
    "        vec2  sg    = floor(uv * scale + vec2(float(li) * 33.7));\n"
    "        vec2  hh    = hash2(sg);\n"
    "        float bright= step(0.93 + float(li)*0.025, hh.x);\n"
    "        float twink = 0.55 + 0.45*sin(t*(4.0+hh.y*6.0)*30.0 + hh.y*6.28);\n"
    "        float size  = (0.6 + 0.4*hh.y) / (scale * 0.8);\n"
    "        vec2  sc    = fract(uv * scale + vec2(float(li)*33.7)) - 0.5;\n"
    "        float sdist = length(sc);\n"
    "        float star  = bright * twink * exp(-sdist*sdist/(size*size)*8.0);\n"
    "        vec3  scol  = mix(vec3(0.7, 0.85, 1.0), vec3(1.0, 0.9, 0.6), hh.y);\n"
    "        col        += scol * star * (0.8 - dens * 0.7);\n"
    "    }\n"

    /* glow halo at nebula core */
    "    float core  = exp(-dist * dist * 8.0) * 0.5;\n"
    "    col        += col1 * core;\n"

    "    col  = col / (col + vec3(0.22));\n"
    "    col  = pow(max(col, vec3(0.0)), vec3(0.78));\n"
    "    out_color = vec4(clamp(col, 0.0, 1.0), pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Embers — glowing particles drifting upward                          */
/* ------------------------------------------------------------------ */

static const char k_embers_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time; float width; float height; float opacity;\n"
    "    float r; float g; float b; float _pad;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"

    "vec2 hash2(vec2 p){\n"
    "    p = vec2(dot(p,vec2(127.1,311.7)), dot(p,vec2(269.5,183.3)));\n"
    "    return fract(sin(p)*43758.5);\n"
    "}\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    float a=fract(sin(dot(i,          vec2(127.1,311.7)))*43758.5);\n"
    "    float b=fract(sin(dot(i+vec2(1,0),vec2(127.1,311.7)))*43758.5);\n"
    "    float c=fract(sin(dot(i+vec2(0,1),vec2(127.1,311.7)))*43758.5);\n"
    "    float d=fract(sin(dot(i+vec2(1,1),vec2(127.1,311.7)))*43758.5);\n"
    "    return mix(mix(a,b,u.x),mix(c,d,u.x),u.y);\n"
    "}\n"

    "void main() {\n"
    "    float t   = pc.time * 0.55;\n"
    "    float ar  = pc.width / pc.height;\n"

    "    vec3 accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float lum   = dot(accent, vec3(0.299,0.587,0.114));\n"
    "    float sat   = length(accent - vec3(lum));\n"
    "    vec3 ember  = sat > 0.07 ? accent : vec3(1.0, 0.38, 0.04);\n"
    "    vec3 ember2 = vec3(ember.r, ember.g * 0.6, ember.b * 0.1);\n"
    "    vec3 hot    = mix(ember, vec3(1.0, 0.95, 0.7), 0.55);\n"

    /* background: deep dark, heat-hazed at the bottom */
    "    float heat  = smoothstep(1.0, 0.6, v_uv.y);\n"
    "    vec3  bg    = mix(vec3(0.01, 0.008, 0.005), ember * 0.12, heat * 0.4);\n"
    "    vec3  col   = bg;\n"

    /* 5 layers of cells, each layer at different scale and speed */
    "    for(int li = 0; li < 5; li++) {\n"
    "        float fl    = float(li);\n"
    "        float scale = 6.0 + fl * 5.0;\n"
    "        float spd   = 0.3 + fl * 0.15;\n"
    "        float drift = fl * 0.17;\n"

    /* tile particle space */
    "        vec2  pv   = vec2(v_uv.x * ar, v_uv.y) * scale;\n"
    "        pv.y      -= t * spd;\n"
    "        pv.x      += sin(t * 0.2 + fl * 1.3) * drift;\n"
    "        vec2  cell = floor(pv);\n"
    "        vec2  fc   = fract(pv) - 0.5;\n"

    /* per-cell random: position offset, size, brightness, color mix */
    "        vec2  rnd  = hash2(cell + fl * 7.3);\n"
    "        vec2  rnd2 = hash2(cell + fl * 13.7 + vec2(3.1, 7.9));\n"
    "        vec2  off  = (rnd - 0.5) * 0.55;\n"
    "        float sz   = 0.04 + rnd2.x * 0.07;\n"
    "        float bri  = 0.3 + rnd2.y * 0.7;\n"
    /* embers appear and fade over their lifetime */
    "        float life  = fract(rnd.x * 3.7 + t * spd * 0.3);\n"
    "        float fade  = sin(life * 3.14159) * smoothstep(0.0, 0.08, life);\n"
    /* slight horizontal wobble */
    "        float wobble = sin(t * (2.0 + rnd.y * 3.0) + rnd.x * 6.28) * sz * 0.6;\n"
    "        vec2  p     = fc - off - vec2(wobble, 0.0);\n"
    /* soft circular ember */
    "        float d     = length(p);\n"
    "        float glow  = exp(-d * d / (sz * sz) * 3.5);\n"
    "        float core  = exp(-d * d / (sz * sz * 0.08));\n"
    /* color: hot core, ember glow */
    "        vec3  ecol  = mix(ember2, ember, glow) * glow\n"
    "                    + hot * core;\n"
    "        col        += ecol * bri * fade * (0.5 + 0.5 / (fl + 1.0));\n"
    "    }\n"

    /* rising heat shimmer: distort slightly at bottom */
    "    float shimmer = noise(vec2(v_uv.x * ar * 4.0 + t * 0.3, t * 1.5)) * 0.008;\n"
    "    col          += ember * shimmer * heat * 2.0;\n"

    "    col = col / (col + vec3(0.3));\n"
    "    col = pow(max(col, vec3(0.0)), vec3(0.80));\n"
    "    out_color = vec4(clamp(col, 0.0, 1.0), pc.opacity);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Black Hole — Schwarzschild geodesic raymarching + accretion disk    */
/* ------------------------------------------------------------------ */

static const char k_blackhole_frag[] =
    "#version 450\n"
    "layout(push_constant) uniform PC {\n"
    "    float time; float width; float height; float opacity;\n"
    "    float r; float g; float b; float _pad;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"

    "#define PI 3.14159265359\n"

    "float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }\n"
    "float noise(vec2 p){\n"
    "    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);\n"
    "    return mix(mix(hash(i),hash(i+vec2(1,0)),u.x),\n"
    "               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),u.x),u.y);\n"
    "}\n"
    "float fbm(vec2 p){\n"
    "    float v=0.0,a=0.5;\n"
    "    for(int i=0;i<5;i++,p=p*2.1+vec2(1.7,2.3),a*=0.5) v+=a*noise(p);\n"
    "    return v;\n"
    "}\n"

    "vec3 starfield(vec2 uv){\n"
    "    vec3 col=vec3(0.0);\n"
    "    for(int i=0;i<3;i++){\n"
    "        float fi=float(i); float sc=55.0+fi*38.0;\n"
    "        vec2 cell=floor(uv*sc), fc=fract(uv*sc)-0.5;\n"
    "        float r1=hash(cell+fi*7.3), r2=hash(cell+fi*17.1+vec2(3.3,6.1));\n"
    "        col+=mix(vec3(1.0,0.85,0.6),vec3(0.75,0.88,1.0),r1)\n"
    "            *pow(r2,5.5)*exp(-dot(fc,fc)/((0.003+r1*0.009)*(0.003+r1*0.009)));\n"
    "    }\n"
    "    return col;\n"
    "}\n"

    /* Disk emission: r in BH units (RS=1), phi = azimuth in disk plane */
    "vec3 disk_color(float r, float phi, float t){\n"
    "    float rn = clamp((r - 3.0) / 9.0, 0.0, 1.0);\n"   /* 0 at ISCO, 1 at outer */
    "    vec3 hot = vec3(1.00, 0.95, 0.80);\n"
    "    vec3 mid = vec3(1.00, 0.46, 0.05);\n"
    "    vec3 dim = vec3(0.28, 0.02, 0.00);\n"
    "    vec3 col = mix(hot, mid, pow(rn, 0.5));\n"
    "    col       = mix(col, dim, pow(rn, 2.0));\n"
    /* brightness: 1/r^2 falloff peaked near ISCO */
    "    float bri = pow(1.0 - rn, 1.6) / (r * r * 0.007 + 0.002);\n"
    /* Doppler beaming: orbital velocity v_kep = 1/sqrt(r), beaming^3 */
    "    float v   = 0.42 / sqrt(max(r, 3.0));\n"
    "    float dop = pow(max(0.0, 1.0 + v * cos(phi)), 3.0);\n"
    /* turbulent structure orbiting with time */
    "    float trb = 0.45 + 0.55 * noise(vec2(r * 0.55 - t * 1.4, phi * 1.6))\n"
    "              * (0.6  + 0.4  * noise(vec2(r * 1.9  + t * 0.8, phi * 4.2 + 1.1)));\n"
    "    return col * bri * dop * trb;\n"
    "}\n"

    "void main(){\n"
    "    float t  = pc.time * 0.32;\n"
    "    float ar = pc.width / pc.height;\n"
    "    vec2  scr = (v_uv - 0.5) * vec2(ar, 1.0) * 3.2;\n"

    "    vec3  accent = vec3(pc.r, pc.g, pc.b);\n"
    "    float alum   = dot(accent, vec3(0.299,0.587,0.114));\n"
    "    float asat   = length(accent - vec3(alum));\n"

    /* ----------------------------------------------------------------
     * Camera: nearly edge-on, ~5° above disk plane.
     * Camera sits at (0, sin(elev), -cos(elev)) * CAM_DIST.
     * We build orthonormal basis and shoot a ray per pixel.
     * -------------------------------------------------------------- */
    "    const float ELEV = 0.08;\n"   /* ~4.6 degrees above disk plane */
    "    float ce = cos(ELEV), se = sin(ELEV);\n"
    /* Camera position (normalised direction from BH) */
    "    vec3 cam_pos = vec3(0.0, se, ce);\n"   /* above and in front */
    /* Forward = toward BH */
    "    vec3 fwd = -cam_pos;\n"
    "    vec3 rgt = vec3(1.0, 0.0, 0.0);\n"
    "    vec3 up  = cross(rgt, fwd);\n"       /* true up for this camera */
    /* Ray direction: FOV factor 1.6 */
    "    vec3 rd = normalize(scr.x * rgt + scr.y * up + 1.6 * fwd);\n"

    /* ----------------------------------------------------------------
     * Schwarzschild geodesic in 3-D.
     * We march the ray in Cartesian space using the geodesic force:
     *   d²x/dλ² = -( (3/2) rs / r^4 ) * (x · ẋ_perp) terms
     * Simplification: use the effective radial geodesic equation with
     * a 3-D force pointing toward the BH:
     *   accel = -1.5 * rs / r^3 * (v - (v·r̂)r̂)  [perpendicular]
     *         + Newtonian-like radial for zeroth order
     * Practical: the standard Verlet/leapfrog approximation used in
     * most real-time BH shaders:
     *   accel = -1.5 * rs * pos / r^3   (isotropic, matches geodesic near BH)
     * -------------------------------------------------------------- */
    "    const float RS    = 1.0;\n"
    "    const float R_CAP = 2.6;\n"    /* photon capture radius (sqrt(27)*RS/2) */
    "    const float R_MAX = 22.0;\n"
    "    const float SCALE = 14.0;\n"   /* 1 screen unit = SCALE BH units */
    "    const int   STEPS = 100;\n"
    "    const float DL    = 0.3;\n"    /* step size in BH units */

    /* Start ray at camera distance */
    "    vec3  pos = cam_pos * R_MAX * 0.7;\n"
    "    vec3  vel = rd * SCALE;\n"    /* scale velocity to BH coordinate units */

    "    vec3  hdr      = vec3(0.0);\n"
    "    float transmit = 1.0;\n"
    "    bool  captured = false;\n"
    "    float disk_spin = t * 0.20;\n"

    /* Track sign of pos.y to detect disk plane crossings */
    "    float prev_y = pos.y;\n"

    "    for(int i = 0; i < STEPS; i++){\n"
    "        float r2 = dot(pos, pos);\n"
    "        float r  = sqrt(r2);\n"

    "        if(r < R_CAP * 0.5){ captured = true; break; }\n"
    "        if(r > R_MAX) break;\n"

    /* Geodesic acceleration: -1.5 * RS / r^3 * pos  (isotropic GR approx) */
    "        vec3 acc = -1.5 * RS / (r2 * r) * pos;\n"

    /* Leapfrog step */
    "        vel += acc * DL;\n"
    "        vec3 new_pos = pos + vel * DL;\n"

    /* ---- Disk plane crossing: pos.y changes sign --------------- */
    "        if(prev_y * new_pos.y < 0.0 && transmit > 0.02){\n"
    /* Interpolate to find exact crossing point */
    "            float frac = prev_y / (prev_y - new_pos.y);\n"
    "            vec3  cross_p = pos + frac * vel * DL;\n"
    "            float cr = length(cross_p);\n"
    "            if(cr >= 3.0 * RS && cr <= 12.0 * RS){\n"
    "                float phi = atan(cross_p.z, cross_p.x) + disk_spin;\n"
    "                vec3 de = disk_color(cr, phi, t);\n"
    "                hdr     += de * transmit;\n"
    "                transmit *= 0.50;\n"
    "            }\n"
    "        }\n"

    "        prev_y = new_pos.y;\n"
    "        pos    = new_pos;\n"
    "    }\n"

    /* ---- Background (lensed stars) ----------------------------- */
    "    float scr_r = length(scr);\n"
    "    float lens  = 1.0 + clamp(0.12 / (scr_r * scr_r + 0.02), 0.0, 3.0);\n"
    "    vec2  bg_uv = scr * lens * 0.22;\n"
    "    vec3  bg    = starfield(bg_uv + vec2(t * 0.001, 0.0));\n"
    "    float gb    = exp(-bg_uv.y * bg_uv.y * 10.0)\n"
    "                * (0.5 + 0.5 * fbm(bg_uv * 2.0 + vec2(t * 0.003, 0.0))) * 0.08;\n"
    "    bg += mix(vec3(0.5,0.65,0.9), vec3(0.9,0.8,0.6), fbm(bg_uv * 2.5)) * gb;\n"

    "    vec3 col = captured ? vec3(0.0) : bg;\n"
    "    col += hdr;\n"

    /* Accent tint */
    "    if(asat > 0.12)\n"
    "        col = mix(col, col * normalize(accent + 0.3) * 1.4, asat * 0.35);\n"

    /* ---- Photon ring ------------------------------------------- */
    "    float pr_r   = R_CAP / SCALE;\n"
    "    float pr_dist = abs(scr_r - pr_r);\n"
    "    float pr     = exp(-pr_dist * pr_dist / (pr_r * pr_r * 0.0015)) * 4.5;\n"
    "    pr           *= smoothstep(pr_r * 0.85, pr_r * 1.15, scr_r);\n"
    "    float eq_fac  = 1.0 - abs(scr.y) / (scr_r + 0.001);\n"
    "    vec3  pr_col  = mix(vec3(0.85,0.92,1.0), vec3(1.0,0.72,0.18), eq_fac);\n"
    "    col += pr_col * pr;\n"

    /* ---- Shadow ------------------------------------------------ */
    "    float sh_r  = R_CAP / SCALE;\n"
    "    col = mix(col, vec3(0.0), smoothstep(sh_r * 1.05, sh_r * 0.88, scr_r));\n"
    "    if(captured) col = vec3(0.0);\n"

    /* ---- Tone map ---------------------------------------------- */
    "    col  = col * 10.0;\n"
    "    col  = pow(max(col, vec3(0.0)), vec3(1.5));\n"
    "    col  = col / (1.0 + col);\n"
    "    col  = pow(col, vec3(1.0/1.5));\n"
    "    col  = mix(col, col*col*(3.0-2.0*col), vec3(1.0));\n"
    "    col  = pow(col, vec3(1.3, 1.2, 1.0));\n"
    "    col  = clamp(col * 1.01, 0.0, 1.0);\n"
    "    col  = pow(col, vec3(0.7/2.2));\n"
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
            .id            = "com.sol.bfx.embers",
            .display_name  = "Embers",
            .fragment_glsl = k_embers_frag,
            .animation_fps = 30u,
        },
        {
            .id            = "com.sol.bfx.blackhole",
            .display_name  = "Black Hole",
            .fragment_glsl = k_blackhole_frag,
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
        "com.sol.bfx.embers",
        "com.sol.bfx.blackhole",
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
