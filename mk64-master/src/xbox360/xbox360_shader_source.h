#ifndef X360_SHADER_SOURCE_H
#define X360_SHADER_SOURCE_H
#include <string>
#include "xbox360/gfx_cc.h"
static std::string x360_operand(unsigned n,bool alpha,int cycle){
    const char *value=0;bool scalar=false;
    switch(n){
    case CC_0:return alpha?"0.0":"float3(0,0,0)";
    case CC_ONE:return alpha?"1.0":"float3(1,1,1)";
    case CC_TEXEL0:case CC_TEXEL0A:value=cycle?"t1":"t0";scalar=n==CC_TEXEL0A;break;
    case CC_TEXEL1:case CC_TEXEL1A:value=cycle?"t0":"t1";scalar=n==CC_TEXEL1A;break;
    case CC_PRIM:case CC_PRIMA:value="v.c0";scalar=n==CC_PRIMA;break;
    case CC_SHADE:case CC_SHADEA:value="v.c1";scalar=n==CC_SHADEA;break;
    case CC_ENV:case CC_ENVA:value="v.c2";scalar=n==CC_ENVA;break;
    case CC_COMBINED:case CC_COMBINEDA:value="combined";scalar=n==CC_COMBINEDA;break;
    case CC_LOD:return alpha?"v.c3.r":"v.c3.rrr";
    case CC_PRIMLOD:return alpha?"v.c3.g":"v.c3.ggg";
    default:return alpha?"0.0":"float3(0,0,0)";
    }
    return std::string(value)+(alpha?".a":scalar?".aaa":".rgb");
}
static std::string x360_formula(const CCFeatures&f,int channel,int cycle){
    const uint8_t *c=cycle?f.c2[channel]:f.c[channel];
    return "("+x360_operand(c[0],channel!=0,cycle)+"-"+x360_operand(c[1],channel!=0,cycle)+")*"+x360_operand(c[2],channel!=0,cycle)+"+"+x360_operand(c[3],channel!=0,cycle);
}
static std::string x360_shader_source(const CCFeatures&f){
    std::string s="struct V {float4 p:POSITION;";
    if(f.used_textures[0])s+="float2 uv0:TEXCOORD0;";
    if(f.used_textures[1])s+="float2 uv1:TEXCOORD1;";
    if(f.opt_fog)s+="float4 fog:TEXCOORD2;";
    for(int i=0;i<f.num_inputs;++i){char n='0'+i,sem='3'+i;s+=std::string("float4 c")+n+":TEXCOORD"+sem+";";}
    s+="}; V VS(V v){return v;} sampler2D s0:register(s0);sampler2D s1:register(s1);\nfloat4 PS(V v";
    if(f.opt_noise&&f.opt_alpha)s+=",float2 screen:VPOS";
    s+="):COLOR0 {float4 t0=float4(1,1,1,1),t1=t0,combined=0;";
    if(f.used_textures[0])s+="t0=tex2D(s0,v.uv0);";
    if(f.used_textures[1])s+="t1=tex2D(s1,v.uv1);";
    s+="combined=float4("+x360_formula(f,0,0)+","+x360_formula(f,1,0)+");";
    s+="float compare_alpha=combined.a;";
    if(f.two_cycle)s+="combined=float4("+x360_formula(f,0,1)+","+x360_formula(f,1,1)+");";
    s+="float4 result=combined;";
    if(!f.opt_alpha)s+="result.a=1.0;";
    if(f.opt_texture_edge){s+="clip(compare_alpha-0.3);";if(!f.opt_preserve_alpha)s+="result.a=1.0;";}
    if(f.opt_noise&&f.opt_alpha){s+="clip(compare_alpha-frac(sin(dot(screen,float2(12.9898,78.233)))*43758.5453));";if(!f.opt_preserve_alpha)s+="result.a=1.0;";}
    if(f.opt_fog)s+="result.rgb=lerp(result.rgb,v.fog.rgb,saturate(v.fog.a));";
    return s+"return saturate(result);}\n";
}
#endif
