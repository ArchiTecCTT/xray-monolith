#include "stdafx.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/irenderable.h"

#include "flod.h"
#include "particlegroup.h"
#include "FTreeVisual.h"
#if RENDER == R_R1
#include "../../xrEngine/shader_bus.h" // obj_hotness, for the R1 heat mask
#include "ParticleEffect.h" // warm particle effects, for the R1 heat mask
#include "LightTrack.h" // sun and sky on an object, for its surface state
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/Environment.h"
#endif

using namespace R_dsgraph;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scene graph actual insertion and sorting ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
float r_ssaDISCARD;
float r_ssaDONTSORT;
float r_ssaLOD_A, r_ssaLOD_B;
float r_ssaGLOD_start, r_ssaGLOD_end;
float r_ssaHZBvsTEX;

ICF float CalcSSA(float& distSQ, Fvector& C, float R)
{
	distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
    return (R * R / distSQ);
}

ICF float CalcSSA(float& distSQ, Fvector& C, dxRender_Visual* V)
{
    return CalcSSA(distSQ, C, V->vis.sphere.R);
}

ICF bool R4FullDetailRejectStatic(dxRender_Visual* pVisual, bool normalPhase)
{
#if RENDER == R_R4
	if (ps_r4_full_detail_distance_scale >= 1.f || !normalPhase ||
		pVisual->flags.test(IRenderVisualFlags::eIgnoreOptimization) || !g_pGamePersistent ||
		!g_pGamePersistent->Environment().CurrentEnv)
		return false;

	// Keep the engine's normal LOD selection alive beyond the full-detail range.
	if (pVisual->Type == MT_LOD)
		return false;

	const float farPlane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	const float fullDetailDistance = std::max(100.f, farPlane * ps_r4_full_detail_distance_scale);
	float distanceSquared;
	const float ssa = CalcSSA(distanceSquared, pVisual->vis.sphere.P, pVisual);
	const float nearestDistance = _sqrt(distanceSquared) - pVisual->vis.sphere.R;
	if (nearestDistance <= fullDetailDistance)
		return false;

	const float transitionRange = std::max(1.f, farPlane - fullDetailDistance);
	const float transition = clampr((nearestDistance - fullDetailDistance) / transitionRange, 0.f, 1.f);
	return ssa <= r_ssaDISCARD * (1.f + transition * 11.f);
#else
	return false;
#endif
}

#if RENDER == R_R1
// R1 thermal, the surface state of one object (docs/r1_thermal.md, "Heat that follows physics"). Each object that is
// drawn warm keeps two numbers on its light tracker: how wet its surface is and how much the sun has warmed it. Rain
// only wets what it reaches (rain density x how much open sky the object has above it), the sun only warms what it
// shines on (sun strength x the object's own sun visibility). Both follow their cause slowly and fade slowly.
// The result shifts the object's hotness a little: wet = dimmer, sun-warmed = brighter. It never hides a body.
static float r1_heat_surface(IRenderable* O, float hotness)
{
	if (!(ps_r1_thermal_wet > 0.f) && !(ps_r1_thermal_sun > 0.f))
		return hotness;
	CROS_impl* R = (CROS_impl*)O->renderable_ROS();
	if (!R || !g_pGamePersistent)
		return hotness;

	const float now = Device.fTimeGlobal;
	if (R->thermal_time != now)
	{
		// what reaches the object right now
		CEnvDescriptor* E = g_pGamePersistent->Environment().CurrentEnv;
		float rain = E ? clampr(E->rain_density, 0.f, 1.f) : 0.f;
		// the sun's strength: its colour as the weather gives it, and nothing once it is under the horizon
		float sun = 0.f;
		if (E && E->sun_dir.y < -0.05f)
			sun = clampr(_max(E->sun_color.x, _max(E->sun_color.y, E->sun_color.z)) * 2.f, 0.f, 1.f);
		const bool forced = (ps_r1_thermal_force_rain >= 0.f) || (ps_r1_thermal_force_sun >= 0.f);
		if (ps_r1_thermal_force_rain >= 0.f) rain = ps_r1_thermal_force_rain;
		if (ps_r1_thermal_force_sun >= 0.f) sun = ps_r1_thermal_force_sun;
		// open sky above it: under a roof about a third of the sky rays still pass, so that counts as cover
		const float sky = clampr((R->get_hemi() - .3f) * 2.5f, 0.f, 1.f);
		const float wet_to = rain * sky;
		const float sun_to = sun * clampr(R->get_sun(), 0.f, 1.f) * (1.f - wet_to);

		if (R->thermal_time < 0.f)
		{
			// first seen: what it would have settled at where it stands
			R->thermal_wet = wet_to;
			R->thermal_sun = sun_to;
		}
		else
		{
			// seconds to follow: soaked in 20, dry in 90; sun-warm in 45, cool in 60. Forced inputs (the test
			// shortcuts) show within a second or two
			const float dt = clampr(now - R->thermal_time, 0.f, 600.f) * (forced ? 20.f : 1.f);
			const float k_wet = dt / (wet_to > R->thermal_wet ? 20.f : 90.f);
			const float k_sun = dt / (sun_to > R->thermal_sun ? 45.f : 60.f);
			R->thermal_wet += (wet_to - R->thermal_wet) * (k_wet < 1.f ? k_wet : 1.f);
			R->thermal_sun += (sun_to - R->thermal_sun) * (k_sun < 1.f ? k_sun : 1.f);
		}
		R->thermal_time = now;
	}
	return hotness * (1.f - ps_r1_thermal_wet * R->thermal_wet) + ps_r1_thermal_sun * R->thermal_sun;
}

