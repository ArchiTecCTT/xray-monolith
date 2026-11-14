#include "stdafx.h"
#include "fstaticrender_rendertarget.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/shader_bus.h"
#include "../xrRender/dxRenderDeviceRender.h"
#include "Blender_lum_adapt.h"


static LPCSTR RTname = "$user$rendertarget";
static LPCSTR RTname_color_map = "$user$rendertarget_color_map";
static LPCSTR RTname_distort = "$user$distort";
static LPCSTR RTname_SecondVP = "$user$viewport2"; //--#SM+#-- +SecondVP+
static LPCSTR RTname_pda_ui = "$user$ui";
static LPCSTR RTname_SecondVP_work = "$user$viewport2_work"; // SVP-lite

CRenderTarget::CRenderTarget()
{
	bAvailable = FALSE;
	RT = 0;
	RT_color_map = 0;
	pTempZB = 0;
	ZB = 0;
	pFB = 0;

	param_blur = 0.f;
	param_gray = 0.f;
	param_noise = 0.f;
	param_duality_h = 0.f;
	param_duality_v = 0.f;
	param_noise_fps = 25.f;
	param_noise_scale = 1.f;

	rt_secondVP = nullptr; //--#SM+# +SecondVP+
	rt_svp_work = nullptr;
	svp_small = FALSE;
	svp_small_frame = FALSE;
	svp_w = 0;
	svp_h = 0;
	zb_svp = 0;

	param_color_map_influence = 0.0f;
	param_color_map_interpolate = 0.0f;

	im_noise_time = 1.0f / 100.0f;
	im_noise_shift_w = 0;
	im_noise_shift_h = 0;

	param_color_base = color_rgba(127, 127, 127, 0);
	param_color_gray = color_rgba(85, 85, 85, 0);
	param_color_add.set(0.0f, 0.0f, 0.0f);

	lum_state = 0;
	b_lum = nullptr;
	lum_frame = 0;
	lum_time = 0.f;
	lum_last_time = 0.f;
	lum_info.set(0.f, 0.f, 0.f, 0.f);

	bAvailable = Create();
	Msg("* SSample: %s", bAvailable ? "enabled" : "disabled");
}

