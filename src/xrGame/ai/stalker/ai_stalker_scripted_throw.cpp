////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_scripted_throw.cpp
//	Description : A grenade throw asked for by a script: npc:throw_grenade(pos [, crouch]). docs/npc_throw.md
//
//	The rules (refusals, states, timeouts) are in scripted_throw.h. This file is what the stalker does about them.
//
//	While a request is active the stalker's own updates are borrowed at the three places where he reads what he was
//	told to do, so it does not matter who told him (a planner action, a script's combat mode, the state manager):
//	 - the object handler (update_object_handler): the goal is "this grenade: hold it / throw it" for the update,
//	   then the goal the others set is put back. When the request ends their goal is simply still there, and the
//	   object handler swaps back to the gun on its own.
//	 - the movement manager (Think): stand, danger, the asked body state for the update, then the others' values
//	   are put back.
//	 - the sight manager (UpdateCL): look at the target point.
//	While a request is active, and until the gun is back after it, he and the items that are changing hands are also
//	kept on the per-frame update (scripted_throw_keep_updated says why).
//	Nothing here runs for a stalker without an active request, or one just ended.
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_stalker.h"
#include "ai_stalker_impl.h"
#include "../../inventory.h"
#include "../../missile.h"
#include "../../grenade.h"
#include "../../weapon.h"
#include "../../ai_space.h"
#include "../../level_graph.h"
#include "../../script_game_object.h"
#include "../../../xrServerEntities/script_engine.h"
#include "../../object_handler_planner.h"
#include "../../object_handler_space.h"
#include "../../stalker_planner.h"
#include "../../stalker_decision_space.h"
#include "../../stalker_movement_manager_smart_cover.h"
#include "../../sight_manager.h"
#include "../../sight_action.h"
#include "../../game_cl_single.h"
#include "../../sound_player.h"
#include "../../stalker_animation_manager.h"
#include "../../../Include/xrRender/KinematicsAnimated.h"
#include "ai_stalker_space.h"

// console vars (console_commands.cpp), saved in user.ltx
int g_ai_scripted_throw = 1; // 0 = every request is refused ("disabled")
int g_ai_scripted_throw_debug = 0; // 1 = one log line per request start / end

using namespace scripted_throw;

static LPCSTR missile_state_name(u32 state)
{
	switch (state)
	{
	case CHUDState::eIdle: return "idle";
	case CHUDState::eShowing: return "showing";
	case CHUDState::eHiding: return "hiding";
	case CHUDState::eHidden: return "hidden";
	case CHUDState::eBore: return "bore";
	case CMissile::eThrowStart: return "throw_start";
	case CMissile::eReady: return "ready";
	case CMissile::eThrow: return "throw";
	case CMissile::eThrowEnd: return "throw_end";
	}
	return "other";
}

