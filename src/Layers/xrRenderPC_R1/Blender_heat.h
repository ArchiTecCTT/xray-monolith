#pragma once

// R1 thermal (r__heatvision): the engine-side passes of the heat mask. See docs/r1_thermal.md.
// The pixel shaders and heat_mask_lq.vs are files in shaders\r1\ (the engine's game data ships them); ui_icons.vs is
// the engine's own flat, fully skinned vertex shader, already used for the 3D icons.
//   E[0] mask    : a warm visual -> the heat mask, depth tested, no depth write    ui_icons.vs     + heat_mask.ps
//   E[1] mask lq : the same for a visual the scene drew with one-bone skinning     heat_mask_lq.vs + heat_mask.ps
//   E[2] demo    : scene + $user$heat -> the screen (r1_thermal_demo)              thermal_demo.ps
//   E[3] paint   : a warm visual blended white over the SVP lens picture (demo)    ui_icons.vs     + thermal_demo_paint.ps
//   E[4] paint lq: the same with one-bone skinning                                 heat_mask_lq.vs + thermal_demo_paint.ps
// E[0], E[1], E[3] and E[4] depend on the skinning mode set while the shader is created (one shader per mode).

#define R1_RT_HEAT "$user$heat"
#define R1_RT_HEAT_VP2 "$user$heat_vp2"
#define R1_RT_HEAT_VP2_WORK "$user$heat_vp2_work"

enum
{
	SE_R1_HEAT_MASK = 0,
	SE_R1_HEAT_MASK_LQ = 1,
	SE_R1_HEAT_DEMO = 2,
	SE_R1_HEAT_PAINT = 3,
	SE_R1_HEAT_PAINT_LQ = 4,
};

class CBlender_r1_heat : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: R1 heat mask"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	CBlender_r1_heat();
	virtual ~CBlender_r1_heat();
};
