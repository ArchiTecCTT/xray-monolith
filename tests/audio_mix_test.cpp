// Standalone regression for the headphones listener-side mix (src/xrSound/SoundRender_Mix.h, docs/audio_api.md);
// not an engine build.
//
// Part 1 runs the header the engine includes: the category fade ramp, the loud-sound limiter step (gain computer and
// attack / release smoother), the compressor (soft-knee curve, makeup, attack / release, switching off) and OpenAL's
// distance formula used by the compressor's detector, including frame-rate independence.
// Part 2 reads the engine sources and checks the wiring: the mix only scales the OpenAL gain and rolloff, the AI hearing
// path (Event_Propagade, SoundEvent_Register) does not read it, play_no_feedback carries the category, the footstep
// gate, the compressor's detector chain and the Lua exports.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/audio_mix_test.cpp -o /tmp/audio_mix_test
//   /tmp/audio_mix_test .
// The argument is the repository root (default ".").

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// stubs for what SoundRender_Mix.h takes from xrCore / Sound.h
typedef unsigned int u32;
template <class T> T _abs(T v) { return v < 0 ? -v : v; }
struct xrCriticalSection { void Enter() {} void Leave() {} };
enum { smc_world = 0, smc_steps_others = 3, smc_count = 6 }; // as in Sound.h (checked in part 2)

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

	// the AI path never reads the mix
	const std::string prop = body(emitter, "void CSoundRender_Emitter::Event_Propagade()");
	CHECK(!prop.empty() && prop.find("mix") == std::string::npos && prop.find("max_ai_distance") != std::string::npos);
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
	const size_t e6 = cats.find("smc_count,");
	CHECK(e0 != std::string::npos && e6 != std::string::npos);
	CHECK(e0 < e1 && e1 < e2 && e2 < e3 && e3 < e4 && e4 < e5 && e5 < e6);
	// the compressor gain reaches a source only through mix_gain, for the categories of the compressed set
	const std::string mg = body(core, "float CSoundRender_Core::mix_gain(CSoundRender_Emitter* E) const");
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
	CHECK(lua.find("static int audio_api() { return 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128; }") != std::string::npos);
	for (const char* fn : {"audio_set_compressor", "audio_set_compressor_detect", "audio_set_compressor_category",
	                       "audio_get_compressor_category", "audio_get_compressor_enabled", "audio_get_compressor_gain",
	                       "audio_get_compressor_level", "audio_set_category_rolloff", "audio_get_category_rolloff"})
	{
		const std::string d = std::string("def(\"") + fn + "\", &" + fn + ")";
		if (lua.find(d) == std::string::npos)
		{
			std::printf("FAIL: no Lua export %s\n", fn);
			++g_fail;
		}
	}

	// play_no_feedback carries the category to its private copy
	const std::string pnf = body(core, "void CSoundRender_Core::play_no_feedback(");
	CHECK(pnf.find("S._p->mix_cat = orig->mix_cat;") != std::string::npos);

	// footsteps: the actor keeps the plain distance, the script callback too; others are tagged
	const std::string upd = body(step, "void CStepManager::update(bool b_hud_view)");
	CHECK(upd.find("(step_is_actor ? b_play_sound : b_play_others_sound)") != std::string::npos);
	CHECK(upd.find("m_step_sound.play_next(mtl_pair, m_object, m_step_info.params.step[i].power, b_hud_view, true);") !=
	      std::string::npos);
	CHECK(upd.find("if (b_play_sound) // as before the headphones range") != std::string::npos);
	// range factor < 1: the callback still fires out to step_sound_distance (the sound alone is cut)
	CHECK(upd.find("if (!step_is_actor && object && b_play_sound && !b_play_others_sound && is_on_ground())") !=
	      std::string::npos);
	const std::string pn = body(step, "void CStepManager::material_sound::play_next(");
	CHECK(pn.find("step_snd.set_mix_category(smc_steps_others);") != std::string::npos);
	CHECK(pn.find("step_snd.set_mix_category(prev_mix);") != std::string::npos);
	CHECK(pn.find("&vol);") != std::string::npos); // the AI-visible volume is still the step power

	std::printf("part 2 (sources in %s): %s\n", root.c_str(), g_fail != before ? "FAIL" : "ok");
	std::printf(g_fail ? "FAILED\n" : "ALL PASS\n");
	return g_fail ? 1 : 0;
}