// R1 thermal (r__heatvision): queue a warm visual for the heat mask pass (CRenderTarget::phase_heat).
// Only called while a heat mask is wanted for this frame. What is warm, and how warm (docs/r1_thermal.md):
//  - the owner object's own model, at the owner's hotness, whatever its shaders and textures are (a living creature,
//    a corpse cooling down to 0, a lit lamp, a script attachment worn by a warm creature). Deciding by owner and not
//    by the hot mark of the texture keeps a body warm after a script swapped its model or textures;
//  - anything else the owner draws (the gun in its hands, carried items, the first-person hands and gun), only where
//    the texture is marked hot: the first-person hands are, guns and devices are not;
//  - whatever the game gave an item heat (set_ItemHeat: a fired gun, hottest at its muzzle);
//  - an object with its own obj_hotness bus value, on every surface;
//  - anything next to a fire, a little (the fire sources of the last frame).
void CDSGraphManager::r1_heat_insert(dxRender_Visual* pVisual, Fmatrix* xform, bool own_model, float ssa, ShaderElement* sh)
{
	// shadow-only draws (the first-person body), camera attachments and custom-FOV attachments are never warm
	if (i_mask[fl_invisible] || i_mask[fl_cam] || val_fCustomFOV != 0.f)
		return;

	float hotness = val_fItemHeat;
	float hotness_far = val_fItemHeatFar;
	Fvector4 point = val_vItemHeatPoint;
	if (val_pObject)
	{
		float owner = val_pObject->GetHotness();
		const bool by_script = ShaderBus::object_hotness(val_pObject->renderable.bus_values, owner);
		bool whole = by_script || own_model;
		if (owner > 0.001f)
		{
			if (!whole)
			{
				CTexture* T = pVisual->GetTexture();
				whole = T && T->m_is_hot;
			}
			if (whole)
			{
				// the surface state belongs to the engine's own heat, not to a value a script set
				if (!by_script)
					owner = r1_heat_surface(val_pObject, owner);
				if (owner > hotness)
				{
					hotness = owner;
					point.w = 0.f;
				}
				if (owner > hotness_far)
					hotness_far = owner;
			}
		}
	}
	// next to a fire: cold things warm up to what the fire gives at that distance, warm things by part of it
	if (!i_mask[fl_hud] && RImplementation.Target->heat_fires_near())
	{
		Fvector C;
		xform->transform_tiny(C, pVisual->vis.sphere.P);
		const float fire = RImplementation.Target->heat_fire_at(C, val_pObject);
		if (fire > 0.001f)
		{
			hotness = _max(fire, hotness + fire * .35f);
			hotness_far = _max(fire, hotness_far + fire * .35f);
		}
	}
	if (!(hotness > 0.001f) && !(hotness_far > 0.001f))
		return;

	HeatItem item;
	item.pVisual = pVisual;
	item.pMatrix = xform;
	item.ssa = ssa;
	item.hotness = hotness < 1.f ? hotness : 1.f;
	item.hotness_far = point.w > 0.f ? (hotness_far < 1.f ? hotness_far : 1.f) : item.hotness;
	item.point = point;
	item.gain = 1.f;
	item.hud = i_mask[fl_hud];
	ShaderElement* lq = pVisual->shader->E[SE_R1_NORMAL_LQ]._get();
	item.lq = (sh == lq) && (lq != pVisual->shader->E[SE_R1_NORMAL_HQ]._get());
	item.particle = false;
	RGraph.mapHeat.push_back(item);
}