BOOL CRenderTarget::Create()
{
	curWidth = Device.dwWidth;
	curHeight = Device.dwHeight;

	// Select mode to operate in
	float amount = ps_r__Supersample ? float(ps_r__Supersample) : 1;
	float scale = _sqrt(amount);
	rtWidth = clampr(iFloor(scale * Device.dwWidth + .5f), 128, 2048);
	rtHeight = clampr(iFloor(scale * Device.dwHeight + .5f), 128, 2048);
	while (rtWidth % 2) rtWidth --;
	while (rtHeight % 2) rtHeight--;
	Msg("* SSample: %dx%d", rtWidth, rtHeight);

	// Bufferts
	RT.create(RTname, rtWidth, rtHeight, HW.Caps.fTarget);
	RT_distort.create(RTname_distort, rtWidth, rtHeight, HW.Caps.fTarget);
	if (RImplementation.o.color_mapping)
	{
		//RT_color_map.create	(RTname_color_map,	rtWidth,rtHeight,HW.Caps.fTarget);
		RT_color_map.create(RTname_color_map, curWidth, curHeight, HW.Caps.fTarget);
	}
	//RImplementation.o.color_mapping = RT_color_map->valid();

	//We need to use Device.resolution in order to maintain performance while rendering image with second viewport + supersampling.
	//Please do not change this. It's just working properly.
	// SVP-lite: optionally render the SVP frame into a small square RT (r1_svp_size) instead of the full-size backbuffer
	if (ps_r1_svp_size > 0)
	{
		const u32 svp_sz = u32(clampr(ps_r1_svp_size, 64, 2048));
		rt_secondVP.create(RTname_SecondVP, svp_sz, svp_sz, HW.Caps.fTarget);
		rt_svp_work.create(RTname_SecondVP_work, svp_sz, svp_sz, HW.Caps.fTarget);
		HRESULT svp_hr = E_FAIL;
		if (rt_secondVP->valid() && rt_svp_work->valid())
			svp_hr = HW.pDevice->CreateDepthStencilSurface(svp_sz, svp_sz, HW.Caps.fDepth, D3DMULTISAMPLE_NONE, 0, TRUE, &zb_svp, NULL);
		if (SUCCEEDED(svp_hr) && zb_svp)
		{
			svp_small = TRUE;
			svp_w = svp_sz;
			svp_h = svp_sz;
			Msg("* SVP-lite: small second viewport %ux%u", svp_sz, svp_sz);
			// default-pool targets start undefined; clear so the lens is black, not garbage, until the first SVP frame is published
			HW.pDevice->ColorFill(rt_secondVP->pRT, nullptr, D3DCOLOR_XRGB(0, 0, 0));
			HW.pDevice->ColorFill(rt_svp_work->pRT, nullptr, D3DCOLOR_XRGB(0, 0, 0));
		}
		else
		{
			Msg("! SVP-lite: can't create %ux%u second viewport target, using full size", svp_sz, svp_sz);
			_RELEASE(zb_svp);
			rt_svp_work.destroy();
			rt_secondVP.destroy();
		}
	}
	if (!svp_small)
		rt_secondVP.create(RTname_SecondVP, Device.dwWidth, Device.dwHeight, HW.Caps.fTarget); //--#SM+#-- +SecondVP+
	rt_ui_pda.create(RTname_pda_ui, Device.dwWidth, Device.dwHeight, HW.Caps.fTarget);

	if ((rtHeight != Device.dwHeight) || (rtWidth != Device.dwWidth))
	{
		R_CHK(HW.pDevice->CreateDepthStencilSurface (rtWidth,rtHeight,HW.Caps.fDepth,D3DMULTISAMPLE_NONE,0,TRUE,&ZB,NULL));
	}
	else
	{
		ZB = HW.pBaseZB;
		ZB->AddRef();
	}

	// Temp ZB, used by some of the shadowing code
	R_CHK(HW.pDevice->CreateDepthStencilSurface (512,512,HW.Caps.fDepth,D3DMULTISAMPLE_NONE,0,TRUE,&pTempZB,NULL));

	//	Igor: TMP
	//	Create an RT for online screenshot makining
	//u32		w = Device.dwWidth, h = Device.dwHeight;
	//HW.pDevice->CreateOffscreenPlainSurface(Device.dwWidth,Device.dwHeight,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&pFB,NULL);
	HW.pDevice->CreateOffscreenPlainSurface(rtWidth, rtHeight, HW.Caps.fTarget, D3DPOOL_SYSTEMMEM, &pFB,NULL);

	// Shaders and stream
	s_postprocess[0].create("postprocess");
	if (RImplementation.o.distortion) s_postprocess_D[0].create("postprocess_d");
	if (RImplementation.o.color_mapping)
	{
		s_postprocess[1].create("postprocess_cm");
		if (RImplementation.o.distortion) s_postprocess_D[1].create("postprocess_dcm");
	}
	g_postprocess.create(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3, RCache.Vertex.Buffer(),
	                     RCache.QuadIB);
	// Same 48-byte vertex as above, but POSITION instead of POSITIONT: used when the pass names a vertex shader
	// (a ps_3_0 needs a vs_3_0, and D3D9 skips the vertex shader for pre-transformed XYZRHW vertices).
	static D3DVERTEXELEMENT9 dcl_postprocess_vs[] =
	{
		{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
		{0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
		{0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1},
		{0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
		{0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
		{0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
		D3DDECL_END()
	};
	g_postprocess_vs.create(dcl_postprocess_vs, RCache.Vertex.Buffer(), RCache.QuadIB);
	return RT->valid() && RT_distort->valid();
}

CRenderTarget::~CRenderTarget()
{
	_RELEASE(pFB);
	_RELEASE(pTempZB);
	_RELEASE(ZB);
	_RELEASE(zb_svp);
	rt_svp_work.destroy();
	lum_destroy();
	g_postprocess_vs.destroy();
	s_postprocess_D[1].destroy();
	s_postprocess[1].destroy();
	s_postprocess_D[0].destroy();
	s_postprocess[1].destroy();
	g_postprocess.destroy();
	RT_distort.destroy();
	RT_color_map.destroy();
	RT.destroy();
	rt_secondVP.destroy(); //--#SM+#-- +SecondVP+s	
}

void CRenderTarget::calc_tc_noise(Fvector2& p0, Fvector2& p1)
{
	//.	CTexture*	T					= RCache.get_ActiveTexture	(2);
	//.	VERIFY2		(T, "Texture #3 in noise shader should be setted up");
	//.	u32			tw					= iCeil(float(T->get_Width	())*param_noise_scale+EPS_S);
	//.	u32			th					= iCeil(float(T->get_Height ())*param_noise_scale+EPS_S);
	u32 tw = iCeil(256 * param_noise_scale + EPS_S);
	u32 th = iCeil(256 * param_noise_scale + EPS_S);
	VERIFY2(tw && th, "Noise scale can't be zero in any way");
	//.	if (bDebug)	Msg			("%d,%d,%f",tw,th,param_noise_scale);

	// calculate shift from FPSes
	im_noise_time -= Device.fTimeDelta;
	if (im_noise_time < 0)
	{
		im_noise_shift_w = ::Random.randI(tw ? tw : 1);
		im_noise_shift_h = ::Random.randI(th ? th : 1);
		float fps_time = 1 / param_noise_fps;
		while (im_noise_time < 0) im_noise_time += fps_time;
	}

	u32 shift_w = im_noise_shift_w;
	u32 shift_h = im_noise_shift_h;
	float start_u = (float(shift_w) + .5f) / (tw);
	float start_v = (float(shift_h) + .5f) / (th);
	u32 _w = Device.dwWidth;
	u32 _h = Device.dwHeight;
	u32 cnt_w = _w / tw;
	u32 cnt_h = _h / th;
	float end_u = start_u + float(cnt_w) + 1;
	float end_v = start_v + float(cnt_h) + 1;

	p0.set(start_u, start_v);
	p1.set(end_u, end_v);
}

void CRenderTarget::calc_tc_duality_ss(Fvector2& r0, Fvector2& r1, Fvector2& l0, Fvector2& l1)
{
	// Calculate ordinaty TCs from blur and SS
	float tw = float(rtWidth);
	float th = float(rtHeight);
	if (rtHeight != Device.dwHeight) param_blur = 1.f;
	Fvector2 shift, p0, p1;
	shift.set(.5f / tw, .5f / th);
	shift.mul(param_blur);
	p0.set(.5f / tw, .5f / th).add(shift);
	p1.set((tw + .5f) / tw, (th + .5f) / th).add(shift);

	// Calculate Duality TC
	float shift_u = param_duality_h * .5f;
	float shift_v = param_duality_v * .5f;

	r0.set(p0.x, p0.y);
	r1.set(p1.x - shift_u, p1.y - shift_v);
	l0.set(p0.x + shift_u, p0.y + shift_v);
	l1.set(p1.x, p1.y);
}

bool CRenderTarget::NeedColorMapping()
{
	return RImplementation.o.color_mapping && (param_color_map_influence > 0.001f);
}

BOOL CRenderTarget::NeedPostProcess()
{
	bool _blur = (param_blur > 0.001f);
	bool _gray = (param_gray > 0.001f);
	bool _noise = (param_noise > 0.001f);
	bool _dual = (param_duality_h > 0.001f) || (param_duality_v > 0.001f);

	bool _menu_pp = g_pGamePersistent ? g_pGamePersistent->OnRenderPPUI_query() : false;

	bool _cmap = NeedColorMapping();

	bool _cbase = false;
	{
		int _r = color_get_R(param_color_base);
		_r = _abs(_r - int(0x7f));
		int _g = color_get_G(param_color_base);
		_g = _abs(_g - int(0x7f));
		int _b = color_get_B(param_color_base);
		_b = _abs(_b - int(0x7f));
		if (_r > 2 || _g > 2 || _b > 2) _cbase = true;
	}
	bool _cadd = false;
	{
		//int		_r	= color_get_R(param_color_add)	;
		//int		_g	= color_get_G(param_color_add)	;
		//int		_b	= color_get_B(param_color_add)	;
		//if (_r>2 || _g>2 || _b>2)	_cadd	= true	;
		int _r = _abs((int)(param_color_add.x * 255));
		int _g = _abs((int)(param_color_add.y * 255));
		int _b = _abs((int)(param_color_add.z * 255));
		if (_r > 2 || _g > 2 || _b > 2) _cadd = true;
	}
	return _blur || _gray || _noise || _dual || _cbase || _cadd || _cmap || _menu_pp;
}

BOOL CRenderTarget::Perform()
{
	return Available() &&
	(((BOOL)RImplementation.m_bMakeAsyncSS) || NeedPostProcess() || (ps_r__Supersample > 1) || (frame_distort == (
		Device.dwFrame - 1)));
}

#include <dinput.h>
#define SHOW(a)		Log			(#a,a);
#define SHOWX(a)	Msg			("%s %x",#a,a);

void CRenderTarget::Begin()
{
	/*
	if (g_pGameLevel->IR_GetKeyState(DIK_LSHIFT))	
	{
		Msg					("[%5d]------------------------",Device.dwFrame);
		SHOW				(param_blur)
		SHOW				(param_gray)
		SHOW				(param_duality_h)
		SHOW				(param_duality_v)
		SHOW				(param_noise)
		SHOW				(param_noise_scale)
		SHOW				(param_noise_fps)

		SHOWX				(param_color_base)
		SHOWX				(param_color_gray)
		SHOWX				(param_color_add)
	}
	*/

	// SVP-lite: SVP frame goes into the small offscreen RT (no supersample / post-process on it)
	svp_small_frame = SVPSmallFrame();
	if (svp_small_frame)
	{
		RCache.set_RT(rt_svp_work->pRT);
		RCache.set_ZB(zb_svp);
		curWidth = svp_w;
		curHeight = svp_h;
		D3DVIEWPORT9 svp_vp = {0, 0, curWidth, curHeight, 0, 1.f};
		CHK_DX(HW.pDevice->SetViewport(&svp_vp));
		Device.Clear();
		return;
	}

	if (!Perform())
	{
		// Base RT
		RCache.set_RT(HW.pBaseRT);
		RCache.set_ZB(HW.pBaseZB);
		curWidth = Device.dwWidth;
		curHeight = Device.dwHeight;
	}
	else
	{
		// Our 
		RCache.set_RT(RT->pRT);
		RCache.set_ZB(ZB);
		curWidth = rtWidth;
		curHeight = rtHeight;
	}
	Device.Clear();
}

struct TL_2c3uv
{
	Fvector4 p;
	u32 color0;
	u32 color1;
	Fvector2 uv [3];
	IC void set(float x, float y, u32 c0, u32 c1, float u0, float v0, float u1, float v1, float u2, float v2)
	{
		p.set(x, y, EPS_S, 1.f);
		color0 = c0;
		color1 = c1;
		uv[0].set(u0, v0);
		uv[1].set(u1, v1);
		uv[2].set(u2, v2);
	}
};

// D3D9 puts pixel centres on integer XYZRHW coordinates and maps clip-space -1 to pixel -0.5, so this is the clip
// position that rasterises exactly where the XYZRHW vertex would (w, h: the size of the render target)
static void tl_to_clip(TL_2c3uv& v, float w, float h)
{
	v.p.x = (v.p.x + .5f) * 2.f / w - 1.f;
	v.p.y = 1.f - (v.p.y + .5f) * 2.f / h;
	v.p.z = 0.f;
	v.p.w = 1.f;
}

// the element's first pass runs a vertex shader (its .s named one instead of "null")
static bool element_has_vs(ShaderElement* E)
{
	if (!E || 0 == E->passes.size()) return false;
	SPass* P = E->passes[0]._get();
	return P && P->vs._get() && P->vs->vs;
}

void CRenderTarget::DoAsyncScreenshot()
{
	//	Igor: screenshot will not have postprocess applied.
	//	TODO: fox that later
	if (RImplementation.m_bMakeAsyncSS)
	{
		HRESULT hr;

		IDirect3DSurface9* pFBSrc = HW.pBaseRT;
		//	Don't addref, no need to release.
		//ID3DTexture2D *pTex = RT->pSurface;

		//hr = pTex->GetSurfaceLevel(0, &pFBSrc);

		//	SHould be async function
		hr = HW.pDevice->GetRenderTargetData(pFBSrc, pFB);

		//pFBSrc->Release();

		RImplementation.m_bMakeAsyncSS = false;
	}
}

BOOL CRenderTarget::SVPSmallFrame()
{
	// the surfaces must exist (they are recreated with this object on every device reset)
	return svp_small && zb_svp && rt_svp_work._get() && rt_svp_work->pRT && rt_secondVP._get() && rt_secondVP->pRT &&
		Device.m_SecondViewport.IsSVPFrame();
}

void CRenderTarget::End()
{
	// SVP-lite: end of a small SVP frame - no PP-UI, distortion or post-process quad
	if (svp_small_frame)
	{
		svp_small_frame = FALSE;

		// maps normally emptied by the distortion phase
		RImplementation.GMBase.RGraph.mapStaticSorted.Distort.clear();
		RImplementation.GMBase.RGraph.mapDynamicSorted.Distort.clear();
		RImplementation.GMBase.RGraph.mapHUDSorted.Distort.clear();

		// keep the "previous frame had distortion" state alive across the skipped phase (see Perform())
		if (frame_distort == (Device.dwFrame - 1)) frame_distort = Device.dwFrame;

		// back to the backbuffer
		RCache.set_RT(HW.pBaseRT);
		RCache.set_ZB(HW.pBaseZB);
		curWidth = Device.dwWidth;
		curHeight = Device.dwHeight;
		D3DVIEWPORT9 base_vp = {0, 0, curWidth, curHeight, 0, 1.f};
		CHK_DX(HW.pDevice->SetViewport(&base_vp));

		// publish the finished image as $user$viewport2 (small, same-size GPU copy)
		if (FAILED(HW.pDevice->StretchRect(rt_svp_work->pRT, nullptr, rt_secondVP->pRT, nullptr, D3DTEXF_POINT)))
			D3DXLoadSurfaceFromSurface(rt_secondVP->pRT, 0, 0, rt_svp_work->pRT, 0, 0, D3DX_DEFAULT, 0);
		return;
	}

	if (g_pGamePersistent) g_pGamePersistent->OnRenderPPUI_main(); // PP-UI

	// find if distortion is needed at all
	BOOL bPerform = Perform();
	BOOL bDistort = RImplementation.o.distortion;
	BOOL bCMap = NeedColorMapping();
	bool _menu_pp = g_pGamePersistent ? g_pGamePersistent->OnRenderPPUI_query() : false;
	if ((0==RImplementation.GMBase.RGraph.mapStaticSorted.Distort.size() && 0 == RImplementation.GMBase.RGraph.mapDynamicSorted.Distort.size() && 0 == RImplementation.GMBase.RGraph.mapHUDSorted.Distort.size()) && !_menu_pp)	bDistort	= FALSE;
	if (bDistort)
	{
		phase_distortion();
	}

	// combination/postprocess
	RCache.set_RT(HW.pBaseRT);
	RCache.set_ZB(HW.pBaseZB);
	curWidth = Device.dwWidth;
	curHeight = Device.dwHeight;

	if (!bPerform) return;

	// R1 luminance adaptation: does nothing (not even create its targets) unless r1_lum_adapt or the bus turns it on
	lum_update();

	int gblend = clampr(iFloor((1 - param_gray) * 255.f), 0, 255);
	int nblend = clampr(iFloor((1 - param_noise) * 255.f), 0, 255);
	u32 p_color = subst_alpha(param_color_base, nblend);
	u32 p_gray = subst_alpha(param_color_gray, gblend);
	Fvector p_brightness = param_color_add;
	// Msg				("param_gray:%f(%d),param_noise:%f(%d)",param_gray,gblend,param_noise,nblend);
	// Msg				("base: %d,%d,%d",	color_get_R(p_color),		color_get_G(p_color),		color_get_B(p_color));
	// Msg				("gray: %d,%d,%d",	color_get_R(p_gray),		color_get_G(p_gray),		color_get_B(p_gray));
	// Msg				("add:  %d,%d,%d",	color_get_R(p_brightness),	color_get_G(p_brightness),	color_get_B(p_brightness));

	// Draw full-screen quad textured with our scene image
	u32 Offset;
	float _w = float(Device.dwWidth);
	float _h = float(Device.dwHeight);

	Fvector2 n0, n1, r0, r1, l0, l1;
	calc_tc_duality_ss(r0, r1, l0, l1);
	calc_tc_noise(n0, n1);

	// A pass that names a vertex shader (ps_3_0 + vs_3_0) gets the same quad in clip space, 4 vertices after the
	// XYZRHW ones; with only vertex shader "null" passes (vanilla) the buffer holds the 4 XYZRHW vertices as before.
	ShaderElement* E_cmap = bCMap ? (bDistort ? s_postprocess_D[1]->E[4]._get() : s_postprocess[1]->E[4]._get()) : nullptr;
	ShaderElement* E_main = (bDistort ? s_postprocess_D[bCMap] : s_postprocess[bCMap])->E[0]._get();
	const bool vs_cmap = element_has_vs(E_cmap);
	const bool vs_main = element_has_vs(E_main);
	const u32 n_verts = (vs_cmap || vs_main) ? 8 : 4;

	// Fill vertex buffer
	float du = ps_r1_pps_u, dv = ps_r1_pps_v;
	TL_2c3uv quad[4];
	quad[0].set(du + 0, dv + float(_h), p_color, p_gray, r0.x, r1.y, l0.x, l1.y, n0.x, n1.y);
	quad[1].set(du + 0, dv + 0, p_color, p_gray, r0.x, r0.y, l0.x, l0.y, n0.x, n0.y);
	quad[2].set(du + float(_w), dv + float(_h), p_color, p_gray, r1.x, r1.y, l1.x, l1.y, n1.x, n1.y);
	quad[3].set(du + float(_w), dv + 0, p_color, p_gray, r1.x, r0.y, l1.x, l0.y, n1.x, n0.y);
	TL_2c3uv* pv = (TL_2c3uv*)RCache.Vertex.Lock(n_verts, g_postprocess.stride(), Offset);
	CopyMemory(pv, quad, sizeof(quad));
	if (8 == n_verts)
	{
		for (u32 i = 0; i < 4; ++i)
			tl_to_clip(quad[i], _w, _h);
		CopyMemory(pv + 4, quad, sizeof(quad));
	}
	RCache.Vertex.Unlock(n_verts, g_postprocess.stride());

	static shared_str s_colormap = "c_colormap";
	static shared_str s_lum_info = "lum_adapt_info";
	if (bCMap)
	{
		RCache.set_RT(RT_color_map->pRT);

		//	Prepare colormapped buffer
		RCache.set_Element(E_cmap);
		RCache.set_Geometry(vs_cmap ? g_postprocess_vs : g_postprocess);
		RCache.set_c(s_colormap, param_color_map_influence, param_color_map_interpolate, 0, 0);
		RCache.set_c(s_lum_info, lum_info.x, lum_info.y, lum_info.z, lum_info.w);
		RCache.Render(D3DPT_TRIANGLELIST, vs_cmap ? Offset + 4 : Offset, 0, 4, 0, 2);

		RCache.set_RT(HW.pBaseRT);
		//return;
	}

	//	Element 0 for for normal post-process
	//	Element 4 for color map post-process
	//int iShaderElement	= bCMap ? 4 : 0;
	//RCache.set_Element	(bDistort ? s_postprocess_D->E[iShaderElement] : s_postprocess->E[iShaderElement]);

	RCache.set_Shader(bDistort ? s_postprocess_D[bCMap] : s_postprocess[bCMap]);

	//RCache.set_Shader	(bDistort ? s_postprocess_D : s_postprocess);


	// Actual rendering
	static shared_str s_brightness = "c_brightness";
	RCache.set_c(s_brightness, p_brightness.x, p_brightness.y, p_brightness.z, 0);
	RCache.set_c(s_colormap, param_color_map_influence, param_color_map_interpolate, 0, 0);
	RCache.set_c(s_lum_info, lum_info.x, lum_info.y, lum_info.z, lum_info.w);
	RCache.set_Geometry(vs_main ? g_postprocess_vs : g_postprocess);
	RCache.Render(D3DPT_TRIANGLELIST, vs_main ? Offset + 4 : Offset, 0, 4, 0, 2);
}

void CRenderTarget::phase_distortion()
{
	frame_distort = Device.dwFrame;
	RCache.set_RT(RT_distort->pRT);
	RCache.set_ZB(ZB);
	RCache.set_CullMode(CULL_CCW);
	RCache.set_ColorWriteEnable();
	CHK_DX(HW.pDevice->Clear ( 0L, NULL, D3DCLEAR_TARGET, color_rgba(127,127,127,127), 1.0f, 0L));

	if (g_pGameLevel && g_pGamePersistent && !g_pGamePersistent->OnRenderPPUI_query())
	{
		RImplementation.GMBase.r_dsgraph_render_distort();
	}
	else
	{
		RImplementation.GMBase.RGraph.mapStaticSorted.Distort.clear();
		RImplementation.GMBase.RGraph.mapDynamicSorted.Distort.clear();
		RImplementation.GMBase.RGraph.mapHUDSorted.Distort.clear();
	}

	if (g_pGamePersistent) g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
}

//////////////////////////////////////////////////////////////////////////
// R1 luminance adaptation (r1_lum_adapt). See R1-PS3-LUM-ADAPT-NOTES.md.
// Runs inside End() only on frames that draw the post-process quad (the only pass that can read it), and only when
// switched on. Off, it creates nothing and draws nothing.

bool CRenderTarget::lum_wanted(float& speed_up, float& speed_down)
{
	bool on = ps_r1_lum_adapt != 0;
	speed_up = ps_r1_lum_adapt_speed;
	speed_down = ps_r1_lum_adapt_speed_down;

	// bus lane "lum_adapt": x > 0 on, x < 0 off, x == 0 (or no such lane) leaves it to the console;
	// y > 0 / z > 0 replace the speeds while the lane holds them
	Fvector4 lane;
	if (ShaderBus::get("lum_adapt", lane))
	{
		if (lane.x > 0.f) on = true;
		else if (lane.x < 0.f) on = false;
		if (lane.y > 0.f) speed_up = lane.y;
		if (lane.z > 0.f) speed_down = lane.z;
	}
	return on;
}

bool CRenderTarget::lum_create()
{
	// Vanilla ships none of these; the mod that wants the adaptation ships them in gamedata\shaders\r1\.
	static const LPCSTR files[] = {"lum_adapt_meter", "lum_adapt_reduce", "lum_adapt_blend"};
	for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
	{
		string_path fn;
		strconcat(sizeof(fn), fn, ::Render->getShaderPath(), files[i], ".ps");
		if (!FS.exist("$game_shaders$", fn))
		{
			Msg("! r1_lum_adapt: shaders\\%s is missing, luminance adaptation stays off until vid_restart", fn);
			return false;
		}
	}

	const D3DFORMAT f16 = D3DFMT_A16B16G16R16F;
	const D3DFORMAT f32 = D3DFMT_A32B32G32R32F;
	const bool rt16 = !!HW.support(f16, D3DRTYPE_TEXTURE, D3DUSAGE_RENDERTARGET);
	const bool filter16 = rt16 && HW.support(f16, D3DRTYPE_TEXTURE, D3DUSAGE_QUERY_FILTER);
	const bool rt32 = !!HW.support(f32, D3DRTYPE_TEXTURE, D3DUSAGE_RENDERTARGET);
	if (!rt16 && !rt32)
	{
		Msg("! r1_lum_adapt: the GPU has no floating-point render target, luminance adaptation stays off");
		return false;
	}
	// the metered images are sampled bilinearly, the 1x1 history only by point; it needs float so that a slow
	// adaptation never stalls on 8-bit steps
	const D3DFORMAT f_small = filter16 ? f16 : D3DFMT_A8R8G8B8;
	const D3DFORMAT f_hist = rt32 ? f32 : f16;

	rt_lum_64.create(R1_RT_LUM_64, 64, 64, f_small);
	rt_lum_8.create(R1_RT_LUM_8, 8, 8, f_small);
	rt_lum_adapt.create(R1_RT_LUM_ADAPT, 1, 1, f_hist);
	rt_lum_prev.create(R1_RT_LUM_PREV, 1, 1, f_hist);
	if (!rt_lum_64->valid() || !rt_lum_8->valid() || !rt_lum_adapt->valid() || !rt_lum_prev->valid())
	{
		Msg("! r1_lum_adapt: can't create the adaptation render targets, luminance adaptation stays off");
		lum_destroy();
		return false;
	}

	b_lum = xr_new<CBlender_r1_lum_adapt>();
	s_lum.create(b_lum, "r1\\lum_adapt");
	if (DEV->_lua_HasShader("lum_adapt_meter"))
		s_lum_meter.create("lum_adapt_meter");

	// the history starts at zero, never at undefined (possibly NaN) memory
	RCache.set_ZB(NULL);
	RCache.set_RT(rt_lum_prev->pRT);
	CHK_DX(HW.pDevice->Clear(0L, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0L));
	RCache.set_RT(rt_lum_adapt->pRT);
	CHK_DX(HW.pDevice->Clear(0L, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0L));
	RCache.set_RT(HW.pBaseRT);
	RCache.set_ZB(HW.pBaseZB);

	lum_frame = 0;
	Msg("* r1_lum_adapt: ready (meter %s, %s 64x64/8x8, %s 1x1)",
	    s_lum_meter._get() ? "lum_adapt_meter.s" : "built-in binding",
	    filter16 ? "A16B16G16R16F" : "A8R8G8B8", rt32 ? "A32B32G32R32F" : "A16B16G16R16F");
	return true;
}

void CRenderTarget::lum_destroy()
{
	s_lum_meter.destroy();
	s_lum.destroy();
	xr_delete(b_lum);
	rt_lum_prev.destroy();
	rt_lum_adapt.destroy();
	rt_lum_8.destroy();
	rt_lum_64.destroy();
}

void CRenderTarget::lum_pass(ShaderElement* E, const ref_rt& target, float step_x, float step_y, const Fvector4* adapt)
{
	if (!E || 0 == E->passes.size()) return;

	const float w = float(target->dwWidth);
	const float h = float(target->dwHeight);

	// shifted half a pixel: every pixel's tc is the centre of its footprint (0..1 spans the whole source)
	const u32 white = 0xffffffff;
	TL_2c3uv quad[4];
	quad[0].set(-.5f, h - .5f, white, white, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f);
	quad[1].set(-.5f, -.5f, white, white, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
	quad[2].set(w - .5f, h - .5f, white, white, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
	quad[3].set(w - .5f, -.5f, white, white, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f);
	const bool vs = element_has_vs(E);
	if (vs)
	{
		for (u32 i = 0; i < 4; ++i)
			tl_to_clip(quad[i], w, h);
	}

	u32 Offset;
	TL_2c3uv* pv = (TL_2c3uv*)RCache.Vertex.Lock(4, g_postprocess.stride(), Offset);
	CopyMemory(pv, quad, sizeof(quad));
	RCache.Vertex.Unlock(4, g_postprocess.stride());

	static shared_str s_step = "c_lum_step";
	static shared_str s_adapt = "c_lum_adapt";
	RCache.set_RT(target->pRT);
	RCache.set_Element(E);
	RCache.set_c(s_step, step_x, step_y, w, h);
	if (adapt)
		RCache.set_c(s_adapt, adapt->x, adapt->y, adapt->z, adapt->w);
	RCache.set_Geometry(vs ? g_postprocess_vs : g_postprocess);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

void CRenderTarget::lum_update()
{
	lum_info.set(0.f, 0.f, 0.f, 0.f);

	float speed_up, speed_down;
	if (lum_state < 0 || !lum_wanted(speed_up, speed_down))
		return;
	if (0 == lum_state)
	{
		lum_state = lum_create() ? 1 : -1;
		if (lum_state < 0)
			return;
	}

	// a gap (adaptation was off, or no post-process for a while) starts over at the metered value
	const bool fresh = lum_frame && (Device.dwFrame - lum_frame) <= 3;

	// the scope picture of a full-size second viewport frame is not metered, it reuses the value of the main view
	if (Device.m_SecondViewport.IsSVPFrame())
	{
		if (fresh)
			lum_info.set(1.f, lum_time, speed_up, speed_down);
		return;
	}

	Fvector4 adapt; // c_lum_adapt: blend factor towards a brighter / darker scene, dt, 1 = start over
	if (fresh)
	{
		const float dt = clampr(Device.fTimeGlobal - lum_last_time, 0.f, 0.25f);
		lum_time += dt;
		adapt.set(1.f - expf(-dt * speed_up), 1.f - expf(-dt * speed_down), dt, 0.f);
	}
	else
	{
		lum_time = 0.f;
		adapt.set(1.f, 1.f, 0.f, 1.f);
	}
	lum_last_time = Device.fTimeGlobal;

	ShaderElement* meter = s_lum_meter._get() ? s_lum_meter->E[0]._get() : nullptr;
	if (!meter || 0 == meter->passes.size())
		meter = s_lum->E[3]._get();

	RCache.set_ZB(NULL);
	lum_pass(meter, rt_lum_64, 1.f / 64.f, 1.f / 64.f, nullptr); // scene -> 64x64
	lum_pass(s_lum->E[0]._get(), rt_lum_8, 1.f / 8.f, 1.f / 8.f, nullptr); // 64x64 -> 8x8
	lum_pass(s_lum->E[1]._get(), rt_lum_adapt, 1.f, 1.f, &adapt); // 8x8 -> 1x1, blended with the history
	lum_pass(s_lum->E[2]._get(), rt_lum_prev, 0.f, 0.f, nullptr); // 1x1 -> history
	RCache.set_RT(HW.pBaseRT);
	RCache.set_ZB(HW.pBaseZB);

	lum_frame = Device.dwFrame;
	lum_info.set(1.f, lum_time, speed_up, speed_down);
}
