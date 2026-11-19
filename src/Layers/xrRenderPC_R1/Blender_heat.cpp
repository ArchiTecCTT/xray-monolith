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
		C.r_Pass("heat_mask", "heat_mask", false, TRUE, FALSE);
		C.r_End();
		break;
	case SE_R1_HEAT_MASK_LQ:
		C.r_Pass("heat_mask_lq", "heat_mask", false, TRUE, FALSE);
		C.r_End();
		break;
	case SE_R1_HEAT_PARTICLE:
		// ONE + ONE; the heat pass switches the blend operation to MAX where the GPU has it. The texture named here
		// only holds the stage: the pass binds the texture list of the particle effect it draws.
		C.r_Pass("heat_particle", "heat_particle", false, TRUE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
		C.r_Sampler("s_base", R1_RT_THERMAL_NOISE, false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
		C.r_End();
		break;
	case SE_R1_HEAT_SKY: // a screen quad, depth tested: it only lands where nothing in the scene wrote depth
		C.r_Pass("null", "heat_flat", false, TRUE, FALSE);
		C.r_End();
		break;
	case SE_R1_HEAT_SPOT: // ONE + ONE, switched to MAX by the pass that draws it; no texture, the disc is computed
		C.r_Pass("heat_particle", "heat_spot", false, TRUE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
		C.r_End();
		break;
	}
}

//////////////////////////////////////////////////////////////////////////

const r1_thermal_look_names& r1_thermal_look_names_of(u32 view)
{
	static const r1_thermal_look_names names[2] =
	{
		{
			"$user$rendertarget", R1_RT_HEAT, "$user$thermal_down", "$user$thermal_glow_a", "$user$thermal_glow",
			"$user$thermal_grey", R1_RT_THERMAL
		},
		{
			"$user$viewport2_work", R1_RT_HEAT_VP2_WORK, "$user$thermal_vp2_down", "$user$thermal_vp2_glow_a",
			"$user$thermal_vp2_glow", "$user$thermal_vp2_grey", "$user$thermal_vp2_work"
		},
	};
	return names[view ? 1 : 0];
}

CBlender_r1_thermal_look::CBlender_r1_thermal_look(u32 _view)
{
	description.CLS = 0;
	view = _view;
}

CBlender_r1_thermal_look::~CBlender_r1_thermal_look()
{
}

void CBlender_r1_thermal_look::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	const r1_thermal_look_names& N = r1_thermal_look_names_of(view);
	const u32 clamp = D3DTADDRESS_CLAMP;
	const u32 lin = D3DTEXF_LINEAR, point = D3DTEXF_POINT, none = D3DTEXF_NONE;
	switch (C.iElement)
	{
	case SE_R1_LOOK_DOWN:
		C.r_Pass("null", "thermal_down", false, FALSE, FALSE);
		C.r_Sampler("s_heat", N.mask, false, clamp, lin, none, lin);
		C.r_End();
		break;
	case SE_R1_LOOK_BLUR_A:
		C.r_Pass("null", "thermal_blur", false, FALSE, FALSE);
		C.r_Sampler("s_image", N.down, false, clamp, lin, none, lin);
		C.r_End();
		break;
	case SE_R1_LOOK_BLUR_B:
		C.r_Pass("null", "thermal_blur", false, FALSE, FALSE);
		C.r_Sampler("s_image", N.glow_a, false, clamp, lin, none, lin);
		C.r_End();
		break;
	case SE_R1_LOOK_GREY:
		C.r_Pass("null", "thermal_look", false, FALSE, FALSE);
		C.r_Sampler("s_scene", N.scene, false, clamp, lin, none, lin);
		C.r_Sampler("s_down", N.down, false, clamp, lin, none, lin);
		C.r_Sampler("s_glow", N.glow, false, clamp, lin, none, lin);
		C.r_Sampler("s_noise", R1_RT_THERMAL_NOISE, false, D3DTADDRESS_WRAP, point, none, point);
		C.r_End();
		break;
	case SE_R1_LOOK_PALETTE:
		C.r_Pass("null", "thermal_palette", false, FALSE, FALSE);
		C.r_Sampler("s_image", N.grey, false, clamp, point, none, point);
		C.r_End();
		break;
	case SE_R1_LOOK_SHOW:
		C.r_Pass("null", "thermal_show", false, FALSE, FALSE);
		C.r_Sampler("s_image", N.out, false, clamp, lin, none, lin);
		C.r_End();
		break;
	}
}

//////////////////////////////////////////////////////////////////////////

CBlender_r1_thermal_extra::CBlender_r1_thermal_extra() { description.CLS = 0; }

CBlender_r1_thermal_extra::~CBlender_r1_thermal_extra()
{
}

void CBlender_r1_thermal_extra::Compile(CBlender_Compile& C)
{
	IBlender::Compile(C);

	switch (C.iElement)
	{
	case SE_R1_LOOK_REGION: // alpha = inside the circle
		C.r_Pass("null", "thermal_show_region", false, FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
		C.r_Sampler("s_image", R1_RT_THERMAL, false, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
		C.r_End();
		break;
	case SE_R1_LOOK_NOISE:
		C.r_Pass("null", "thermal_noise", false, FALSE, FALSE);
		C.r_End();
		break;
	}
}
