// Standalone regression for grenade throws asked for by scripts (src/xrGame/scripted_throw.h, docs/npc_throw.md);
// not an engine build.
//
// Part 1 runs the header the engine includes: every refusal of a request, in the order the engine checks them.
// Part 2 plays requests frame by frame the way ai_stalker_scripted_throw.cpp does: a clean throw, the timeouts, a
// blocked arc, a grenade that goes missing, a wound before and after the throw is ordered, death, cancel.
// Part 3 reads the engine sources and the game data and checks the wiring: each hook is there once and only acts
// for a stalker with an active request, the script calls, the vars and their defaults, the Lua glue.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/scripted_throw_test.cpp -o /tmp/scripted_throw_test
//   /tmp/scripted_throw_test .
// The argument is the repository root (default ".").
#include "../src/xrGame/scripted_throw.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace scripted_throw;

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
static bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
// the text of a function: from its signature to the first line that is a lone closing brace
static std::string body(const std::string& text, const std::string& signature)
{
	const size_t a = text.find(signature);
	if (a == std::string::npos) return "";
	size_t b = text.find("\n}", a);
	return text.substr(a, b == std::string::npos ? b : b - a);
}
// a comes before b in text, and both are there
static bool before(const std::string& text, const std::string& a, const std::string& b)
{
	const size_t pa = text.find(a), pb = text.find(b);
	return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

// ---------------------------------------------------------------------------------------------------- part 1
static request_facts good_request()
{
	request_facts f;
	f.enabled = true;
	f.alive = true;
	f.active = false;
	f.wounded = false;
	f.script_control = false;
	f.smart_cover = false;
	f.animation = false;
	f.has_grenade = true;
	f.grenade_in_hand = false;
	f.target_valid = true;
	f.reloading = false;
	f.distance = 20.f;
	f.difficulty = 1;
	f.now = 100000;
	f.last_throw_time = 0;
	return f;
}

static void test_requests()
{
	CHECK(check_request(good_request()) == r_none);

	{ request_facts f = good_request(); f.enabled = false; CHECK(check_request(f) == r_disabled); }
	{ request_facts f = good_request(); f.alive = false; CHECK(check_request(f) == r_dead); }
	{ request_facts f = good_request(); f.active = true; CHECK(check_request(f) == r_busy); }
	{ request_facts f = good_request(); f.wounded = true; CHECK(check_request(f) == r_wounded); }
	{ request_facts f = good_request(); f.script_control = true; CHECK(check_request(f) == r_script_control); }
	{ request_facts f = good_request(); f.smart_cover = true; CHECK(check_request(f) == r_smart_cover); }
	{ request_facts f = good_request(); f.animation = true; CHECK(check_request(f) == r_animation); }
	{ request_facts f = good_request(); f.has_grenade = false; CHECK(check_request(f) == r_no_grenade); }
	// the grenade is already in his hand: the planner (or somebody) is throwing it
	{ request_facts f = good_request(); f.grenade_in_hand = true; CHECK(check_request(f) == r_busy); }
	{ request_facts f = good_request(); f.target_valid = false; CHECK(check_request(f) == r_bad_target); }
	{ request_facts f = good_request(); f.reloading = true; CHECK(check_request(f) == r_reloading); }

	// "disabled" wins over everything: the A/B switch gives one answer whatever the stalker is doing
	{
		request_facts f = good_request();
		f.enabled = false; f.alive = false; f.has_grenade = false;
		CHECK(check_request(f) == r_disabled);
	}

	// distance: 5 m and the engine's range for the difficulty, less 1 m
	{ request_facts f = good_request(); f.distance = 4.9f; CHECK(check_request(f) == r_too_close); }
	{ request_facts f = good_request(); f.distance = 5.f; CHECK(check_request(f) == r_none); }
	CHECK(max_distance(0) == 30.f && max_distance(1) == 40.f && max_distance(2) == 50.f && max_distance(3) == 60.f);
	CHECK(max_distance(-1) == 30.f && max_distance(9) == 60.f);
	for (int difficulty = 0; difficulty < 4; ++difficulty)
	{
		request_facts f = good_request();
		f.difficulty = difficulty;
		f.distance = max_distance(difficulty) - k_range_margin;
		CHECK(check_request(f) == r_none);
		f.distance += .1f;
		CHECK(check_request(f) == r_too_far);
	}

	// cooldown: 2 s after his last throw, whoever asked for that one; never thrown = no cooldown
	{ request_facts f = good_request(); f.last_throw_time = f.now - 1999; CHECK(check_request(f) == r_cooldown); }
	{ request_facts f = good_request(); f.last_throw_time = f.now - 2000; CHECK(check_request(f) == r_none); }
	{ request_facts f = good_request(); f.now = 500; f.last_throw_time = 0; CHECK(check_request(f) == r_none); }

	// every reason has its own short name, and a number out of range is harmless
	for (int a = 0; a < r_count; ++a)
	{
		CHECK(std::strlen(reason_name(a)) > 0);
		for (int b = a + 1; b < r_count; ++b)
			CHECK(std::strcmp(reason_name(a), reason_name(b)) != 0);
	}
	CHECK(!std::strcmp(reason_name(r_none), "none"));
	CHECK(!std::strcmp(reason_name(r_arc_blocked), "arc_blocked"));
	CHECK(!std::strcmp(reason_name(r_timeout), "timeout"));
	CHECK(!std::strcmp(reason_name(r_no_grenade), "no_grenade"));
	CHECK(!std::strcmp(reason_name(r_too_close), "too_close"));
	CHECK(!std::strcmp(reason_name(r_too_far), "too_far"));
	CHECK(!std::strcmp(reason_name(r_cooldown), "cooldown"));
	CHECK(!std::strcmp(reason_name(r_busy), "busy"));
	CHECK(!std::strcmp(reason_name(r_wounded), "wounded"));
	CHECK(!std::strcmp(reason_name(r_disabled), "disabled"));
	CHECK(!std::strcmp(reason_name(-5), "none") && !std::strcmp(reason_name(r_count), "none"));
}

// ---------------------------------------------------------------------------------------------------- part 2
// the grenade as the engine's object handler would move it, 16 ms a frame
struct sim
{
	machine m;
	unsigned now = 50000;
	bool present = true; // in the slot
	bool in_hand = false;
	bool primed = false;
	bool aligned = false;
	bool alive = true, wounded = false, script_control = false, smart_cover = false, animation = false;
	unsigned swap_ms = 900; // gun down, grenade up
	unsigned turn_ms = 400;
	unsigned windup_ms = 1500; // ai_grenade_throw_delay_base + step, then the throw animation to its mark
	unsigned vanish_ms = 300; // the thrown grenade object leaves the slot this long after the release
	int arc_clear_on_try = 1; // 0 = never
	bool in_range = true;
	unsigned held_since = 0, throw_since = 0, released_at = 0;
	int arc_asked = 0;
	step last = s_end;

	tick_facts facts() const
	{
		tick_facts f;
		f.now = now;
		f.alive = alive;
		f.wounded = wounded;
		f.script_control = script_control;
		f.smart_cover = smart_cover;
		f.animation = animation;
		f.grenade_present = present;
		f.in_hand_idle = present && in_hand && !primed;
		f.aligned = aligned;
		return f;
	}

	// one frame; false = the request is over
	bool frame()
	{
		now += 16;
		if (m.active && !held_since) held_since = m.start;
		if (m.active && present && (now - m.start >= swap_ms)) in_hand = true;
		if (m.active && (now - m.start >= turn_ms)) aligned = true;
		if (released_at && (now - released_at >= vanish_ms)) present = false;

		step s = m.tick(facts());
		if (s == s_aim)
		{
			++arc_asked;
			s = m.arc(arc_clear_on_try && arc_asked >= arc_clear_on_try, in_range);
		}
		last = s;
		if (s == s_end)
			return false;

		if (s == s_throw && present && in_hand && !released_at)
		{
			if (!throw_since) throw_since = now;
			primed = true;
			if (now - throw_since >= windup_ms)
			{
				released_at = now;
				m.on_release(now);
			}
		}
		return true;
	}

	// run until the request ends; returns the ms it took
	unsigned run(unsigned limit_ms = 20000)
	{
		const unsigned t0 = now;
		while (now - t0 < limit_ms && frame()) {}
		return now - t0;
	}
};

static void test_machine()
{
	// a fresh stalker
	{
		machine m;
		CHECK(!m.active && m.st == st_idle && m.why == r_none);
		tick_facts f = sim().facts();
		CHECK(m.tick(f) == s_end); // nothing asked: nothing to do
		CHECK(m.st == st_idle);
		CHECK(!m.cancel());
		m.on_release(1);
		m.on_death();
		CHECK(m.st == st_idle && !m.released);
	}

	// a clean throw: hold, aim once, throw, done about 3 s later
	{
		sim s;
		s.m.accept(s.now);
		CHECK(s.m.active && s.m.st == st_preparing);
		CHECK(s.frame() && s.last == s_hold); // gun still in hand, not turned yet
		const unsigned took = s.run();
		CHECK(!s.m.active && s.m.st == st_thrown && s.m.why == r_none);
		CHECK(s.arc_asked == 1);
		CHECK(s.released_at != 0);
		CHECK(took > 2400 && took < 3200); // 900 swap + 1500 wind-up + 300 until the grenade object is gone
	}

	// the wind-up may run past the 3 s: the prepare timeout only counts until the throw is ordered
	{
		sim s;
		s.swap_ms = 2500;
		s.windup_ms = 2500;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.st == st_thrown);
	}

	// the gun never gets out of the way (a long reload, a stuck animation): timeout after 3 s, nothing thrown
	{
		sim s;
		s.swap_ms = 100000;
		s.m.accept(s.now);
		const unsigned took = s.run();
		CHECK(s.m.st == st_failed && s.m.why == r_timeout);
		CHECK(took >= k_prepare_timeout_ms && took < k_prepare_timeout_ms + 40);
		CHECK(!s.released_at && !s.primed);
	}

	// he never faces the target: the same
	{
		sim s;
		s.turn_ms = 100000;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.st == st_failed && s.m.why == r_timeout && s.arc_asked == 0);
	}

	// the release never comes (no throw mark in the animation): the hard cap ends it
	{
		sim s;
		s.windup_ms = 100000;
		s.m.accept(s.now);
		const unsigned took = s.run();
		CHECK(s.m.st == st_failed && s.m.why == r_timeout);
		CHECK(took >= k_total_timeout_ms && took < k_total_timeout_ms + 40);
	}

	// arc blocked on every try: four tries, then failed, and the pin was never pulled
	{
		sim s;
		s.arc_clear_on_try = 0;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.st == st_failed && s.m.why == r_arc_blocked);
		CHECK(s.arc_asked == k_arc_tries);
		CHECK(!s.primed && !s.released_at);
	}

	// blocked twice, clear on the third miss offset: thrown
	{
		sim s;
		s.arc_clear_on_try = 3;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.st == st_thrown && s.arc_asked == 3);
	}

	// the engine refuses the distance from his hand (the target sits at the edge of the range): "too_far"
	{
		sim s;
		s.arc_clear_on_try = 0;
		s.in_range = false;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.st == st_failed && s.m.why == r_too_far);
	}

	// the grenade leaves the slot before the throw (dropped, traded, taken by a script)
	{
		sim s;
		s.m.accept(s.now);
		for (int i = 0; i < 10; ++i) s.frame();
		s.present = false;
		CHECK(!s.frame());
		CHECK(s.m.st == st_failed && s.m.why == r_no_grenade);
	}

	// wounded while still preparing: called off
	{
		sim s;
		s.m.accept(s.now);
		for (int i = 0; i < 10; ++i) s.frame();
		s.wounded = true;
		CHECK(!s.frame());
		CHECK(s.m.st == st_failed && s.m.why == r_wounded);
	}
	// the same for a script taking him over, a smart cover, an animation that moves him
	{
		sim s; s.m.accept(s.now); s.frame(); s.script_control = true; CHECK(!s.frame());
		CHECK(s.m.why == r_script_control);
	}
	{
		sim s; s.m.accept(s.now); s.frame(); s.smart_cover = true; CHECK(!s.frame());
		CHECK(s.m.why == r_smart_cover);
	}
	{
		sim s; s.m.accept(s.now); s.frame(); s.animation = true; CHECK(!s.frame());
		CHECK(s.m.why == r_animation);
	}

	// once the throw is ordered nothing calls it off (the engine cannot put a grenade away mid-throw): a wound,
	// a script action, a cancel all leave it running, and it ends thrown
	{
		sim s;
		s.m.accept(s.now);
		while (!s.m.aimed && s.frame()) {}
		CHECK(s.m.aimed && s.m.active);
		s.wounded = true;
		s.script_control = true;
		s.smart_cover = true;
		s.animation = true;
		CHECK(!s.m.cancel());
		CHECK(s.m.active && s.m.st == st_preparing);
		s.run();
		CHECK(s.m.st == st_thrown && s.m.why == r_none);
	}
	// ... the very frame the arc is accepted counts already: the order goes out in that frame
	{
		sim s;
		s.m.accept(s.now);
		while (!s.m.aimed && s.frame()) {}
		CHECK(!s.primed || s.throw_since == s.now); // the grenade itself has not moved yet
		CHECK(!s.m.cancel());
	}
	// the engine side breaks: over in any phase
	{
		sim s;
		s.m.accept(s.now);
		while (!s.m.aimed && s.frame()) {}
		s.m.abort();
		CHECK(!s.m.active && s.m.st == st_failed && s.m.why == r_cancelled);
		s.m.abort();
		CHECK(s.m.st == st_failed);
	}

	// death while preparing: failed "dead". Death after the release: it was thrown
	{
		sim s;
		s.m.accept(s.now);
		for (int i = 0; i < 10; ++i) s.frame();
		s.m.on_death();
		CHECK(!s.m.active && s.m.st == st_failed && s.m.why == r_dead);
	}
	{
		sim s;
		s.m.accept(s.now);
		while (!s.released_at && s.frame()) {}
		CHECK(s.released_at && s.m.active);
		s.m.on_death();
		CHECK(!s.m.active && s.m.st == st_thrown && s.m.why == r_none);
	}
	// ... and the same through a frame that sees him dead
	{
		sim s;
		s.m.accept(s.now);
		s.frame();
		s.alive = false;
		CHECK(!s.frame());
		CHECK(s.m.st == st_failed && s.m.why == r_dead);
	}

	// after the release the request ends when the grenade object is gone, or 1.5 s later if it never goes
	{
		sim s;
		s.vanish_ms = 100000;
		s.m.accept(s.now);
		while (!s.released_at && s.frame()) {}
		const unsigned t0 = s.now;
		s.run();
		CHECK(s.m.st == st_thrown);
		CHECK(s.now - t0 >= k_release_tail_ms && s.now - t0 < k_release_tail_ms + 40);
	}

	// cancel while he is still turning or swapping: failed "cancelled", once
	{
		sim s;
		s.m.accept(s.now);
		s.frame();
		CHECK(!s.m.aimed);
		CHECK(s.m.cancel());
		CHECK(!s.m.active && s.m.st == st_failed && s.m.why == r_cancelled);
		CHECK(!s.m.cancel());
		CHECK(s.m.tick(s.facts()) == s_end);
		CHECK(s.m.why == r_cancelled);
	}

	// a refusal while a request runs ("busy") does not disturb it; one on an idle stalker shows as failed
	{
		sim s;
		s.m.accept(s.now);
		s.frame();
		s.m.refuse(r_busy);
		CHECK(s.m.active && s.m.st == st_preparing && s.m.why == r_busy);
		s.run();
		CHECK(s.m.st == st_thrown && s.m.why == r_none);

		s.m.refuse(r_cooldown);
		CHECK(!s.m.active && s.m.st == st_failed && s.m.why == r_cooldown);
	}

	// a second request on the same stalker starts clean
	{
		sim s;
		s.arc_clear_on_try = 0;
		s.m.accept(s.now);
		s.run();
		CHECK(s.m.why == r_arc_blocked);
		s.m.accept(s.now);
		CHECK(s.m.active && s.m.st == st_preparing && s.m.why == r_none && s.m.arc_tries == 0 && !s.m.aimed);
		s.m.reset();
		CHECK(!s.m.active && s.m.st == st_idle && s.m.why == r_none);
	}

	// his frames stop coming (tick is never called): the slower update ends it 10 s after the start
	{
		machine m;
		m.accept(1000);
		CHECK(!m.overdue(1000 + k_overdue_ms - 1) && m.active);
		CHECK(m.overdue(1000 + k_overdue_ms));
		CHECK(!m.active && m.st == st_failed && m.why == r_timeout);
		CHECK(!m.overdue(1000 + 2 * k_overdue_ms));
		CHECK(k_overdue_ms > k_total_timeout_ms);

		machine thrown;
		thrown.accept(1000);
		thrown.on_release(3000);
		CHECK(thrown.overdue(1000 + k_overdue_ms) && thrown.st == st_thrown);
	}

	// a release reported for a request that is over changes nothing
	{
		machine m;
		m.accept(10);
		CHECK(m.cancel());
		m.on_release(20);
		CHECK(!m.released && m.st == st_failed);
	}

	// time wraps (49 days of dwTimeGlobal): the timeouts still count from the start
	{
		sim s;
		s.now = 0xffffff00u;
		s.swap_ms = 100000;
		s.m.accept(s.now);
		const unsigned t0 = s.now;
		while (s.frame()) {}
		CHECK(s.m.why == r_timeout);
		CHECK(s.now - t0 >= k_prepare_timeout_ms && s.now - t0 < k_prepare_timeout_ms + 40);
	}
}

