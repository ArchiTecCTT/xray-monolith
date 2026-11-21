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
//	Nothing here runs for a stalker without an active request.
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
#include "ai_stalker_space.h"

// console vars (console_commands.cpp), saved in user.ltx
int g_ai_scripted_throw = 1; // 0 = every request is refused ("disabled")
int g_ai_scripted_throw_debug = 0; // 1 = one log line per request start / end

using namespace scripted_throw;

// [THROW] id=.. sec=.. dist=.. result=.. reason=..
static void scripted_throw_log(CAI_Stalker const& stalker, Fvector const& target, LPCSTR result, int why)
{
	if (!g_ai_scripted_throw_debug)
		return;

	Msg("[THROW] id=%u sec=%s dist=%.1f result=%s reason=%s", u32(stalker.ID()), stalker.cNameSect().c_str(),
	    stalker.Position().distance_to(target), result, reason_name(why));
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

// the planner action's test (stalker_combat_actions.cpp), on the horizontal plane only: at 5 m the eye looks down
// at the target more steeply than its PI/8 allows
bool CAI_Stalker::scripted_throw_aligned()
{
	Fvector to_target = Fvector().sub(m_scripted_throw_target, Position());
	to_target.y = 0.f;
	if (to_target.square_magnitude() < EPS_L)
		return true;

	to_target.normalize();
	Fvector const head = Fvector().setHP(-movement().m_head.current.yaw, 0.f);
	float cos_alpha = head.dotproduct(to_target);
	clamp(cos_alpha, -1.f, 1.f);
	return acosf(cos_alpha) < k_aim_angle;
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
	facts.has_grenade = false;
	facts.grenade_in_hand = false;
	facts.reloading = false;
	facts.distance = 0.f;

	CInventoryItem* grenade = nullptr;
	if (facts.enabled && facts.alive && !facts.active)
	{
		facts.wounded = scripted_throw_wounded();
		facts.script_control = !!GetScriptControl();
		facts.smart_cover = movement().in_smart_cover();
		facts.animation = scripted_throw_animation_locked();

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
		scripted_throw_log(*this, facts.target_valid ? target : Position(), "refused", why);
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

	scripted_throw_log(*this, target, "start", r_none);
	return true;
}

bool CAI_Stalker::scripted_throw_cancel()
{
	if (!m_scripted_throw.cancel())
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
}

void CAI_Stalker::scripted_throw_on_death()
{
	if (!m_scripted_throw.active)
		return;

	m_scripted_throw.on_death();
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

	if (!ok && g_Alive())
		scripted_throw_disarm();

	scripted_throw_log(*this, m_scripted_throw_target, ok ? "thrown" : "failed", why);

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

// the arc from his hand to the target, with the engine's own miss rule. A blocked arc gets a few more tries (each
// one picks a new miss offset) and then fails the request: nothing is thrown at a wall.
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

	return m_scripted_throw.arc(clear, in_range);
}

// update_object_handler() while a request is active
void CAI_Stalker::scripted_throw_update_object_handler()
{
	CInventoryItem* const grenade = scripted_throw_grenade();
	CMissile* const missile = grenade ? smart_cast<CMissile*>(grenade) : nullptr;
	bool const in_hand = missile && (inventory().ActiveItem() == grenade);
	u32 const missile_state = missile ? missile->GetState() : u32(CMissile::eHidden);

	tick_facts facts;
	facts.now = Device.dwTimeGlobal;
	facts.alive = !!g_Alive();
	facts.wounded = scripted_throw_wounded();
	facts.script_control = !!GetScriptControl();
	facts.smart_cover = movement().in_smart_cover();
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
		m_scripted_throw.abort(); // does nothing after s_end
		scripted_throw_ended();

		try
		{
			CObjectHandler::update();
		}
		catch (...)
		{
			CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle);
			CObjectHandler::update();
		}
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
		m_scripted_throw.abort();
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
