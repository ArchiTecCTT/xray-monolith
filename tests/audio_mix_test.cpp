// Standalone regression for the headphones listener-side mix (src/xrSound/SoundRender_Mix.h, docs/audio_api.md);
// not an engine build.
//
// Part 1 runs the header the engine includes: the category fade ramp, the loud-sound limiter step (gain computer and
// attack / release smoother), the compressor (soft-knee curve, makeup, attack / release, switching off), OpenAL's
// distance formula used by the detectors, the per-source auto-gain (curve, per-source smoother, first update, switching
// off) and the bang guard, including frame-rate independence. It also times the auto-gain for many sources.
// Part 2 reads the engine sources and checks the wiring: the mix only scales the OpenAL gain and rolloff, the AI hearing
// path (Event_Propagade, SoundEvent_Register) does not read it, play_no_feedback carries the category, the footstep
// gate and its AI-neutral flags, the compressor's detector chain, the auto-gain and bang guard wiring, the "own"
// category and the Lua exports.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/audio_mix_test.cpp -o /tmp/audio_mix_test
//   /tmp/audio_mix_test .
// The argument is the repository root (default ".").

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// stubs for what SoundRender_Mix.h takes from xrCore / Sound.h
typedef unsigned int u32;
template <class T> T _abs(T v) { return v < 0 ? -v : v; }
struct xrCriticalSection { void Enter() {} void Leave() {} };
enum { smc_world = 0, smc_steps_others = 3, smc_ui = 4, smc_music = 5, smc_own = 6, smc_count = 7 }; // as in Sound.h (checked in part 2)

#include "../src/xrSound/SoundRender_Mix.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static float run_limiter(float fps, float seconds, float level, float thr, float ratio, float att, float rel, float gr0)
{
	float gr = gr0;
	const float dt = 1.f / fps;
	const int n = int(std::lround(seconds * fps));
	for (int i = 0; i < n; ++i)
		sound_mix_limiter_step(gr, level, true, thr, ratio, att, rel, dt);
	return gr;
}

static std::string read_file(const std::string& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::string strip_comments(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
		{
			while (i < s.size() && s[i] != '\n') ++i;
			if (i < s.size()) out += '\n';
			continue;
		}
		out += s[i];
	}
	return out;
}

