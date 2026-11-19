// R1 thermal (r__heatvision): a warm particle effect redrawn into the heat mask. The stock particle.vs without fog.
#include "common.h"

struct heat_particle_in
{
    float4 P : POSITION;
    float2 tc : TEXCOORD0;
    float4 c : COLOR0;
};

struct heat_particle_out
{
    float4 hpos : POSITION;
    float2 tc : TEXCOORD0;
    float4 c : COLOR0;
};

heat_particle_out main(heat_particle_in v)
{
    heat_particle_out o;
    o.hpos = mul(m_WVP, v.P);
    o.tc = v.tc;
    o.c = v.c;
    return o;
}