// A particle effect whose name configs\r1_thermal.ltx lists (fire, embers, heat haze) is drawn once more into the mask.
void CDSGraphManager::r1_heat_insert_particle(dxRender_Visual* pVisual, float ssa)
{
	if (i_mask[fl_invisible])
		return;
	PS::CParticleEffect* effect = (PS::CParticleEffect*)pVisual->dcast_ParticleCustom();
	if (!effect || !effect->GetDefinition())
		return;
	float hotness, gain;
	if (!RImplementation.Target->heat_particle(effect->GetDefinition()->m_Name, hotness, gain))
		return;

	// an open fire also warms what is around it (docs/r1_thermal.md, "Heat spreads from fires")
	RImplementation.Target->heat_add_fire(pVisual->vis.sphere.P, pVisual->vis.sphere.R, hotness);

	HeatItem item;
	item.pVisual = pVisual;
	item.pMatrix = nullptr;
	item.ssa = ssa;
	item.hotness = hotness;
	item.hotness_far = hotness;
	item.point.set(0.f, 0.f, 0.f, 0.f);
	item.gain = gain;
	item.hud = false; // a HUD particle effect switches to the HUD projection by itself
	item.lq = false;
	item.particle = true;
	RGraph.mapHeat.push_back(item);
}
#endif

void CDSGraphManager::r_dsgraph_insert_dynamic(dxRender_Visual *pVisual, Fmatrix* xform)
{
	Fvector Center;
	xform->transform_tiny(Center, pVisual->vis.sphere.P);
#if RENDER == R_R1
	// R1 thermal: the body of an object is added with the object's own matrix, the things it carries (a gun, a held
	// item) with theirs; a script attachment is its own owner. Read before the matrix is replaced by a copy below.
	const bool heat_collect = RImplementation.r1_heat_collect && this == &RImplementation.GMBase && !get_UI3D();
	const bool heat_own_model = heat_collect && val_pObject &&
		(!val_pObject->dcast_CObject() || xform == &val_pObject->renderable.xform);
#endif
    // Attachments can submit the same visual at several instance transforms in one pass.
    if (!get_UI3D() && val_pObject && !val_pObject->dcast_CObject())
    {
        owned_transforms.push_back(*xform);
        xform = &owned_transforms.back();
    }
    if (get_UI3D())
    {
        VERIFY(pVisual->shader._get());
        const float distance_squared = Device.vCameraPosition.distance_to_sqr(Center);
#if RENDER == R_R4
        // Match main's DX11 icon classification and SSA test, outside world HOM culling.
        const float icon_ssa = pVisual->vis.sphere.R / (distance_squared + EPS);
        if (icon_ssa <= r_ssaDISCARD) return;
        ShaderElement* shader = RImplementation.rimp_select_sh_dynamic(pVisual, distance_squared + EPS);
        if (!shader) return;
#endif
        owned_transforms.push_back(*xform);
        Fmatrix* icon_transform = &owned_transforms.back();
#if RENDER == R_R4
        if (shader->flags.bStrictB2F)
            RGraph.mapUI3DSorted.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual, icon_transform, shader, false);
        else
        {
            RGraph.mapUI3D.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual, icon_transform, shader, false);
            if (shader->flags.bEmissive)
                RGraph.mapUI3DEmissive.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual,
                    icon_transform, pVisual->shader->E[4]._get(), false);
        }
#else
        // DX8/9/10 use the dedicated icon shader for every mesh, far to near.
        RGraph.mapUI3DSorted.emplace_back(distance_squared, 1.f, nullptr, pVisual, icon_transform, nullptr, false);
