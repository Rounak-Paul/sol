// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* LocalDocsMD-inspired animated backgrounds for Sol. */

#include "sol_bg_effect.h"
#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

#define BFX_HEADER \
    "#version 450\n" \
    "layout(push_constant) uniform PC {\n" \
    " float time,width,height,opacity; vec3 primary; float _pad0; vec3 accent; float _pad1;\n" \
    "} pc;\n" \
    "layout(location=0) in vec2 v_uv;\n" \
    "layout(location=0) out vec4 out_color;\n" \
    "float h11(float p){return fract(sin(p*127.1)*43758.5453);}\n" \
    "float h21(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}\n" \
    "vec2 h22(vec2 p){return fract(sin(vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3))))*43758.5453);}\n" \
    "vec3 primary(){return pow(max(pc.primary,vec3(0.0)),vec3(2.2));}\n" \
    "vec3 accent(){return pow(max(pc.accent,vec3(0.0)),vec3(2.2));}\n" \
    "vec4 finish(vec3 c,float a){return vec4(max(c,vec3(0.0)),clamp(a*pc.opacity,0.0,1.0));}\n"

#define BFX_NOISE \
    "float noise(vec2 p){vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);" \
    "return mix(mix(h21(i),h21(i+vec2(1,0)),u.x),mix(h21(i+vec2(0,1)),h21(i+vec2(1,1)),u.x),u.y);}\n" \
    "float fbm(vec2 p){float v=0.0,a=0.5;for(int i=0;i<5;i++){v+=a*noise(p);p=p*2.03+vec2(1.7,2.9);a*=0.5;}return v;}\n"

static const char k_particles_frag[] = BFX_HEADER
    "vec2 point(vec2 cell){vec2 r=h22(cell);return cell+r+0.18*sin(vec2(1.1,1.4)*pc.time*0.18+r*6.283);}\n"
    "float segment(vec2 p,vec2 a,vec2 b){vec2 pa=p-a,ba=b-a;float t=clamp(dot(pa,ba)/dot(ba,ba),0.0,1.0);return length(pa-ba*t);}\n"
    "void main(){\n"
    " vec2 scale=vec2(pc.width,pc.height)/140.0;vec2 p=v_uv*scale;vec2 base=floor(p);\n"
    " float dots=0.0,links=0.0;\n"
    " for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){vec2 c=base+vec2(x,y);vec2 a=point(c);\n"
    "  dots=max(dots,1.0-smoothstep(0.025,0.075,length(p-a)));\n"
    "  vec2 b=point(c+vec2(1,0));float d=segment(p,a,b);float fade=1.0-smoothstep(0.65,1.35,length(a-b));links=max(links,(1.0-smoothstep(0.006,0.018,d))*fade);\n"
    "  b=point(c+vec2(0,1));d=segment(p,a,b);fade=1.0-smoothstep(0.65,1.35,length(a-b));links=max(links,(1.0-smoothstep(0.006,0.018,d))*fade);\n"
    " }\n"
    " vec3 col=primary()*(dots*1.15+links*0.40)+accent()*dots*0.30;out_color=finish(col,dots*0.90+links*0.34);\n"
    "}\n";

static const char k_waves_frag[] = BFX_HEADER
    "void main(){\n"
    " vec2 uv=v_uv;float t=pc.time;vec3 col=vec3(0.0);float alpha=0.0;\n"
    " for(int i=0;i<4;i++){float fi=float(i);float center=0.48+fi*0.12;float amp=0.035-fi*0.003;\n"
    "  float wave=center+sin(uv.x*(7.0+fi*3.1)+t*(0.18+fi*0.035))*amp+sin(uv.x*(13.0+fi*2.0)-t*0.11)*amp*0.42;\n"
    "  float fill=smoothstep(wave-0.035,wave+0.01,uv.y);float edge=exp(-abs(uv.y-wave)*75.0);\n"
    "  vec3 hue=(i==1||i==3)?accent():primary();float strength=(0.14-fi*0.018)*fill+0.20*edge;col+=hue*strength;alpha=max(alpha,fill*(0.28-fi*0.025)+edge*0.22);\n"
    " }\n"
    " out_color=finish(col,alpha);\n"
    "}\n";

