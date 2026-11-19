// R1 thermal (r__heatvision): the vertex shader of the heat mask redraw, for visuals the scene drew with its
// high-quality element (model_def_hq.vs). The position line is the stock one, so the redraw lands on the depth the
// scene wrote. "facing" tells how much the surface faces the camera (1 = straight at it, 0 = seen edge-on).
#include "common.h"
#include "skin.h"

struct heat_pixel
{
    float4 position : POSITION;
    float2 tc : TEXCOORD0;
    float facing : TEXCOORD1;
};

heat_pixel _main(v_model I)
{
    heat_pixel O;
    float3 pos_w = mul(m_W, I.pos);
    float3 norm_w = normalize(mul(m_W, I.norm));
    O.position = mul(m_WVP, I.pos);
    O.tc = I.tc.xy;
    O.facing = dot(norm_w, normalize(eye_position - pos_w));
    return O;
}

#define SKIN_VF heat_pixel
#include "skin_main.h"