// ---------------------------------------------------------------------------------------------------- part 3
static void test_wiring(const std::string& root)
{
	const std::string game = root + "/src/xrGame/";
	const std::string glue = read(game + "ai/stalker/ai_stalker_scripted_throw.cpp");
	const std::string stalker = read(game + "ai/stalker/ai_stalker.cpp");
	const std::string header = read(game + "ai/stalker/ai_stalker.h");

	// the stalker's three updates: borrowed only while a request is active, the plain path otherwise
	{
		const std::string f = body(stalker, "void CAI_Stalker::update_object_handler()");
		CHECK(before(f, "if (!g_Alive())", "if (m_scripted_throw.active)"));
		CHECK(before(f, "scripted_throw_update_object_handler();", "CObjectHandler::update();"));
		CHECK(count(stalker, "scripted_throw_update_object_handler()") == 1);
	}
	{
		const std::string f = body(stalker, "void CAI_Stalker::Think()");
		CHECK(has(f, "if (m_scripted_throw.active)\n\t\t\t\tscripted_throw_update_movement(update_delta);\n"
		             "\t\t\telse\n\t\t\t\tmovement().update(update_delta);"));
		CHECK(count(stalker, "scripted_throw_update_movement(") == 1);
	}
	{
		const std::string f = body(stalker, "void CAI_Stalker::UpdateCL()");
		CHECK(before(f, "if (m_scripted_throw.active)\n\t\t\t\t\t\tscripted_throw_update_sight();", "sight().update();"));
		CHECK(count(stalker, "scripted_throw_update_sight()") == 1);
	}
	// a request does not outlive him or his object
	// ... after the engine has marked him dead, so a new request from the callback is refused
	CHECK(before(body(stalker, "void CAI_Stalker::Die(CObject* who)"), "inherited::Die(who);",
	             "scripted_throw_on_death();"));
	CHECK(count(stalker, "scripted_throw_on_death();") == 1);
	CHECK(before(body(stalker, "void CAI_Stalker::net_Destroy()"), "scripted_throw_reset();",
	             "inherited::net_Destroy();"));
	CHECK(has(body(stalker, "void CAI_Stalker::reinit()"), "scripted_throw_reset();"));
	// ... and is not written to a save
	CHECK(!has(body(stalker, "void CAI_Stalker::save(NET_Packet& packet)"), "scripted_throw"));
	CHECK(!has(body(stalker, "void CAI_Stalker::net_Save(NET_Packet& P)"), "scripted_throw"));
	CHECK(has(header, "scripted_throw::machine m_scripted_throw;"));
	CHECK(has(header, "#include \"../../scripted_throw.h\""));

	// the glue: the planner's own path for the throw, and their goal goes back after every update
	{
		const std::string f = body(glue, "scripted_throw::step CAI_Stalker::scripted_throw_aim()");
		CHECK(has(f, "throw_target(m_scripted_throw_target, vertex_id, nullptr);"));
		CHECK(before(f, "valid_vertex_position(m_scripted_throw_target)", "graph.vertex_id(m_scripted_throw_target)"));
		CHECK(before(f, "update_throw_params();", "m_throw_enabled"));
		CHECK(has(f, "m_scripted_throw.arc(clear, in_range)"));
	}
	{
		const std::string f = body(glue, "void CAI_Stalker::scripted_throw_update_object_handler()");
		CHECK(before(f, "theirs = planner.target_state();",
		             "MonsterSpace::eObjectActionFire1 : MonsterSpace::eObjectActionIdle"));
		CHECK(before(f, "CObjectHandler::update();", "planner.set_target_state(theirs);"));
		CHECK(has(f, "planner.evaluators().find(condition.condition()) == planner.evaluators().end()"));
	}
	{
		const std::string f = body(glue, "void CAI_Stalker::scripted_throw_update_movement(u32 time_delta)");
		CHECK(before(f, "manager.set_movement_type(MonsterSpace::eMovementTypeStand);",
		             "manager.update(time_delta);\n\n\t// ... and back in the other order"));
		CHECK(before(f, "manager.set_mental_state(MonsterSpace::eMentalStateDanger);",
		             "manager.set_body_state(MonsterSpace::EBodyState(m_scripted_throw_body));"));
		CHECK(before(f, "manager.set_body_state(MonsterSpace::EBodyState(m_scripted_throw_body));",
		             "manager.set_body_state(their_body_state);"));
		CHECK(before(f, "m_scripted_throw.overdue(Device.dwTimeGlobal)", "body_state_combat_override("));
		CHECK(before(f, "manager.set_body_state(their_body_state);", "manager.set_mental_state(their_mental_state);"));
		CHECK(has(f, "manager.set_movement_type(their_movement_type);"));
		// crouch not said: the fork's body state hook decides, as for a planner throw
		CHECK(has(f, "body_state_combat_override(\n\t\t\tStalkerDecisionSpace::eWorldOperatorThrowGrenade"));
	}
	{
		const std::string f = body(glue, "void CAI_Stalker::scripted_throw_ended()");
		CHECK(before(f, "if (ok && g_Alive())\n\t\ton_throw_completed();", "CAI_Stalker__OnScriptedThrow"));
		CHECK(has(f, "funct(self, ok, reason_name(why));"));
		// a failed end leaves the grenade at rest, so that the inventory can put it away
		CHECK(before(f, "if (!ok && g_Alive())\n\t\tscripted_throw_disarm();", "CAI_Stalker__OnScriptedThrow"));
	}
	{
		const std::string f = body(glue, "void CAI_Stalker::scripted_throw_disarm()");
		CHECK(has(f, "(state != CMissile::eThrowStart) && (state != CMissile::eReady)"));
		CHECK(before(f, "missile->StopCurrentAnimWithoutCallback();", "missile->SetState(CHUDState::eIdle);"));
		CHECK(has(f, "missile->SetNextState(CHUDState::eIdle);"));
	}
	CHECK(has(glue, "int g_ai_scripted_throw = 1;"));
	CHECK(has(glue, "int g_ai_scripted_throw_debug = 0;"));
	CHECK(has(glue, "[THROW] id=%u sec=%s dist=%.1f result=%s reason=%s"));
	// a scripted request never asks the "should throw" gate: the caller has decided
	CHECK(!has(glue, "ShouldThrow"));

	// the grenade tells him it has left his hand
	{
		const std::string grenade = read(game + "Grenade.cpp");
		const std::string f = body(grenade, "void CGrenade::Throw()");
		CHECK(before(f, "m_thrown = true;", "stalker->scripted_throw_on_release(ID());"));
		CHECK(has(f, "if (stalker && stalker->scripted_throw_active())"));
	}

	// the combat planner steps aside for this stalker while his request is active, and only then
	{
		const std::string evaluators = read(game + "stalker_property_evaluators.cpp");
		const std::string f = body(evaluators, "_value_type CStalkerPropertyEvaluatorShouldThrowGrenade::evaluate()");
		CHECK(before(f, "if (object().scripted_throw_active())\n\t\treturn (false);",
		             "m_storage->property(eWorldPropertyStartedToThrowGrenade)"));
		CHECK(count(f, "scripted_throw") == 1);
		// the planner's own gate is still where it was
		CHECK(before(f, "if (!object().throw_enabled())", "_G.CAI_Stalker__ShouldThrow"));
		CHECK(count(evaluators, "scripted_throw") == 1);
	}
	{
		const std::string actions = read(game + "stalker_combat_actions.cpp");
		const std::string f = body(actions, "void CStalkerCombatActionThrowGrenade::execute()");
		CHECK(before(f, "if (object().scripted_throw_active())", "ItemFromSlot(GRENADE_SLOT)"));
		CHECK(count(actions, "scripted_throw") == 1);
	}
	{
		const std::string animation = read(game + "stalker_animation_manager_update.cpp");
		const std::string f = body(animation, "bool CStalkerAnimationManager::play_script()");
		CHECK(before(f, "if (script_animations().empty())", "if (object().scripted_throw_active())"));
		CHECK(before(f, "if (object().scripted_throw_active())", "play_script_impl();"));
		CHECK(count(animation, "scripted_throw") == 1);
	}

	// the script calls
	{
		const std::string exported = read(game + "script_game_object_script2.cpp");
		CHECK(count(exported, ".def(\"throw_grenade\",") == 2);
		CHECK(count(exported, ".def(\"throw_grenade_state\",") == 1);
		CHECK(count(exported, ".def(\"throw_grenade_reason\",") == 1);
		CHECK(count(exported, ".def(\"cancel_throw_grenade\",") == 1);

		const std::string calls = read(game + "script_game_object_inventory_owner.cpp");
		// a non-stalker answers quietly
		const std::string f = body(calls, "bool CScriptGameObject::throw_grenade(const Fvector& target)");
		CHECK(has(f, "return stalker && stalker->scripted_throw_request(target, -1);"));
		CHECK(!has(f, "script_log"));
		CHECK(has(calls, "stalker->scripted_throw_request(target, crouch ? 1 : 0)"));
		CHECK(has(calls, "scripted_throw::reason_name(scripted_throw::r_not_stalker)"));

		const std::string level = read(game + "level_script.cpp");
		CHECK(count(level, "def(\"scripted_throw_version\", script_scripted_throw_version)") == 1);
	}

	// the vars: saved (CMD4 with no flags is saved by this console), 0..1
	{
		const std::string console = read(game + "console_commands.cpp");
		CHECK(count(console, "CMD4(CCC_Integer, \"ai_scripted_throw\",       &g_ai_scripted_throw,       0, 1);") == 1);
		CHECK(count(console, "CMD4(CCC_Integer, \"ai_scripted_throw_debug\", &g_ai_scripted_throw_debug, 0, 1);") == 1);
	}

	// the project builds the new file with the script precompiled header it includes
	{
		const std::string project = read(game + "xrGame.vcxproj");
		CHECK(count(project, "ai_stalker_scripted_throw.cpp") == 1);
		CHECK(has(project, "<ClCompile Include=\"Ai\\Stalker\\ai_stalker_scripted_throw.cpp\">"));
		CHECK(count(project, "<ClInclude Include=\"scripted_throw.h\" />") == 1);
		CHECK(has(glue, "#include \"pch_script.h\""));
	}

	// the game data
	{
		const std::string scripts = root + "/gamedata/scripts/";
		const std::string callbacks = read(scripts + "callbacks_gameobject.script");
		CHECK(count(callbacks, "AddScriptCallback(\"npc_on_scripted_throw\")") == 1);
		CHECK(has(callbacks, "_G.CAI_Stalker__OnScriptedThrow = function(npc, ok, reason)\n"
		                     "\tSendScriptCallback(\"npc_on_scripted_throw\", npc, ok, reason)\nend"));
		// the planner's gate and the body state hook are untouched
		CHECK(count(callbacks, "_G.CAI_Stalker__ShouldThrow = function(npc)") == 1);
		CHECK(count(callbacks, "_G.CAI_Stalker__CombatSetBodyState = function(npc,world_operator,body_state)") == 1);

		const std::string options = read(scripts + "options_modded_exes_stalkers.script");
		CHECK(has(options, "get_item_if_exists(list_bool {\n\t\tid = \"ai_scripted_throw\",\n\t\tdef = \"1\",\n\t}),"));
		CHECK(has(options, "get_item_if_exists(list_bool {\n\t\tid = \"ai_scripted_throw_debug\",\n\t\tdef = \"0\",\n\t}),"));

		const std::string help = read(scripts + "lua_help_ex.script");
		CHECK(has(help, "ai_scripted_throw [0; 1]"));
		CHECK(has(help, "ai_scripted_throw_debug [0; 1]"));
		CHECK(has(help, "bool throw_grenade(vector pos)"));
		CHECK(has(help, "bool throw_grenade(vector pos, bool crouch)"));
		CHECK(has(help, "int throw_grenade_state()"));
		CHECK(has(help, "string throw_grenade_reason()"));
		CHECK(has(help, "bool cancel_throw_grenade()"));
		CHECK(has(help, "int scripted_throw_version()"));
		// every reason the engine can give is in the help text and in the doc
		const std::string doc = read(root + "/docs/npc_throw.md");
		for (int r = 0; r < r_count; ++r)
		{
			const std::string quoted = std::string("\"") + reason_name(r) + "\"";
			if (!has(help, quoted)) { std::printf("FAIL lua_help_ex.script lacks %s\n", quoted.c_str()); ++g_fail; }
			const std::string ticked = std::string("`") + reason_name(r) + "`";
			if (!has(doc, ticked)) { std::printf("FAIL docs/npc_throw.md lacks %s\n", ticked.c_str()); ++g_fail; }
		}
		CHECK(has(doc, "level.scripted_throw_version ~= nil"));

		const std::string labels = read(root + "/gamedata/configs/text/eng/ui_mm_modded_exes_scripted_throw.xml");
		CHECK(has(labels, "ui_mm_modded_exes_gameplay_stalkers_ai_scripted_throw\""));
		CHECK(has(labels, "ui_mm_modded_exes_gameplay_stalkers_ai_scripted_throw_desc\""));
		CHECK(has(labels, "ui_mm_modded_exes_gameplay_stalkers_ai_scripted_throw_debug\""));
		CHECK(has(labels, "ui_mm_modded_exes_gameplay_stalkers_ai_scripted_throw_debug_desc\""));
	}

	// CI builds this branch and runs this test
	{
		const std::string workflow = read(root + "/.github/workflows/eft-parts.yml");
		CHECK(has(workflow, "combat-cover-spacing, npc-throw-grenade]"));
		CHECK(has(workflow, "scripted_throw_test"));
	}
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";
	test_requests();
	test_machine();
	test_wiring(root);
	if (g_fail)
	{
		std::printf("scripted_throw_test: %d FAILED\n", g_fail);
		return 1;
	}
	std::printf("scripted_throw_test: all passed\n");
	return 0;
}
