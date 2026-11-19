#include "stdafx.h"

#include "../../xrEngine/render.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "CHudInitializer.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "flod.h"

#include "../../xrEngine/xr_object.h"

using namespace R_dsgraph;

extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

#if	RENDER==R_R1
// ---------------------------------------------------------------------------------------------
// Guarded front-to-back sort of a spatial list (R1 sorts lstRenderables and lstLights this way).
//
// The old code handed std::sort a comparator that read spatial.sphere.P of both objects on every
// comparison. That is only safe when each comparison of the same pair always gives the same answer.
// It does not: the scheduler and particle tasks run on other threads while the frame is being
// calculated (see CRenderDevice::on_idle / XRay::Engine::GameThread) and CObject::spatial_move()
// rewrites sphere.P in place, so a position can change in the middle of the sort. std::sort then
// walks past the start of the vector (native crash in _Insertion_sort_unchecked). A null entry or
// a NaN position also made the old comparator an invalid ordering.
//
// Here every position is read exactly once, turned into an integer key, and only the keys are
// sorted. Entries that cannot be ordered (null, or a position / radius that is not a finite
// number) are left out of the list for this frame and reported in the log by name.
// Cost: one pass over the list plus a sort of 64-bit integers; the scratch vectors keep their
// capacity between frames, so there is no allocation once they have grown.
// ---------------------------------------------------------------------------------------------
namespace
{
	ICF u32 spatial_sort_float_bits(float value) noexcept
	{
		u32 bits;
		memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	// false for NaN and +-Inf (exponent bits all set)
	ICF bool spatial_sort_is_finite(float value) noexcept
	{
		return (spatial_sort_float_bits(value) & 0x7f800000u) != 0x7f800000u;
	}

	// Log limiter: an object that stays broken is reported once, and no matter how many different
	// objects break, the log gets at most 32 lines plus one line per 60 frames after that.
	// R1 calculates the frame on the main thread only, so plain statics are enough here.
	bool spatial_sort_report_allowed(u64 identity)
	{
		static u64 recent[16] = {};
		static u32 recent_next = 0;
		static u32 lines_total = 0;
		static u32 last_frame = 0;

		for (u64 known : recent)
			if (known == identity) return false;

		if (lines_total >= 32 && (Device.dwFrame - last_frame) < 60)
			return false;

		recent[recent_next] = identity;
		recent_next = (recent_next + 1) % 16;
		++lines_total;
		last_frame = Device.dwFrame;
		return true;
	}

	void spatial_sort_report(ISpatial* spatial, const Fvector& P, float R, LPCSTR list_name)
	{
		if (CObject* object = spatial->dcast_CObject())
		{
			// top bit set: can never collide with a pointer value
			if (!spatial_sort_report_allowed(0x8000000000000000ull | u64(object->ID())))
				return;

			LPCSTR name = object->cName().c_str();
			LPCSTR section = object->cNameSect().c_str();
			Msg("! [render] object '%s' (section '%s', id %u) has a broken position [%f, %f, %f], radius %f: left out of the %s list (reported once)",
				name ? name : "?", section ? section : "?", u32(object->ID()), P.x, P.y, P.z, R, list_name);
		}
		else
		{
			if (!spatial_sort_report_allowed(u64(size_t(spatial))))
				return;

			Msg("! [render] spatial %p (type 0x%x, no game object) has a broken position [%f, %f, %f], radius %f: left out of the %s list (reported once)",
				(void*)spatial, spatial->spatial.type, P.x, P.y, P.z, R, list_name);
		}
	}

	void spatial_sort_by_distance(xr_vector<ISpatialShared>& list, xr_vector<u64>& keys,
	                              xr_vector<ISpatialShared>& scratch, LPCSTR list_name)
	{
		const u32 count = u32(list.size());
		if (0 == count) return;

		// one copy of the camera position for the whole sort
		const Fvector eye = Device.vCameraPosition_saved;
		if (!spatial_sort_is_finite(eye.x) || !spatial_sort_is_finite(eye.y) || !spatial_sort_is_finite(eye.z))
			return; // no usable camera: keep the list as it is, the order is only an optimisation

		keys.clear();
		for (u32 it = 0; it < count; ++it)
		{
			ISpatial* spatial = list[it].get();
			if (0 == spatial) continue; // dropped

			// read the sphere once; from here on only this copy is used
			const Fvector P = spatial->spatial.sphere.P;
			const float R = spatial->spatial.sphere.R;

			// A sum of squares is never negative, and it is NaN or Inf whenever any coordinate is,
			// so this one test covers the whole position (and a distance that overflows).
			const float distance = eye.distance_to_sqr(P);
			if (!spatial_sort_is_finite(distance) || !spatial_sort_is_finite(R))
			{
				spatial_sort_report(spatial, P, R, list_name);
				continue; // dropped
			}

			// For finite floats >= 0 the bit pattern orders the same way as the value. The index in
			// the low half makes every key unique, so the order is total and repeatable.
			keys.push_back((u64(spatial_sort_float_bits(distance)) << 32) | u64(it));
		}

		std::sort(keys.begin(), keys.end());

		// Rebuild the list in key order. The pointers are swapped across, so no reference counts
		// change; what stays behind in the old storage are the dropped entries.
		const u32 kept = u32(keys.size());
		scratch.clear();
		scratch.resize(kept);
		for (u32 it = 0; it < kept; ++it)
			scratch[it].swap(list[u32(keys[it] & 0xffffffffull)]);

		list.swap(scratch);
		scratch.clear(); // releases the dropped entries, keeps the capacity
	}
}
#endif

ICF bool R4FullDetailRejectSpatial(ISpatial* spatial, bool normalPhase)
{
#if RENDER == R_R4
	if (ps_r4_full_detail_distance_scale >= 1.f || !normalPhase || !spatial || !g_pGamePersistent ||
		!g_pGamePersistent->Environment().CurrentEnv)
		return false;

	const float farPlane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	const float fullDetailDistance = std::max(100.f, farPlane * ps_r4_full_detail_distance_scale);
	if (Device.vCameraPosition.distance_to_sqr(spatial->spatial.sphere.P) <=
		_sqr(fullDetailDistance + spatial->spatial.sphere.R))
		return false;

	if (spatial->spatial.type & STYPE_LIGHTSOURCE)
	{
		light* L = (light*)spatial->dcast_Light();
		if (L && L->flags.bHudMode)
			return false;
	}

	return true;
#else
	return false;
#endif
}

ICF float calcLOD(float ssa/*fDistSq*/, float R)
{
	return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

namespace
{
class CCustomFOVInitializer
{
	bool enabled;
	bool near_depth;
	Fmatrix saved_project;
	Fmatrix saved_full;

public:
	explicit CCustomFOVInitializer(float fov, bool adjust_depth = false)
		: enabled(fov != 0.f), near_depth(adjust_depth)
	{
		if (!enabled) return;
		saved_project = RCache.get_xform_project();
		saved_full = Device.mFullTransform;
		Fmatrix project;
		project.build_projection(deg2rad(fov), Device.fASPECT, R_VIEWPORT_NEAR,
			g_pGamePersistent->Environment().CurrentEnv->far_plane);
		if (near_depth) RImplementation.rmNear();
		Device.mFullTransform.mul(project, Device.mView);
		RCache.set_xform_project(project);
	}

	~CCustomFOVInitializer()
	{
		if (!enabled) return;
		if (near_depth) RImplementation.rmNormal();
		Device.mFullTransform = saved_full;
		RCache.set_xform_project(saved_project);
	}
};
}

template<typename T, bool Reverse>
void CDSGraphManager::r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear)
{
    if (graph.empty())
        return;

    std::sort(graph.begin(), graph.end());

	for (auto& item : graph)
	{
		dxRender_Visual* V = item.pVisual;
		VERIFY(V && V->shader._get());
		RCache.set_Element(item.pSE);
		CCustomFOVInitializer projection(item.fov);
		RCache.set_xform_world(*item.pMatrix);
		RImplementation.apply_object(item.pObject);
		RImplementation.apply_lmaterial();
		//if (item.b_hud_mode)
		//{
		//	//new feature
		//}
		V->Render(calcLOD(item.ssa, V->vis.sphere.R));
	}

	if (_clear)
		graph.clear();

	RCache.set_xform_world(Fidentity);
}

void CDSGraphManager::r_dsgraph_render_graph(RenderQueueArray& queues, u32 _priority, bool _clear, bool static_geometry)
{
	RCache.set_xform_world(Fidentity);

	for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
	{
		auto& queue = queues[_priority][iPass];
		if (queue.empty())
			continue;

		// 1. Sort by generated sort key to replicate previous fixed map behaviour
		if (queue.size() < 4096)
            std::sort(queue.begin(), queue.end());
		else
            xr_parallel_sort(queue.begin(), queue.end());

		// 2. Render
		vs_type pVS = nullptr;

#if defined(USE_DX10) || defined(USE_DX11)
		gs_type pGS = nullptr;
#endif

		ps_type pPS = nullptr;

#ifdef USE_DX11
		hs_type pHS = nullptr;
		ds_type pDS = nullptr;
#endif
		
		R_constant_table* pCS = nullptr;
		ID3DState* pState = nullptr;
		STextureList* pTextures = nullptr;

        u64 high = 0;

        for (auto& packet : queue)
        {
            auto& currentKey = packet.sortKey;
            if (currentKey.high != high)
            {
                high = currentKey.high;

                if (packet.pState != pState)
                {
                    pState = packet.pState;
                    RCache.set_States(pState);
                }

#if defined(USE_DX10) || defined(USE_DX11)
                if (packet.pGS != pGS)
                {
                    pGS = packet.pGS;
                    RCache.set_GS(pGS);
                }
#endif

#ifdef USE_DX11
                if (packet.pHS != pHS)
                {
                    pHS = packet.pHS;
                    RCache.set_HS(pHS);
                }
                if (packet.pDS != pDS)
                {
                    pDS = packet.pDS;
                    RCache.set_DS(pDS);
                }
#endif
            }

            // Compare low key stuff regardless, too high collision probability
            if (packet.pVS != pVS)
            {
                pVS = packet.pVS;
                RCache.set_VS(pVS);
            }

            if (packet.pPS != pPS)
            {
                pPS = packet.pPS;
                RCache.set_PS(pPS);
            }

            if (packet.pCS != pCS)
            {
                pCS = packet.pCS;
                RCache.set_Constants(pCS);
            }

            if (packet.pTextures != pTextures)
            {
                pTextures = packet.pTextures;
                RCache.set_Textures(pTextures);
                RImplementation.apply_lmaterial();
            }

			auto& item = packet.item;
			// Opaque custom-FOV geometry keeps all of its original material passes.
			CCustomFOVInitializer projection(item.fov, true);
			if (!static_geometry)
			{
				RCache.set_xform_world(*item.pMatrix);
				RImplementation.apply_object(item.pObject);
				RImplementation.apply_lmaterial();
			}

			float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
			RCache.LOD.set_LOD(LOD);
#endif
			item.pVisual->Render(LOD);
		}

		if (_clear)
			queue.clear();
	}
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void CDSGraphManager::r_dsgraph_render_hud()
{
	PROF_EVENT("r_dsgraph_render_hud");
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUD);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOV);