// ai_scripted_throw_debug 1. A refused request gets the short line:
//	[THROW] id=.. sec=.. dist=.. result=refused reason=..
// A request's start and its end get the facts as well (docs/npc_throw.md has the list):
//	[THROW] id=.. sec=.. dist=.. result=start|thrown|failed reason=.. phase=.. limit=.. ms=.. in_hand_ms=..
//	aligned_ms=.. active=.. astate=.. mstate=.. yaw_err=.. body=.. script_anim=.. arc_tries=.. crouch=.. op=..
//	strap=.. frames=.. cam=..
void CAI_Stalker::scripted_throw_log(LPCSTR result, int why, Fvector const& target, bool facts)
{
	if (!g_ai_scripted_throw_debug)
		return;

	float const distance = Position().distance_to(target);
	if (!facts)
	{
		Msg("[THROW] id=%u sec=%s dist=%.1f result=%s reason=%s", u32(ID()), cNameSect().c_str(), distance, result,
		    reason_name(why));
		return;
	}

	CInventoryItem* const active = inventory().ActiveItem();
	CHudItem* const active_hud = active ? active->cast_hud_item() : nullptr;
	CInventoryItem* const grenade = scripted_throw_grenade();
	CMissile* const missile = grenade ? smart_cast<CMissile*>(grenade) : nullptr;

	CObjectHandlerPlanner& planner = CObjectHandler::planner();
	bool const planned = planner.initialized();
	int const operator_id = planned ? int(planner.current_action_state_id()) : -1;
	bool const strapped = planned && (planner.m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped)
		|| planner.m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped2Idle));

	LPCSTR const script_animation = animation().script_animations().empty()
		                                ? "0"
		                                : (scripted_throw_prone() ? "prone" : "1");
	int const crouch = (m_scripted_throw_body == u8(MonsterSpace::eBodyStateCrouch))
		                   ? 1
		                   : ((m_scripted_throw_body == u8(MonsterSpace::eBodyStateStand)) ? 0 : -1);

	Msg("[THROW] id=%u sec=%s dist=%.1f result=%s reason=%s phase=%s limit=%s ms=%d in_hand_ms=%d aligned_ms=%d "
	    "active=%s astate=%d mstate=%s yaw_err=%.0f body=%s script_anim=%s arc_tries=%d crouch=%d op=%d strap=%d "
	    "frames=%u cam=%.0f",
	    u32(ID()), cNameSect().c_str(), distance, result, reason_name(why),
	    phase_name(m_scripted_throw.where()), limit_name(m_scripted_throw.lim),
	    m_scripted_throw.ms_total(), m_scripted_throw.ms_in_hand(), m_scripted_throw.ms_aligned(),
	    active ? active->object().cNameSect().c_str() : "none",
	    active_hud ? int(active_hud->GetState()) : -1,
	    missile ? missile_state_name(missile->GetState()) : "none",
	    rad2deg(scripted_throw_yaw_error()),
	    (movement().body_state() == MonsterSpace::eBodyStateCrouch) ? "crouch" : "stand",
	    script_animation, m_scripted_throw.arc_tries, crouch, operator_id, strapped ? 1 : 0,
	    m_scripted_throw.ticks, Device.vCameraPosition.distance_to(Position()));
}

// the request's grenade, while it is still the one in his grenade slot
CInventoryItem* CAI_Stalker::scripted_throw_grenade()
{
	CInventoryItem* const item = inventory().ItemFromSlot(GRENADE_SLOT);
	if (!item || (item->object().ID() != m_scripted_throw_grenade_id))
		return nullptr;

	return item;
}

bool CAI_Stalker::scripted_throw_wounded()
{
	if (wounded())
		return true;

	return brain().CStalkerPlanner::m_storage.property(StalkerDecisionSpace::eWorldPropertyCriticallyWounded);
}

// in a smart cover, entering one, or sent to one: its animations own him
bool CAI_Stalker::scripted_throw_smart_cover()
{
	return movement().in_smart_cover() || movement().target_params().cover();
}

// an animation that owns his position, or a mounted weapon: no throw from there
bool CAI_Stalker::scripted_throw_animation_locked()
{
	if (animation_movement())
		return true;

#ifdef HOLDERCUSTOM_NEW
	if (m_holder)
		return true;
#endif

	return false;
}

// He lies in a prone pose: the script animation at the head of his queue is one of the state manager's "prone" /
// "prone_idle" set (state_mgr_animation_list.script; they are the vanilla prone smart cover's animations). There is
// no throw animation for a lying man: he would have to leave the pose for the whole throw, in a spot picked for
// lying down. Such a request is refused at once.
bool CAI_Stalker::scripted_throw_prone()
{
	if (animation().script_animations().empty())
		return false;

	IKinematicsAnimated* const skeleton = smart_cast<IKinematicsAnimated*>(Visual());
	if (!skeleton)
		return false;

	static LPCSTR const prone_animations[] = {"loophole_9_idle_0", "loophole_9_in_front_0"};
	MotionID const playing = animation().script_animations().front().animation();
	for (LPCSTR const name : prone_animations)
	{
		MotionID const id = skeleton->ID_Cycle_Safe(name);
		if (id.valid() && (id == playing))
			return true;
	}

	return false;
}

