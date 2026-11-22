#pragma once
// A grenade throw asked for by a script (docs/npc_throw.md): npc:throw_grenade(pos [, crouch]).
//
// This file is the rules only: when a request is refused, and how an accepted one moves from "preparing" to
// "thrown" or "failed". It includes nothing from the engine, so tests/scripted_throw_test.cpp runs it as it is.
// The engine side (who stands still, who holds the grenade, the arc) is ai/stalker/ai_stalker_scripted_throw.cpp.
//
// A request lives in the stalker object only. It is never saved, and reinit / death / net_destroy drop it.

namespace scripted_throw
{
	// what npc:throw_grenade_state() returns
	enum state : int
	{
		st_failed = -1, // the last request was refused or ended without a throw
		st_idle = 0, // nothing asked yet
		st_preparing = 1, // a request is active: turning, swapping to the grenade, aiming, throwing
		st_thrown = 2, // the last request released the grenade
	};

	// what npc:throw_grenade_reason() returns, as text (reason_name)
	enum reason : int
	{
		r_none = 0,
		r_not_stalker,
		r_disabled, // ai_scripted_throw 0
		r_dead,
		r_busy, // a throw (scripted or the planner's) is already under way
		r_wounded,
		r_script_control, // the stalker is run by a script action (npc:script(true, ...))
		r_smart_cover,
		r_animation, // an animation moves him (animpoint, cutscene) or he is on a mounted weapon
		r_no_grenade,
		r_bad_target,
		r_too_close,
		r_too_far,
		r_cooldown,
		r_reloading,
		r_arc_blocked,
		r_timeout,
		r_cancelled,
		r_prone, // he lies in a prone pose (a script animation): there is no throw from lying
		r_count
	};

	inline const char* reason_name(int r)
	{
		static const char* const names[r_count] = {
			"none", "not_stalker", "disabled", "dead", "busy", "wounded", "script_control", "smart_cover",
			"animation", "no_grenade", "bad_target", "too_close", "too_far", "cooldown", "reloading",
			"arc_blocked", "timeout", "cancelled", "prone"
		};
		return (r >= 0 && r < r_count) ? names[r] : "none";
	}

	const float k_min_distance = 5.f; // m, closer is refused
	const float k_range_margin = 1.f; // m, kept free below the engine's range for its own miss offset
	const unsigned k_cooldown_ms = 2000; // after any completed throw of this stalker
	// The time before the throw is ordered has two limits. The swap (gun away, grenade out) is timed by the two
	// items' own animations, a machine gun's being the longest; turning and the arc tries need the grenade in the
	// hand, so their limit counts from there.
	const unsigned k_swap_timeout_ms = 4500; // from the accept: the grenade is in his hand and at rest
	const unsigned k_aim_timeout_ms = 2000; // from "in his hand": he faces the target and the arc is found clear
	const unsigned k_total_timeout_ms = 8000; // the whole request, wind-up included; the hard cap
	const unsigned k_restore_ms = 4000; // after the end: he is kept on the per-frame update until the gun is back
	const unsigned k_release_tail_ms = 1500; // after the release: wait for the thrown grenade to leave the slot
	const unsigned k_overdue_ms = k_total_timeout_ms + 2000; // nobody ended it by then: see machine::overdue
	const int k_arc_tries = 4; // the engine picks a new miss offset for each try
	const float k_aim_angle = 0.3926991f; // PI/8, the planner action's own "is facing the target" test

	// the engine's throw range by game difficulty (CAI_Stalker::update_throw_params)
	inline float max_distance(int difficulty)
	{
		static const float distances[] = {30.f, 40.f, 50.f, 60.f};
		if (difficulty < 0) difficulty = 0;
		if (difficulty > 3) difficulty = 3;
		return distances[difficulty];
	}

	// what the engine knows when the script asks
	struct request_facts
	{
		bool enabled; // ai_scripted_throw
		bool alive;
		bool active; // a scripted request is already running
		bool wounded;
		bool script_control;
		bool smart_cover;
		bool animation;
		bool prone; // the script animation he plays is one of the lying poses
		bool has_grenade; // a grenade in the grenade slot
		bool grenade_in_hand; // ... and it is already the active item: somebody else is throwing it
		bool target_valid;
		bool reloading;
		float distance; // stalker to target, m
		int difficulty;
		unsigned now; // ms
		unsigned last_throw_time; // ms, 0 = never
	};