	if (!RGraph.mapCamAttached.empty())
	{
		initializer.SetCamMode();
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttached);
	}
	RImplementation.rmNormal();

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	if (scope_3D_fake_enabled)
	{
		RCache.set_RT(RImplementation.Target->rt_ssfx_temp->pRT, 3); // Render scope_3D to any buffer

		initializer.SetHudMode();
		r_dsgraph_render_graph_sorted(RGraph.mapScopeHUD);
		initializer.SetCamMode();
		r_dsgraph_render_graph_sorted(RGraph.mapScopeCamAttached);
		initializer.SetHudMode();

		if (!RImplementation.o.ssfx_motionvectors)
			RCache.set_RT(NULL, 3);
		else
			RCache.set_RT(RImplementation.Target->rt_ssfx_motion_vectors->pRT, 3);
	}
	RGraph.mapScopeHUD.clear();
	RGraph.mapScopeCamAttached.clear();
#endif
}

void CDSGraphManager::r_dsgraph_render_hud_ui()
{
	PROF_EVENT("r_dsgraph_render_hud_ui");
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	g_hud->RenderActiveItemUI();
	RImplementation.rmNormal();
}

void CDSGraphManager::r_dsgraph_render_cam_ui()
{
	PROF_EVENT("r_dsgraph_render_cam_ui");

	// Change projection
	CHudInitializer initializer(2);
	
	// Rendering
	RImplementation.rmNear();
	g_hud->RenderCamAttachedUI();
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_sorted(bool render_hud)
{
	r_dsgraph_render_ui();
	{
		PROF_EVENT("r_dsgraph_render_sorted");
		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Sorted, true);
	}

	if (render_hud)
		r_dsgraph_render_sorted_hud();

}

