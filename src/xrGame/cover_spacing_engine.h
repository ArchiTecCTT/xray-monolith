#pragma once

// Ally cover spacing, the engine side (docs/cover_spacing.md): who claims which spot, the console vars and the
// debug lines. The data and the rules themselves are in cover_spacing.h.

class CAI_Stalker;
class CMemberOrder;
class CCoverPoint;

// console vars (console_commands.cpp), saved in user.ltx. 0 switches the matching rule off (old behaviour).
extern float g_ai_cover_ally_spacing; // m, a cover this close to an ally's claimed cover is refused
extern float g_ai_cover_body_spacing; // m, a cover this close to an ally who has no cover is refused
extern float g_ai_cover_fallback_side_offset; // m, no cover found: stand this far to the side of a teammate
extern int g_ai_cover_spacing_scope; // whose claims count: 0 = own group, 1 = the same team on the level
extern int g_ai_cover_spacing_debug; // 1 = "[COVER] ..." lines in the log

namespace cover_spacing_engine
{
	// CMemberOrder (one per living stalker, made and deleted by his agent manager) calls these three
	int on_member_add(const CMemberOrder* order);
	void on_member_remove(const CMemberOrder* order, int slot);
	void on_member_cover(const CMemberOrder* order, const CCoverPoint* cover);

	// drops whatever the stalker has claimed (he leaves combat). His member cover is left as it is.
	void release(CAI_Stalker* stalker);
	// drops his fallback side-step spot only
	void release_side_step(CAI_Stalker* stalker);

	// CAgentLocationManager::suitable: is this spot too close to what an ally has claimed?
	bool refuses(CAI_Stalker* stalker, const Fvector& spot);
	// the same question for one teammate who is in combat and has no cover: is the spot his, by his body?
	bool body_refuses(CAI_Stalker* stalker, const CAI_Stalker& teammate, const Fvector& spot);
	// CAgentLocationManager::make_suitable: the stalker took this cover, allies of other groups that held a cover
	// too close to it lose theirs (the vanilla loop does this for his own group)
	void evict_neighbours(CAI_Stalker* stalker, const CCoverPoint* cover);

	// CCoverEvaluatorBest: false when both spacing vars are 0 (the old +1000 body check is used then)
	bool penalty_enabled();
	// what a spot costs for the allies heading to or standing near it: 0 = free
	float penalty(CAI_Stalker* stalker, const Fvector& spot);

	// CStalkerActionTakeCover, no cover found: a spot beside one of the nearest teammates that have an enemy.
	// false = none (the caller stays where it is, as before)
	bool side_step(CAI_Stalker* stalker, const Fvector& enemy_position, u32& vertex_id, Fvector& position);
	// is the side-step spot he uses still clear of other allies (their bodies and the spots they head for)?
	bool side_step_free(CAI_Stalker* stalker, const Fvector& position);
}