static const char k_matrix_frag[] = BFX_HEADER
    "void main(){\n"
    " vec2 cells=vec2(pc.width,pc.height)/16.0;vec2 g=v_uv*cells;vec2 id=floor(g),f=fract(g);\n"
    " float rows=cells.y;float seed=h11(id.x+17.0);float speed=2.2+seed*2.4;float head=mod(pc.time*speed+seed*rows*1.7,rows+28.0)-10.0;\n"
    " float behind=head-id.y;behind=behind<0.0?behind+rows+28.0:behind;float trail=(1.0-smoothstep(0.0,22.0,behind))*step(0.0,behind);\n"
    " vec2 pix=floor(f*vec2(5.0,7.0));float glyph=step(0.53,h21(id*19.7+pix))*step(0.10,f.x)*step(f.x,0.90)*step(0.08,f.y)*step(f.y,0.92);\n"
    " float headGlow=exp(-behind*behind*1.8);vec3 col=mix(primary()*0.82,vec3(0.82,1.0,0.88),headGlow)*glyph*(trail+headGlow);\n"
    " out_color=finish(col,glyph*clamp(trail*0.84+headGlow,0.0,0.96));\n"
    "}\n";

static const char k_aurora_frag[] = BFX_HEADER BFX_NOISE
    "void main(){\n"
    " vec2 uv=v_uv;float t=pc.time*0.07;float n=fbm(uv*vec2(2.5,1.2)+vec2(t*0.4,t*0.2));\n"
    " float n2=fbm(uv*vec2(1.8,2.0)+vec2(-t*0.3,t*0.5)+3.7);\n"
    " float b1=smoothstep(0.0,1.0,1.0-abs(uv.y-0.38-n*0.28)*4.0);\n"
    " float b2=smoothstep(0.0,1.0,1.0-abs(uv.y-0.62-n2*0.22)*5.0);\n"
    " vec3 col=primary()*b1*0.72+accent()*b2*0.68;out_color=finish(col,clamp(b1*0.68+b2*0.62,0.0,0.84));\n"
    "}\n";

static const char k_starfield_frag[] = BFX_HEADER
    "float segment(vec2 p,vec2 a,vec2 b){vec2 pa=p-a,ba=b-a;float t=clamp(dot(pa,ba)/max(dot(ba,ba),0.00001),0.0,1.0);return length(pa-ba*t);}\n"
    "void main(){\n"
    " float ar=pc.width/pc.height;vec2 p=(v_uv-0.5)*vec2(ar,1.0);float glow=0.0;\n"
    " for(int i=0;i<52;i++){float fi=float(i);vec2 seed=h22(vec2(fi,fi*3.17))*2.0-1.0;seed.x*=ar;\n"
    "  float z=0.08+0.92*fract(h11(fi*7.3)-pc.time*(0.028+h11(fi)*0.018));\n"
    "  vec2 pos=seed*(0.14/z);vec2 prev=seed*(0.14/min(z+0.025,1.0));float d=segment(p,pos,prev);\n"
    "  float fade=(1.0-z)*(1.0-z);glow=max(glow,(1.0-smoothstep(0.0008,0.0045,d))*fade);\n"
    " }\n"
    " vec3 col=mix(primary(),vec3(1.0),0.72)*glow;out_color=finish(col,glow*0.90);\n"
    "}\n";

