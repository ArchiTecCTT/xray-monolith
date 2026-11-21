#include "stdafx.h"
#include "cover_spacing_engine.h"
#include "cover_spacing.h"
#include "member_order.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "cover_point.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai_object_location.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "stalker_movement_manager_smart_cover.h"
#include "restricted_object.h"

// Ally cover spacing (docs/cover_spacing.md). Defaults: 4 m between claimed covers, 2.5 m from a teammate with no
// cover, 3.5 m side-step, claims of the whole team count. 0 = that rule off.
float g_ai_cover_ally_spacing = 4.f;
float g_ai_cover_body_spacing = 2.5f;
float g_ai_cover_fallback_side_offset = 3.5f;
int g_ai_cover_spacing_scope = 1;
int g_ai_cover_spacing_debug = 0;

namespace
{
	typedef cover_spacing::registry<const CMemberOrder> registry_type;
	typedef registry_type::member member_type;

	// Every stalker thinks on the main thread (the scheduler), so the list needs no lock.
	registry_type s_registry;

	const u32 k_note_interval_ms = 1000; // one debug line per stalker and reason per second
	const float k_side_step_mate_range = 30.f; // a teammate further than this is not worth running to
	const u32 k_side_step_mates = 4; // how many of the nearest teammates the side-step tries to stand beside

	IC cover_spacing::vec to_vec(const Fvector& v)
	{
		return cover_spacing::make_vec(v.x, v.y, v.z);
	}

	IC Fvector to_fvector(const cover_spacing::vec& v)
	{
		return Fvector().set(v.x, v.y, v.z);
	}

	// the stalker's own record, 0 when he has none (dead, or his agent manager is gone)
	const CMemberOrder* order_of(const CAI_Stalker* stalker)
	{
		if (!stalker->g_Alive())
			return 0;
		// CAI_Stalker::agent_manager() takes for granted that his group has a manager. It has one for as long as
		// a stalker is registered in the group, so look for him there first.
		const CGroupHierarchyHolder& group = Level().seniority_holder().team(stalker->g_Team())
		                                            .squad(stalker->g_Squad()).group(stalker->g_Group());
		const CEntity* entity = stalker;
		if (std::find(group.members().begin(), group.members().end(), entity) == group.members().end())
			return 0;
		return group.agent_manager().member().get_member(stalker->ID());
	}

	LPCSTR reason_name(cover_spacing::note_reason reason)
	{
		switch (reason)
		{
		case cover_spacing::note_ally_claim: return "ally_claim";
		case cover_spacing::note_body: return "body";
		default: return "side_step";
		}
	}

	// "[COVER] id=.. reason=.." at most once a second for one stalker and one reason; `more` counts the picks of
	// that kind that were not printed since the last line, so the log still adds up
	void note(const CAI_Stalker* stalker, cover_spacing::note_reason reason, LPCSTR what, u32 other_id, float distance,
	          float value)
	{
		const CMemberOrder* order = order_of(stalker);
		if (!order)
			return;
		unsigned skipped = 0;
		if (!s_registry.note(order->spacing_slot(), reason, Device.dwTimeGlobal, k_note_interval_ms, skipped))
			return;
		Msg("[COVER] id=%u reason=%s what=%s other=%u dist=%.1f value=%.2f more=%u name=%s time=%u",
		    u32(stalker->ID()), reason_name(reason), what, other_id, distance, value, skipped,
		    stalker->cName().c_str(), Device.dwTimeGlobal);
	}

	struct scope_filter
	{
		const CAI_Stalker* m_self;
		int m_team, m_squad, m_group, m_scope;

		scope_filter(const CAI_Stalker* stalker) :
			m_self(stalker), m_team(stalker->g_Team()), m_squad(stalker->g_Squad()), m_group(stalker->g_Group()),
			m_scope(g_ai_cover_spacing_scope)
		{
		}

		// an ally other than the stalker himself, inside the chosen scope
		IC bool operator()(const member_type& m) const
		{
			return (&m.owner->object() != m_self) &&
				registry_type::in_scope(m, m_team, m_squad, m_group, m_scope);
		}
	};

