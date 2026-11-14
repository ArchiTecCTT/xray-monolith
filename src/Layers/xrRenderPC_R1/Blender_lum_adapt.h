#pragma once

// R1 luminance adaptation (r1_lum_adapt): the engine-side passes. The meter pass (scene -> $user$lum_64) comes from
// shaders\r1\lum_adapt_meter.s when a mod ships one (so it can add its own samplers, e.g. a mask), otherwise from
// element 3 here. All pixel shaders are files a mod ships in shaders\r1\ (see R1-PS3-LUM-ADAPT-NOTES.md).
//   E[0] reduce : $user$lum_64 (64x64)            -> $user$lum_8 (8x8)        lum_adapt_reduce.ps
//   E[1] blend  : $user$lum_8 + $user$lum_prev    -> $user$lum_adapt (1x1)    lum_adapt_blend.ps
//   E[2] copy   : $user$lum_adapt                 -> $user$lum_prev (1x1)     lum_adapt_reduce.ps, zero step
//   E[3] meter  : $user$rendertarget (the scene)  -> $user$lum_64             lum_adapt_meter.ps (default binding)

#define R1_RT_LUM_64 "$user$lum_64"
#define R1_RT_LUM_8 "$user$lum_8"
#define R1_RT_LUM_ADAPT "$user$lum_adapt"
#define R1_RT_LUM_PREV "$user$lum_prev"

class CBlender_r1_lum_adapt : public IBlender
{
public:
	virtual LPCSTR getComment() { return "INTERNAL: R1 luminance adaptation"; }
	virtual BOOL canBeDetailed() { return FALSE; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Compile(CBlender_Compile& C);

	CBlender_r1_lum_adapt();
	virtual ~CBlender_r1_lum_adapt();
};
