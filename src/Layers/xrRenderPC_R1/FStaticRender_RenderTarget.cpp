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
	svp_vp2_w = 0;
	svp_vp2_h = 0;
	svp_vp2_lens = 0;
	svp_lens_valid = FALSE;
	svp_lens_frame = 0;
	svp_lens_margin = 0;
	svp_lens_hat = 1.f;
	svp_lens_aspect = 1.f;
	svp_lens_fov = 0.f;
	svp_lens_view.identity();
	svp_shift_x = 0;
	svp_shift_y = 0;
	svp_stat_forced = 0;
	svp_stat_copies = 0;

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
	for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
	{
		lum_rb[i] = nullptr;
		lum_rb_frame[i] = 0;
	}

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
		// SVP lens (r1_svp_fit): the lens shaders read the middle height/width of $user$viewport2's width and all of
		// its height as the lens square. Shape viewport2 so that square is exactly the lens square of the work
		// image (lens x lens pixels, copied 1:1): height = lens, width = lens * width/height of the screen.
		u32 vp2_w = svp_sz, vp2_h = svp_sz, vp2_lens = 0;
		if (ps_r1_svp_fit && Device.dwWidth > Device.dwHeight && Device.dwHeight > 0)
		{
			const u32 lens = svp_sz - 2 * CRenderDevice::CSecondVPParams::LensMarginPx(svp_sz);
			const float wide = float(lens) * float(Device.dwWidth) / float(Device.dwHeight);
			// same parity as 'lens', so the lens square sits exactly in the middle
			const u32 w = lens + 2 * u32(iFloor((wide - float(lens)) * .5f + .5f));
			if (w <= 4096)
			{
				vp2_w = w;
				vp2_h = lens;
				vp2_lens = lens;
			}
		}
		rt_secondVP.create(RTname_SecondVP, vp2_w, vp2_h, HW.Caps.fTarget);
		if (vp2_lens && !rt_secondVP->valid())
		{
			Msg("! SVP lens: can't create %ux%u lens-shaped second viewport, using %ux%u", vp2_w, vp2_h, svp_sz, svp_sz);
			rt_secondVP.destroy();
			vp2_w = vp2_h = svp_sz;
			vp2_lens = 0;
			rt_secondVP.create(RTname_SecondVP, vp2_w, vp2_h, HW.Caps.fTarget);
		}
		rt_svp_work.create(RTname_SecondVP_work, svp_sz, svp_sz, HW.Caps.fTarget);
		HRESULT svp_hr = E_FAIL;
		if (rt_secondVP->valid() && rt_svp_work->valid())
			svp_hr = HW.pDevice->CreateDepthStencilSurface(svp_sz, svp_sz, HW.Caps.fDepth, D3DMULTISAMPLE_NONE, 0, TRUE, &zb_svp, NULL);
		if (SUCCEEDED(svp_hr) && zb_svp)
		{
			svp_small = TRUE;
			svp_w = svp_sz;
			svp_h = svp_sz;
			svp_vp2_w = vp2_w;
			svp_vp2_h = vp2_h;
			svp_vp2_lens = vp2_lens;
			Msg("* SVP-lite: small second viewport %ux%u", svp_sz, svp_sz);
			if (vp2_lens)
				Msg("* SVP lens: $user$viewport2 %ux%u, lens square %ux%u", vp2_w, vp2_h, vp2_lens, vp2_lens);
			// default-pool targets start undefined; clear so the lens is black, not garbage, until the first SVP frame is published
			HW.pDevice->ColorFill(rt_secondVP->pRT, nullptr, D3DCOLOR_XRGB(0, 0, 0));
			HW.pDevice->ColorFill(rt_svp_work->pRT, nullptr, D3DCOLOR_XRGB(0, 0, 0));
			// lets CCameraManager::ApplyDevice build the lens-fitted projection (r1_svp_fit)
			Device.m_SecondViewport.lensWork = svp_sz;
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
	{
		Device.m_SecondViewport.lensWork = 0;
		rt_secondVP.create(RTname_SecondVP, Device.dwWidth, Device.dwHeight, HW.Caps.fTarget); //--#SM+#-- +SecondVP+
	}
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
	Device.m_SecondViewport.lensWork = 0;
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

	// SVP lens: on a normal frame, move the lens image with the aim (r1_svp_reproject)
	svp_reproject();

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

// copy a rect between two render-target surfaces: GPU StretchRect (point for a 1:1 copy, linear when scaled), the old
// D3DX copy if the driver refuses
static void svp_copy(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const RECT* dst_rect, bool same_size)
{
	if (SUCCEEDED(HW.pDevice->StretchRect(src, src_rect, dst, dst_rect, same_size ? D3DTEXF_POINT : D3DTEXF_LINEAR)))
		return;
	if (!same_size && SUCCEEDED(HW.pDevice->StretchRect(src, src_rect, dst, dst_rect, D3DTEXF_POINT)))
		return;
	D3DXLoadSurfaceFromSurface(dst, 0, dst_rect, src, 0, src_rect, D3DX_DEFAULT, 0);
}

void CRenderTarget::svp_lens_rect(u32 lens, RECT& r)
{
	const u32 W = svp_vp2_w, H = svp_vp2_h;
	if (svp_vp2_lens && lens == svp_vp2_lens)
	{
		// viewport2 was shaped for this lens square: 1:1, in the middle
		r.left = LONG((W - lens) / 2);
		r.top = 0;
		r.right = r.left + LONG(lens);
		r.bottom = LONG(lens);
		return;
	}
	// otherwise the lens shaders read the full height and the middle height/width of the width (scaled copy)
	u32 w = u32(clampr(iFloor(float(W) * float(Device.dwHeight) / float(Device.dwWidth) + .5f), 1, int(W)));
	if ((W - w) & 1)
		w += (w < W) ? 1 : u32(-1);
	r.left = LONG((W - w) / 2);
	r.top = 0;
	r.right = r.left + LONG(w);
	r.bottom = LONG(H);
}

void CRenderTarget::svp_publish(int shift_x, int shift_y)
{
	const int M = int(svp_lens_margin);
	const u32 lens = svp_w - 2 * svp_lens_margin;
	RECT src;
	src.left = M + shift_x;
	src.top = M + shift_y;
	src.right = src.left + LONG(lens);
	src.bottom = src.top + LONG(lens);
	RECT dst;
	svp_lens_rect(lens, dst);
	const bool same_size = u32(dst.right - dst.left) == lens && u32(dst.bottom - dst.top) == lens;
	// the lens edge reads up to half a texel past the square (the width of viewport2 is rounded): copy one more
	// column on each side when there is room, so bilinear filtering does not blend in the unused sides
	if (same_size && dst.left >= 1 && u32(dst.right) + 1 <= svp_vp2_w && src.left >= 1 && u32(src.right) + 1 <= svp_w)
	{
		src.left--;
		src.right++;
		dst.left--;
		dst.right++;
	}
	svp_copy(rt_svp_work->pRT, &src, rt_secondVP->pRT, &dst, same_size);
}

void CRenderTarget::svp_reproject()
{
	CRenderDevice::CSecondVPParams& svp = Device.m_SecondViewport;
	// no margin = nothing to shift into (r1_svp_margin 0): keep the plain schedule
	if (!svp_lens_valid || !svp_lens_margin || !ps_r1_svp_reproject || !svp.IsSVPActive() || svp.IsSVPFrame())
		return;
	if (!svp_small || !rt_svp_work._get() || !rt_svp_work->pRT || !rt_secondVP._get() || !rt_secondVP->pRT)
		return;

	bool fresh = false;

	// an old image (the scope was lowered and raised again, a pause in SVP frames): render a new one
	if (Device.dwFrame - svp_lens_frame > 2u * svp.GetSVPFrameDelay() + 2u)
		fresh = true;

	// the zoom changed: the shift would not be right any more
	if (g_pGamePersistent && _abs(g_pGamePersistent->m_pGShaderConstants->hud_params.y - svp_lens_fov) > 0.01f)
		fresh = true;

	// where the current aim lands in the last SVP image (pure rotation; moving the camera is not compensated)
	int sx = svp_shift_x, sy = svp_shift_y;
	Fvector v;
	svp_lens_view.transform_dir(v, Device.vCameraDirection);
	if (v.z > EPS_L && svp_lens_hat > EPS_S)
	{
		const float half = float(svp_w) * .5f;
		const float fx = (v.x / v.z) * svp_lens_aspect / svp_lens_hat * half; // + = right
		const float fy = -(v.y / v.z) / svp_lens_hat * half; // + = down (pixel rows)
		const int M = int(svp_lens_margin);
		const float lim = float(M) + 1.f;
		sx = iFloor(clampr(fx, -lim, lim) + .5f);
		sy = iFloor(clampr(fy, -lim, lim) + .5f);
		if (sx < -M || sx > M || sy < -M || sy > M)
		{
			// out of margin: show the edge of what we have and render a new image on the next frame
			fresh = true;
			sx = clampr(sx, -M, M);
			sy = clampr(sy, -M, M);
		}
	}
	else
		fresh = true;

	if (fresh)
		svp.RequestSVPFrame();

	if (sx != svp_shift_x || sy != svp_shift_y)
	{
		svp_shift_x = sx;
		svp_shift_y = sy;
		svp_publish(sx, sy);
		svp_stat_copies++;
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

		CRenderDevice::CSecondVPParams& svp = Device.m_SecondViewport;
		if (svp.forceFrame == Device.dwFrame)
			svp_stat_forced++;

		if (svp.lensFitFrame == Device.dwFrame && svp.lensMarginPx * 4 <= svp_w)
		{
			// SVP lens: this frame used the lens-fitted projection (CCameraManager::ApplyDevice): keep what is needed
			// to shift it later and publish its lens square
			svp_lens_valid = TRUE;
			svp_lens_frame = Device.dwFrame;
			svp_lens_margin = svp.lensMarginPx;
			svp_lens_hat = svp.lensHAT;
			svp_lens_aspect = svp.lensAspect;
			svp_lens_fov = svp.lensFov;
			svp_lens_view.set(Device.mView);
			svp_shift_x = 0;
			svp_shift_y = 0;
			svp_publish(0, 0);
			return;
		}

		// publish the finished image as $user$viewport2 (small GPU copy; scaled if viewport2 was shaped for the lens)
		svp_lens_valid = FALSE;
		svp_copy(rt_svp_work->pRT, nullptr, rt_secondVP->pRT, nullptr, svp_vp2_w == svp_w && svp_vp2_h == svp_h);
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

	if (!bPerform)
	{
		// the adaptation starts over after more than 3 frames without the post-process quad; so does its CPU copy
		if (lum_frame && (Device.dwFrame - lum_frame) > 3)
			lum_readback_clear();
		return;
	}

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

	// CPU copies for r1_lum_adapt_value (scripts). Optional: without them the adaptation still runs, the value stays 0
	for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
	{
		lum_rb_frame[i] = 0;
		if (FAILED(HW.pDevice->CreateOffscreenPlainSurface(1, 1, f_hist, D3DPOOL_SYSTEMMEM, &lum_rb[i], NULL)) || !lum_rb[i])
		{
			Msg("! r1_lum_adapt: can't create the CPU copy, r1_lum_adapt_value stays 0");
			for (u32 j = 0; j < LUM_RB_SLOTS; ++j)
				_RELEASE(lum_rb[j]);
			break;
		}
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
	lum_readback_clear();
	for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
		_RELEASE(lum_rb[i]);
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
	{
		if (lum_state > 0)
			lum_readback_clear();
		return;
	}
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
		{
			lum_info.set(1.f, lum_time, speed_up, speed_down);
			lum_readback(false); // nothing new to copy, but a copy of the main view may have finished
		}
		else
			lum_readback_clear();
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
		lum_readback_clear(); // copies from before the gap are stale
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

	lum_readback(true);
}

//////////////////////////////////////////////////////////////////////////
// Async CPU copy of $user$lum_adapt for scripts: console outputs r1_lum_adapt_value (.a) and r1_lum_adapt_rgb (.rgb).
// Each run queues GetRenderTargetData into one of LUM_RB_SLOTS 1x1 system-memory surfaces and reads the newest copy
// that is at least 2 frames old with D3DLOCK_DONOTWAIT, which never waits: a copy the GPU has not finished answers
// D3DERR_WASSTILLDRAWING and the previous value stays (an older copy is tried next). Four slots so that one copy is
// always finished even with three frames queued ahead (the D3D9 default) on a GPU-bound machine.

static float lum_half_to_float(u16 h)
{
	const u32 sign = u32(h & 0x8000) << 16;
	u32 exp = (h >> 10) & 0x1f;
	u32 man = h & 0x3ff;
	u32 bits;
	if (0 == exp)
	{
		if (0 == man)
			bits = sign; // +-0
		else
		{
			// denormal: normalise
			exp = 127 - 15 + 1;
			while (!(man & 0x400))
			{
				man <<= 1;
				--exp;
			}
			man &= 0x3ff;
			bits = sign | (exp << 23) | (man << 13);
		}
	}
	else if (31 == exp)
		bits = sign | 0x7f800000 | (man << 13); // inf / NaN
	else
		bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
	float f;
	CopyMemory(&f, &bits, sizeof(f));
	return f;
}

void CRenderTarget::lum_readback(bool queue)
{
	if (!lum_rb[0])
		return; // no CPU copy (creation failed), r1_lum_adapt_value stays 0

	const u32 now = Device.dwFrame;

	// read: newest copy that is at least 2 frames old first, then older ones
	u32 below = u32(-1); // only copies queued before this frame are still worth trying
	for (;;)
	{
		u32 best = LUM_RB_SLOTS;
		for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
		{
			const u32 f = lum_rb_frame[i];
			if (f && f < below && (now - f) >= 2 && (LUM_RB_SLOTS == best || f > lum_rb_frame[best]))
				best = i;
		}
		if (LUM_RB_SLOTS == best)
			break;

		D3DLOCKED_RECT lr;
		const HRESULT hr = lum_rb[best]->LockRect(&lr, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
		if (D3DERR_WASSTILLDRAWING == hr)
		{
			below = lum_rb_frame[best]; // not finished: keep it for next frame, try an older copy now
			continue;
		}
		if (FAILED(hr))
		{
			lum_rb_frame[best] = 0; // lost copy, forget it
			continue;
		}

		float v[4];
		if (D3DFMT_A32B32G32R32F == rt_lum_adapt->fmt)
			CopyMemory(v, lr.pBits, sizeof(v));
		else
		{
			const u16* hv = (const u16*)lr.pBits;
			for (u32 c = 0; c < 4; ++c)
				v[c] = lum_half_to_float(hv[c]);
		}
		lum_rb[best]->UnlockRect();

		if (_valid(v[0]) && _valid(v[1]) && _valid(v[2]) && _valid(v[3]))
		{
			ps_r1_lum_adapt_value = v[3];
			ps_r1_lum_adapt_rgb.set(v[0], v[1], v[2]);
		}

		// this copy and every older one are done with
		const u32 done = lum_rb_frame[best];
		for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
		{
			if (lum_rb_frame[i] && lum_rb_frame[i] <= done)
				lum_rb_frame[i] = 0;
		}
		break;
	}

	if (!queue)
		return;

	// queue this frame's copy into a free slot, else over the oldest one
	u32 slot = 0;
	for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
	{
		if (!lum_rb_frame[i])
		{
			slot = i;
			break;
		}
		if (lum_rb_frame[i] < lum_rb_frame[slot])
			slot = i;
	}
	if (SUCCEEDED(HW.pDevice->GetRenderTargetData(rt_lum_adapt->pRT, lum_rb[slot])))
		lum_rb_frame[slot] = now ? now : 1;
	else
		lum_rb_frame[slot] = 0;
}

void CRenderTarget::lum_readback_clear()
{
	for (u32 i = 0; i < LUM_RB_SLOTS; ++i)
		lum_rb_frame[i] = 0;
	ps_r1_lum_adapt_value = 0.f;
	ps_r1_lum_adapt_rgb.set(0.f, 0.f, 0.f);
}