	// r_none = accept
	inline reason check_request(const request_facts& f)
	{
		if (!f.enabled) return r_disabled;
		if (!f.alive) return r_dead;
		if (f.active) return r_busy;
		if (f.wounded) return r_wounded;
		if (f.script_control) return r_script_control;
		if (f.smart_cover) return r_smart_cover;
		if (f.animation) return r_animation;
		if (f.prone) return r_prone;
		if (!f.has_grenade) return r_no_grenade;
		if (f.grenade_in_hand) return r_busy;
		if (!f.target_valid) return r_bad_target;
		if (f.distance < k_min_distance) return r_too_close;
		if (f.distance > max_distance(f.difficulty) - k_range_margin) return r_too_far;
		if (f.last_throw_time && (f.now - f.last_throw_time < k_cooldown_ms)) return r_cooldown;
		if (f.reloading) return r_reloading;
		return r_none;
	}

	// what the engine sees on a frame of an active request
	struct tick_facts
	{
		unsigned now; // ms
		bool alive;
		bool wounded;
		bool script_control;
		bool smart_cover;
		bool animation;
		bool grenade_present; // the request's grenade is still in the grenade slot
		bool in_hand_idle; // ... it is the active item and at rest: the arc can be computed from the hand
		bool aligned; // the head points at the target
	};

	// what the engine has to do with the grenade this frame
	enum step
	{
		s_end, // the request is over (see st / why): nothing to hold
		s_hold, // bring the grenade into the hand and wait
		s_aim, // compute the arc now and report it with arc()
		s_throw, // throw
	};

	// where a request is (or was when it ended): the "phase" of the debug line
	enum phase : int
	{
		p_hold = 0, // the grenade is not in his hand yet: the gun goes away, the grenade comes out
		p_aim, // in his hand: turning to the target, trying the arc
		p_throw, // the throw is ordered: wind-up, swing
		p_tail, // the grenade has left his hand: waiting for it to leave the slot
		p_count
	};

	inline const char* phase_name(int p)
	{
		static const char* const names[p_count] = {"hold", "aim", "throw", "tail"};
		return (p >= 0 && p < p_count) ? names[p] : "hold";
	}

	// which time limit ended a request with "timeout": the "limit" of the debug line. The script sees "timeout" only.
	enum limit : int
	{
		l_none = 0,
		l_swap, // k_swap_timeout_ms
		l_aim, // k_aim_timeout_ms
		l_total, // k_total_timeout_ms
		l_overdue, // k_overdue_ms: his frame updates had stopped
		l_count
	};

	inline const char* limit_name(int l)
	{
		static const char* const names[l_count] = {"none", "swap", "aim", "total", "overdue"};
		return (l >= 0 && l < l_count) ? names[l] : "none";
	}

	struct machine
	{
		bool active;
		// The arc was clear when computed and the throw was ordered. From here on nothing calls it off: the engine
		// cannot put a grenade away once its throw has started (CInventory::Update only hides an idle item), so
		// the one way to give him back with a gun in his hands is to let the throw happen.
		bool aimed;
		bool released; // the grenade has left the hand
		bool in_hand; // the grenade has been in his hand and at rest at least once
		bool was_aligned; // he has faced the target at least once
		int arc_tries;
		unsigned start; // ms
		unsigned release_time; // ms
		unsigned in_hand_time; // ms, valid with in_hand
		unsigned aligned_time; // ms, valid with was_aligned
		unsigned end_time; // ms, of the last end
		unsigned ticks; // frames the request was run for
		int st; // state
		int why; // reason of the last refusal / end
		int lim; // limit: which timeout ended it, l_none for any other end

		machine() { reset(); }

		void reset()
		{
			active = false;
			aimed = false;
			released = false;
			in_hand = false;
			was_aligned = false;
			arc_tries = 0;
			start = 0;
			release_time = 0;
			in_hand_time = 0;
			aligned_time = 0;
			end_time = 0;
			ticks = 0;
			st = st_idle;
			why = r_none;
			lim = l_none;
		}

		// the phase a request is in, or was in when it ended
		int where() const
		{
			if (released) return p_tail;
			if (aimed) return p_throw;
			if (in_hand) return p_aim;
			return p_hold;
		}