	void refresh_bodies()
	{
		s_registry.refresh_bodies(Device.dwTimeGlobal, [](const CMemberOrder* order, cover_spacing::vec& p)
		{
			p = to_vec(order->object().Position());
		});
	}

	// Does an ally stand within `spacing` of the spot, or head for a spot that close? `except` (may be 0) is a
	// teammate whose body does not count. refresh_bodies() must have run.
	bool crowded(const scope_filter& ally, const Fvector& spot, float spacing, const CAI_Stalker* except)
	{
		if (spacing <= 0.f)
			return false;
		bool result = false;
		const cover_spacing::vec p = to_vec(spot);
		s_registry.claims_near(p, spacing, [&](const member_type& m, float)
		{
			result = result || ally(m);
		});
		s_registry.bodies_near(p, spacing, [&](const member_type& m, float)
		{
			result = result || (ally(m) && (&m.owner->object() != except));
		});
		return result;
	}
}

int cover_spacing_engine::on_member_add(const CMemberOrder* order)
{
	// called from his spawn: the team, squad and group are set by now, the object id and the position are not
	const CAI_Stalker& stalker = order->object();
	return s_registry.add(stalker.g_Team(), stalker.g_Squad(), stalker.g_Group(), order);
}

void cover_spacing_engine::on_member_remove(const CMemberOrder* order, int slot)
{
	// death, net_Destroy and a change of team all end here
	s_registry.remove(slot, order);
}

void cover_spacing_engine::on_member_cover(const CMemberOrder* order, const CCoverPoint* cover)
{
	// called on every brain update of a stalker in cover, nearly always with the cover he already holds:
	// set_claim and clear_claim are a compare and nothing else then
	if (cover)
		s_registry.set_claim(order->spacing_slot(), cover_spacing::claim_cover, to_vec(cover->position()));
	else
		s_registry.clear_claim(order->spacing_slot(), cover_spacing::claim_cover);
}

void cover_spacing_engine::release(CAI_Stalker* stalker)
{
	if (const CMemberOrder* order = order_of(stalker))
		s_registry.clear_claim(order->spacing_slot());
}

void cover_spacing_engine::release_side_step(CAI_Stalker* stalker)
{
	if (const CMemberOrder* order = order_of(stalker))
		s_registry.clear_claim(order->spacing_slot(), cover_spacing::claim_side);
}

bool cover_spacing_engine::refuses(CAI_Stalker* stalker, const Fvector& spot)
{
	const float ally_spacing = g_ai_cover_ally_spacing;
	const float body_spacing = g_ai_cover_body_spacing;
	if (ally_spacing <= 0.f && body_spacing <= 0.f)
		return false;
	if (!s_registry.claim_count())
		return false;

	const scope_filter ally(stalker);
	const float me_to_spot_sqr = stalker->Position().distance_to_sqr(spot);
	const member_type* refused_by = 0;
	float refused_distance_sqr = 0.f;

	s_registry.claims_near(to_vec(spot), _max(ally_spacing, body_spacing), [&](const member_type& m, float d)
	{
		if (refused_by || !ally(m))
			return;
		bool refuse;
		if (m.kind == cover_spacing::claim_cover)
			refuse = cover_spacing::claim_refuses(
				d, ally_spacing, m.owner->object().Position().distance_to_sqr(to_fvector(m.claim)), me_to_spot_sqr);
		else
			// a side-step spot is no cover: it counts as the body that stands (or will stand) there
			refuse = cover_spacing::body_refuses(d, body_spacing, me_to_spot_sqr);
		if (refuse)
		{
			refused_by = &m;
			refused_distance_sqr = d;
		}
	});

	if (!refused_by)
		return false;

	if (g_ai_cover_spacing_debug)
	{
		const bool by_cover = (refused_by->kind == cover_spacing::claim_cover);
		note(stalker, by_cover ? cover_spacing::note_ally_claim : cover_spacing::note_body, "refuse",
		     refused_by->owner->object().ID(), _sqrt(refused_distance_sqr), 0.f);
	}
	return true;
}

