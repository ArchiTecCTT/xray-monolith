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
		r_count
	};

	inline const char* reason_name(int r)
	{
		static const char* const names[r_count] = {
			"none", "not_stalker", "disabled", "dead", "busy", "wounded", "script_control", "smart_cover",
			"animation", "no_grenade", "bad_target", "too_close", "too_far", "cooldown", "reloading",
			"arc_blocked", "timeout", "cancelled"
		};
		return (r >= 0 && r < r_count) ? names[r] : "none";
	}

	const float k_min_distance = 5.f; // m, closer is refused
	const float k_range_margin = 1.f; // m, kept free below the engine's range for its own miss offset
	const unsigned k_cooldown_ms = 2000; // after any completed throw of this stalker
	const unsigned k_prepare_timeout_ms = 3000; // to turn, get the grenade into the hand and pull the pin
	const unsigned k_total_timeout_ms = 8000; // the whole request, wind-up included; a safety net
	const unsigned k_release_tail_ms = 1500; // after the release: wait for the thrown grenade to leave the slot
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
		bool primed; // ... the pin is out (throw start / ready / throw): the throw cannot be called off cleanly
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

	struct machine
	{
		bool active;
		bool aimed; // the arc was clear when computed
		bool released; // the grenade has left the hand
		int arc_tries;
		unsigned start; // ms
		unsigned release_time; // ms
		int st; // state
		int why; // reason of the last refusal / end

		machine() { reset(); }

		void reset()
		{
			active = false;
			aimed = false;
			released = false;
			arc_tries = 0;
			start = 0;
			release_time = 0;
			st = st_idle;
			why = r_none;
		}

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
			arc_tries = 0;
			start = now;
			release_time = 0;
			st = st_preparing;
			why = r_none;
		}

		void finish(bool ok, reason r)
		{
			active = false;
			st = ok ? st_thrown : st_failed;
			why = ok ? r_none : r;
		}

		step fail(reason r)
		{
			finish(false, r);
			return s_end;
		}

		step tick(const tick_facts& f)
		{
			if (!active)
				return s_end;

			if (!f.alive)
			{
				on_death();
				return s_end;
			}

			if (released)
			{
				// as the planner's action does: it is over when the thrown grenade is gone from the slot
				if (!f.grenade_present || (f.now - release_time >= k_release_tail_ms))
				{
					finish(true, r_none);
					return s_end;
				}
				return s_throw;
			}

			if (!f.grenade_present)
				return fail(r_no_grenade);

			if (!f.primed)
			{
				// nothing is committed yet: these still call it off
				if (f.wounded) return fail(r_wounded);
				if (f.script_control) return fail(r_script_control);
				if (f.smart_cover) return fail(r_smart_cover);
				if (f.animation) return fail(r_animation);
				if (f.now - start >= k_prepare_timeout_ms) return fail(r_timeout);
			}

			if (f.now - start >= k_total_timeout_ms)
				return fail(r_timeout);

			if (aimed)
				return s_throw;

			if (f.in_hand_idle && f.aligned)
				return s_aim;

			return s_hold;
		}

		// the answer to s_aim. in_range = false: the engine refused the distance, not the arc
		step arc(bool clear, bool in_range)
		{
			if (!active)
				return s_end;

			if (clear)
			{
				aimed = true;
				return s_throw;
			}

			if (++arc_tries >= k_arc_tries)
				return fail(in_range ? r_arc_blocked : r_too_far);

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
		void on_death()
		{
			if (active)
				finish(released, r_dead);
		}

		// true = a running request was stopped
		bool cancel()
		{
			if (!active)
				return false;

			finish(false, r_cancelled);
			return true;
		}
	};
}