#endif
        return;
    }


	float distSQ;
	float SSA = CalcSSA(distSQ, Center, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
        {
            //Msg("SSA %.2f discarded", SSA);
            return;
        }
    }
    

	// HOM occlusion culling for dynamic objects
    if (ps_r__common_flags.test(RFLAG_HOM_DYNAMIC))
    {
#if RENDER!=R_R1
        if (i_mask[CDSGraphManager::fl_normal])
#endif
        {
            Fbox world_bb;
            world_bb.xform(pVisual->vis.box, *xform);
            if (!RImplementation.HOM.visible(world_bb))
                return;
        }
    }

#if RENDER == R_R1
	// R1 thermal: warm particle effects. Here, because one that only distorts (heat haze) has no normal element and
	// leaves this function below
	if (heat_collect && MT_PARTICLE_EFFECT == pVisual->Type)
		r1_heat_insert_particle(pVisual, SSA);
	// R1 thermal: warm but not to be seen (a fully cloaked bloodsucker, IDSGraphManager::set_HeatOnly). The heat mask
	// gets it, tested against the depth of the scene around it; the scene itself does not.
	if (val_bHeatOnly)
	{
		if (heat_collect && MT_PARTICLE_EFFECT != pVisual->Type)
			r1_heat_insert(pVisual, xform, heat_own_model, SSA, RImplementation.rimp_select_sh_dynamic(pVisual, distSQ));
		return;
	}
#endif

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());

	if (sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
	{
		if (val_fCustomFOV != 0.f)
			RGraph.mapCustomFOVSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, false, val_fCustomFOV);
		else if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
		else
			RGraph.mapDynamicSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
	}

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ);

	if (0==sh)
		return;
	u32 shader_priority = sh->flags.iPriority/2;
	if (!i_mask[shader_priority])
		return;

#if RENDER == R_R1
	// R1 thermal: only the main view's graph, only on frames that build a heat mask (false while r__heatvision is off)
	if (heat_collect && MT_PARTICLE_EFFECT != pVisual->Type)
		r1_heat_insert(pVisual, xform, heat_own_model, SSA, sh);
#endif

	// Create common node
	// NOTE: Invisible elements exist only in R1

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	switch (sh->flags.iScopeLense) {	
		case 0:
			break;

		case 1: {
			(get_CamAttached() ? RGraph.mapCamAttached : RGraph.mapHUD).emplace_back(EPS, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);

			// SSS: Deprecated
			/*if (!sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = HUDMask.insertInAnyWay(EPS);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}*/
			return;
		}

		case 2: {
			(get_CamAttached() ? RGraph.mapScopeCamAttached : RGraph.mapScopeHUD).emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			return;
		}

		case 3: {
			if (get_CamAttached())
				RGraph.mapScopeCamAttachedSorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			else
				RGraph.mapScopeHUDSorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			return;
		}
	}
#endif
    if (get_HUD() && val_fCustomFOV != 0.f)
    {
        if (sh->flags.bStrictB2F)
            RGraph.mapCustomFOVSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, false, val_fCustomFOV);
        else
            RGraph.mapCustomFOV.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, false, val_fCustomFOV);
#if RENDER != R_R1
        if (sh->flags.bEmissive)
            RGraph.mapCustomFOVSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, false, val_fCustomFOV);
#endif
        return;
    }

	// HUD rendering
	if (i_mask[CDSGraphManager::fl_hud])
	{
		if (sh->flags.bStrictB2F)
		{
#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
#endif // RENDER!=R_R1
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttachedSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUDSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
			return;
		}
		else
		{
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttached.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUD.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);

			/*
#if RENDER==R_R4
			if (RImplementation.o.ssfx_core && !sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = val_bCamAttached ? HUDMaskCamAttached.insertInAnyWay(distSQ) : HUDMask.insertInAnyWay(distSQ);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}
#endif*/

#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
				
#endif	//	RENDER!=R_R1

			return;
		}
	}

	// Shadows registering
#if RENDER==R_R1
	DSGraphItem<u32, false> item = { 0, SSA, val_pObject, pVisual, xform, nullptr, get_HUD(), val_fCustomFOV };
	R_dsgraph::mapDSGraphItemsMap<u32, false>::TNode N = { 0, item };
	RImplementation.L_Shadows->add_element(N);