// the angle between where his head points and the target, on the horizontal plane only: at 5 m the eye looks down
// at the target more steeply than the planner action's PI/8 allows
float CAI_Stalker::scripted_throw_yaw_error()
{
	Fvector to_target = Fvector().sub(m_scripted_throw_target, Position());
	to_target.y = 0.f;
	if (to_target.square_magnitude() < EPS_L)
		return 0.f;

	to_target.normalize();
	Fvector const head = Fvector().setHP(-movement().m_head.current.yaw, 0.f);
	float cos_alpha = head.dotproduct(to_target);
	clamp(cos_alpha, -1.f, 1.f);
	return acosf(cos_alpha);
}

// the planner action's test (stalker_combat_actions.cpp)
bool CAI_Stalker::scripted_throw_aligned()
{
	return scripted_throw_yaw_error() < k_aim_angle;
}

// The hide, show and throw animations of an NPC's items are counted by the items' own frame updates: each one adds
// one frame's time (CHudItem::UpdateCL). A grenade gets a frame update every frame (CMissile::AlwaysTheCrow), and so
// does the man who has one in his slot. A gun does not: farther than 30 m from the camera and not drawn, it gets one
// only after its scheduler update, a few times a second. Its half-second "put away" then takes many seconds, and so
// does drawing it again. So during a request, and until the gun is back after it:
//	- he and the items that are changing hands are put on the next frame's update list, wherever the camera is;
//	- the inventory's own update, which makes the slot change once an item is hidden and which he otherwise gets from
//	  the scheduler only, runs every frame (as the actor's does).
void CAI_Stalker::scripted_throw_keep_updated()
{
	MakeMeCrow();

	CInventory& items = inventory();
	if (CInventoryItem* const active = items.ActiveItem())
		active->object().MakeMeCrow();

	u16 const next_slot = items.GetNextActiveSlot();
	if (next_slot != NO_ACTIVE_SLOT)
	{
		if (CInventoryItem* const next = items.ItemFromSlot(next_slot))
			next->object().MakeMeCrow();
	}

	items.Update();
}

// update_object_handler() after a request has ended, until the gun is back in his hands and at rest (or
// k_restore_ms have passed: whoever runs him may want his hands empty)
void CAI_Stalker::scripted_throw_restore()
{
	CWeapon* const weapon = smart_cast<CWeapon*>(inventory().ActiveItem());
	bool const back = weapon && (weapon->GetState() == CHUDState::eIdle) && !weapon->IsPending();
	if (back || (Device.dwTimeGlobal - m_scripted_throw.end_time >= k_restore_ms))
	{
		m_scripted_throw_restoring = false;
		return;
	}

	scripted_throw_keep_updated();
}

