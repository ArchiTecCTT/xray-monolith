#include "stdafx.h"
#pragma hdrstop

#include "Blender_heat.h"

CBlender_r1_heat::CBlender_r1_heat() { description.CLS = 0; }

CBlender_r1_heat::~CBlender_r1_heat()
{
}

void CBlender_r1_heat::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	switch (C.iElement)
	{
	case SE_R1_HEAT_MASK: // no fog, depth test (LESSEQUAL) without depth write, no blending
		C.r_Pass("ui_icons", "heat_mask", false, TRUE, FALSE);
		C.r_End();
		break;
	case SE_R1_HEAT_MASK_LQ:
		C.r_Pass("heat_mask_lq", "heat_mask", false, TRUE, FALSE);
		C.r_End();
		break;
	case SE_R1_HEAT_DEMO: // full-screen quad, like the stock post-process pass
		C.r_Pass("null", "thermal_demo", false, FALSE, FALSE);
		C.r_Sampler("s_base0", "$user$rendertarget", false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE,
		            D3DTEXF_LINEAR);
		C.r_Sampler("s_heat", R1_RT_HEAT, false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
		C.r_End();
		break;
	case SE_R1_HEAT_PAINT: // white, alpha = hotness, blended over what is there
		C.r_Pass("ui_icons", "thermal_demo_paint", false, TRUE, FALSE, TRUE, D3DBLEND_SRCALPHA,
		         D3DBLEND_INVSRCALPHA);
		C.r_End();
		break;
	case SE_R1_HEAT_PAINT_LQ:
		C.r_Pass("heat_mask_lq", "thermal_demo_paint", false, TRUE, FALSE, TRUE, D3DBLEND_SRCALPHA,
		         D3DBLEND_INVSRCALPHA);
		C.r_End();
		break;
	}
}
