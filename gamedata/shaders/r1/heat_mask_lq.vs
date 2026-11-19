// R1 thermal (r__heatvision): heat_mask.vs with one-bone skinning for two-bone meshes (SKIN_LQ), for warm visuals
// the scene drew with its low-quality element (model_def_lq.vs), so the heat mask lands on the same depth.
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

#define SKIN_LQ
#define SKIN_VF heat_pixel
#include "skin_main.h"