bool CAI_Stalker::scripted_throw_request(const Fvector& target, int crouch)
{
	request_facts facts;
	facts.enabled = !!g_ai_scripted_throw;
	facts.alive = !!g_Alive();
	facts.active = m_scripted_throw.active;
	facts.target_valid = !!_valid(target);
	facts.now = Device.dwTimeGlobal;
	facts.last_throw_time = m_last_throw_time;
	facts.difficulty = int(g_SingleGameDifficulty);

	// the rest is only read for a living stalker that may be asked at all
	facts.wounded = false;
	facts.script_control = false;
	facts.smart_cover = false;
	facts.animation = false;
	facts.prone = false;
	facts.has_grenade = false;
	facts.grenade_in_hand = false;
	facts.reloading = false;
	facts.distance = 0.f;

	CInventoryItem* grenade = nullptr;
	if (facts.enabled && facts.alive && !facts.active)
	{
		facts.wounded = scripted_throw_wounded();
		facts.script_control = !!GetScriptControl();
		facts.smart_cover = scripted_throw_smart_cover();
		facts.animation = scripted_throw_animation_locked();
		facts.prone = scripted_throw_prone();

		CInventoryItem* const item = inventory().ItemFromSlot(GRENADE_SLOT);
		grenade = (item && smart_cast<CGrenade*>(item)) ? item : nullptr;
		facts.has_grenade = !!grenade;
		facts.grenade_in_hand = grenade && (inventory().ActiveItem() == grenade);

		CWeapon* const weapon = smart_cast<CWeapon*>(inventory().ActiveItem());
		facts.reloading = weapon && (weapon->GetState() == CWeapon::eReload);

		if (facts.target_valid)
			facts.distance = Position().distance_to(target);
	}

	scripted_throw::reason const why = check_request(facts);
	if (why != r_none)
	{
		m_scripted_throw.refuse(why);
		scripted_throw_log("refused", why, facts.target_valid ? target : Position(), false);
		return false;
	}

	if (!grenade) // check_request accepts nothing without one
		return false;

	m_scripted_throw.accept(facts.now);
	m_scripted_throw_target = target;
	m_scripted_throw_aim = target;
	m_scripted_throw_grenade_id = grenade->object().ID();
	// crouch: 1 = crouch, 0 = stand, anything else = not said: the npc_on_combat_set_body_state hook decides, as it
	// does for a throw of the combat planner. It is asked on his next update, not from inside the script's call.
	m_scripted_throw_body = (crouch == 1)
		                        ? u8(MonsterSpace::eBodyStateCrouch)
		                        : ((crouch == 0) ? u8(MonsterSpace::eBodyStateStand) : u8(0xff));

	// the planner's throw action shouts here too
	sound().play(StalkerSpace::eStalkerSoundThrowGrenade);

	// his next frame update is the request's first, wherever the camera is
	m_scripted_throw_restoring = false;
	MakeMeCrow();

	scripted_throw_log("start", r_none, target, true);
	return true;
}

bool CAI_Stalker::scripted_throw_cancel()
{
	if (!m_scripted_throw.cancel(Device.dwTimeGlobal))
		return false;

	scripted_throw_ended();
	return true;
}

// A request that ended with no throw after the throw was ordered (the 8 s cap, an exception): the grenade may sit
// in his hand with the pin out, and the inventory only puts away an item that is idle. Put it at rest the way
// CObjectActionHide::finalize does, so that the gun can come back.
void CAI_Stalker::scripted_throw_disarm()
{
	CInventoryItem* const grenade = scripted_throw_grenade();
	if (!grenade || (inventory().ActiveItem() != grenade))
		return;

	CMissile* const missile = smart_cast<CMissile*>(grenade);
	if (!missile)
		return;

	u32 const state = missile->GetState();
	if ((state != CMissile::eThrowStart) && (state != CMissile::eReady))
		return;

	missile->StopCurrentAnimWithoutCallback();
	missile->SetState(CHUDState::eIdle);
	missile->SetNextState(CHUDState::eIdle);
}

// reinit / net_destroy: forget it, tell nobody
void CAI_Stalker::scripted_throw_reset()
{
	m_scripted_throw.reset();
	m_scripted_throw_target.set(0.f, 0.f, 0.f);
	m_scripted_throw_aim.set(0.f, 0.f, 0.f);
	m_scripted_throw_grenade_id = u16(-1);
	m_scripted_throw_body = u8(0xff);
	m_scripted_throw_restoring = false;
}

void CAI_Stalker::scripted_throw_on_death()
{
	m_scripted_throw_restoring = false;

	if (!m_scripted_throw.active)
		return;

	m_scripted_throw.on_death(Device.dwTimeGlobal);
	scripted_throw_ended();
}

// CGrenade::Throw: a grenade has left his hand
void CAI_Stalker::scripted_throw_on_release(u16 grenade_id)
{
	if (!m_scripted_throw.active || (grenade_id != m_scripted_throw_grenade_id))
		return;

	m_scripted_throw.on_release(Device.dwTimeGlobal);
}

