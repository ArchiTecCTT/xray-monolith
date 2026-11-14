#include "stdafx.h"
#pragma hdrstop

#include "Blender_lum_adapt.h"

CBlender_r1_lum_adapt::CBlender_r1_lum_adapt() { description.CLS = 0; }

CBlender_r1_lum_adapt::~CBlender_r1_lum_adapt()
{
}

void CBlender_r1_lum_adapt::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	switch (C.iElement)
	{
	case 0: // reduce 64x64 -> 8x8
		C.r_Pass("null", "lum_adapt_reduce", false, FALSE, FALSE);
		C.r_Sampler("s_image", R1_RT_LUM_64, false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
		C.r_End();
		break;
	case 1: // reduce 8x8 -> 1x1 and blend with the previous frame
		C.r_Pass("null", "lum_adapt_blend", false, FALSE, FALSE);
		C.r_Sampler("s_image", R1_RT_LUM_8, false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
		C.r_Sampler("s_lum_prev", R1_RT_LUM_PREV, false, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_NONE, D3DTEXF_POINT);
		C.r_End();
		break;
	case 2: // copy the adapted value into the history target (point sampled, zero tap step)
		C.r_Pass("null", "lum_adapt_reduce", false, FALSE, FALSE);
		C.r_Sampler("s_image", R1_RT_LUM_ADAPT, false, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_NONE, D3DTEXF_POINT);
		C.r_End();
		break;
	case 3: // default meter (used when no lum_adapt_meter.s exists)
		C.r_Pass("null", "lum_adapt_meter", false, FALSE, FALSE);
		C.r_Sampler("s_image", "$user$rendertarget", false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE,
		            D3DTEXF_LINEAR);
		C.r_End();
		break;
	}
}
