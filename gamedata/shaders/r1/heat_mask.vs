// R1 thermal (r__heatvision): the vertex shader of the heat mask redraw, for visuals the scene drew with its
// high-quality element (model_def_hq.vs). The position line is the stock one, so the redraw lands on the depth the
// scene wrote. "heat.x" tells how much the surface faces the camera (1 = straight at it, 0 = seen edge-on), "heat.y"
// is the hotness of this vertex: the same everywhere, or highest at a heat point (a gun's muzzle) and falling off.
#include "common.h"
#include "skin.h"

uniform float4 r1_heat_vs;    // x = hotness at the heat point, y = hotness far from it
uniform float4 r1_heat_depth; // x = pull towards the camera in metres, y = near plane distance, z = near * far / (far - near)
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
    // Pulled towards the camera by a fixed distance in metres, the same at any range: the vertex gets the depth it
    // would have 'pull' metres nearer (w is the distance along the view axis). Never past the near plane, and a
    // vertex in front of the near plane is left alone, so it is clipped exactly as the scene clipped it.
    float pull = clamp(O.position.w - r1_heat_depth.y, 0.0, r1_heat_depth.x);
    O.position.z -= r1_heat_depth.z * pull / max(O.position.w - pull, r1_heat_depth.y);
    O.tc = I.tc.xy;
    float near_point = saturate(1.0 - distance(pos_w, r1_heat_point.xyz) * r1_heat_point.w);
    O.heat.x = dot(norm_w, normalize(eye_position - pos_w));
    O.heat.y = lerp(r1_heat_vs.y, r1_heat_vs.x, near_point);
    return O;
}

#define SKIN_VF heat_pixel
#include "skin_main.h"
