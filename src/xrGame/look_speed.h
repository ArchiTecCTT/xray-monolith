#pragma once

// Look speed factor for scripts (docs/look_speed.md).
//
// One number that scales how fast the actor's view turns from the mouse and from the look keys. A mod sets it
// (level.set_look_speed_factor) for things like heavy gear. It is not a console var: it is never saved, and a new
// CLevel (every game start, loaded save and level change) puts it back to 1.
//
// This header has no engine includes on purpose: tests/look_speed_test.cpp compiles it alone.

namespace look_speed
{
	// 1 = the player's own speed. The factor only slows:
	//  - above 1 it would be a second mouse sensitivity the player never chose and cannot see in the options;
	//  - under 0.2 the view hardly answers the mouse, which reads as a frozen game, and 0 or a negative number
	//    (a script's bad arithmetic) would lock or mirror the view.
	const float k_min = 0.2f;
	const float k_max = 1.f;

	// what a script's number becomes: clamped to [k_min, k_max]; not a number -> 1 (no change)
	inline float sanitize(float f)
	{
		if (!(f == f)) return k_max;
		if (f < k_min) return k_min;
		if (f > k_max) return k_max;
		return f;
	}
}

// the factor in use (ActorInput.cpp). Read by CActor's look input, written by set_look_speed_factor only.
extern float g_look_speed_factor;

inline float set_look_speed_factor(float f) { return g_look_speed_factor = look_speed::sanitize(f); }
inline void reset_look_speed_factor() { g_look_speed_factor = look_speed::k_max; }
