#pragma once

#include "../xrRender/ColorMapManager.h"


class CRenderTarget : public IRender_Target
{
public:
	ref_rt rt_secondVP; //--#SM+#-- +SecondVP+
	ref_rt rt_ui_pda;
	ref_rt rt_svp_work; // SVP-lite: scratch RT the small SVP frame is rendered into (copied to rt_secondVP)
	
private:
	// SVP-lite: small second viewport (r1_svp_size)
	BOOL svp_small; // small SVP render target + depth buffer were created
	BOOL svp_small_frame; // the current frame (between Begin and End) is rendered into the small RT
	u32 svp_w;
	u32 svp_h;
	IDirect3DSurface9* zb_svp;

	// SVP lens (r1_svp_fit / r1_svp_reproject), see SVP-LITE-NOTES.md
	u32 svp_vp2_w; // size of $user$viewport2
	u32 svp_vp2_h;
	u32 svp_vp2_lens; // lens square (pixels) $user$viewport2 was shaped for, 0 = square viewport2 (old layout)
	BOOL svp_lens_valid; // the work target holds a lens-fitted SVP image that can be shifted
	u32 svp_lens_frame; // Device.dwFrame it was rendered on
	u32 svp_lens_margin; // its margin (pixels on each side of the lens square)
	float svp_lens_hat; // its projection: tan of half the vertical angle of the whole work image
	float svp_lens_aspect; // its projection aspect
	float svp_lens_fov; // the lens FOV (hud_params.y) it was rendered with
	Fmatrix svp_lens_view; // its view matrix
	int svp_shift_x; // shift (work pixels) of the lens square currently in $user$viewport2
	int svp_shift_y;

	void svp_lens_rect(u32 lens, RECT& r); // where the lens shaders read the lens square in $user$viewport2
	void svp_publish(int shift_x, int shift_y); // copy the (shifted) lens square of the work image to $user$viewport2
	void svp_reproject(); // normal frame: follow the aim, ask for a fresh SVP frame when needed

	BOOL bAvailable;
	u32 rtWidth;
	u32 rtHeight;

	u32 curWidth;
	u32 curHeight;

	ref_rt RT;
	ref_rt RT_color_map;
	ref_rt RT_distort;
	IDirect3DSurface9* ZB;

	//	Can't implement in a single pass of a shader since
	//	should be compiled only for the hardware that supports it.
	ref_shader s_postprocess[2]; //	Igor: 0 - plain, 1 - colormapped
	ref_shader s_postprocess_D[2]; //	Igor: 0 - plain, 1 - colormapped
	ref_geom g_postprocess;
	// the same quad for a pass that names a vertex shader (ps_3_0 needs vs_3_0): POSITION in clip space, not XYZRHW
	ref_geom g_postprocess_vs;

	// R1 luminance adaptation (r1_lum_adapt / bus lane "lum_adapt"), see R1-PS3-LUM-ADAPT-NOTES.md
	int lum_state; // 0 = not created yet, 1 = ready, -1 = unavailable (files or formats missing) until vid_restart
	ref_rt rt_lum_64; // 64x64 metered scene
	ref_rt rt_lum_8; // 8x8
	ref_rt rt_lum_adapt; // 1x1 adapted value, sampled by post-process shaders as $user$lum_adapt
	ref_rt rt_lum_prev; // 1x1 history (last frame's adapted value)
	IBlender* b_lum;
	ref_shader s_lum; // engine passes (reduce, blend, copy, default meter)
	ref_shader s_lum_meter; // shaders\r1\lum_adapt_meter.s when a mod ships one
	u32 lum_frame; // last frame the adaptation ran, 0 = never
	float lum_time; // seconds since the adaptation (re)started
	float lum_last_time; // Device.fTimeGlobal of the last run
	Fvector4 lum_info; // post-process constant lum_adapt_info: live 0/1, lum_time, speed up, speed down

	float im_noise_time;
	u32 im_noise_shift_w;
	u32 im_noise_shift_h;

	float param_blur;
	float param_gray;
	float param_duality_h;
	float param_duality_v;
	float param_noise;
	float param_noise_scale;
	float param_noise_fps;

	//	Color mapping
	float param_color_map_influence;
	float param_color_map_interpolate;
	ColorMapManager color_map_manager;

	u32 param_color_base;
	u32 param_color_gray;
	Fvector param_color_add;

	u32 frame_distort;
public:
	IDirect3DSurface9* pTempZB;

	//	Igor: for async screenshots
	IDirect3DSurface9* pFB; //32bit		(r,g,b,a) is situated in the system memory

private:
	BOOL Create();
	bool NeedColorMapping();
	BOOL NeedPostProcess();
	BOOL Available() { return bAvailable; }
	BOOL Perform();

	bool lum_wanted(float& speed_up, float& speed_down);
	bool lum_create();
	void lum_destroy();
	void lum_update();
	void lum_pass(ShaderElement* E, const ref_rt& target, float step_x, float step_y, const Fvector4* adapt);

	void calc_tc_noise(Fvector2& p0, Fvector2& p1);
	void calc_tc_duality_ss(Fvector2& r0, Fvector2& r1, Fvector2& l0, Fvector2& l1);
	void phase_distortion();
public:
	CRenderTarget();
	~CRenderTarget();

	void Begin();
	void End();

	BOOL SVPSmallEnabled() { return svp_small; }
	BOOL SVPSmallFrame(); // small SVP target is enabled and this frame is an SVP frame
	u32 get_svp_width() { return svp_w; }
	u32 get_svp_height() { return svp_h; }

	// r1_svp_stats counters (reset by the stats log)
	u32 svp_stat_forced; // forced SVP frames rendered (r1_svp_reproject)
	u32 svp_stat_copies; // lens copies into $user$viewport2 on normal frames (r1_svp_reproject)

	void DoAsyncScreenshot();

	virtual void set_blur(float f) { param_blur = f; }
	virtual void set_gray(float f) { param_gray = f; }
	virtual void set_duality_h(float f) { param_duality_h = _abs(f); }
	virtual void set_duality_v(float f) { param_duality_v = _abs(f); }
	virtual void set_noise(float f) { param_noise = f; }
	virtual void set_noise_scale(float f) { param_noise_scale = f; }
	virtual void set_noise_fps(float f) { param_noise_fps = _abs(f) + EPS_S; }

	virtual void set_color_base(u32 f) { param_color_base = f; }
	virtual void set_color_gray(u32 f) { param_color_gray = f; }
	virtual void set_color_add(const Fvector& f) { param_color_add = f; }

	virtual void set_cm_imfluence(float f) { param_color_map_influence = f; }
	virtual void set_cm_interpolate(float f) { param_color_map_interpolate = f; }

	virtual void set_cm_textures(const shared_str& tex0, const shared_str& tex1)
	{
		color_map_manager.SetTextures(tex0, tex1);
	}

	virtual u32 get_width() { return curWidth; }
	virtual u32 get_height() { return curHeight; }

	u32 get_rtwidth() { return rtWidth; }
	u32 get_rtheight() { return rtHeight; }
};