void CDSGraphManager::r_dsgraph_capture_hud()
{
	if (g_hud)
	{
		g_hud->Render_Last(dcast_IPortalTraverser());
		set_Object();
	}
}

#if defined(USE_DX11)
//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_ScopeSorted()  //  Redotix99: for 3D Shader Based Scopes 	
{
	// Change projection
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeHUDSorted, true);
	initializer.SetCamMode();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeCamAttachedSorted, true);
	RImplementation.rmNormal();
}
#endif

void CDSGraphManager::r_dsgraph_render_sorted_hud()
{
	PROF_EVENT("r_dsgraph_render_sorted_hud");
#if	RENDER==R_R4
	HW.pContext->CopyResource(RImplementation.Target->rt_Accumulator->pSurface, RImplementation.Target->rt_Generic_0->pSurface);
#endif
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, true);
	initializer.SetCamMode();
	r_dsgraph_render_graph_sorted(RGraph.mapCamAttachedSorted.Sorted, true);
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_emissive(bool clear, bool renderHUD)
{
	PROF_EVENT("r_dsgraph_render_emissive");
#if	RENDER!=R_R1
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Emissive, clear);
	
	//	HACK: Calculate this only once
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Emissive, clear);
	
	if (renderHUD)
		r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, false);

	RImplementation.rmNormal();