// the request has just ended (the machine is not active any more): the cooldowns, the log line, the script callback
void CAI_Stalker::scripted_throw_ended()
{
	VERIFY(!m_scripted_throw.active);
	bool const ok = (m_scripted_throw.st == st_thrown);
	int const why = m_scripted_throw.why;

	// as after a throw of the combat planner: his own and his group's "last throw" time
	if (ok && g_Alive())
		on_throw_completed();

	// the facts of the debug line are those of the moment it ended, before anything is put right
	scripted_throw_log(ok ? "thrown" : "failed", why, m_scripted_throw_target, true);

	if (!ok && g_Alive())
		scripted_throw_disarm();

	// the gun has to come back: he stays on the per-frame update until it has (scripted_throw_restore)
	m_scripted_throw_restoring = !!g_Alive() && !getDestroy();

	if (getDestroy())
		return;

	CScriptGameObject* const self = lua_game_object();
	if (!self)
		return;

	// npc_on_scripted_throw(npc, ok, reason) in callbacks_gameobject.script
	::luabind::functor<void> funct;
	if (ai().script_engine().functor("_G.CAI_Stalker__OnScriptedThrow", funct))
		funct(self, ok, reason_name(why));
}

// The arc from his hand to the target, with the engine's own miss rule. A blocked arc gets a few more tries (each
// one picks a new miss offset) and then fails the request: nothing is thrown at a wall in front of him.
// As for the planner's throws this is the arc of this moment: update_throw_params applies its force spread after
// the check, and computes the arc again at the release if he has turned since.
scripted_throw::step CAI_Stalker::scripted_throw_aim()
{
	CLevelGraph const& graph = ai().level_graph();
	u32 vertex_id = u32(-1);
	if (graph.valid_vertex_position(m_scripted_throw_target))
		vertex_id = graph.vertex_id(m_scripted_throw_target);

	if (graph.valid_vertex_id(vertex_id))
		throw_target(m_scripted_throw_target, vertex_id, nullptr);
	else
		throw_target(m_scripted_throw_target, nullptr); // off the AI map: no miss offset can be picked there

	update_throw_params();
	bool const clear = m_throw_enabled;
	bool const in_range = m_throw_position.distance_to(m_throw_target_position)
		<= scripted_throw::max_distance(int(g_SingleGameDifficulty));

	if (clear)
		m_scripted_throw_aim = m_throw_target_position;

	return m_scripted_throw.arc(clear, in_range, Device.dwTimeGlobal);
}

// what update_object_handler() does for a stalker without a request
void CAI_Stalker::scripted_throw_plain_update()
{
	try
	{
		CObjectHandler::update();
	}
	catch (...)
	{
		CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle);
		CObjectHandler::update();
	}
}