		// for the debug line, of the last accepted request: ms from the accept, -1 = never
		int ms_total() const { return int(end_time - start); }
		int ms_in_hand() const { return in_hand ? int(in_hand_time - start) : -1; }
		int ms_aligned() const { return was_aligned ? int(aligned_time - start) : -1; }

		// a request that was turned down. One that is running is not disturbed by it.
		void refuse(reason r)
		{
			why = r;
			if (!active)
				st = st_failed;
		}

		void accept(unsigned now)
		{
			active = true;
			aimed = false;
			released = false;
			in_hand = false;
			was_aligned = false;
			arc_tries = 0;
			start = now;
			release_time = 0;
			in_hand_time = 0;
			aligned_time = 0;
			end_time = now;
			ticks = 0;
			st = st_preparing;
			why = r_none;
			lim = l_none;
		}

		void finish(bool ok, reason r, unsigned now, limit l = l_none)
		{
			active = false;
			st = ok ? st_thrown : st_failed;
			why = ok ? r_none : r;
			lim = ok ? l_none : l;
			end_time = now;
		}

		step fail(reason r, unsigned now, limit l = l_none)
		{
			finish(false, r, now, l);
			return s_end;
		}

		step tick(const tick_facts& f)
		{
			if (!active)
				return s_end;

			++ticks;

			if (!f.alive)
			{
				on_death(f.now);
				return s_end;
			}

			if (released)
			{
				// as the planner's action does: it is over when the thrown grenade is gone from the slot
				if (!f.grenade_present || (f.now - release_time >= k_release_tail_ms))
				{
					finish(true, r_none, f.now);
					return s_end;
				}
				return s_throw;
			}

			if (!f.grenade_present)
				return fail(r_no_grenade, f.now);

			if (!aimed)
			{
				// the first time each: the clock of the aim limit, and the two times of the debug line
				if (f.in_hand_idle && !in_hand)
				{
					in_hand = true;
					in_hand_time = f.now;
				}
				if (f.aligned && !was_aligned)
				{
					was_aligned = true;
					aligned_time = f.now;
				}

				// the throw is not ordered yet: these still call it off
				if (f.wounded) return fail(r_wounded, f.now);
				if (f.script_control) return fail(r_script_control, f.now);
				if (f.smart_cover) return fail(r_smart_cover, f.now);
				if (f.animation) return fail(r_animation, f.now);
				if (!in_hand)
				{
					if (f.now - start >= k_swap_timeout_ms) return fail(r_timeout, f.now, l_swap);
				}
				else if (f.now - in_hand_time >= k_aim_timeout_ms)
					return fail(r_timeout, f.now, l_aim);
			}

			if (f.now - start >= k_total_timeout_ms)
				return fail(r_timeout, f.now, l_total);

			if (aimed)
				return s_throw;

			if (f.in_hand_idle && f.aligned)
				return s_aim;

			return s_hold;
		}

		// the answer to s_aim. in_range = false: the engine refused the distance, not the arc
		step arc(bool clear, bool in_range, unsigned now)
		{
			if (!active)
				return s_end;

			if (clear)
			{
				aimed = true;
				return s_throw;
			}

			if (++arc_tries >= k_arc_tries)
				return fail(in_range ? r_arc_blocked : r_too_far, now);

			return s_hold;
		}

		void on_release(unsigned now)
		{
			if (!active || released)
				return;

			released = true;
			release_time = now;
		}

		// a grenade that has already left the hand was thrown, whatever happens to the thrower
		void on_death(unsigned now)
		{
			if (active)
				finish(released, r_dead, now);
		}

		// The request is ended frame by frame (tick). If his frames stop coming while he is still thought about,
		// he would stand there for good: the slower update asks this and ends it.
		bool overdue(unsigned now)
		{
			if (!active || (now - start < k_overdue_ms))
				return false;

			finish(released, r_timeout, now, l_overdue);
			return true;
		}

		// true = a running request was stopped. Too late once the throw is ordered: it goes through.
		bool cancel(unsigned now)
		{
			if (!active || aimed)
				return false;

			finish(false, r_cancelled, now);
			return true;
		}

		// the engine side broke (an exception in his object handler): over, whatever the phase
		void abort(unsigned now)
		{
			if (active)
				finish(released, r_cancelled, now);
		}
	};
}