static const char k_metaballs_frag[] = BFX_HEADER
    "void main(){\n"
    " float ar=pc.width/pc.height;vec2 uv=(v_uv*2.0-1.0)*vec2(ar,1.0);float t=pc.time*0.18,field=0.0;\n"
    " for(int i=0;i<7;i++){float fi=float(i),spd=0.5+fi*0.13;vec2 c=vec2(0.7*sin(t*spd+fi*2.39996),0.7*cos(t*spd*0.7+fi*1.61803));float r=0.18+0.06*sin(t*1.1+fi);field+=r*r/max(dot(uv-c,uv-c),0.0001);}\n"
    " float body=smoothstep(0.9,1.0,field);float edge=smoothstep(0.7,0.9,field)*(1.0-body);vec3 col=mix(primary(),accent(),clamp(field*0.3,0.0,1.0));\n"
    " out_color=finish(col*(body+edge*0.72),body*0.78+edge*0.42);\n"
    "}\n";

static const char k_flowfield_frag[] = BFX_HEADER
    "float segment(vec2 p,vec2 a,vec2 b){vec2 pa=p-a,ba=b-a;float t=clamp(dot(pa,ba)/max(dot(ba,ba),0.00001),0.0,1.0);return length(pa-ba*t);}\n"
    "vec2 flow(vec2 p,float t){float a=sin(p.x*5.1+t*0.37)*cos(p.y*4.3-t*0.29)+sin((p.x+p.y)*3.2+t*0.21);return vec2(cos(a*3.1),sin(a*3.1));}\n"
    "void main(){\n"
    " float ar=pc.width/pc.height;vec2 p=v_uv*vec2(ar,1.0);float glow=0.0;\n"
    " for(int i=0;i<56;i++){float fi=float(i);vec2 origin=h22(vec2(fi*2.7,fi*9.1))*vec2(ar,1.0);float phase=fract(h11(fi*4.7)+pc.time*(0.018+h11(fi)*0.014));\n"
    "  vec2 dir=flow(origin,pc.time);vec2 a=fract(origin+dir*phase*0.24);a.x*=ar;vec2 b=a-dir*0.018;float d=segment(p,a,b);glow=max(glow,(1.0-smoothstep(0.001,0.004,d))*(0.35+0.65*sin(phase*3.14159)));\n"
    " }\n"
    " vec3 col=mix(primary(),accent(),0.42)*glow;out_color=finish(col,glow*0.58);\n"
    "}\n";

static const char k_fireflies_frag[] = BFX_HEADER
    "void main(){\n"
    " float ar=pc.width/pc.height;vec2 p=v_uv*vec2(ar,1.0);vec3 col=vec3(0.0);float alpha=0.0;\n"
    " for(int i=0;i<44;i++){float fi=float(i);vec2 base=h22(vec2(fi*3.1,fi*7.9))*vec2(ar,1.0);vec2 drift=vec2(sin(pc.time*(0.09+h11(fi)*0.08)+fi),cos(pc.time*(0.07+h11(fi+5.0)*0.07)+fi*1.7))*0.045;vec2 q=base+drift;\n"
    "  float d=length(p-q);float pulse=0.55+0.45*sin(pc.time*(0.8+h11(fi)*0.9)+fi*2.3);float g=exp(-d*d*24000.0)*pulse;float halo=exp(-d*d*1800.0)*pulse;vec3 hue=mix(primary(),accent(),h11(fi*11.0));col+=hue*(g+halo*0.32);alpha=max(alpha,g*0.94+halo*0.34);\n"
    " }\n"
    " out_color=finish(col,clamp(alpha,0.0,0.92));\n"
    "}\n";

static const char k_circuit_frag[] = BFX_HEADER
    "void main(){\n"
    " vec2 grid=vec2(pc.width,pc.height)/44.0;vec2 g=v_uv*grid,id=floor(g),f=fract(g);float choice=h21(id);\n"
    " float horizontal=1.0-smoothstep(0.025,0.055,abs(f.y-0.5));float vertical=1.0-smoothstep(0.025,0.055,abs(f.x-0.5));\n"
    " float trace=choice>0.5?horizontal:vertical;float node=1.0-smoothstep(0.055,0.105,length(f-0.5));\n"
    " float phase=fract(pc.time*0.035+h21(id*3.7));float along=choice>0.5?f.x:f.y;float pulse=exp(-pow(abs(along-phase),2.0)*420.0)*trace;\n"
    " vec3 col=primary()*(trace*0.18+node*0.42)+accent()*pulse*1.15;out_color=finish(col,trace*0.23+node*0.48+pulse*0.78);\n"
    "}\n";