// update_object_handler() while a request is active
void CAI_Stalker::scripted_throw_update_object_handler()
{
	scripted_throw_keep_updated();

	// the inventory's update calls a script on a slot change, and a script may call the request off
	if (!m_scripted_throw.active)
	{
		if (g_Alive())
			scripted_throw_plain_update();
		return;
	}

	CInventoryItem* const grenade = scripted_throw_grenade();
	CMissile* const missile = grenade ? smart_cast<CMissile*>(grenade) : nullptr;
	bool const in_hand = missile && (inventory().ActiveItem() == grenade);
	u32 const missile_state = missile ? missile->GetState() : u32(CMissile::eHidden);

	tick_facts facts;
	facts.now = Device.dwTimeGlobal;
	facts.alive = !!g_Alive();
	facts.wounded = scripted_throw_wounded();
	facts.script_control = !!GetScriptControl();
	facts.smart_cover = scripted_throw_smart_cover();
	facts.animation = scripted_throw_animation_locked();
	facts.grenade_present = !!missile;
	facts.in_hand_idle = in_hand && (missile_state == CMissile::eIdle);
	facts.aligned = scripted_throw_aligned();

	scripted_throw::step what = m_scripted_throw.tick(facts);
	if (what == s_aim)
		what = scripted_throw_aim();

	if ((what == s_end) || !missile)
	{
		// over: the others' goal was never taken away, so this is the plain update again
		m_scripted_throw.abort(Device.dwTimeGlobal); // does nothing after s_end
		scripted_throw_ended();
		scripted_throw_plain_update();
		return;
	}

	// somebody else may have aimed him elsewhere since the arc was computed (the planner's evaluator is told not
	// to, see stalker_property_evaluators.cpp): the throw goes where the request said
	if (m_scripted_throw.aimed && !m_throw_target_position.similar(m_scripted_throw_aim, .01f))
		throw_target(m_scripted_throw_aim, nullptr);

	CObjectHandlerPlanner& planner = CObjectHandler::planner();
	CObjectHandlerPlanner::CState const theirs = planner.target_state();

	CObjectHandler::set_goal((what == s_throw) ? MonsterSpace::eObjectActionFire1 : MonsterSpace::eObjectActionIdle,
	                         grenade);

	bool failed = false;
	try
	{
		CObjectHandler::update();
	}
	catch (...)
	{
		failed = true;
	}

	if (failed)
	{
		// as update_object_handler does, and the request is off
		m_scripted_throw.abort(Device.dwTimeGlobal);
		scripted_throw_ended();

		CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle);
		CObjectHandler::update();
		return;
	}

	// put their goal back. An item of theirs that left the inventory during the update has no evaluators any
	// more, and a goal on it would have no solution: empty hands then, as CObjectHandlerPlanner::remove_item does.
	bool valid = !theirs.conditions().empty();
	for (auto const& condition : theirs.conditions())
	{
		if (planner.evaluators().find(condition.condition()) == planner.evaluators().end())
		{
			valid = false;
			break;
		}
	}

	if (valid)
		planner.set_target_state(theirs);
	else
		CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle);
}

// Think() while a request is active: he stands, in danger, in the asked body state, for this update only
void CAI_Stalker::scripted_throw_update_movement(u32 time_delta)
{
	stalker_movement_manager_smart_cover& manager = movement();

	// the request is ended from his frame update; should that have stopped coming, it ends here
	if (m_scripted_throw.overdue(Device.dwTimeGlobal))
	{
		scripted_throw_ended();
		manager.update(time_delta);
		return;
	}

	if (m_scripted_throw_body == u8(0xff))
	{
		MonsterSpace::EBodyState const asked = manager.body_state_combat_override(
			StalkerDecisionSpace::eWorldOperatorThrowGrenade, MonsterSpace::eBodyStateStand);
		m_scripted_throw_body = (asked == MonsterSpace::eBodyStateCrouch)
			                        ? u8(MonsterSpace::eBodyStateCrouch)
			                        : u8(MonsterSpace::eBodyStateStand);
	}

	MonsterSpace::EMovementType const their_movement_type = manager.target_movement_type();
	MonsterSpace::EBodyState const their_body_state = manager.target_body_state();
	MonsterSpace::EMentalState const their_mental_state = manager.target_mental_state();

	// "crouch" and "free" do not go together: danger first, then the body
	manager.set_movement_type(MonsterSpace::eMovementTypeStand);
	manager.set_mental_state(MonsterSpace::eMentalStateDanger);
	manager.set_body_state(MonsterSpace::EBodyState(m_scripted_throw_body));

	manager.update(time_delta);

	// ... and back in the other order. "crouch" with "free" is not a pair the setter takes: should somebody have
	// left it there, he stays in "danger" until they set it again
	manager.set_body_state(their_body_state);
	if ((their_body_state != MonsterSpace::eBodyStateCrouch) || (their_mental_state != MonsterSpace::eMentalStateFree))
		manager.set_mental_state(their_mental_state);
	manager.set_movement_type(their_movement_type);
}

// UpdateCL() while a request is active, before the sight manager updates
void CAI_Stalker::scripted_throw_update_sight()
{
	sight().setup(CSightAction(SightManager::eSightTypePosition, m_scripted_throw_target, true));
}