#endif
}

void CDSGraphManager::r_dsgraph_render_water_ssr()
{
#ifdef USE_DX11
	PROF_EVENT("r_dsgraph_render_water_ssr");
	std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
	for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

		RCache.set_Shader(RImplementation.Target->s_ssfx_water_ssr);

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		RCache.set_c("cam_pos", RImplementation.Target->Position_previous.x, RImplementation.Target->Position_previous.y, RImplementation.Target->Position_previous.z, 0.0f);

		// Previous matrix data
		RCache.set_c("m_current", RImplementation.Target->Matrix_current);
		RCache.set_c("m_previous", RImplementation.Target->Matrix_previous);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
#endif
}

void CDSGraphManager::r_dsgraph_render_water()
{
	PROF_EVENT("r_dsgraph_render_water_ssr");
    std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
    for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

#ifdef USE_DX11
		if (RImplementation.o.ssfx_water)
		{
			RCache.set_Shader(RImplementation.Target->s_ssfx_water);
		}
#endif

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		// Wind settings
		float WindDir = g_pGamePersistent->Environment().CurrentEnv->wind_direction;
		float WindVel = g_pGamePersistent->Environment().CurrentEnv->wind_velocity;
		RCache.set_c("wind_setup", WindDir, WindVel, 0, 0);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
	RGraph.mapWater.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_wmarks()
{
	PROF_EVENT("r_dsgraph_render_wmarks");
#if	RENDER!=R_R1
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Wmark);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Wmark);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Wmark);
	RImplementation.rmNormal();
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_distort()
{
	PROF_EVENT("r_dsgraph_render_distort");
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Distort, true);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Distort);
	RImplementation.rmNormal();
}