#endif
	if (i_mask[CDSGraphManager::fl_invisible])
		return;

	// strict-sorting selection
	if (sh->flags.bStrictB2F && !pVisual->dcast_ParticleCustom())
	{
		auto& graph = val_fCustomFOV != 0.f ? RGraph.mapCustomFOVSorted.Sorted : RGraph.mapDynamicSorted.Sorted;
		graph.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
	{
		auto& graph = val_fCustomFOV != 0.f ? RGraph.mapCustomFOVSorted.Emissive : RGraph.mapDynamicSorted.Emissive;
		graph.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, get_HUD(), val_fCustomFOV);
	}

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		else
			RGraph.mapDynamicSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		return;
	}
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		
		if (sh->passes[iPass] == nullptr)
		{
			continue;
		}

		SPass& pass = *sh->passes[iPass];

#if RENDER==R_R1
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], item, pass);
#else
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], { 0, SSA, val_pObject, pVisual, xform, nullptr, get_HUD(), val_fCustomFOV }, pass);
#endif
	}
}

extern float ps_r__ssaDISCARD_exp;
extern float ps_r__ssaDISCARD_fade_k;
void CDSGraphManager::r_dsgraph_insert_static(dxRender_Visual *pVisual)
{
	if (m_static_seen.find(pVisual) != m_static_seen.end())
	{
		if (PortalTraverseDbg_Enabled())
		{
			PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
			const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
			++dbg.static_dedup_skipped;
			if (opt_bucket)
				++dbg.static_dedup_skipped_opt;
			else
				++dbg.static_dedup_skipped_noopt;
		}
		return;
	}
	m_static_seen.insert(pVisual);
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		++dbg.static_dedup_seen;
		if (opt_bucket)
			++dbg.static_dedup_seen_opt;
		else
			++dbg.static_dedup_seen_noopt;
	}

	float distSQ;
	float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
            return;

        // demonized: Replace hard cutoff with gradient cutoff
        // Smaller objects that fail the SSA test will still render depending on how much smaller they are than the discard limit.
        // Reduces the "rendering radius" effect and makes pop-in less noticeable
        // Allows to increase the discard limit for better performance without making pop-in much worse
        // Define where the "thinning" begins. 
        // E.g., objects 4x the size of the discard limit start fading.
        float fade_start = r_ssaDISCARD * ps_r__ssaDISCARD_fade_k;

        // The Gradient Zone
        if (SSA < fade_start)
        {
            // Calculate a linear survival probability between 0.0 and 1.0
            float survival_chance = (SSA - r_ssaDISCARD) / (fade_start - r_ssaDISCARD);

            // Convert the 32-bit hash to a float between 0.0 and 1.0
            // Multiplying by 1.0 / 2^32 is faster than float division
            u32 hash = GetFvectorHash(pVisual->vis.sphere.P);
            constexpr float hash_to_float = 1.0f / 4294967296.0f;
            float val = hash * hash_to_float;

            // If the object's hash value is higher than its survival chance, cull it
            if (val > _powf(survival_chance, ps_r__ssaDISCARD_exp))
                return;
        }
    }

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());
	if (sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
		RGraph.mapStaticSorted.Distort.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false);

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ);

	if (0 == sh)
		return;
	u32 shader_priority = sh->flags.iPriority / 2;
	if (!i_mask[shader_priority])
		return;

	// Water rendering
#if RENDER==R_R4
	if (sh->flags.isWater && RImplementation.o.ssfx_water)
	{
		RGraph.mapWater.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	// strict-sorting selection
	if (sh->flags.bStrictB2F)
	{
		RGraph.mapStaticSorted.Sorted.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
		RGraph.mapStaticSorted.Emissive.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false );

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		RGraph.mapStaticSorted.Wmark.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		if (sh->passes[iPass] == nullptr)
			continue;

		SPass& pass	= *sh->passes[iPass];

		AddToRenderQueue(RGraph.mapStaticPasses[shader_priority][iPass], { 0, SSA, nullptr, pVisual, nullptr, nullptr, false }, pass);
	}
}

