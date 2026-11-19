// R1 thermal (r__heatvision): the vertex shader of the heat mask redraw, for visuals the scene drew with its
// high-quality element (model_def_hq.vs). The position line is the stock one, so the redraw lands on the depth the
// scene wrote. "heat.x" tells how much the surface faces the camera (1 = straight at it, 0 = seen edge-on), "heat.y"
// is the hotness of this vertex: the same everywhere, or highest at a heat point (a gun's muzzle) and falling off.
#include "common.h"
#include "skin.h"

uniform float4 r1_heat_vs;    // x = hotness at the heat point, y = hotness far from it, z = depth pull in metres
uniform float4 r1_heat_point; // xyz = the heat point in world space, w = 1 / reach in metres (0 = the same hotness everywhere)

struct heat_pixel
{
    float4 position : POSITION;
    float2 tc : TEXCOORD0;
    float2 heat : TEXCOORD1; // x = facing, y = hotness
};

heat_pixel _main(v_model I)
{
    heat_pixel O;
    float3 pos_w = mul(m_W, I.pos);
    float3 norm_w = normalize(mul(m_W, I.norm));
    O.position = mul(m_WVP, I.pos);
    // pulled towards the camera by a fixed distance (a few centimetres), the same at any range
    O.position.z -= r1_heat_vs.z;
    O.tc = I.tc.xy;
    float near_point = saturate(1.0 - distance(pos_w, r1_heat_point.xyz) * r1_heat_point.w);
    O.heat.x = dot(norm_w, normalize(eye_position - pos_w));
    O.heat.y = lerp(r1_heat_vs.y, r1_heat_vs.x, near_point);
    return O;
}

#define SKIN_VF heat_pixel
#include "skin_main.h"