static const char k_voronoi_frag[] = BFX_HEADER
    "vec2 seed(int i,float t){float fi=float(i),spd=0.03+fi*0.007;return vec2(0.5+0.42*sin(t*spd+fi*2.3999),0.5+0.42*cos(t*spd*0.71+fi*1.6180));}\n"
    "void main(){\n"
    " vec2 uv=v_uv;float d1=9.0,d2=9.0;int ci=0;for(int i=0;i<14;i++){float d=distance(uv,seed(i,pc.time));if(d<d1){d2=d1;d1=d;ci=i;}else if(d<d2)d2=d;}\n"
    " float edge=1.0-smoothstep(0.0,0.012,d2-d1);float interior=smoothstep(0.0,0.18,d2-d1)*(1.0-smoothstep(0.18,0.55,d1));float hue=fract(float(ci)*0.618);\n"
    " vec3 cell=mix(primary(),accent(),hue);vec3 col=mix(cell*interior*0.55,accent(),edge);out_color=finish(col,edge*0.76+interior*0.25);\n"
    "}\n";

typedef struct BuiltinEffect {
    const char *id;
    const char *name;
    const char *shader;
    uint32_t fps;
} BuiltinEffect;

#define BFX_ANIMATION_FPS 15u

static const BuiltinEffect k_effects[] = {
    { "com.sol.bfx.particles", "Particles", k_particles_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.waves", "Waves", k_waves_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.matrix", "Matrix Rain", k_matrix_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.aurora", "Aurora", k_aurora_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.starfield", "Starfield", k_starfield_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.metaballs", "Metaballs", k_metaballs_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.flowfield", "Flow Field", k_flowfield_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.fireflies", "Fireflies", k_fireflies_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.circuit", "Circuit", k_circuit_frag, BFX_ANIMATION_FPS },
    { "com.sol.bfx.voronoi", "Voronoi", k_voronoi_frag, BFX_ANIMATION_FPS },
};

/* Register every built-in animated background. */
static bool bfx_on_load(SolPluginCtx *ctx)
{
    SolBgEffectRegistry *registry = (SolBgEffectRegistry *)
        sol_plugin_get_service(ctx, "sol.bg_effect_registry", 0);
    if (!registry) {
        sol_plugin_log(ctx, "sol.bg_effect_registry not available; skipping effect registration");
        return true;
    }
    for (size_t i = 0u; i < sizeof(k_effects) / sizeof(k_effects[0]); ++i) {
        const BuiltinEffect *effect = &k_effects[i];
        if (!sol_bg_effect_register(registry, &(SolBgEffectDesc){
                .id = effect->id,
                .display_name = effect->name,
                .fragment_glsl = effect->shader,
                .animation_fps = effect->fps,
            })) {
            sol_plugin_log(ctx, "failed to register effect '%s'", effect->id);
        }
    }
    return true;
}

/* Unregister every effect owned by this plugin. */
static void bfx_on_unload(SolPluginCtx *ctx)
{
    SolBgEffectRegistry *registry = (SolBgEffectRegistry *)
        sol_plugin_get_service(ctx, "sol.bg_effect_registry", 0);
    if (!registry) return;
    for (size_t i = 0u; i < sizeof(k_effects) / sizeof(k_effects[0]); ++i)
        sol_bg_effect_unregister(registry, k_effects[i].id);
}

/* Publish the animated-background plugin descriptor. */
bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION || !out_api) return false;
    *out_api = (SolPluginAPI){
        .api_version = SOL_PLUGIN_API_VERSION,
        .id = "com.sol.bfx",
        .display_name = "Sol Background Effects",
        .version = "2.0.0",
        .after = { NULL },
        .on_load = bfx_on_load,
        .on_unload = bfx_on_unload,
    };
    return true;
}
