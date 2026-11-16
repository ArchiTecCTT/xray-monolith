// Standalone regression for the headphones listener-side mix (src/xrSound/SoundRender_Mix.h, docs/audio_api.md);
// not an engine build.
//
// Part 1 runs the header the engine includes: the category fade ramp and the loud-sound limiter step (gain computer and
// attack / release smoother), including frame-rate independence.
// Part 2 reads the engine sources and checks the wiring: the mix only scales the OpenAL gain, the AI hearing path
// (Event_Propagade, SoundEvent_Register) does not read it, play_no_feedback carries the category, the footstep gate.
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
enum { smc_count = 6 };

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
	std::printf("part 1 (math): %s\n", g_fail ? "FAIL" : "ok");

	// ---- part 2: sources
	const int before = g_fail;
	const std::string snd = root + "/src/xrSound/";
	const std::string target = read_file(snd + "SoundRender_TargetA.cpp");
	const std::string emitter = read_file(snd + "SoundRender_Emitter.cpp");
	const std::string core = read_file(snd + "SoundRender_Core.cpp");
	const std::string level = read_file(root + "/src/xrEngine/IGame_Level.cpp");
	const std::string step = read_file(root + "/src/xrGame/step_manager.cpp");
	CHECK(!target.empty() && !emitter.empty() && !core.empty() && !level.empty() && !step.empty());

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