#if RENDER == R_R1
//////////////////////////////////////////////////////////////////////////
// R1 thermal (r__heatvision): redraw the warm visuals queued by r1_heat_insert into the bound heat mask.
// The caller (CRenderTarget::phase_heat) has bound the target and the depth buffer of the scene that was just drawn;
// the heat passes test depth and never write it, so only the visible part of a warm thing lands in the mask.
// A model is drawn with the vertex shader variant, world matrix and LOD the scene pass used, so its depth matches
// what that pass wrote. It writes hotness (red), the hands flag (green) and the edge term (alpha).
// A warm particle effect is drawn with its own sprites and texture, MAX-blended into red only: a soft patch that
// never goes above the hotness of the effect and never darkens what is already there.

// the texture list of the first pass a particle effect has (its normal one, or the distortion one of a heat haze)
static STextureList* heat_particle_textures(dxRender_Visual* V)
{
	Shader* S = V->shader._get();
	if (!S)
		return nullptr;
	for (u32 e = 0; e < SHADER_ELEMENTS_MAX; ++e)
	{
		ShaderElement* E = S->E[e]._get();
		if (!E || E->passes.empty())
			continue;
		STextureList* T = E->passes[0]->T._get();
		// the sprite's picture is what its pixel shader reads from stage 0
		if (T && !T->empty() && 0 == T->front().first && T->front().second._get())
			return T;
	}
	return nullptr;
}

void CDSGraphManager::r_dsgraph_render_heat()
{
	PROF_EVENT("r_dsgraph_render_heat");
	xr_vector<HeatItem>& list = RGraph.mapHeat;
	if (list.empty())
		return;

	static shared_str s_item = "r1_heat_item"; // models: x = hotness, y = 1 for a HUD item (the player's hands)
	static shared_str s_vs = "r1_heat_vs"; // x = hotness at the heat point, y = far from it
	static shared_str s_depth = "r1_heat_depth"; // x = pull towards the camera in metres, y = near plane, z = near * far / (far - near)
	static shared_str s_point = "r1_heat_point"; // xyz = the heat point (world), w = 1 / reach
	// the first-person hands and gun keep the plain depth test: nothing changes their pose between the two draws
	const float depth_pull = ps_r1_thermal_depth * .01f;
	bool has_hud = false, has_particles = false;
	for (int pass = 0; pass < 2; ++pass)
	{
		const bool hud = (1 == pass);
		if (hud && !has_hud)
			break;
		// the HUD part: HUD projection and the near depth range, exactly as the HUD itself was drawn
		CHudInitializer initializer(hud);
		if (hud) RImplementation.rmNear();
		// The depth pull is a distance in metres along the view axis, so the vertex shader needs the near plane of the
		// projection the scene was drawn with: depth = Q - Q * near / distance, with Q = far / (far - near) = _33 and
		// Q * near = -_43. Any other kind of projection, and the HUD part, gets no pull.
		const Fmatrix& P = RCache.get_xform_project();
		Fvector4 depth = {0.f, 1.f, 0.f, 0.f};
		if (!hud && depth_pull > 0.f && fsimilar(P._34, 1.f) && fis_zero(P._44) && P._33 > EPS && P._43 < -EPS)
			depth.set(depth_pull, -P._43 / P._33, -P._43, 0.f);
		for (HeatItem& item : list)
		{
			if (item.particle)
			{
				has_particles = true;
				continue;
			}
			if (item.hud != hud)
			{
				has_hud |= item.hud;
				continue;
			}
			dxRender_Visual* V = item.pVisual;
			ShaderElement* E = RImplementation.Target->heat_element(V->skinning, item.lq);
			if (!E)
				continue;
			RCache.set_Element(E);
			RCache.set_xform_world(*item.pMatrix);
			RCache.set_c(s_item, item.hotness, hud ? 1.f : 0.f, 0.f, 0.f);
			// z stays 0: round 3's shader files took it as a pull in clip space, which was 9 percent of the distance
			RCache.set_c(s_vs, item.hotness, item.hotness_far, 0.f, 0.f);
			RCache.set_c(s_depth, depth);
			RCache.set_c(s_point, item.point);
			V->Render(calcLOD(item.ssa, V->vis.sphere.R));
		}
		if (hud) RImplementation.rmNormal();
	}

	ShaderElement* E_particle = has_particles ? RImplementation.Target->heat_particle_element() : nullptr;
	if (E_particle)
	{
		RCache.set_ColorWriteEnable(D3DCOLORWRITEENABLE_RED);
		RCache.set_Element(E_particle);
		// the pass blends ONE + ONE; with MAX (where the GPU has it) sprites on top of each other do not add up
		const bool blend_max = RImplementation.Target->heat_blend_max();
		if (blend_max)
			CHK_DX(HW.pDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MAX));
		for (HeatItem& item : list)
		{
			if (!item.particle)
				continue;
			STextureList* T = heat_particle_textures(item.pVisual);
			if (!T)
				continue;
			RCache.set_Textures(T);
			// x = hotness of the effect, y = gain on the sprite's own brightness
			RCache.set_c(s_item, item.hotness, item.gain, 0.f, 0.f);
			item.pVisual->Render(calcLOD(item.ssa, item.pVisual->vis.sphere.R)); // sets its own world matrix and culling
		}
		if (blend_max)
			CHK_DX(HW.pDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
		RCache.set_ColorWriteEnable();
		RCache.set_CullMode(CULL_CCW);
	}

	list.clear();
	RCache.set_xform_world(Fidentity);
}
#endif