bool cover_spacing_engine::body_refuses(CAI_Stalker* stalker, const CAI_Stalker& teammate, const Fvector& spot)
{
	const float body_to_spot_sqr = teammate.Position().distance_to_sqr(spot);
	if (!cover_spacing::body_refuses(body_to_spot_sqr, g_ai_cover_body_spacing,
	                                 stalker->Position().distance_to_sqr(spot)))
		return false;

	if (g_ai_cover_spacing_debug)
		note(stalker, cover_spacing::note_body, "refuse", teammate.ID(), _sqrt(body_to_spot_sqr), 0.f);
	return true;
}

void cover_spacing_engine::evict_neighbours(CAI_Stalker* stalker, const CCoverPoint* cover)
{
	const float ally_spacing = g_ai_cover_ally_spacing;
	if (ally_spacing <= 0.f || !cover || !s_registry.claim_count())
		return;

	// collect first: taking a cover away changes the list that is being walked
	const scope_filter ally(stalker);
	const u32 k_max_losers = 16;
	const CMemberOrder* losers[k_max_losers];
	u32 loser_count = 0;
	s_registry.claims_near(to_vec(cover->position()), ally_spacing, [&](const member_type& m, float)
	{
		if (m.kind != cover_spacing::claim_cover || !ally(m) || !m.owner->cover())
			return;
		if (loser_count < k_max_losers)
			losers[loser_count++] = m.owner;
	});

	for (u32 i = 0; i < loser_count; ++i)
	{
		const CMemberOrder* loser = losers[i];
		loser->object().on_cover_blocked(loser->cover());
		loser->cover(0);
	}
}

bool cover_spacing_engine::penalty_enabled()
{
	return (g_ai_cover_ally_spacing > 0.f) || (g_ai_cover_body_spacing > 0.f);
}

float cover_spacing_engine::penalty(CAI_Stalker* stalker, const Fvector& spot)
{
	const float ally_spacing = g_ai_cover_ally_spacing;
	const float body_spacing = g_ai_cover_body_spacing;
	const scope_filter ally(stalker);
	const cover_spacing::vec p = to_vec(spot);

	const float me_to_spot_sqr = stalker->Position().distance_to_sqr(spot);
	float total = 0.f, worst = 0.f, worst_distance_sqr = 0.f;
	const member_type* worst_member = 0;
	auto add = [&](const member_type& m, float d, float value)
	{
		total += value;
		if (value > worst)
		{
			worst = value;
			worst_member = &m;
			worst_distance_sqr = d;
		}
	};

	// allies that have a spot count by the spot (where they are heading) ...
	s_registry.claims_near(p, _max(ally_spacing, body_spacing), [&](const member_type& m, float d)
	{
		if (!ally(m))
			return;
		if (m.kind == cover_spacing::claim_cover)
			add(m, d, cover_spacing::crowd_penalty(d, ally_spacing));
		else
			add(m, d, cover_spacing::body_penalty(d, body_spacing, me_to_spot_sqr));
	});

	// ... and allies that have none count by where they stand
	if (body_spacing > 0.f)
	{
		refresh_bodies();
		s_registry.bodies_near(p, body_spacing, [&](const member_type& m, float d)
		{
			if (ally(m))
				add(m, d, cover_spacing::body_penalty(d, body_spacing, me_to_spot_sqr));
		});
	}

	if (total <= 0.f)
		return 0.f;

	clamp(total, 0.f, cover_spacing::k_penalty_cap);
	if (g_ai_cover_spacing_debug)
	{
		const bool by_cover = (worst_member->kind == cover_spacing::claim_cover);
		note(stalker, by_cover ? cover_spacing::note_ally_claim : cover_spacing::note_body, "penalty",
		     worst_member->owner->object().ID(), _sqrt(worst_distance_sqr), total);
	}
	return total;
}

