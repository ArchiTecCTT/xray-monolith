#pragma once

#include "../xrRender/ColorMapManager.h"


class CRenderTarget : public IRender_Target
{
public:
	ref_rt rt_secondVP; //--#SM+#-- +SecondVP+
	ref_rt rt_ui_pda;
	ref_rt rt_svp_work; // SVP-lite: scratch RT the small SVP frame is rendered into (copied to rt_secondVP)
	ref_shader s_ui_icons[6]; // SKIN_NONE, SKIN_0 ... SKIN_4
	IDirect3DSurface9* ui_icons_depth = nullptr; // Reused; never clear scene depth for icons.
	
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
	int svp_grow; // pixels the lens square in $user$viewport2 is grown (+, zoom out) or shrunk (-, zoom in) on each
	              // side, for a lens FOV change since the image was rendered (scroll-wheel lens zoom; scaled copy)

	void svp_lens_rect(u32 lens, RECT& r); // where the lens shaders read the lens square in $user$viewport2
	// copy the (shifted, grown by 'grow' pixels on each side) lens square of the work image to $user$viewport2
	void svp_publish(int shift_x, int shift_y, int grow = 0);
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
	// async CPU copy of $user$lum_adapt for scripts (console outputs r1_lum_adapt_value / r1_lum_adapt_rgb)
	enum { LUM_RB_SLOTS = 4 };
	IDirect3DSurface9* lum_rb[LUM_RB_SLOTS]; // 1x1 D3DPOOL_SYSTEMMEM copies, same format as rt_lum_adapt
	u32 lum_rb_frame[LUM_RB_SLOTS]; // Device.dwFrame the copy was queued on, 0 = empty / already read

	// R1 thermal (r__heatvision / bus lane "thermal"): heat masks of the warm things, see docs/r1_thermal.md
	int heat_state; // 0 = not created yet, 1 = ready, -1 = unavailable (files missing, no target) until vid_restart
	u32 heat_mode; // what the frame being built gets: 0 = nothing, 1 = the screen mask, 2 = the SVP lens mask
	ref_rt RT_heat; // $user$heat, the size of the scene target: r hotness, g hands, b sky, a edge term
	ref_rt rt_heat_svp_work; // the mask of the last small SVP frame, the size of $user$viewport2_work
	ref_rt rt_heat_svp; // $user$heat_vp2: its lens part, laid out exactly like $user$viewport2
	bool heat_screen_live; // $user$heat holds a mask (cleared once when thermal goes off)
	bool heat_svp_live; // $user$heat_vp2 holds a mask
	bool heat_svp_valid; // the work mask belongs to the SVP image in $user$viewport2_work (copied along with it)
	bool heat_alpha; // the mask targets have an alpha channel (it holds the edge term)
	bool heat_max; // the GPU blends with D3DBLENDOP_MAX
	IBlender* b_heat;
	ref_shader s_heat[6]; // per skinning mode, like s_ui_icons: SKIN_NONE, SKIN_0 ... SKIN_4
	// this frame's settings: the console vars, replaced by the bus lanes "thermal" and "thermal_region"
	int heat_cfg_targets; // 1 screen, 2 SVP lens, 3 both
	int heat_cfg_look; // 0 masks only, 1 the engine's look is shown, 2 built ($user$thermal) but not shown on screen
	int heat_cfg_palette; // 0 white hot, 1 black hot, 2 colour
	Fvector4 heat_cfg_region; // screen look inside a circle: x, y, radius, soft edge; radius 0 = whole screen
	// the sensor (bus lane "thermal_sensor" over the console vars): screen pixels per sensor pixel, grain, fixed
	// pattern, and a factor on the world's contrast
	float heat_cfg_soft, heat_cfg_grain, heat_cfg_fpn, heat_cfg_contrast;
	// soft round spots drawn into the mask after the models: what a fire warms around it, and lit lamps that are
	// only a glow (level geometry). Collected while the frame is built and drawn, dropped at its end
	struct heat_spot
	{
		Fvector pos;
		float radius; // metres
		float heat; // hotness in its middle
		float hard; // 0 = falls off from the middle, 1 = a disc with a soft rim
		float pull; // metres it is drawn towards the camera, so the thing it sits in does not hide it
	};
	xr_vector<heat_spot> heat_spots;
	// open fires: this frame's (they become spots) and the last frame's (they warm the objects near them)
	xr_vector<heat_spot> heat_fires, heat_fires_prev;
	ref_geom g_heat_spot;
	u32 heat_lens_wait; // frames thermal has been on for the lens only without an SVP frame (logged once)
	// warm particle effects: name patterns from configs\r1_thermal.ltx, and the answer per effect name
	struct heat_pattern
	{
		xr_string text;
		float hotness;
		float gain;
	};
	xr_vector<heat_pattern> heat_patterns;
	xr_map<const void*, Fvector2> heat_particle_cache;