#include"LightTrack.h"
void CDSGraphManager::r_dsgraph_capture_static()
{
	PROF_EVENT("r_dsgraph_capture_static")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		// Traverse sector/portal structure
		if (psDeviceFlags.test(rsDrawStatic))
		{
			if (dbg)
			{
				const u32 sector_nodes = u32(m_sector_frustums.size());
				dbg->static_sector_nodes += sector_nodes;
				if (opt_bucket)
					dbg->static_sector_nodes_opt += sector_nodes;
				else
					dbg->static_sector_nodes_noopt += sector_nodes;
			}

			// Determine visibility for static geometry hierrarhy
			for (auto& pair : m_sector_frustums)
			{
				xr_vector<CFrustum>& frustums = pair.val.first;
				if (frustums.empty())
					continue;

				if (frustums.size() == 1)
				{
					if (dbg)
					{
						++dbg->static_frustum_nodes;
						++dbg->static_add_root_calls;
						if (opt_bucket)
						{
							++dbg->static_frustum_nodes_opt;
							++dbg->static_add_root_calls_opt;
						}
						else
						{
							++dbg->static_frustum_nodes_noopt;
							++dbg->static_add_root_calls_noopt;
						}
					}
					add_Static((IRenderVisual*)pair.key->root(), frustums.front(), frustums.front().getMask());
					continue;
				}

				xr_vector<u32> masks;
				masks.reserve(frustums.size());
				for (CFrustum& frustum_node : frustums)
					masks.push_back(frustum_node.getMask());

				if (dbg)
				{
					dbg->static_frustum_nodes += u32(frustums.size());
					++dbg->static_add_root_calls;
					if (opt_bucket)
					{
						dbg->static_frustum_nodes_opt += u32(frustums.size());
						++dbg->static_add_root_calls_opt;
					}
					else
					{
						dbg->static_frustum_nodes_noopt += u32(frustums.size());
						++dbg->static_add_root_calls_noopt;
					}
				}

				add_Static_MultiFrustum((IRenderVisual*)pair.key->root(), frustums, masks);
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_lights()
{
	PROF_EVENT("r_dsgraph_capture_lights")
	g_SpatialSpaceLights->q_frustum
	(
		lstLights,
		ISpatial_DB::O_ORDERED,
		STYPE_LIGHTSOURCE,
		i_frustum
	);

#if	RENDER==R_R1
	spatial_sort_by_distance(lstLights, lstSpatialSortKeys, lstSpatialSortScratch, "lights");
#endif

	for (ISpatialShared spatial : lstLights)
	{
		if (0 == spatial) continue;
		if (R4FullDetailRejectSpatial(spatial.get(), i_mask[CDSGraphManager::fl_normal])) continue;
		spatial->spatial_updatesector();
		CSector* sector = (CSector*)spatial->spatial.sector;
		if (0 == sector) continue;

		if (!RImplementation.HOM.visible(spatial->spatial.sphere)) continue;

		if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
		{
			// lightsource
			if (light* L = (light*)(spatial->dcast_Light()))
			{
#if	RENDER==R_R1
				RImplementation.L_DB->add_light(L);
#else
				if (L->get_LOD() > ps_r2_shadow_lod_min && L->has_light_visible_from_sectors(*this))
				{
					RImplementation.Lights.add_light(L);
				}
#endif
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_dynamic(CObject* O)
{
	PROF_EVENT("r_dsgraph_capture_dynamic")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		if (psDeviceFlags.test(rsDrawDynamic))
		{
			// Traverse object database
			g_SpatialSpace->q_frustum
			(
				lstRenderables,
				ISpatial_DB::O_ORDERED,
				i_doptions,
				i_frustum
			);
			if (dbg)
			{
				const u32 spatial_count = u32(lstRenderables.size());
				dbg->dynamic_spatials += spatial_count;
				if (opt_bucket)
					dbg->dynamic_spatials_opt += spatial_count;
				else
					dbg->dynamic_spatials_noopt += spatial_count;
			}
			set_Object();
#if	RENDER==R_R1
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				spatial_sort_by_distance(lstRenderables, lstSpatialSortKeys, lstSpatialSortScratch, "renderables");

				if (ps_actor_shadow_flags.test(1))
					g_hud->Render_First(dcast_IPortalTraverser());

				r_dsgraph_capture_hud();
			}
#endif
			u32 uID_LTRACK = u32(-1);
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				// update light-vis for current entity / actor
				if (CObject* O = g_pGameLevel->CurrentViewEntity())
				{
					if (!O->getDestroy())
					{
						if (CROS_impl* R = (CROS_impl*)O->ROS())
							R->update(O);
					}
				}

				RImplementation.uLastLTRACK++;
				if (!lstRenderables.empty())
				{
					uID_LTRACK = RImplementation.uLastLTRACK % lstRenderables.size();
#if	RENDER!=R_R1
					// update light-vis for selected entity
					// track lighting environment
					if (IRenderable* renderable = (IRenderable*)lstRenderables[uID_LTRACK]->dcast_Renderable())
					{
						if (CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
							T->update(renderable);
					}
#endif
				}

			}

			// Determine visibility for dynamic part of scene
			for (u32 o_it = 0; o_it < lstRenderables.size(); o_it++)
			{
				ISpatialShared spatial = lstRenderables[o_it];
				if (0 == spatial) continue;
				if (R4FullDetailRejectSpatial(spatial.get(), i_mask[CDSGraphManager::fl_normal])) continue;
				CSector* sector = (CSector*)spatial->spatial.sector;
				if (0 == sector) continue;

				if (i_mask[CDSGraphManager::fl_normal] && !RImplementation.HOM.visible(spatial->spatial.sphere))
					continue;

#if	RENDER==R_R1
				if ((spatial->spatial.type & STYPE_GLOW))
				{
					if (CGlow* glow = spatial->dcast_CGlow())
					{
						// It may be an glow
						RImplementation.L_Glows->add(glow);
					}
					continue;
				}
#endif

//				if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
//				{
//					// lightsource
//					if (light* L = (light*)(spatial->dcast_Light()))
//					{
//#if	RENDER==R_R1
//						RImplementation.L_DB->add_light(L);
//#else
//						if (L->get_LOD() > EPS_L && L->has_light_visible_from_sectors(PT))
//						{
//							RImplementation.Lights.add_light(L);
//						}
//#endif
//					}
//					continue;
//				}

				if(!(spatial->spatial.type & STYPE_RENDERABLE) && !(spatial->spatial.type & STYPE_PARTICLE) && !(spatial->spatial.type & STYPE_RENDERABLESHADOW))
					continue;
				if (!is_sector_visible(sector))
					continue;

				for (CFrustum& frustum : m_sector_frustums.find(sector)->val.first)
				{
					if (dbg)
					{
						++dbg->dynamic_frustum_tests;
						if (opt_bucket)
							++dbg->dynamic_frustum_tests_opt;
						else
							++dbg->dynamic_frustum_tests_noopt;
					}

					if (frustum.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
					{
						if (dbg)
						{
							++dbg->dynamic_frustum_hits;
							if (opt_bucket)
								++dbg->dynamic_frustum_hits_opt;
							else
								++dbg->dynamic_frustum_hits_noopt;
						}

						// renderable
						IRenderable* renderable = spatial->dcast_Renderable();
						if (0 == renderable) break;

						if (O && O->dcast_Renderable() == renderable) break;

						// Rendering
#if	RENDER==R_R1
						if (i_mask[CDSGraphManager::fl_normal] && o_it == uID_LTRACK && renderable->renderable_ROS())
						{
							// track lighting environment
							if(CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
								T->update(renderable);
						}
#endif
						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object(renderable);

						renderable->renderable_Render(dcast_IPortalTraverser());
						if (dbg)
						{
							++dbg->dynamic_rendered;
							if (opt_bucket)
								++dbg->dynamic_rendered_opt;
							else
								++dbg->dynamic_rendered_noopt;
						}

						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object();
						break;
					}
				}
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture(bool lights, bool dynamic, CObject* O)
{
	PROF_EVENT("r_dsgraph_capture")
	r_dsgraph_capture_static();

	if(lights)
		r_dsgraph_capture_lights();

	if (dynamic)
		r_dsgraph_capture_dynamic(O);
}

void CDSGraphManager::r_dsgraph_render_ui_3d()
{
    const Fmatrix saved_world = RCache.get_xform_world();
    const Fmatrix saved_view = RCache.get_xform_view();
    const Fmatrix saved_project = RCache.get_xform_project();
    const Fmatrix saved_full = Device.mFullTransform;
    Device.mFullTransform = Device.mFullTransform3DIcons;
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject3DIcons);
    auto render_icons = [&](auto& graph, bool material_shader = false)
    {
        std::sort(graph.begin(), graph.end());
        for (auto& item : graph)
        {
            dxRender_Visual* visual = item.pVisual;
            VERIFY(visual && visual->shader._get());
            if (material_shader)
            {
                // DX11 sorted icons use main's selected mesh material and distance-based LOD.
                RCache.set_Element(item.pSE);
                RCache.set_xform_world(*item.pMatrix);
                RImplementation.apply_object(item.pObject);
                RImplementation.apply_lmaterial();
                visual->Render(calcLOD(item.sortKey, visual->vis.sphere.R));
                continue;
            }
            R_ASSERT(visual->skinning >= -1 && visual->skinning <= 4);
            RCache.set_Shader(RImplementation.Target->s_ui_icons[visual->skinning + 1]);
            RCache.set_Textures(visual->shader->E[0]->passes[0]->T);
            RCache.set_xform_world(*item.pMatrix);
#if RENDER == R_R4
            RImplementation.apply_object(nullptr);
            RImplementation.apply_lmaterial();
#endif
            visual->Render(0.f);
        }
        graph.clear();
    };
    render_icons(RGraph.mapUI3D);
#if RENDER != R_R1
    render_icons(RGraph.mapUI3DEmissive);
#endif
#if RENDER == R_R4
    render_icons(RGraph.mapUI3DSorted, true);
#else
    render_icons(RGraph.mapUI3DSorted);
#endif
    owned_transforms.clear();
    Device.mFullTransform = saved_full;
    RCache.set_xform_world(saved_world);
    RCache.set_xform_view(saved_view);
    RCache.set_xform_project(saved_project);
}

void CDSGraphManager::r_dsgraph_render_ui()
{
	// Capture may run on workers; UI drawing touches RCache and runs here on the render thread.
	for (IRenderable* object : renderable_ui)
		object->renderable_RenderUI();
	renderable_ui.clear();
}