static std::string body(const std::string& s, const std::string& head)
{
	const size_t a = s.find(head);
	if (a == std::string::npos) return "";
	size_t b = s.find('{', a), depth = 0;
	for (size_t i = b; i < s.size(); ++i)
	{
		if (s[i] == '{') ++depth;
		else if (s[i] == '}' && --depth == 0) return s.substr(a, i + 1 - a);
	}
	return "";
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---- part 1: ramp
	{
		CSoundRender_Mix::ramp r;
		CHECK(r.cur == 1.f && r.target == 1.f);
		r.set(0.5f, 1.f);
		r.step(0.5f);
		CHECK(near(r.cur, 0.75f, 1e-6f));
		r.step(0.75f);
		CHECK(r.cur == 0.5f); // stops on the target
		r.set(3.f, 0.f); // no fade: jumps
		CHECK(r.cur == 3.f);
		r.set(1.f, 2.f); // 3 -> 1 over 2 s
		for (int i = 0; i < 60; ++i) r.step(1.f / 60.f);
		CHECK(near(r.cur, 2.f, 1e-4f));
		r.set(0.f, 0.5f); // retarget mid-fade: from where it is, over the new time
		for (int i = 0; i < 30; ++i) r.step(1.f / 60.f);
		CHECK(r.cur == 0.f);
	}

	// ---- part 1: limiter
	{
		float gr = 0.f;
		// off: never reduces
		for (int i = 0; i < 100; ++i) sound_mix_limiter_step(gr, 1.f, false, -12.f, 20.f, .005f, .4f, 1.f / 60.f);
		CHECK(gr == 0.f);
		// below threshold: nothing
		CHECK(run_limiter(60.f, 1.f, 0.1f /* -20 dB */, -12.f, 20.f, .005f, .4f, 0.f) == 0.f);
		// full-scale shot, threshold -12, ratio 20: -(12)(1 - 1/20) = -11.4 dB after the attack
		const float full = run_limiter(60.f, 0.1f, 1.f, -12.f, 20.f, .005f, .4f, 0.f);
		CHECK(near(full, -11.4f, 0.01f));
		// ratio 2: half the overshoot
		CHECK(near(run_limiter(60.f, 1.f, 1.f, -12.f, 2.f, .005f, .4f, 0.f), -6.f, 0.01f));
		// release: one time constant after the sound is gone, e^-1 of the reduction is left
		const float rel = run_limiter(1000.f, 0.4f, 0.f, -12.f, 20.f, .005f, .4f, -11.4f);
		CHECK(near(rel, -11.4f * std::exp(-1.f), 0.02f));
		// and it gets back to exactly 0
		CHECK(run_limiter(60.f, 10.f, 0.f, -12.f, 20.f, .005f, .4f, -11.4f) == 0.f);
		// frame-rate independence of the smoother (30 vs 150 fps, attack 50 ms, at 0.1 s)
		const float a30 = run_limiter(30.f, 0.1f, 1.f, -12.f, 20.f, .05f, .4f, 0.f);
		const float a144 = run_limiter(150.f, 0.1f, 1.f, -12.f, 20.f, .05f, .4f, 0.f);
		CHECK(near(a30, a144, 0.001f));
		std::printf("limiter: full-scale %.2f dB, release after 1 tau %.2f dB, 30 vs 150 fps %.3f / %.3f dB\n", full, rel,
		            a30, a144);
		// zero attack: instant
		gr = 0.f;
		sound_mix_limiter_step(gr, 1.f, true, -12.f, 20.f, 0.f, .4f, 1.f / 60.f);
		CHECK(near(gr, -11.4f, 1e-4f));
	}

	// ---- part 1: compressor curve (gain reduction in dB for a detector level)
	{
		const float T = -24.f, R = 4.f;
		// hard knee: nothing below the threshold, (1/R - 1) dB per dB above it
		CHECK(sound_mix_compressor_curve(-40.f, T, R, 0.f) == 0.f);
		CHECK(sound_mix_compressor_curve(T, T, R, 0.f) == 0.f);
		CHECK(near(sound_mix_compressor_curve(0.f, T, R, 0.f), -18.f, 1e-4f)); // 24 dB over -> 6 dB over
		CHECK(near(sound_mix_compressor_curve(-12.f, T, R, 0.f), -9.f, 1e-4f));
		// ratio 1 (or less) never reduces
		CHECK(sound_mix_compressor_curve(0.f, T, 1.f, 6.f) == 0.f);
		// soft knee of 12 dB: untouched below T - 6, the straight line above T + 6, joined without a step or a corner
		const float W = 12.f;
		CHECK(sound_mix_compressor_curve(T - 6.f, T, R, W) == 0.f);
		CHECK(sound_mix_compressor_curve(T - 20.f, T, R, W) == 0.f);
		CHECK(near(sound_mix_compressor_curve(T + 6.f, T, R, W), -4.5f, 1e-4f)); // = (1/4 - 1) * 6
		CHECK(near(sound_mix_compressor_curve(T + 5.999f, T, R, W), -4.5f, 2e-3f));
		CHECK(near(sound_mix_compressor_curve(T - 5.999f, T, R, W), 0.f, 1e-3f));
		// at the threshold the knee already takes (1/R - 1) * W / 8
		CHECK(near(sound_mix_compressor_curve(T, T, R, W), -0.75f * W / 8.f, 1e-4f));
		// monotonic: a louder input never comes out quieter, and never gets more gain
		float prev_out = -1000.f, prev_gr = 1.f;
		bool mono = true;
		for (float l = -80.f; l <= 12.f; l += 0.25f)
		{
			const float gr = sound_mix_compressor_curve(l, T, R, W);
			mono = mono && gr <= 0.f && gr <= prev_gr + 1e-5f && l + gr >= prev_out - 1e-4f;
			prev_out = l + gr;
			prev_gr = gr;
		}
		CHECK(mono);
		// the loudest boosted source (+12 dB after a x4 category gain) comes out at T + 36 / R + makeup
		CHECK(near(12.f + sound_mix_compressor_curve(12.f, T, R, 0.f) + 15.f, 0.f, 1e-4f));
	}

	// ---- part 1: compressor step (makeup + reduction, attack / release, switching off)
	{
		auto run = [](float fps, float seconds, float level, bool on, float makeup, float att, float rel, float g0) {
			float g = g0;
			const int n = int(std::lround(seconds * fps));
			for (int i = 0; i < n; ++i)
				sound_mix_compressor_step(g, level, on, -24.f, 4.f, 0.f, makeup, att, rel, 1.f / fps);
			return g;
		};
		// off from rest: exactly 0 dB, whatever plays
		CHECK(run(60.f, 2.f, 1.f, false, 12.f, .01f, .25f, 0.f) == 0.f);
		// quiet scene (nothing counted, or only sounds under the threshold): the gain settles on the makeup
		CHECK(run(60.f, 5.f, 0.f, true, 12.f, .01f, .25f, 0.f) == 12.f);
		CHECK(run(60.f, 5.f, 0.03f /* -30.5 dB */, true, 12.f, .01f, .25f, 0.f) == 12.f);
		// a full-scale shot: makeup 12 - 18 = -6 dB, reached with the attack time
		CHECK(near(run(60.f, 0.5f, 1.f, true, 12.f, .01f, .25f, 12.f), -6.f, 0.01f));
		// the level it returns is the detector level in dB
		float g = 0.f;
		CHECK(near(sound_mix_compressor_step(g, 0.5f, true, -24.f, 4.f, 0.f, 0.f, 0.f, 0.f, 1.f / 60.f), -6.0206f, 1e-3f));
		CHECK(sound_mix_compressor_step(g, 0.f, true, -24.f, 4.f, 0.f, 0.f, 0.f, 0.f, 1.f / 60.f) == -120.f);
		// release: one time constant after the shot, e^-1 of the way back is left
		const float rel = run(1000.f, 0.25f, 0.f, true, 12.f, .01f, .25f, -6.f);
		CHECK(near(rel, 12.f - 18.f * std::exp(-1.f), 0.03f));
		// frame-rate independence (30 vs 150 fps, attack 50 ms, at 0.1 s)
		const float c30 = run(30.f, 0.1f, 1.f, true, 12.f, .05f, .25f, 12.f);
		const float c150 = run(150.f, 0.1f, 1.f, true, 12.f, .05f, .25f, 12.f);
		CHECK(near(c30, c150, 0.002f));
		// zero attack / release: instant
		g = 12.f;
		sound_mix_compressor_step(g, 1.f, true, -24.f, 4.f, 0.f, 12.f, 0.f, .25f, 1.f / 60.f);
		CHECK(near(g, -6.f, 1e-4f));
		// switched off while boosted or while reduced: back to exactly 0 dB with the release time, no jump
		g = 12.f;
		sound_mix_compressor_step(g, 0.f, false, -24.f, 4.f, 0.f, 12.f, .01f, .25f, 1.f / 60.f);
		CHECK(g < 12.f && g > 10.f); // one frame of a 250 ms release, not the 10 ms attack
		CHECK(run(60.f, 10.f, 0.f, false, 12.f, .01f, .25f, 12.f) == 0.f);
		CHECK(run(60.f, 10.f, 1.f, false, 12.f, .01f, .25f, -6.f) == 0.f);
		// dt 0 (paused): holds
		g = 5.f;
		sound_mix_compressor_step(g, 1.f, true, -24.f, 4.f, 0.f, 12.f, .01f, .25f, 0.f);
		CHECK(g == 5.f);
		std::printf("compressor: release after 1 tau %.2f dB, 30 vs 150 fps %.3f / %.3f dB\n", rel, c30, c150);
	}

	// ---- part 1: OpenAL's inverse-distance-clamped gain (the compressor's detector and the measurement table)
	{
		// inside the reference distance: 1; a footstep (ref 1.5, max 40) at 15 m with the engine's rolloff 0.75
		CHECK(sound_mix_al_distance_gain(0.5f, 1.5f, 40.f, 0.75f) == 1.f);
		CHECK(near(sound_mix_al_distance_gain(15.f, 1.5f, 40.f, 0.75f), 1.5f / (1.5f + 0.75f * 13.5f), 1e-6f));
		CHECK(near(20.f * std::log10(sound_mix_al_distance_gain(15.f, 1.5f, 40.f, 0.75f)), -17.8f, 0.05f));
		// a gunshot (ref 5, max 300) at 15 m: -8 dB
		CHECK(near(20.f * std::log10(sound_mix_al_distance_gain(15.f, 5.f, 300.f, 0.75f)), -7.96f, 0.05f));
		// beyond max the distance is clamped; rolloff 0 = no attenuation; a bad range = none
		CHECK(sound_mix_al_distance_gain(500.f, 5.f, 300.f, 0.75f) == sound_mix_al_distance_gain(300.f, 5.f, 300.f, 0.75f));
		CHECK(sound_mix_al_distance_gain(30.f, 1.5f, 40.f, 0.f) == 1.f);
		CHECK(sound_mix_al_distance_gain(30.f, 40.f, 1.5f, 0.75f) == 1.f);
		CHECK(sound_mix_al_distance_gain(30.f, 0.f, 40.f, 0.75f) == 1.f);
		// a rolloff factor of 0.25 on footsteps: +9.2 dB at 15 m, nothing at the reference distance
		const float k1 = sound_mix_al_distance_gain(15.f, 1.5f, 40.f, 0.75f);
		const float k025 = sound_mix_al_distance_gain(15.f, 1.5f, 40.f, 0.75f * 0.25f);
		CHECK(near(20.f * std::log10(k025 / k1), 9.2f, 0.1f));
		CHECK(sound_mix_al_distance_gain(1.5f, 1.5f, 40.f, 0.75f * 0.25f) == 1.f);
	}

	// ---- part 1: defaults of a fresh mix (everything off, every factor 1)
	{
		CSoundRender_Mix m;
		CHECK(!m.comp_on && m.comp_gain == 1.f && m.comp_gain_db == 0.f && m.comp_cats_cur == 0);
		CHECK(m.comp_cats == ((1u << smc_world) | (1u << smc_steps_others)));
		CHECK(m.comp_mask == 0x07FFFF00u && m.comp_own_mask == 0x00200100u && m.comp_own_k == 1.f);
		for (int i = 0; i < smc_count; ++i)
			CHECK(m.rolloff_k[i] == 1.f && m.cat[i].cur == 1.f);
		CHECK(!m.lim_on && m.lim_gain == 1.f);
		CHECK(!m.ag_on && !m.ag_on_cur && !m.bang_on && m.bang_gain == 1.f && m.bang_gr_db == 0.f && !m.own_split);
		CHECK(m.listener_obj == nullptr && m.bang_mask == 0x00200100u);
		// auto-gain acts on world, others' steps and the player's own sounds unless a script says otherwise
		for (int i = 0; i < smc_count; ++i)
			CHECK(m.ag_amount[i] == ((i == smc_world || i == smc_steps_others || i == smc_own) ? 1.f : 0.f));
	}

	// ---- part 1: auto-gain curve (one source's gain in dB for its own level)
	{
		const float T = -14.f, R = 3.f, B = 12.f, C = 12.f;
		// at the target nothing changes; quieter sounds come up, louder ones go down, by 2/3 of the distance
		CHECK(sound_mix_autogain_curve(T, T, R, B, C) == 0.f);
		CHECK(near(sound_mix_autogain_curve(-29.f, T, R, B, C), 10.f, 1e-4f));
		CHECK(near(sound_mix_autogain_curve(-8.f, T, R, B, C), -4.f, 1e-4f));
		CHECK(near(sound_mix_autogain_curve(0.f, T, R, B, C), -9.3333f, 1e-3f));
		// the makeup ceiling and the deepest cut
		CHECK(sound_mix_autogain_curve(-60.f, T, R, B, C) == B);
		CHECK(sound_mix_autogain_curve(-120.f, T, R, B, C) == B);
		CHECK(sound_mix_autogain_curve(0.f, T, R, B, 5.f) == -5.f);
		// a boost ceiling of 0 (the player's own sounds): never lifted, still turned down
		CHECK(sound_mix_autogain_curve(-40.f, T, R, 0.f, C) == 0.f);
		CHECK(near(sound_mix_autogain_curve(0.f, T, R, 0.f, C), -9.3333f, 1e-3f));
		// ratio 1 (or less): no auto-gain
		CHECK(sound_mix_autogain_curve(-40.f, T, 1.f, B, C) == 0.f);
		// a louder source never comes out quieter than a quieter one (distance order is kept)
		float prev = -1000.f;
		bool mono = true;
		for (float l = -100.f; l <= 12.f; l += 0.25f)
		{
			const float out = l + sound_mix_autogain_curve(l, T, R, B, C);
			mono = mono && out >= prev - 1e-4f;
			prev = out;
		}
		CHECK(mono);
		// a 30 dB spread (walking step at 15 m .. own shot) comes out within 10 dB
		const float lo = -29.8f + sound_mix_autogain_curve(-29.8f, T, R, B, C);
		const float hi = 0.f + sound_mix_autogain_curve(0.f, T, R, B, C);
		CHECK(near(hi - lo, 29.8f / 3.f, 0.01f));
		// dB to gain
		CHECK(sound_mix_db_to_gain(0.f) == 1.f);
		CHECK(near(sound_mix_db_to_gain(-6.f), std::pow(10.f, -6.f / 20.f), 1e-5f));
		CHECK(near(sound_mix_db_to_gain(12.f), std::pow(10.f, 12.f / 20.f), 1e-4f));
	}

	// ---- part 1: one source's auto-gain over time
	{
		const float T = -14.f, R = 3.f, B = 12.f, C = 12.f;
		auto run = [&](float fps, float seconds, float level, bool on, float amount, float att, float rel, float g0,
		               bool set0) {
			float g = g0;
			bool set = set0;
			const float dt = 1.f / fps;
			const float ka = sound_mix_smooth_k(dt, att), kr = sound_mix_smooth_k(dt, rel);
			const int n = int(std::lround(seconds * fps));
			for (int i = 0; i < n; ++i)
				sound_mix_autogain_source(g, set, level, on, amount, T, R, B, C, ka, kr);
			return g;
		};
		const float quiet = std::pow(10.f, -29.f / 20.f), loud = 1.f;
		// off: a source never gets a gain, and its first update marks it as seen
		{
			float g = 0.f;
			bool set = false;
			CHECK(sound_mix_autogain_source(g, set, loud, false, 1.f, T, R, B, C, 1.f, 1.f) == 0.f && set);
		}
		// the first update of a source takes the gain at once: a loud onset is not played at full gain first
		{
			float g = 0.f;
			bool set = false;
			const float ka = sound_mix_smooth_k(1.f / 60.f, .005f), kr = sound_mix_smooth_k(1.f / 60.f, .08f);
			CHECK(near(sound_mix_autogain_source(g, set, loud, true, 1.f, T, R, B, C, ka, kr), -9.3333f, 1e-3f));
			g = 0.f;
			set = false;
			CHECK(near(sound_mix_autogain_source(g, set, quiet, true, 1.f, T, R, B, C, ka, kr), 10.f, 1e-3f));
		}
		// a category amount scales the gain; amount 0 = untouched
		CHECK(near(run(60.f, 1.f, quiet, true, 0.5f, .005f, .08f, 0.f, false), 5.f, 1e-3f));
		CHECK(run(60.f, 1.f, quiet, true, 0.f, .005f, .08f, 0.f, false) == 0.f);
		// a source that was already playing when the auto-gain is switched on ramps to its gain: release when it has to
		// rise, attack when it has to fall, and the attack is the faster one
		const float up = run(60.f, 0.08f, quiet, true, 1.f, .005f, .08f, 0.f, true);
		CHECK(up > 5.5f && up < 7.5f); // about 1 - e^-1 of the way to +10 after one release time
		const float down = run(60.f, 0.05f, loud, true, 1.f, .005f, .08f, 0.f, true);
		CHECK(near(down, -9.3333f, 0.01f)); // there within 50 ms
		// it lands on exactly the target, and exactly 0 after switching off (so the gain factor is exactly 1 again)
		const float quiet_gain = sound_mix_autogain_curve(20.f * std::log10(quiet), T, R, B, C);
		CHECK(near(quiet_gain, 10.f, 1e-3f));
		CHECK(run(60.f, 3.f, quiet, true, 1.f, .005f, .08f, 0.f, true) == quiet_gain);
		CHECK(run(60.f, 3.f, quiet, false, 1.f, .005f, .08f, 10.f, true) == 0.f);
		CHECK(run(60.f, 3.f, loud, false, 1.f, .005f, .08f, -9.3333f, true) == 0.f);
		// nothing "goes quiet for a bit": 250 ms after a source dropped from full level to a quiet one (a door closing
		// between) its gain has come all the way from the deepest cut to within 1 dB of its lift
		const float back = run(60.f, 0.25f, quiet, true, 1.f, .005f, .08f, -9.3333f, true);
		CHECK(back > 9.f);
		// frame-rate independence (30 vs 150 fps, release 80 ms, at 0.1 s)
		const float r30 = run(30.f, 0.1f, quiet, true, 1.f, .005f, .08f, 0.f, true);
		const float r150 = run(150.f, 0.1f, quiet, true, 1.f, .005f, .08f, 0.f, true);
		CHECK(near(r30, r150, 0.002f));
		// paused (dt 0): holds; zero time constants: jumps
		CHECK(sound_mix_smooth_k(0.f, .08f) == 0.f && sound_mix_smooth_k(.016f, 0.f) == 1.f);
		std::printf("auto-gain: +10 dB lift after one release time %.2f dB, 30 vs 150 fps %.3f / %.3f dB\n", up, r30, r150);
	}

	// ---- part 1: bang guard (a short duck at the start of a loud bang)
	{
		float gr = 0.f;
		// off, or under the threshold: nothing
		sound_mix_bang_step(gr, 1.f, false, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(gr == 0.f);
		sound_mix_bang_step(gr, 0.3f /* -10.5 dB */, true, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(gr == 0.f);
		// a full-scale bang: the duck is there in the same update, limited to max_duck
		CHECK(near(sound_mix_bang_step(gr, 1.f, true, -9.f, 6.f, .025f, 1.f / 60.f), 0.f, 1e-4f));
		CHECK(gr == -6.f);
		// a bang 4 dB over the threshold ducks 4 dB
		float gr2 = 0.f;
		sound_mix_bang_step(gr2, std::pow(10.f, -5.f / 20.f), true, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(near(gr2, -4.f, 1e-3f));
		// a deeper duck replaces a shallower one at once; a shallower one does not lift a deeper one at once
		sound_mix_bang_step(gr2, 1.f, true, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(gr2 == -6.f);
		sound_mix_bang_step(gr2, std::pow(10.f, -5.f / 20.f), true, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(gr2 < -4.f && gr2 > -6.f);
		// once the onset is over (the detector sees no bang) it recovers within tens of milliseconds: under 1 dB left
		// after 50 ms, under 0.1 dB after 100 ms, exactly 0 soon after, at 60 and at 30 fps
		for (float fps : {60.f, 30.f})
		{
			float g = -6.f;
			int frames = 0;
			float at50 = 0.f, at100 = 0.f;
			while (g != 0.f && frames < 1000)
			{
				sound_mix_bang_step(g, 0.f, true, -9.f, 6.f, .025f, 1.f / fps);
				++frames;
				if (frames == int(std::ceil(0.05f * fps))) at50 = g;
				if (frames == int(std::ceil(0.10f * fps))) at100 = g;
			}
			CHECK(at50 > -1.f && at100 > -0.15f);
			CHECK(g == 0.f && frames / fps < 0.3f);
		}
		// switched off mid-duck: releases, no jump
		float g3 = -6.f;
		sound_mix_bang_step(g3, 1.f, false, -9.f, 6.f, .025f, 1.f / 60.f);
		CHECK(g3 < 0.f && g3 > -6.f);
	}

	// ---- part 1: cost of the per-source auto-gain (the engine calls sound_mix_autogain_source and sound_mix_db_to_gain
	// once per rendering source per sound update; snd_targets allows 32 .. 1024 sources). Printed, not asserted: the
	// number depends on the machine and the build (this test is built without optimisation and with sanitizers).
	{
		for (int sources : {64, 256, 1024})
		{
			std::vector<float> g(sources, 0.f), lvl(sources);
			std::vector<char> set(sources, 0);
			for (int i = 0; i < sources; ++i)
				lvl[i] = std::pow(10.f, -(float(i % 60)) / 20.f);
			const int frames = 400;
			const float ka = sound_mix_smooth_k(1.f / 60.f, .005f), kr = sound_mix_smooth_k(1.f / 60.f, .08f);
			volatile float sink = 0.f;
			const auto t0 = std::chrono::steady_clock::now();
			for (int f = 0; f < frames; ++f)
			{
				float acc = 0.f;
				for (int i = 0; i < sources; ++i)
				{
					bool s = set[i] != 0;
					// a moving source: its level changes every update, so the smoother and both transcendental calls run
					const float l = lvl[i] * (1.f + 0.2f * float((f + i) & 7));
					acc += sound_mix_db_to_gain(sound_mix_autogain_source(g[i], s, l, true, 1.f, -14.f, 3.f, 12.f, 12.f, ka, kr));
					set[i] = s;
				}
				sink = sink + acc;
			}
			const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
			std::printf("auto-gain cost: %4d sources: %.2f us per update, %.1f ns per source\n", sources, us / frames,
			            us * 1000. / frames / sources);
		}
	}
	std::printf("part 1 (math): %s\n", g_fail ? "FAIL" : "ok");

	// ---- part 2: sources
	const int before = g_fail;
	const std::string snd = root + "/src/xrSound/";
	const std::string target = read_file(snd + "SoundRender_TargetA.cpp");
	const std::string emitter = read_file(snd + "SoundRender_Emitter.cpp");
	const std::string core = read_file(snd + "SoundRender_Core.cpp");
	const std::string level = read_file(root + "/src/xrEngine/IGame_Level.cpp");
	const std::string step = read_file(root + "/src/xrGame/step_manager.cpp");
	const std::string fsm = read_file(snd + "SoundRender_Emitter_FSM.cpp");
	const std::string sound_h = read_file(snd + "Sound.h");
	const std::string lua = read_file(root + "/src/xrGame/script_sound_script.cpp");
	CHECK(!target.empty() && !emitter.empty() && !core.empty() && !level.empty() && !step.empty());
	CHECK(!fsm.empty() && !sound_h.empty() && !lua.empty());

	// the mix is applied to the OpenAL gain after the engine's own clamp to 1, then clamped to SOUND_MIX_MAX_GAIN
	const std::string fill = body(target, "void CSoundRender_TargetA::fill_parameters()");
	const size_t c1 = fill.find("clamp(_gain, EPS_S, 1.f);");
	const size_t c2 = fill.find("_gain *= SoundRender->mix_gain(m_pEmitter);");
	const size_t c3 = fill.find("clamp(_gain, 0.f, SOUND_MIX_MAX_GAIN);");
	const size_t c4 = fill.find("alSourcef (pSource, AL_GAIN, _gain)");
	CHECK(c1 != std::string::npos && c2 != std::string::npos && c3 != std::string::npos && c4 != std::string::npos);
	CHECK(c1 < c2 && c2 < c3 && c3 < c4);
	CHECK(target.find("AL_MAX_GAIN, SOUND_MIX_MAX_GAIN") != std::string::npos);

	// the AI path never reads the mix. The one thing it reads is the listener-only flag of a play: a footstep that
	// exists only because a headset stretches the player's step range makes no AI event, as it made none before
	const std::string prop = strip_comments(body(emitter, "void CSoundRender_Emitter::Event_Propagade()"));
	CHECK(!prop.empty() && prop.find("max_ai_distance") != std::string::npos);
	const size_t lo_at = prop.find("if (owner_data->mix_flags & smf_listener_only) return;");
	CHECK(lo_at != std::string::npos);
	{
		std::string rest = prop;
		if (lo_at != std::string::npos)
			rest.erase(lo_at, std::string("if (owner_data->mix_flags & smf_listener_only) return;").size());
		CHECK(rest.find("mix") == std::string::npos); // nothing else of the mix, in particular no gain, range or category
	}
	const std::string reg = body(level, "void IGame_Level::SoundEvent_Register(");
	CHECK(!reg.empty() && reg.find("mix") == std::string::npos);
	// and the mix never writes what the AI reads
	const size_t mix_at = core.find("// Listener-side mix (headphones)");
	CHECK(mix_at != std::string::npos);
	const std::string mixcode = strip_comments(core.substr(mix_at));
	CHECK(mixcode.find("p_source.volume") == std::string::npos && mixcode.find("max_ai_distance") == std::string::npos &&
	      mixcode.find("set_volume") == std::string::npos);

	// ---- compressor and distance curve
	// the stub enum above matches Sound.h (world is the first category, steps_others the fourth, six in all)
	const std::string cats = body(sound_h, "enum esound_mix_category");
	const size_t e0 = cats.find("smc_world = 0,"), e1 = cats.find("smc_ambient,"), e2 = cats.find("smc_weather,");
	const size_t e3 = cats.find("smc_steps_others,"), e4 = cats.find("smc_ui,"), e5 = cats.find("smc_music,");
	const size_t e6 = cats.find("smc_own,"), e7 = cats.find("smc_count,");
	CHECK(e0 != std::string::npos && e7 != std::string::npos);
	CHECK(e0 < e1 && e1 < e2 && e2 < e3 && e3 < e4 && e4 < e5 && e5 < e6 && e6 < e7);
	// the compressor gain reaches a source only through mix_gain, for the categories of the compressed set
	const std::string mg = body(core, "float CSoundRender_Core::mix_gain(CSoundRender_Emitter* E)");
	CHECK(mg.find("if (mix.comp_cats_cur & (1u << c))") != std::string::npos);
	CHECK(mg.find("g *= mix.comp_gain;") != std::string::npos);
	// menus and music can never be put into the compressed set
	const std::string scc = body(core, "bool CSoundRender_Core::mix_set_compressor_category(u32 cat, bool enabled)");
	CHECK(scc.find("cat == smc_ui || cat == smc_music") != std::string::npos);
	// the detector: heard level x category gain x limiter (the chain category -> limiter -> compressor), 2D weight
	const std::string mu = body(core, "void CSoundRender_Core::mix_update(float dt)");
	CHECK(mu.find("float l = E->mix_heard * mix.cat[c].cur * mix.lim_gain;") != std::string::npos);
	CHECK(mu.find("l *= c_own_k;") != std::string::npos);
	// 2D sounds (the actor's own) are picked by their own type mask, shots and explosions by default
	CHECK(mu.find("!(u32(d->g_type) & (E->b2D ? c_own_mask : c_mask))") != std::string::npos);
	const size_t lim_at = mu.find("mix.lim_gain = "), comp_at = mu.find("float l = E->mix_heard");
	CHECK(lim_at != std::string::npos && comp_at != std::string::npos && lim_at < comp_at);
	CHECK(mu.find("if (c_on || mix.comp_gain_db == 0.f)") != std::string::npos); // categories held while releasing
	// the heard level is the engine level (at most 1) x OpenAL's distance gain with the category's rolloff
	const std::string cull = body(fsm, "BOOL CSoundRender_Emitter::update_culling(float dt)");
	CHECK(cull.find("mix_heard = _min(mix_level, 1.f);") != std::string::npos);
	CHECK(cull.find("psSoundRolloff * SoundRender->mix_rolloff_k(this)") != std::string::npos);
	// and computing it never writes the source parameters (the AI reads p_source.volume and max_ai_distance)
	const size_t heard_at = cull.find("mix_heard = _min(mix_level, 1.f);");
	const std::string heard = heard_at == std::string::npos ? "" : cull.substr(heard_at, cull.find("if (smooth_volume < psSoundCull)") - heard_at);
	CHECK(!heard.empty() && heard.find("p_source.volume") == std::string::npos &&
	      heard.find("max_ai_distance") == std::string::npos && heard.find("p_source.min_distance =") == std::string::npos &&
	      heard.find("p_source.max_distance =") == std::string::npos);
	// OpenAL gets the same rolloff the detector used; with no script call the factor is exactly 1
	CHECK(fill.find("AL_ROLLOFF_FACTOR, psSoundRolloff * SoundRender->mix_rolloff_k(m_pEmitter)") != std::string::npos);
	CHECK(target.find("AL_ROLLOFF_FACTOR, psSoundRolloff)") == std::string::npos);
	// audio_reset switches the compressor off and puts every rolloff back
	const std::string rst = body(core, "void CSoundRender_Core::mix_reset(float fade_ms)");
	CHECK(rst.find("mix.comp_on = false;") != std::string::npos && rst.find("mix.rolloff_k[i] = 1.f;") != std::string::npos);
	CHECK(rst.find("mix.comp_cats = (1u << smc_world) | (1u << smc_steps_others);") != std::string::npos);
	// Lua: the feature bits and every export
	CHECK(lua.find("static int audio_api() { return 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024; }") !=
	      std::string::npos);
	CHECK(lua.find("\"steps_others\", \"ui\", \"music\",") != std::string::npos && lua.find("\"own\"};") != std::string::npos);
	for (const char* fn : {"audio_set_compressor", "audio_set_compressor_detect", "audio_set_compressor_category",
	                       "audio_get_compressor_category", "audio_get_compressor_enabled", "audio_get_compressor_gain",
	                       "audio_get_compressor_level", "audio_set_category_rolloff", "audio_get_category_rolloff",
	                       "audio_set_autogain", "audio_set_autogain_category", "audio_get_autogain_category",
	                       "audio_get_autogain_enabled", "audio_get_autogain_min", "audio_get_autogain_max",
	                       "audio_set_bang_guard", "audio_set_bang_guard_detect", "audio_get_bang_guard_enabled",
	                       "audio_get_bang_guard_reduction", "audio_set_own_split", "audio_get_own_split"})
	{
		const std::string d = std::string("def(\"") + fn + "\", &" + fn + ")";
		if (lua.find(d) == std::string::npos)
		{
			std::printf("FAIL: no Lua export %s\n", fn);
			++g_fail;
		}
	}

	// ---- auto-gain, bang guard, the own category
	// one call per source per update: the level is the source's own (no fade) x its category gain; the player's own
	// sounds get a boost ceiling of 0; the result multiplies the gain only when it is not 0 dB
	CHECK(mg.find("sound_mix_autogain_source(E->mix_ag_db, E->mix_ag_set, E->mix_src_level * mix.cat[c].cur, mix.ag_on_cur,") !=
	      std::string::npos);
	CHECK(mg.find("c == smc_own ? 0.f : mix.ag_boost_cur") != std::string::npos);
	CHECK(mg.find("if (mix.ag_on_cur || E->mix_ag_db != 0.f || !E->mix_ag_set)") != std::string::npos);
	const size_t ag1 = mg.find("if (E->mix_ag_db != 0.f)"), ag2 = mg.find("g *= sound_mix_db_to_gain(E->mix_ag_db);");
	CHECK(ag1 != std::string::npos && ag2 != std::string::npos && ag1 < ag2);
	// the bang guard ducks what the limiter ducks (everything but menus and music), and a muted step is silent
	CHECK(mg.find("g *= mix.lim_gain * mix.bang_gain;") != std::string::npos);
	const size_t mute_at = mg.find("if (d && (d->mix_flags & smf_mute))");
	CHECK(mute_at != std::string::npos && mg.find("g = 0.f;", mute_at) != std::string::npos);
	// menus and music can never get auto-gain
	const std::string sac = body(core, "bool CSoundRender_Core::mix_set_autogain_category(u32 cat, float amount)");
	CHECK(sac.find("cat == smc_ui || cat == smc_music") != std::string::npos);
	// the source's own level leaves out the fade in / out and gets the same OpenAL distance gain as the heard level
	CHECK(cull.find("mix_src_level = _min(p_source.base_volume * volume_att * occluder_volume, 1.f);") != std::string::npos);
	CHECK(cull.find("mix_src_level *= al_att;") != std::string::npos && cull.find("mix_heard *= al_att;") != std::string::npos);
	// the bang guard's detector counts only the onset of a one-shot bang, with its category gain
	CHECK(mu.find("const float age = E->mix_onset_age(fTimer_Value);") != std::string::npos);
	CHECK(mu.find("if (age < 0.f || age > b_hold)") != std::string::npos);
	CHECK(mu.find("const float l = E->mix_heard * mix.cat[c].cur;") != std::string::npos);
	const std::string emitter_h = read_file(snd + "SoundRender_Emitter.h");
	CHECK(emitter_h.find("return (m_current_state == stPlaying || m_current_state == stSimulating) ? now - fTimeStarted : -1.f;") !=
	      std::string::npos);
	// every new source starts without an auto-gain and unseen
	const std::string startstop = read_file(snd + "SoundRender_Emitter_StartStop.cpp");
	CHECK(startstop.find("mix_ag_db = 0.f;") != std::string::npos && startstop.find("mix_ag_set = false;") != std::string::npos);
	CHECK(emitter.find("mix_ag_db = 0.f;") != std::string::npos && emitter.find("mix_ag_set = false;") != std::string::npos);
	// the own category: only once a script asked for it, only for untagged sounds with a game object
	const std::string mc = body(core, "u32 CSoundRender_Core::mix_category(CSoundRender_Emitter* E) const");
	const size_t tag_at = mc.find("if (d->mix_cat < smc_count)"), ui_at = mc.find("return smc_ui;");
	const size_t own_at = mc.find("if (mix.own_split && d->g_object && (E->b2D || d->g_object == mix.listener_obj))");
	CHECK(tag_at != std::string::npos && ui_at != std::string::npos && own_at != std::string::npos);
	CHECK(tag_at < ui_at && ui_at < own_at && mc.find("return smc_own;") > own_at);
	const std::string xray = read_file(root + "/src/xrEngine/x_ray.cpp");
	CHECK(xray.find("::Sound->mix_set_listener_object(g_pGameLevel ? g_pGameLevel->CurrentViewEntity() : NULL);") !=
	      std::string::npos);
	// audio_reset switches all of it off
	CHECK(rst.find("mix.ag_on = false;") != std::string::npos && rst.find("mix.bang_on = false;") != std::string::npos &&
	      rst.find("mix.own_split = false;") != std::string::npos);

	// play_no_feedback carries the category and the flags to its private copy
	const std::string pnf = body(core, "void CSoundRender_Core::play_no_feedback(");
	CHECK(pnf.find("S._p->mix_cat = orig->mix_cat;") != std::string::npos);
	CHECK(pnf.find("S._p->mix_flags = orig->mix_flags;") != std::string::npos);

	// footsteps: the actor keeps the plain distance, the script callback too; others are tagged
	const std::string upd = body(step, "void CStepManager::update(bool b_hud_view)");
	CHECK(upd.find("const bool b_play_step = step_is_actor ? b_play_sound : (b_play_sound || b_play_others_sound);") !=
	      std::string::npos);
	CHECK(upd.find("if (b_play_sound) // as before the headphones range") != std::string::npos);
	// the AI hears others' steps exactly as without a headset: a step the headset range adds beyond the step gate is
	// listener-only (no AI event), a step a range below 1 cuts inside the gate still plays, muted (its AI event stays)
	CHECK(upd.find("const u8 step_flags = u8((b_play_sound ? 0 : smf_listener_only) | (b_play_others_sound ? 0 : smf_mute));") !=
	      std::string::npos);
	CHECK(upd.find("m_step_sound.play_next(mtl_pair, m_object, m_step_info.params.step[i].power, b_hud_view, true, step_flags);") !=
	      std::string::npos);
	const std::string pn = body(step, "void CStepManager::material_sound::play_next(");
	CHECK(pn.find("step_snd.set_mix_category(smc_steps_others);") != std::string::npos);
	CHECK(pn.find("step_snd.set_mix_category(prev_mix);") != std::string::npos);
	CHECK(pn.find("step_snd.set_mix_flags(mix_flags);") != std::string::npos);
	CHECK(pn.find("step_snd.set_mix_flags(prev_flags);") != std::string::npos);
	CHECK(pn.find("&vol);") != std::string::npos); // the AI-visible volume is still the step power

	std::printf("part 2 (sources in %s): %s\n", root.c_str(), g_fail != before ? "FAIL" : "ok");
	std::printf(g_fail ? "FAILED\n" : "ALL PASS\n");
	return g_fail ? 1 : 0;
}