	// the engine's thermal look (r1_thermal_look): one chain of small targets per view, 0 = the screen, 1 = the SVP lens
	struct look_chain
	{
		ref_rt down; // the mask at sensor size
		ref_rt glow_a;
		ref_rt glow; // its blur, half that size
		ref_rt grey; // the picture as grey values
		ref_rt out; // the finished picture (the screen's is $user$thermal)
		u32 w, h; // sensor size, 0 = not created
		IBlender* blender;
		ref_shader shader;
	};
	look_chain look[2];
	IBlender* b_look_extra;
	ref_shader s_look_extra; // region show, noise fill
	ref_rt rt_look_noise;
	float look_noise_time; // Device.fTimeGlobal of the last grain change
	Fvector2 look_noise_shift;
	bool look_screen_live; // $user$thermal holds this frame's picture
	struct look_consts
	{
		Fvector4 tap_a, tap_b, ring_a, ring_b, body, world, sky, grain, noise_a, noise_b, col_a, col_b, palette, region, hot;
	};
	look_consts look_c;
	// the part of the picture the passes are limited to (a region is set): 0..1 of the target, x0 > x1 = all of it
	Fvector4 look_clip;

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
	void lum_readback(bool queue); // read a finished copy (never waits), then optionally queue this frame's one
	void lum_readback_clear(); // forget queued copies and publish 0 (adaptation off / restarting)

	bool heat_wanted(); // r__heatvision or the bus lane "thermal"; also reads this frame's settings
	bool heat_create();
	bool heat_create_svp();
	void heat_destroy();
	void heat_load_patterns();
	void heat_idle(); // a normal frame without a screen mask: blank a mask left over from when thermal was on
	void heat_sky_flag(float w, float h); // blue = 1 where nothing in the scene wrote depth
	void heat_draw_spots(); // fire spread and lamp glows -> red, never above what is there
	void heat_end_frame(); // the spots and fires of this frame are done with
	void phase_heat(); // normal frame: warm things -> $user$heat
	void phase_heat_svp(); // small SVP frame: warm things -> the lens work mask, then the look into the lens picture

	bool look_prepare(u32 view, u32 src_w, u32 src_h); // (re)creates the chain for this source size
	// one screen quad into 'target' (w x h pixels), texture coordinates 0..1, with every look constant set
	void look_quad(ShaderElement* E, IDirect3DSurface9* target, float w, float h);
	void look_draw(ShaderElement* E, IDirect3DSurface9* target, const void* quad4); // the same with 4 given vertices
	bool look_build(u32 view, u32 src_w, u32 src_h); // scene + mask -> the finished picture of this view
	void look_show_region(); // the screen picture blended over the frame inside heat_cfg_region

	void calc_tc_noise(Fvector2& p0, Fvector2& p1);
	void calc_tc_duality_ss(Fvector2& r0, Fvector2& r1, Fvector2& l0, Fvector2& l1);
	void phase_distortion();
public:
	CRenderTarget();
	~CRenderTarget();

	void Begin();
	void End();

	// R1 thermal. heat_begin_frame: called once per frame before the scene graph is built; true = queue warm visuals.
	bool heat_begin_frame();
	// the pass that draws a warm visual with this skinning mode into the mask
	ShaderElement* heat_element(int skinning, bool lq);
	ShaderElement* heat_particle_element(); // the pass that draws a warm particle effect into the mask
	bool heat_blend_max() { return heat_max; }
	// is this particle effect warm (configs\r1_thermal.ltx)? hotness 0..1 and the gain on its sprites' brightness
	bool heat_particle(const shared_str& name, float& hotness, float& gain);
	// an open fire (a warm particle effect of hotness 0.75 and up) at this sphere: it warms what is around it
	void heat_add_fire(const Fvector& centre, float radius, float hotness);
	// a glow (a lit lamp) the scene drew, 'visible' 0..1 as the glow manager faded it
	void heat_add_glow(const Fvector& pos, float radius, float visible);
	bool heat_fires_near() { return !heat_fires_prev.empty(); }
	float heat_fire_at(const Fvector& pos); // hotness a fire gives a thing at this place, 0 = none

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