void CDSGraphManager::AddToRenderQueue(R_dsgraph::RenderQueue& queue, const R_dsgraph::DSGraphItem<u32, false>& item, const SPass& pass)
{
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		// Static items use nullptr matrix/object in current pipeline.
		if (item.pMatrix == nullptr && item.pObject == nullptr)
		{
			++dbg.queue_static_packets;
			if (opt_bucket)
				++dbg.queue_static_packets_opt;
			else
				++dbg.queue_static_packets_noopt;
		}
		else
		{
			++dbg.queue_dynamic_packets;
			if (opt_bucket)
				++dbg.queue_dynamic_packets_opt;
			else
				++dbg.queue_dynamic_packets_noopt;
		}
	}

	queue.emplace_back(item, pass);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Dynamic(IRenderVisual* piVisual, Fmatrix* xform)
{
	
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;

	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}return;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Dynamic(pV->children, xform);
	}return;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		BOOL _use_lod = FALSE;
		if (pV->m_lod && !get_UI3D())
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (get_UI3D())
				pV->CalculateBones(TRUE);
			else if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			add_leafs_Dynamic(pV->children, xform);
		}
	}return;

	default:
	{
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}return;
	}
}

void CDSGraphManager::add_Dynamic(dxRender_Visual* pVisual, Fmatrix* xform)
{
	if (!pVisual) return;

	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}break;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Dynamic(pV->children, xform);
	}break;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		BOOL _use_lod = FALSE;
		if (pV->m_lod && !get_UI3D())
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (get_UI3D())
				pV->CalculateBones(TRUE);
			else if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			add_leafs_Dynamic(pV->children, xform);
		}
	}break;

	default:
	{
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}break;
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<dxRender_Visual*>& children, Fmatrix* xform)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<IRenderVisual*>& children, Fmatrix* xform)
{
	for (IRenderVisual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Static(IRenderVisual* piVisual, CFrustum& frustum, u32 planes)
{
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check frustum visibility and calculate distance to visual's center
	vis_data& vis = pVisual->vis;
	EFC_Visible VIS = frustum.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);

	if (fcvNone == VIS)
		return;

	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible or partially visible
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		// Add all children
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (fcvPartial == VIS)
		{
			for (auto V : pV->children)
				add_Static(V, frustum, planes);
		}
		else
			add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, perform tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_Static_MultiFrustum(IRenderVisual* piVisual, const xr_vector<CFrustum>& frustums, const xr_vector<u32>& masks)
{
	constexpr u32 FULLY_VISIBLE_MASK = u32(-1);

	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check visibility against all active frustums and propagate per-frustum masks.
	vis_data& vis = pVisual->vis;
	bool anyVisible = false;
	bool hasFullyVisibleFrustum = false;
	xr_vector<u32> childMasks;
	childMasks.resize(masks.size(), 0);

	for (u32 i = 0; i < masks.size(); ++i)
	{
		u32 planeMask = masks[i];
		if (!planeMask)
			continue;

		// Propagated from parent: this visual is fully visible in this frustum.
		if (planeMask == FULLY_VISIBLE_MASK)
		{
			anyVisible = true;
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
			continue;
		}

		EFC_Visible VIS = frustums[i].testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planeMask);
		if (VIS == fcvNone)
		{
			continue;
		}

		anyVisible = true;
		if (VIS == fcvFully)
		{
			// In original per-frustum traversal this path immediately adds leafs.
			// Preserve union semantics by marking this frustum as fully visible.
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
		}
		else
		{
			childMasks[i] = planeMask;
		}
	}

	if (!anyVisible)
		return;

	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible in at least one frustum
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (hasFullyVisibleFrustum)
			add_leafs_Static(pV->children);
		else
		{
			for (auto V : pV->children)
				add_Static_MultiFrustum(V, frustums, childMasks);
		}
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_leaf_Static(dxRender_Visual* pVisual)
{
	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(pVisual->vis))
			return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				break;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, doesn't perform any tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_PM:
	case MT_TREE_ST:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}break;
	}
}

void CDSGraphManager::add_leafs_Static(xr_vector<dxRender_Visual*>& children)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_leaf_Static(pVisual);
	}
}

void CDSGraphManager::add_leafs_Static(xr_vector<IRenderVisual*>& children)
{
	for (IRenderVisual* pVisual : children)
	{
		dxRender_Visual* pV = (dxRender_Visual*)pVisual;
		if (!pV) continue;
		add_leaf_Static(pV);
	}
}
