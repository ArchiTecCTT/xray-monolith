// R1 thermal (r1_thermal_shadows): the projected shadows of people, mutants and objects, drawn into the scene while
// the thermal picture is shown inside a circle (r1_thermal_region). Position, colour and texture coordinate are what
// the fixed-function pass effects\shadow_world uses; "scr" is the pixel's place on the screen relative to the
// circle's centre, in screen heights, times w (divided per pixel, so it is exact).
#include "common.h"

uniform float4 thermal_shadow_pos; // x = half the aspect ratio, y = (0.5 - centre x) * aspect, z = 0.5 - centre y

struct heat_shadow_in
{
    float4 P : POSITION;
    float4 c : COLOR0;
    float2 tc : TEXCOORD0;
};

struct heat_shadow_out
{
    float4 hpos : POSITION;
    float4 c : COLOR0;
    float2 tc : TEXCOORD0;
    float3 scr : TEXCOORD1;
};

heat_shadow_out main(heat_shadow_in v)
{
    heat_shadow_out o;
    o.hpos = mul(m_WVP, v.P);
    o.c = v.c;
    o.tc = v.tc;
    o.scr.x = o.hpos.x * thermal_shadow_pos.x + o.hpos.w * thermal_shadow_pos.y;
    o.scr.y = o.hpos.w * thermal_shadow_pos.z - o.hpos.y * 0.5;
    o.scr.z = o.hpos.w;
    return o;
}