bool cover_spacing_engine::side_step(CAI_Stalker* stalker, const Fvector& enemy_position, u32& vertex_id,
                                     Fvector& position)
{
	const float offset = g_ai_cover_fallback_side_offset;
	const CMemberOrder* order = order_of(stalker);
	if (offset <= 0.f || !order)
		return false;

	// The teammates of the own group that have an enemy, nearest first, k_side_step_mates at most (the old code
	// took the first one in the list, the same man for everybody). A man that already stands beside somebody
	// has an enemy too, so the next one can stand beside him: with little cover a squad ends up in a line across
	// the enemy's direction, not in a knot.
	// (order_of found his record, so his group has an agent manager)
	const CAgentMemberManager::MEMBER_STORAGE& members = stalker->agent_manager().member().members();
	CAI_Stalker* mates[k_side_step_mates];
	float mate_distances_sqr[k_side_step_mates];
	u32 mate_count = 0;
	u32 my_index = 0;
	for (u32 i = 0; i < members.size(); ++i)
	{
		CAI_Stalker* candidate = &members[i]->object();
		if (candidate == stalker)
		{
			my_index = i;
			continue;
		}
		if (!candidate->memory().enemy().selected())
			continue;
		const float distance_sqr = candidate->Position().distance_to_sqr(stalker->Position());
		if (distance_sqr >= _sqr(k_side_step_mate_range))
			continue;

		// keep the nearest few, sorted
		u32 place = mate_count;
		while (place > 0 && mate_distances_sqr[place - 1] > distance_sqr)
			--place;
		if (place >= k_side_step_mates)
			continue;
		if (mate_count < k_side_step_mates)
			++mate_count;
		for (u32 j = mate_count - 1; j > place; --j)
		{
			mates[j] = mates[j - 1];
			mate_distances_sqr[j] = mate_distances_sqr[j - 1];
		}
		mates[place] = candidate;
		mate_distances_sqr[place] = distance_sqr;
	}

	const CLevelGraph& graph = ai().level_graph();
	const float body_spacing = g_ai_cover_body_spacing;
	const scope_filter ally(stalker);
	if (mate_count && body_spacing > 0.f)
		refresh_bodies();

	// my place in the list decides the side that is tried first: odd places the left, even ones the right
	const bool left_first = !!(my_index & 1);
	bool found = false, left = left_first;
	CAI_Stalker* mate = 0;
	float mate_distance_sqr = 0.f;
	for (u32 i = 0; i < mate_count && !found; ++i)
	{
		mate = mates[i];
		mate_distance_sqr = mate_distances_sqr[i];
		const u32 mate_vertex_id = mate->ai_location().level_vertex_id();
		if (!graph.valid_vertex_id(mate_vertex_id))
			continue;

		for (u32 attempt = 0; attempt < 2 && !found; ++attempt)
		{
			left = (attempt == 0) ? left_first : !left_first;
			cover_spacing::vec target;
			if (!cover_spacing::side_step_target(to_vec(mate->Position()), to_vec(enemy_position), offset, left,
			                                     target))
				break;

			// the spot must be on the AI map and reachable from the teammate in a straight line: the same open
			// ground he stands on, not the far side of a wall or another floor
			Fvector spot = to_fvector(target);
			if (!graph.valid_vertex_position(spot))
				continue;
			const u32 spot_vertex_id = graph.check_position_in_direction(mate_vertex_id, mate->Position(), spot);
			if (!graph.valid_vertex_id(spot_vertex_id))
				continue;
			spot.y = graph.vertex_plane_y(spot_vertex_id, spot.x, spot.z);
			if (!stalker->movement().restrictions().accessible(spot_vertex_id) ||
				!stalker->movement().restrictions().accessible(spot))
				continue;

			// not where another ally stands or is heading (the teammate himself is `offset` away by design)
			if (crowded(ally, spot, body_spacing, mate))
				continue;

			vertex_id = spot_vertex_id;
			position = spot;
			found = true;
		}
	}

	if (found)
		s_registry.set_claim(order->spacing_slot(), cover_spacing::claim_side, to_vec(position));
	else
		s_registry.clear_claim(order->spacing_slot(), cover_spacing::claim_side);

	if (g_ai_cover_spacing_debug)
		note(stalker, cover_spacing::note_side_step, found ? (left ? "left" : "right") : "none",
		     mate ? u32(mate->ID()) : u32(-1), mate ? _sqrt(mate_distance_sqr) : 0.f, offset);
	return found;
}

bool cover_spacing_engine::side_step_free(CAI_Stalker* stalker, const Fvector& position)
{
	const float body_spacing = g_ai_cover_body_spacing;
	if (body_spacing <= 0.f)
		return true;
	refresh_bodies();
	return !crowded(scope_filter(stalker), position, body_spacing, 0);
}
