#pragma once

// R1 thermal (r__heatvision): the engine-side passes of the heat mask and of the thermal look. See docs/r1_thermal.md.
// All vertex and pixel shaders are files in shaders\r1\ (the engine's game data ships them).
//
// CBlender_r1_heat, the heat mask:
//   E[0] mask    : a warm visual -> the mask, depth tested, no depth write        heat_mask.vs     + heat_mask.ps
//   E[1] mask lq : the same for a visual the scene drew with one-bone skinning    heat_mask_lq.vs  + heat_mask.ps
//   E[2] particle: a warm particle effect, blended into red                       heat_particle.vs + heat_particle.ps
//   E[3] sky     : a screen quad at the far end of the depth range                heat_flat.ps
// E[0] and E[1] depend on the skinning mode set while the shader is created (one shader per mode).
//
// CBlender_r1_thermal_look, the thermal look of one view (0 = the screen, 1 = the SVP lens); screen quads:
//   E[0] down    : mask -> sensor size                                            thermal_down.ps
//   E[1] blur a  : sensor size -> half of it                                      thermal_blur.ps
//   E[2] blur b  : once more, wider                                               thermal_blur.ps
//   E[3] grey    : scene + mask + blur + noise -> one grey value per sensor pixel thermal_look.ps
//   E[4] palette : grey -> colour, the finished picture ($user$thermal)           thermal_palette.ps
//   E[5] show    : the finished picture stretched over the view                   thermal_show.ps
//
// CBlender_r1_thermal_extra:
//   E[0] region  : the finished screen picture blended over the frame in a circle thermal_show_region.ps
//   E[1] noise   : fills the noise texture                                        thermal_noise.ps

#define R1_RT_HEAT "$user$heat"
#define R1_RT_HEAT_VP2 "$user$heat_vp2"
#define R1_RT_HEAT_VP2_WORK "$user$heat_vp2_work"
#define R1_RT_THERMAL "$user$thermal" // the finished thermal picture of the screen, sensor size
#define R1_RT_THERMAL_NOISE "$user$thermal_noise"

enum
{
	SE_R1_HEAT_MASK = 0,
	SE_R1_HEAT_MASK_LQ = 1,
	SE_R1_HEAT_PARTICLE = 2,
	SE_R1_HEAT_SKY = 3,
};

enum
{
	SE_R1_LOOK_DOWN = 0,
	SE_R1_LOOK_BLUR_A = 1,
	SE_R1_LOOK_BLUR_B = 2,
	SE_R1_LOOK_GREY = 3,
	SE_R1_LOOK_PALETTE = 4,
	SE_R1_LOOK_SHOW = 5,
};

enum
{
	SE_R1_LOOK_REGION = 0,
	SE_R1_LOOK_NOISE = 1,
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

// the render target names of one view's thermal look
struct r1_thermal_look_names
{
	LPCSTR scene; // the picture of the view
	LPCSTR mask; // its heat mask
	LPCSTR down;
	LPCSTR glow_a;
	LPCSTR glow;
	LPCSTR grey;
	LPCSTR out;
};
const r1_thermal_look_names& r1_thermal_look_names_of(u32 view);

class CBlender_r1_thermal_look : public IBlender
{
	u32 view;
public:
	virtual LPCSTR getComment() { return "INTERNAL: R1 thermal look"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	CBlender_r1_thermal_look(u32 _view);
	virtual ~CBlender_r1_thermal_look();
};

class CBlender_r1_thermal_extra : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: R1 thermal look, region and noise"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	CBlender_r1_thermal_extra();
	virtual ~CBlender_r1_thermal_extra();
};
