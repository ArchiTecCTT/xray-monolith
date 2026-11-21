// Standalone regression for ally cover spacing (src/xrGame/cover_spacing.h, docs/cover_spacing.md); not an engine
// build.
//
// Part 1 runs the header the engine includes: the refusal rules, the soft penalty, the side-step spot, the grid and
// the level-wide list (claims, bodies, scope, the debug rate limit).
// Part 2 plays a small squad on a row of cover points with those rules, the way the engine's search uses them:
// with the default spacing nobody ends up near anybody, with 0 they stack as before.
// Part 3 is a size check: 200 stalkers, every one of them searching 300 spots.
// Part 4 reads the engine sources and the game data and checks the wiring (the sudden attack switch included).
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/cover_spacing_test.cpp -o /tmp/cover_spacing_test
//   /tmp/cover_spacing_test .
// The argument is the repository root (default ".").
#include "../src/xrGame/cover_spacing.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace cover_spacing;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::printf("FAIL cannot read %s\n", path.c_str()); ++g_fail; return ""; }
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}
static int count(const std::string& text, const std::string& what)
{
	int n = 0;
	for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
	return n;
}
// the text of a function: from its signature to the first line that is a lone closing brace
static std::string body(const std::string& text, const std::string& signature)
{
	const size_t a = text.find(signature);
	if (a == std::string::npos) return "";
	size_t b = text.find("\n}", a);
	return text.substr(a, b == std::string::npos ? b : b - a);
}
static bool near_to(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// a stand-in for CMemberOrder: where the man is
struct man
{
	vec pos;
	int slot;
};
typedef registry<man> list;

static void position_of(man* m, vec& p) { p = m->pos; }

// what CAgentLocationManager::suitable asks of the level-wide list (cover_spacing_engine::refuses)
static bool refuses(const list& l, const man& me, const vec& spot, float ally_spacing, float body_spacing, int scope)
{
	const list::member* self = l.find(me.slot);
	const float me_sqr = dist_sqr(me.pos, spot);
	bool refused = false;
	l.claims_near(spot, ally_spacing > body_spacing ? ally_spacing : body_spacing, [&](const list::member& m, float d)
	{
		if (m.owner == &me || !list::in_scope(m, self->team, self->squad, self->group, scope))
			return;
		if (m.kind == claim_cover)
			refused = refused || claim_refuses(d, ally_spacing, dist_sqr(m.owner->pos, m.claim), me_sqr);
		else
			refused = refused || body_refuses(d, body_spacing, me_sqr);
	});
	l.bodies_near(spot, body_spacing, [&](const list::member& m, float d)
	{
		if (m.owner != &me && list::in_scope(m, self->team, self->squad, self->group, scope))
			refused = refused || body_refuses(d, body_spacing, me_sqr);
	});
	return refused;
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---- part 1: the rules
	// a claimed cover: refused inside the spacing when its owner is as close to it as I am to the spot (+2 m2 tie)
	CHECK(claim_refuses(9.f, 4.f, 1.f, 1.f)); // 3 m apart, both 1 m away: the owner keeps it
	CHECK(claim_refuses(9.f, 4.f, 2.9f, 1.f)); // inside the tie margin
	CHECK(!claim_refuses(9.f, 4.f, 100.f, 1.f)); // he is 10 m from his cover, I am 1 m from mine: mine
	CHECK(!claim_refuses(16.1f, 4.f, 0.f, 0.f)); // further apart than the spacing
	CHECK(claim_refuses(16.f, 4.f, 0.f, 0.f)); // exactly the spacing still counts (the vanilla <=)
	CHECK(!claim_refuses(0.f, 0.f, 0.f, 0.f)); // spacing 0 = off
	// a teammate with no cover: his body owns the spots he is nearer to than I am
	CHECK(body_refuses(1.f, 2.5f, 25.f));
	CHECK(!body_refuses(1.f, 2.5f, 0.25f)); // I sit on the spot, he walks past: still mine
	CHECK(!body_refuses(9.f, 2.5f, 25.f)); // outside the spacing
	CHECK(!body_refuses(1.f, 0.f, 25.f)); // off
	// the soft penalty: 2 on the spot, 0 from the spacing on, a straight line between
	CHECK(near_to(crowd_penalty(0.f, 4.f), 2.f));
	CHECK(near_to(crowd_penalty(4.f, 4.f), 1.f)); // 2 m of 4
	CHECK(crowd_penalty(16.f, 4.f) == 0.f && crowd_penalty(100.f, 4.f) == 0.f);
	CHECK(crowd_penalty(1.f, 0.f) == 0.f);
	CHECK(k_penalty_cap < 10.f); // a crowded spot behind a wall must still beat a free one in the open (+10)
	// an ally with no cover costs only on spots he is nearer to than I am: not on the cover I sit on
	CHECK(near_to(body_penalty(1.f, 2.5f, 100.f), crowd_penalty(1.f, 2.5f)) && body_penalty(1.f, 2.5f, 100.f) > 0.f);
	CHECK(body_penalty(1.f, 2.5f, 0.25f) == 0.f);
	CHECK(body_penalty(1.f, 2.5f, 1.f) == 0.f); // a tie is mine

	// the side-step: enemy to the north (+z), so "right" is east (+x) and "back" is south
	vec spot;
	const vec mate = make_vec(10.f, 1.f, 10.f), enemy = make_vec(10.f, 0.f, 60.f);
	CHECK(side_step_target(mate, enemy, 3.5f, false, spot));
	CHECK(near_to(spot.x, 13.5f) && near_to(spot.z, 9.f) && near_to(spot.y, 1.f));
	CHECK(side_step_target(mate, enemy, 3.5f, true, spot));
	CHECK(near_to(spot.x, 6.5f) && near_to(spot.z, 9.f));
	CHECK(!side_step_target(mate, mate, 3.5f, true, spot)); // enemy on top of him: no line to stand beside
	// never behind the teammate as seen from the enemy: both spots are `offset` off his line to the enemy
	const vec enemy2 = make_vec(-30.f, 0.f, -20.f);
	for (int side = 0; side < 2; ++side)
	{
		CHECK(side_step_target(mate, enemy2, 3.5f, side == 1, spot));
		const float lx = enemy2.x - mate.x, lz = enemy2.z - mate.z, ll = std::sqrt(lx * lx + lz * lz);
		const float across = ((spot.x - mate.x) * lz - (spot.z - mate.z) * lx) / ll;
		const float along = ((spot.x - mate.x) * lx + (spot.z - mate.z) * lz) / ll;
		CHECK(near_to(std::fabs(across), 3.5f) && near_to(along, -k_side_step_back));
	}

	// the grid: negative coordinates, cell borders, removal, a second point in the same cell
	{
		point_grid g;
		g.insert(0, make_vec(-0.1f, 0.f, -0.1f));
		g.insert(1, make_vec(0.1f, 0.f, 0.1f)); // the next cell over, 0.28 m away
		g.insert(2, make_vec(0.2f, 0.f, 0.2f));
		g.insert(7, make_vec(500.f, 0.f, -730.f));
		int hits = 0;
		g.query(make_vec(0.f, 0.f, 0.f), 1.f, [&](unsigned, float) { ++hits; });
		CHECK(hits == 3 && g.count() == 4);
		g.remove(1);
		g.remove(1); // twice is harmless
		hits = 0;
		g.query(make_vec(0.f, 0.f, 0.f), 1.f, [&](unsigned s, float) { ++hits; CHECK(s == 0 || s == 2); });
		CHECK(hits == 2 && g.count() == 3 && !g.has(1) && g.has(7));
		// height counts: the floor above is not "near"
		hits = 0;
		g.query(make_vec(0.f, 5.f, 0.f), 1.f, [&](unsigned, float) { ++hits; });
		CHECK(hits == 0);
		// moving a point: found at the new place only
		g.insert(7, make_vec(40.f, 0.f, 40.f));
		hits = 0;
		g.query(make_vec(500.f, 0.f, -730.f), 9.f, [&](unsigned, float) { ++hits; });
		g.query(make_vec(41.f, 0.f, 41.f), 9.f, [&](unsigned s, float d) { hits += 10; CHECK(s == 7 && near_to(d, 2.f)); });
		CHECK(hits == 10);
		// every point is seen once, however many cells the query covers and whatever the buckets share
		point_grid many;
		for (unsigned i = 0; i < 3000; ++i)
			many.insert(i, make_vec(float(i % 60) * 3.f - 90.f, 0.f, float(i / 60) * 3.f - 75.f));
		std::vector<int> seen(3000, 0);
		many.query(make_vec(0.f, 0.f, 0.f), 10.f, [&](unsigned s, float) { ++seen[s]; });
		int inside = 0;
		for (unsigned i = 0; i < 3000; ++i)
		{
			const vec p = make_vec(float(i % 60) * 3.f - 90.f, 0.f, float(i / 60) * 3.f - 75.f);
			const bool in = dist_sqr(p, make_vec(0.f, 0.f, 0.f)) <= 100.f;
			CHECK(seen[i] == (in ? 1 : 0));
			inside += in;
		}
		CHECK(inside > 20);
	}

	// the list: claims, release, scope, bodies, dead slots
	{
		list l;
		man a = {make_vec(0.f, 0.f, 0.f), -1}, b = {make_vec(3.f, 0.f, 0.f), -1}, c = {make_vec(50.f, 0.f, 0.f), -1};
		a.slot = l.add(5, 0, 0, &a); // team 5, group 0
		b.slot = l.add(5, 0, 1, &b); // the same team, another group
		c.slot = l.add(9, 0, 0, &c); // another team
		CHECK(l.count() == 3 && l.claim_count() == 0);

		const vec cover = make_vec(1.f, 0.f, 0.f);
		CHECK(l.set_claim(a.slot, claim_cover, cover));
		CHECK(!l.set_claim(a.slot, claim_cover, cover)); // the same cover again: nothing changes
		CHECK(l.claim_count() == 1);
		const vec near_cover = make_vec(3.f, 0.f, 1.f); // 2.2 m from a's cover
		// b is of another group: scope 1 sees a's claim, scope 0 does not
		CHECK(refuses(l, b, near_cover, 4.f, 2.5f, 1));
		CHECK(!refuses(l, b, near_cover, 4.f, 2.5f, 0));
		// c is another team: never
		CHECK(!refuses(l, c, near_cover, 4.f, 2.5f, 1));
		// a himself is not refused by his own claim
		CHECK(!refuses(l, a, cover, 4.f, 2.5f, 1));
		// spacing 0: off
		CHECK(!refuses(l, b, near_cover, 0.f, 0.f, 1));
		// a is far from his cover and b is right at the spot: b may take it
		a.pos = make_vec(-20.f, 0.f, 0.f);
		CHECK(!refuses(l, b, near_cover, 4.f, 2.5f, 1));
		a.pos = make_vec(0.f, 0.f, 0.f);

		// clear_claim by kind: a cover(0) does not drop a side-step spot, a release drops anything
		CHECK(!l.clear_claim(a.slot, claim_side));
		CHECK(l.clear_claim(a.slot, claim_cover) && l.claim_count() == 0);
		CHECK(!l.clear_claim(a.slot));
		CHECK(l.set_claim(a.slot, claim_side, cover));
		CHECK(!l.clear_claim(a.slot, claim_cover) && l.claim_count() == 1);
		// a side-step spot counts as a body: it refuses only spots it is nearer to than the asker
		CHECK(refuses(l, b, make_vec(1.5f, 0.f, 0.f), 4.f, 2.5f, 1)); // 0.5 m from a's spot, 1.5 m from b
		CHECK(!refuses(l, b, make_vec(3.f, 0.f, 0.f), 4.f, 2.5f, 1)); // b stands on it
		CHECK(l.clear_claim(a.slot) && l.claim_count() == 0);

		// bodies: a has no claim, so his body counts, from the next refresh on
		l.refresh_bodies(1000, position_of);
		CHECK(refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1));
		CHECK(!refuses(l, b, make_vec(2.9f, 0.f, 0.f), 4.f, 2.5f, 1)); // nearer to b
		// a moves: the grid is up to k_body_refresh_ms old, then it follows
		a.pos = make_vec(0.f, 0.f, 30.f);
		l.refresh_bodies(1000 + k_body_refresh_ms - 1, position_of);
		CHECK(refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1));
		l.refresh_bodies(1000 + k_body_refresh_ms, position_of);
		CHECK(!refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1));
		// a man with a claim counts by the claim, not by his body
		a.pos = make_vec(0.f, 0.f, 0.f);
		l.set_claim(a.slot, claim_cover, make_vec(0.f, 0.f, 40.f));
		l.refresh_bodies(5000, position_of);
		CHECK(!refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1));
		// ... and by his body again at once when the claim goes (no waiting for the refresh interval)
		l.clear_claim(a.slot);
		l.refresh_bodies(5001, position_of);
		CHECK(refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1));

		// the debug rate limit: one line a second for one man and one reason, the rest are counted
		unsigned skipped = 99;
		CHECK(l.note(a.slot, note_ally_claim, 10000, 1000, skipped) && skipped == 0);
		CHECK(!l.note(a.slot, note_ally_claim, 10100, 1000, skipped));
		CHECK(!l.note(a.slot, note_ally_claim, 10900, 1000, skipped));
		CHECK(l.note(a.slot, note_body, 10100, 1000, skipped) && skipped == 0); // another reason: its own clock
		CHECK(l.note(b.slot, note_ally_claim, 10100, 1000, skipped) && skipped == 0); // another man
		CHECK(l.note(a.slot, note_ally_claim, 11000, 1000, skipped) && skipped == 2);

		// death: the claim and the body go with the man; his slot is reused and the old slot number is dead
		l.set_claim(a.slot, claim_cover, cover);
		const int old_slot = a.slot;
		l.remove(a.slot, &a);
		CHECK(l.count() == 2 && l.claim_count() == 0 && !l.find(old_slot));
		l.remove(old_slot, &a); // twice is harmless
		CHECK(!l.set_claim(old_slot, claim_cover, cover) && !l.clear_claim(old_slot));
		CHECK(!l.note(old_slot, note_body, 20000, 1000, skipped));
		man d = {make_vec(7.f, 0.f, 7.f), -1};
		d.slot = l.add(5, 0, 0, &d);
		CHECK(d.slot == old_slot && l.find(d.slot)->kind == claim_none);
		l.remove(old_slot, &a); // a's stale slot number must not remove d
		CHECK(l.count() == 3 && l.find(d.slot) && l.find(d.slot)->owner == &d);
		l.refresh_bodies(30000, position_of);
		CHECK(!refuses(l, b, make_vec(0.5f, 0.f, 0.f), 4.f, 2.5f, 1)); // a's body is gone
	}

	// ---- part 2: a squad on a row of cover points
	// 12 cover points 2 m apart along a wall, 10 men walking up from 30 m away, each takes the nearest point the
	// rules allow, in turn. A man who finds none stays coverless (the engine's side-step takes over there).
	for (int pass = 0; pass < 2; ++pass)
	{
		const float ally_spacing = pass ? 0.f : 4.f, body_spacing = pass ? 0.f : 2.5f;
		list l;
		std::vector<man> men(10);
		std::vector<vec> covers;
		for (int i = 0; i < 12; ++i)
			covers.push_back(make_vec(float(i) * 2.f, 0.f, 0.f));
		for (int i = 0; i < 10; ++i)
		{
			men[i].pos = make_vec(8.f + float(i) * 0.7f, 0.f, -30.f);
			men[i].slot = l.add(3, 0, i < 5 ? 0 : 1, &men[i]); // two groups of one team
		}
		std::vector<int> taken(10, -1);
		for (int round = 0; round < 3; ++round)
			for (int i = 0; i < 10; ++i)
			{
				l.refresh_bodies(unsigned(round * 1000 + i), position_of);
				int best = -1;
				float best_sqr = 1e9f;
				for (int c = 0; c < 12; ++c)
				{
					const float d = dist_sqr(men[i].pos, covers[c]);
					if (d < best_sqr && !refuses(l, men[i], covers[c], ally_spacing, body_spacing, 1))
					{
						best = c;
						best_sqr = d;
					}
				}
				taken[i] = best;
				if (best >= 0)
					l.set_claim(men[i].slot, claim_cover, covers[best]);
				else
					l.clear_claim(men[i].slot);
			}
		int covered = 0, shared = 0;
		float closest = 1e9f;
		for (int i = 0; i < 10; ++i)
		{
			if (taken[i] < 0)
				continue;
			++covered;
			for (int j = i + 1; j < 10; ++j)
				if (taken[j] >= 0)
				{
					shared += (taken[i] == taken[j]);
					const float d = std::sqrt(dist_sqr(covers[taken[i]], covers[taken[j]]));
					if (d < closest)
						closest = d;
				}
		}
		std::printf("squad of 10, spacing %.1f: %d in cover, %d pairs on one point, closest pair %.1f m\n",
		            ally_spacing, covered, shared, closest);
		if (!pass)
		{
			// 12 points over 22 m hold 4 men at more than 4 m apart (0, 6, 12, 18 or the like), never two on one
			CHECK(shared == 0);
			CHECK(closest > 4.f);
			CHECK(covered >= 4);
		}
		else
		{
			// the rules off: the list refuses nothing, everybody takes the point nearest to him
			CHECK(covered == 10);
			CHECK(closest < 4.f);
		}
	}

	// ---- part 3: size. 200 stalkers on a 100 x 100 m field, 100 a team, half of them holding a cover; every one of
	// them runs a search over 300 spots (refusal and penalty for each). Nothing here may grow with the head count.
	{
		list l;
		std::vector<man> men(200);
		unsigned seed = 12345;
		auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
		for (int i = 0; i < 200; ++i)
		{
			men[i].pos = make_vec(rnd() * 100.f, 0.f, rnd() * 100.f);
			men[i].slot = l.add(i & 1, 0, 0, &men[i]);
			if (i % 4 < 2)
				l.set_claim(men[i].slot, claim_cover, make_vec(rnd() * 100.f, 0.f, rnd() * 100.f));
		}
		const auto start = std::chrono::steady_clock::now();
		unsigned long refused = 0, visited = 0;
		float penalty_sum = 0.f;
		for (int i = 0; i < 200; ++i)
		{
			l.refresh_bodies(unsigned(i), position_of);
			for (int c = 0; c < 300; ++c)
			{
				const vec p = make_vec(men[i].pos.x + rnd() * 60.f - 30.f, 0.f, men[i].pos.z + rnd() * 60.f - 30.f);
				refused += refuses(l, men[i], p, 4.f, 2.5f, 1);
				l.claims_near(p, 4.f, [&](const list::member&, float d) { ++visited; penalty_sum += crowd_penalty(d, 4.f); });
				l.bodies_near(p, 2.5f, [&](const list::member&, float d) { ++visited; penalty_sum += crowd_penalty(d, 2.5f); });
			}
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// 60000 spots: the grid hands back a few neighbours for each, not the 200
		std::printf("200 stalkers x 300 spots: %.1f ms in all (sanitizer build), %.2f neighbours looked at per spot, "
		            "%lu refused\n", ms, double(visited) / 60000., refused);
		CHECK(double(visited) / 60000. < 5.);
		CHECK(refused > 0 && penalty_sum > 0.f);
		CHECK(ms < 5000.);
	}

	// ---- part 4: the wiring
	const std::string game = root + "/src/xrGame/";
	const std::string order = read(game + "member_order_inline.h");
	const std::string location = read(game + "agent_location_manager.cpp");
	const std::string evaluators = read(game + "cover_evaluators.cpp");
	const std::string actions = read(game + "stalker_combat_actions.cpp");
	const std::string planner = read(game + "stalker_combat_planner.cpp");
	const std::string engine = read(game + "cover_spacing_engine.cpp");
	const std::string console = read(game + "console_commands.cpp");
	const std::string project = read(game + "xrGame.vcxproj");

	// every stalker's member record joins the list when it is made and leaves it when it is deleted (death,
	// net_Destroy, change of team), and every set or clear of his cover goes through the one setter
	CHECK(count(body(order, "IC CMemberOrder::CMemberOrder(CAI_Stalker* object)"), "cover_spacing_engine::on_member_add(this)") == 1);
	CHECK(count(body(order, "IC CMemberOrder::~CMemberOrder()"), "cover_spacing_engine::on_member_remove(this, m_spacing_slot)") == 1);
	CHECK(count(body(order, "IC void CMemberOrder::cover(const CCoverPoint* object_cover) const"), "cover_spacing_engine::on_member_cover(this, object_cover)") == 1);
	CHECK(count(order, "m_cover = ") == 2); // the constructor and the setter: nothing else writes it
	// leaving combat frees the claim; a dead stalker's record is gone already
	const std::string finalize = body(planner, "void CStalkerCombatPlanner::finalize()");
	CHECK(count(finalize, "cover_spacing_engine::release(m_object)") == 1);
	CHECK(finalize.find("g_Alive()") < finalize.find("cover_spacing_engine::release"));

	// suitable(): the vanilla 5 m rule is still there, a coverless teammate in combat is no longer skipped, and the
	// level-wide claims are asked once
	const std::string suitable = body(location, "bool CAgentLocationManager::suitable(");
	CHECK(count(suitable, "<= _sqr(5.f)") == 2);
	CHECK(count(suitable, "+ 2.f)") == 1);
	CHECK(count(suitable, "cover_spacing_engine::body_refuses(object, (*I)->object(), location->position())") == 1);
	CHECK(count(suitable, "cover_spacing_engine::refuses(object, location->position())") == 1);
	CHECK(suitable.find("registered_in_combat") < suitable.find("cover_spacing_engine::body_refuses"));
	CHECK(count(body(location, "void CAgentLocationManager::make_suitable("), "cover_spacing_engine::evict_neighbours(object, location)") == 1);

	// the evaluator: the soft penalty when the feature is on, the old +1000 only when both spacings are 0
	const std::string evaluate = body(evaluators, "void CCoverEvaluatorBest::evaluate_cover(");
	CHECK(count(evaluate, "value += cover_spacing_engine::penalty(m_stalker, cover_point->position());") == 1);
	CHECK(evaluate.find("cover_spacing_engine::penalty_enabled()") < evaluate.find("value += 1000.f"));
	CHECK(evaluate.find("else if (m_stalker") < evaluate.find("value += 1000.f"));
	// in that penalty an ally's cover always counts, an ally with no cover by body_penalty (twice: side-step spots and bodies)
	const std::string penalty = body(engine, "float cover_spacing_engine::penalty(");
	CHECK(count(penalty, "cover_spacing::crowd_penalty(d, ally_spacing)") == 1);
	CHECK(count(penalty, "cover_spacing::body_penalty(d, body_spacing, me_to_spot_sqr)") == 2);

	// the fallback: the side-step when the offset is set, the old 1.2 m behind only when it is 0
	const std::string execute = body(actions, "void CStalkerActionTakeCover::execute()");
	CHECK(count(execute, "cover_spacing_engine::side_step(m_object, position,") == 1);
	// a spot that is still free is kept (two coverless men stepping beside each other would walk backwards otherwise)
	CHECK(count(execute, "if (!m_side_step_valid || !cover_spacing_engine::side_step_free(m_object, m_side_step_position))") == 1);
	CHECK(execute.find("g_ai_cover_fallback_side_offset > 0.f") < execute.find("cover_spacing_engine::side_step"));
	CHECK(execute.find("cover_spacing_engine::side_step") < execute.find("-1.2f"));
	CHECK(count(execute, "set_nearest_accessible_position()") == 1);
	CHECK(count(body(actions, "void CStalkerActionTakeCover::finalize()"), "cover_spacing_engine::release_side_step(m_object)") == 1);

	// the vars: defaults, limits, all five registered (CCC_Float and CCC_Integer are saved to user.ltx)
	CHECK(count(engine, "float g_ai_cover_ally_spacing = 4.f;") == 1);
	CHECK(count(engine, "float g_ai_cover_body_spacing = 2.5f;") == 1);
	CHECK(count(engine, "float g_ai_cover_fallback_side_offset = 3.5f;") == 1);
	CHECK(count(engine, "int g_ai_cover_spacing_scope = 1;") == 1);
	CHECK(count(engine, "int g_ai_cover_spacing_debug = 0;") == 1);
	CHECK(count(console, "CMD4(CCC_Float,   \"ai_cover_ally_spacing\",         &g_ai_cover_ally_spacing,         0.f, 10.f);") == 1);
	CHECK(count(console, "CMD4(CCC_Float,   \"ai_cover_body_spacing\",         &g_ai_cover_body_spacing,         0.f, 10.f);") == 1);
	CHECK(count(console, "CMD4(CCC_Float,   \"ai_cover_fallback_side_offset\", &g_ai_cover_fallback_side_offset, 0.f, 10.f);") == 1);
	CHECK(count(console, "CMD4(CCC_Integer, \"ai_cover_spacing_scope\",        &g_ai_cover_spacing_scope,        0, 1);") == 1);
	CHECK(count(console, "CMD4(CCC_Integer, \"ai_cover_spacing_debug\",        &g_ai_cover_spacing_debug,        0, 1);") == 1);

	// the debug lines carry the three reasons, and are only made when the var is on
	CHECK(count(engine, "\"[COVER] id=%u reason=%s") == 1);
	CHECK(count(engine, "\"ally_claim\"") == 1 && count(engine, "\"body\"") == 1 && count(engine, "\"side_step\"") == 1);
	CHECK(count(engine, "if (g_ai_cover_spacing_debug)") == count(engine, "note(stalker, "));

	// the new file is in the project
	CHECK(count(project, "<ClCompile Include=\"cover_spacing_engine.cpp\" />") == 1);

	// the sudden attack switch: the action reads the hit time and the near bullet time, the sound memory writes the
	// latter where the sound arrives with the spot the bullet hit, the var is registered and on by default
	const std::string sudden = body(actions, "void CStalkerActionSuddenAttack::execute()");
	CHECK(count(sudden, "if (g_ai_sudden_attack_break_on_fire)") == 1);
	CHECK(count(sudden, "object().memory().hit().last_hit_time() >= since") == 1);
	CHECK(count(sudden, "object().memory().sound().last_near_bullet_hit_time() >= since") == 1);
	CHECK(sudden.find("g_ai_sudden_attack_break_on_fire") < sudden.find("const CEntityAlive* enemy"));
	CHECK(count(actions, "int   g_ai_sudden_attack_break_on_fire = 1;") == 1);
	CHECK(count(console, "CMD4(CCC_Integer, \"ai_sudden_attack_break_on_fire\", &g_ai_sudden_attack_break_on_fire, 0, 1);") == 1);
	const std::string sounds = read(game + "sound_memory_manager.cpp");
	const std::string heard = body(sounds, "void CSoundMemoryManager::feel_sound_new(");
	CHECK(count(sounds, "m_last_near_bullet_hit_time = Device.dwTimeGlobal;") == 1);
	CHECK(count(heard, "is_sound_type(sound_type, SOUND_TYPE_WEAPON_BULLET_HIT)") == 1);
	CHECK(count(sounds, "m_last_near_bullet_hit_time = 0;") == 1); // reinit
	CHECK(count(read(game + "sound_memory_manager_inline.h"), "m_last_near_bullet_hit_time = 0;") == 1); // the constructor

	// the game data: every var is on the Stalkers options page (hidden on an exe without it), in lua_help_ex, and
	// has a label and a description
	const std::string options = read(root + "/gamedata/scripts/options_modded_exes_stalkers.script");
	const std::string help = read(root + "/gamedata/scripts/lua_help_ex.script");
	const std::string strings = read(root + "/gamedata/configs/text/eng/ui_mm_modded_exes_cover_spacing.xml");
	const char* vars[] = {"ai_cover_ally_spacing", "ai_cover_body_spacing", "ai_cover_fallback_side_offset",
	                      "ai_cover_spacing_scope", "ai_cover_spacing_debug", "ai_sudden_attack_break_on_fire"};
	for (const char* v : vars)
	{
		const std::string name = v;
		CHECK(count(console, "\"" + name + "\"") == 1);
		CHECK(count(options, "id = \"" + name + "\"") == 1);
		CHECK(count(help, "        " + name + " [0; ") == 1);
		CHECK(count(strings, "<string id=\"ui_mm_modded_exes_gameplay_stalkers_" + name + "\">") == 1);
		CHECK(count(strings, "<string id=\"ui_mm_modded_exes_gameplay_stalkers_" + name + "_desc\">") == 1);
	}
	CHECK(count(options, "get_item_if_exists(") == 7); // the six new ones and ai_enhanced_vision
	// the page's defaults are the engine's
	CHECK(options.find("id = \"ai_cover_ally_spacing\",\n\t\tdef = 4.0,") != std::string::npos);
	CHECK(options.find("id = \"ai_cover_body_spacing\",\n\t\tdef = 2.5,") != std::string::npos);
	CHECK(options.find("id = \"ai_cover_fallback_side_offset\",\n\t\tdef = 3.5,") != std::string::npos);
	CHECK(options.find("id = \"ai_cover_spacing_scope\",\n\t\tdef = \"1\",") != std::string::npos);
	CHECK(options.find("id = \"ai_cover_spacing_debug\",\n\t\tdef = \"0\",") != std::string::npos);
	CHECK(options.find("id = \"ai_sudden_attack_break_on_fire\",\n\t\tdef = \"1\",") != std::string::npos);

	if (g_fail)
	{
		std::printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("cover_spacing_test: all checks passed\n");
	return 0;
}
