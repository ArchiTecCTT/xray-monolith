// Standalone regression for the EFT parts aim-in springs (src/xrGame/EftAimSettle.h, used by
// CWeapon::UpdateHudAdditional; script strength SetAimSettleK, API bit 1024); not an engine build.
//
// Part 1 runs the header the engine includes: the spring math against a fine RK4 integration, a model of the offset part
// of UpdateHudAdditional (the copy of InterpolateOffset below, the HUD index rules, the pending test, the matrix offset
// with the kick) for frame-rate independence, off by default, the off states and resets, the fitted curve shapes, the
// hold, the time base, the strength setter and the key reader.
// Part 2 reads the engine sources and checks that the change is what is written there (it fails on 9c39838's sources).
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/eft_aimsettle_test.cpp -o /tmp/aimsettle_test
//   /tmp/aimsettle_test .
// The argument is the repository root (default "."); a second argument is a directory holding Weapon.cpp, Weapon.h,
// WeaponAK74.cpp and EftAimSettle.h to check instead of the ones under src/xrGame (e.g. the old sources from git show).
#include "../src/xrGame/EftAimSettle.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace as = eft_aim_settle;

struct V3
{
	float x = 0, y = 0, z = 0;
};
static bool same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static float maxdiff(const V3& a, const V3& b)
{
	return std::fmax(std::fabs(a.x - b.x), std::fmax(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
}
static float maxabs(const V3& a) { return std::fmax(std::fabs(a.x), std::fmax(std::fabs(a.y), std::fabs(a.z))); }
static const float EPS_ = 0.0000100f; // xrCore EPS
static const double DEG = 180.0 / 3.14159265358979323846;

// CWeapon::InterpolateOffset, as in Weapon.cpp (part 2 checks the text)
static void interpolate(V3& current, const V3& target, const float factor)
{
	if (std::fabs(target.x - current.x) < EPS_ && std::fabs(target.y - current.y) < EPS_ &&
		std::fabs(target.z - current.z) < EPS_)
		current = target;
	else
	{
		V3 diff = target;
		diff.x -= current.x; diff.y -= current.y; diff.z -= current.z;
		const float f = factor * 2.5f;
		diff.x *= f; diff.y *= f; diff.z *= f;
		current.x += diff.x; current.y += diff.y; current.z += diff.z;
	}
}

// ------------------------------------------------------------------------------------------------ the key reader
struct MapReader
{
	std::map<std::string, std::string> kv;
	std::vector<std::string> bads;
	bool has(const char* k) const { return kv.count(k) != 0; }
	const char* str(const char* k) const
	{
		auto it = kv.find(k);
		return it == kv.end() ? nullptr : it->second.c_str();
	}
	void bad(const char* k, const char* why) { bads.push_back(std::string(k) + ": " + why); }
	bool logged(const char* k) const
	{
		for (auto& b : bads)
			if (b.compare(0, std::string(k).size() + 1, std::string(k) + ":") == 0) return true;
		return false;
	}
};

// the AK-74 values the script side will set (proposal "What this means for the AK", as given in the brief)
static std::map<std::string, std::string> ak_keys()
{
	return {
		{"aim_travel_pos_freq", "15.5"}, {"aim_travel_pos_damping", "0.76"}, {"aim_travel_pos_hold", "0.045"},
		{"aim_travel_rot_freq", "10.0"}, {"aim_travel_rot_damping", "0.64"}, {"aim_travel_rot_hold", "0.135"},
		{"aim_travel_ref_time", "0.28"},
		{"aim_settle_rot_kick", "85, 0, 570"}, {"aim_settle_rot_freq", "19"}, {"aim_settle_rot_damping", "0.21"},
		{"aim_settle_pos_kick", "-870, -670, -2295"}, {"aim_settle_pos_freq", "23.5"}, {"aim_settle_pos_damping", "0.20"},
		{"aim_settle_falloff", "0.35"},
	};
}
static as::config load_cfg(const std::map<std::string, std::string>& kv, MapReader* out = nullptr)
{
	MapReader r;
	r.kv = kv;
	as::config c;
	as::load(c, r);
	if (out) *out = r;
	return c;
}

// ------------------------------------------------------------------- the offset part of CWeapon::UpdateHudAdditional
struct Hud
{
	as::config c;
	as::state s;
	float k = 1.f;            // m_aim_settle_k
	V3 pos, rot;              // m_hud_offset[0], [1]
	int last_idx = 0;
	float rotate_time = .28f; // zoom_rotate_time x addon factors
	float safemode_time = 1.f;
	bool adjust = false;
	V3 pos_to[5], rot_to[5];  // the targets per HUD idx (idx 0 = m_hands_offset[..][5])
	V3 rpos, rrot;            // what the HUD matrix gets
	float t = 0.f;            // time since the last zoom-in (for the logs)

	void frame(int idx, bool zoomed, float dt)
	{
		const float factor = (idx == 4 || last_idx == 4) ? dt / safemode_time : dt / rotate_time;
		const V3 po = pos_to[idx], ro = rot_to[idx];
		if (c.any())
		{
			const bool aim = (idx == 1 || idx == 3) && last_idx != 4 && !adjust;
			as::frame(c, s, aim, idx == 0, zoomed, dt, rotate_time, k, pos, po, rot, ro,
			          [factor](V3& current, const V3& target) { interpolate(current, target, factor); });
		}
		else
		{
			interpolate(pos, po, factor);
			interpolate(rot, ro, factor);
		}
		if (maxdiff(po, pos) < .02f && maxdiff(ro, rot) < .02f) last_idx = idx;
		rpos = pos;
		rrot = rot;
		if (s.kick) s.add_kick(rpos, rrot);
		t += dt;
	}
};

static const V3 HIP_POS{0.f, 0.f, 0.f}, HIP_ROT{0.f, 0.f, 0.f};
static const V3 AIM_POS{-.12f, .03f, -.13f};    // 179 mm from the hip
static const V3 AIM_ROT{.015f, .018f, .008f};   // 1.42 deg
static const V3 ALT_POS{-.10f, .05f, -.14f}, ALT_ROT{.01f, .01f, .7f};

static Hud ak_hud(const as::config& c, float k)
{
	Hud h;
	h.c = c;
	h.k = k;
	h.pos_to[0] = HIP_POS; h.rot_to[0] = HIP_ROT;
	h.pos_to[1] = AIM_POS; h.rot_to[1] = AIM_ROT;
	h.pos_to[2] = V3{-.05f, -.02f, -.05f}; h.rot_to[2] = V3{.05f, 0.f, 0.f};
	h.pos_to[3] = ALT_POS; h.rot_to[3] = ALT_ROT;
	h.pos_to[4] = V3{.05f, -.08f, 0.f}; h.rot_to[4] = V3{-.4f, .3f, 0.f};
	h.pos = HIP_POS;
	h.rot = HIP_ROT;
	h.frame(0, false, 1.f / 60.f); // settled at the hip
	h.t = 0.f;
	return h;
}

struct Sample
{
	double t;
	V3 pos, rot;    // m_hud_offset (the travel)
	V3 kpos, krot;  // the kick
};
static Sample sample(const Hud& h)
{
	Sample s;
	s.t = h.t;
	s.pos = h.pos;
	s.rot = h.rot;
	s.kpos = V3{h.s.ky[0][0], h.s.ky[0][1], h.s.ky[0][2]};
	s.krot = V3{h.s.ky[1][0], h.s.ky[1][1], h.s.ky[1][2]};
	return s;
}
// zoom in at t = 0 and run the frames of `dts`
static std::vector<Sample> run(const as::config& c, float k, const std::vector<float>& dts, int idx = 1)
{
	Hud h = ak_hud(c, k);
	std::vector<Sample> out;
	for (float dt : dts)
	{
		h.frame(idx, true, dt);
		out.push_back(sample(h));
	}
	return out;
}
// the exact state at t: a fresh rifle, zoomed in, one frame of length t
static Sample exact(const as::config& c, float k, float t)
{
	return run(c, k, {t}).back();
}

// ------------------------------------------------------------------------------------------- RK4 of the same ODE
static void rk4(double w, double z, double a, double c, double T, double& x, double& v)
{
	const int n = int(T / 1e-6 + .5);
	const double h = T / n;
	auto acc = [&](double t, double xx, double vv) { return a + c * t - 2 * z * w * vv - w * w * xx; };
	for (int i = 0; i < n; ++i)
	{
		const double t = i * h;
		const double k1x = v, k1v = acc(t, x, v);
		const double k2x = v + .5 * h * k1v, k2v = acc(t + .5 * h, x + .5 * h * k1x, v + .5 * h * k1v);
		const double k3x = v + .5 * h * k2v, k3v = acc(t + .5 * h, x + .5 * h * k2x, v + .5 * h * k2v);
		const double k4x = v + h * k3v, k4v = acc(t + h, x + h * k3x, v + h * k3v);
		x += h / 6 * (k1x + 2 * k2x + 2 * k3x + k4x);
		v += h / 6 * (k1v + 2 * k2v + 2 * k3v + k4v);
	}
}

static void test_math()
{
	const double ws[] = {1.0, 10.0, 19.0, 100.0};
	const double zs[] = {.05, .21, .64, .9999, 1.0, 1.00005, 1.5, 2.0};
	double worst = 0;
	for (double w : ws)
		for (double z : zs)
			for (int forced = 0; forced < 2; ++forced)
			{
				const double dt = .05, x0 = .3, v0 = -2.0, a = forced ? 400.0 : 0.0, c = forced ? -900.0 : 0.0;
				double rx = x0, rv = v0;
				rk4(w, as::critical(z) ? 1.0 : z, a, c, dt, rx, rv); // BetterSpring's band: zeta within 1e-4 of 1 is 1
				float x = float(x0), v = float(v0);
				const as::step_matrix m = as::transition(w, z, dt);
				if (forced) as::forced_axis(m, w, z, dt, a, c, x, v);
				else as::free_axis(m, x, 0.0, v);
				const double scale = std::fmax(1.0, std::fmax(std::fabs(rx), std::fabs(rv) / w));
				const double err = std::fmax(std::fabs(x - rx), std::fabs(v - rv) / w) / scale;
				worst = std::fmax(worst, err);
				if (err > 2e-5) std::printf("  math w %g zeta %g forced %d: x %.9g vs %.9g, v %.9g vs %.9g\n", w, z, forced, x, rx, v, rv);
				CHECK(err < 2e-5);
			}
	std::printf("  exact step vs RK4 (1 us), w 1..100, zeta 0.05..2, free and linear input: worst rel. error %.2e\n", worst);
	// a huge step stays finite
	float x = .2f, v = 3.f;
	const as::step_matrix m = as::transition(100.0 * 1000, .05, 10.0);
	as::free_axis(m, x, 0.0, v);
	CHECK(std::isfinite(x) && std::isfinite(v));
}

// ------------------------------------------------------------------------------------------- frame-rate independence
static std::vector<float> grid(float dt, float T)
{
	std::vector<float> d;
	for (float t = 0; t < T; t += dt) d.push_back(dt);
	return d;
}
static void test_framerate()
{
	const as::config c = load_cfg(ak_keys());
	CHECK(c.any());
	std::mt19937 rng(12345);
	std::uniform_real_distribution<float> jit(.004f, .040f);
	std::vector<float> jdts;
	for (float t = 0; t < 1.5f;) { jdts.push_back(jit(rng)); t += jdts.back(); }
	for (float k : {1.f, 2.f})
	{
		const std::vector<Sample> fine = run(c, k, grid(.0001f, 1.5f));
		float kp = 0, kr = 0;
		for (auto& s : fine) { kp = std::fmax(kp, maxabs(s.kpos)); kr = std::fmax(kr, maxabs(s.krot)); }
		const float tp = maxdiff(AIM_POS, HIP_POS), tr = maxdiff(AIM_ROT, HIP_ROT);
		std::printf("  k %g: largest axis of the travel %.1f mm / %.3f deg, of the kick %.2f mm / %.3f deg (the peaks below)\n",
		            k, tp * 1e3, tr * DEG, kp * 1e3, kr * DEG);
		struct Run { const char* name; std::vector<float> dts; };
		const Run runs[] = {{"30 fps", grid(1.f / 30.f, 1.5f)}, {"144 fps", grid(1.f / 144.f, 1.5f)}, {"jitter 4-40 ms", jdts}};
		std::vector<Sample> r30, r144;
		for (const Run& r : runs)
		{
			const std::vector<Sample> got = run(c, k, r.dts);
			float ep = 0, er = 0, ekp = 0, ekr = 0;
			for (const Sample& g : got)
			{
				const Sample e = exact(c, k, float(g.t));
				ep = std::fmax(ep, maxdiff(g.pos, e.pos));
				er = std::fmax(er, maxdiff(g.rot, e.rot));
				ekp = std::fmax(ekp, maxdiff(g.kpos, e.kpos));
				ekr = std::fmax(ekr, maxdiff(g.krot, e.krot));
			}
			std::printf("  k %g %-15s %3zu frames vs the exact state: travel %.2e / %.2e, kick %.2e / %.2e (fraction of peak)\n",
			            k, r.name, got.size(), ep / tp, er / tr, ekp / kp, ekr / kr);
			CHECK(ep / tp < .01f && er / tr < .01f && ekp / kp < .01f && ekr / kr < .01f);
			if (r.dts[0] == 1.f / 30.f) r30 = got;
			if (r.dts[0] == 1.f / 144.f) r144 = got;
		}
		// 30 against 144 fps at their common times (every 1/6 s: frame 5n and 24n)
		float d = 0;
		int n = 0;
		for (size_t i = 5, j = 24; i <= r30.size() && j <= r144.size(); i += 5, j += 24, ++n)
		{
			const Sample &a = r30[i - 1], &b = r144[j - 1];
			d = std::fmax(d, std::fmax(maxdiff(a.pos, b.pos) / tp, maxdiff(a.rot, b.rot) / tr));
			d = std::fmax(d, std::fmax(maxdiff(a.kpos, b.kpos) / kp, maxdiff(a.krot, b.krot) / kr));
		}
		std::printf("  k %g 30 vs 144 fps at %d common times (1/6 s apart): max deviation %.2e of peak\n", k, n, d);
		CHECK(n >= 8 && d < .01f);
	}
}

// ------------------------------------------------------------------------------------------- off by default
static void test_off()
{
	MapReader r;
	const as::config none = load_cfg({}, &r);
	CHECK(!none.any() && r.bads.empty());
	const as::config zero = load_cfg({{"aim_travel_pos_freq", "0"}, {"aim_travel_rot_freq", "-3"}, {"aim_settle_rot_freq", "0"},
	                                  {"aim_settle_pos_freq", "0"}, {"aim_settle_falloff", "0.3"}},
	                                 &r);
	CHECK(!zero.any() && r.bads.empty());
	// the same frames with and without the header: identical numbers, frame by frame
	Hud h = ak_hud(none, 1.f);
	V3 pos = h.pos, rot = h.rot;
	int last_idx = h.last_idx, frames = 0, diff = 0;
	const int seq[][2] = {{0, 0}, {1, 1}, {3, 1}, {1, 1}, {2, 1}, {1, 1}, {0, 0}, {4, 0}, {0, 0}, {1, 1}};
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> jit(.004f, .040f);
	for (auto& sq : seq)
		for (int i = 0; i < 25; ++i, ++frames)
		{
			const float dt = jit(rng);
			const int idx = sq[0];
			h.frame(idx, sq[1] != 0, dt);
			const float factor = (idx == 4 || last_idx == 4) ? dt / h.safemode_time : dt / h.rotate_time;
			interpolate(pos, h.pos_to[idx], factor);
			interpolate(rot, h.rot_to[idx], factor);
			if (maxdiff(h.pos_to[idx], pos) < .02f && maxdiff(h.rot_to[idx], rot) < .02f) last_idx = idx;
			if (!same(pos, h.pos) || !same(rot, h.rot) || !same(h.rpos, h.pos) || !same(h.rrot, h.rot) || last_idx != h.last_idx) ++diff;
		}
	std::printf("  no keys: %d frames (hip, aim, V, launcher, lowered), %d differ from InterpolateOffset\n", frames, diff);
	CHECK(diff == 0);
}

// ------------------------------------------------------------------------------------------- off states and resets
static bool state_zero(const as::state& s)
{
	for (int i = 0; i < 2; ++i)
	{
		if (s.hold[i] != 0.f) return false;
		for (int a = 0; a < 3; ++a)
			if (s.tv[i][a] != 0.f || s.ky[i][a] != 0.f || s.kv[i][a] != 0.f) return false;
	}
	return !s.kick && s.kt == 0.f;
}
static void test_off_states()
{
	const as::config c = load_cfg(ak_keys());
	const float dt = 1.f / 60.f;
	// zoom-out mid-bump: no pop, state zero, then a zoom-in exactly like a fresh one
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) h.frame(1, true, dt); // 0.117 s: the bump near its peak
		CHECK(h.s.kick && maxabs(V3{h.s.ky[1][0], h.s.ky[1][1], h.s.ky[1][2]}) > .02f);
		const V3 rp = h.rpos, rr = h.rrot;
		h.frame(0, false, dt);
		V3 ep = rp, er = rr; // the slide from what was on screen
		interpolate(ep, HIP_POS, dt / h.rotate_time);
		interpolate(er, HIP_ROT, dt / h.rotate_time);
		std::printf("  zoom-out mid-bump: rendered step %.2f mm / %.3f deg, the slide's own step from the shown pose; diff %.1e / %.1e\n",
		            maxdiff(h.rpos, rp) * 1e3, maxdiff(h.rrot, rr) * DEG, maxdiff(h.rpos, ep), maxdiff(h.rrot, er));
		CHECK(same(h.rpos, ep) && same(h.rrot, er));
		CHECK(state_zero(h.s));
		int n = 0;
		for (; n < 400 && !h.s.hip; ++n) h.frame(0, false, dt);
		CHECK(h.s.hip && maxdiff(h.pos, HIP_POS) < EPS_ && maxdiff(h.rot, HIP_ROT) < EPS_); // settled = within EPS
		h.frame(0, false, dt); // and the slide's next frame puts it exactly on the hip, like a fresh rifle
		CHECK(h.s.hip && same(h.pos, HIP_POS) && same(h.rot, HIP_ROT));
		std::printf("  settled at the hip %d frames after the zoom-out (%.2f s)\n", n, n * dt);
		Hud f = ak_hud(c, 1.f);
		int diff = 0;
		for (int i = 0; i < 90; ++i)
		{
			h.frame(1, true, dt);
			f.frame(1, true, dt);
			if (!same(h.rpos, f.rpos) || !same(h.rrot, f.rrot)) ++diff;
		}
		CHECK(diff == 0);
	}
	// a re-aim during the aim-out (not settled at the hip): no hold, from the current offset at rest
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 30; ++i) h.frame(1, true, dt);
		for (int i = 0; i < 5; ++i) h.frame(0, false, dt);
		const V3 p0 = h.pos;
		h.frame(1, true, dt);
		CHECK(!h.s.hip && h.s.hold[0] == 0.f && h.s.hold[1] == 0.f && !same(h.pos, p0));
		CHECK(h.s.kick); // the bump comes with every zoom-in
	}
	// idx 4 (lowered) mid-bump, and the frames after it while last_idx is still 4: the slide, nothing live
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) h.frame(1, true, dt);
		const V3 rp = h.rpos;
		h.frame(4, false, dt);
		V3 ep = rp;
		interpolate(ep, h.pos_to[4], dt / h.safemode_time);
		CHECK(same(h.rpos, ep) && state_zero(h.s));
		for (int i = 0; i < 200; ++i) h.frame(4, false, dt);
		CHECK(h.last_idx == 4 && state_zero(h.s));
		h.frame(0, false, dt);
		CHECK(h.last_idx == 4);
		V3 sp = h.pos;
		h.frame(1, true, dt); // zoom-in while last_idx == 4: no spring, no bump
		interpolate(sp, AIM_POS, dt / h.safemode_time);
		CHECK(h.last_idx == 4 && same(h.pos, sp) && state_zero(h.s));
	}
	// idx 2 (launcher): zoom-in on it = the slide, no bump; launcher off while aimed (2 -> 1) = spring from rest, no
	// hold, no bump
	{
		Hud h = ak_hud(c, 1.f);
		V3 sp = h.pos;
		h.frame(2, true, dt);
		interpolate(sp, h.pos_to[2], dt / h.rotate_time);
		CHECK(same(h.pos, sp) && state_zero(h.s));
		for (int i = 0; i < 20; ++i) h.frame(2, true, dt);
		const V3 p0 = h.pos;
		h.frame(1, true, dt);
		CHECK(!h.s.kick && h.s.hold[0] == 0.f && h.s.hold[1] == 0.f && !same(h.pos, p0));
		// and the bump folded when the launcher comes on mid-bump
		Hud g = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) g.frame(1, true, dt);
		const V3 rr = g.rrot;
		g.frame(2, true, dt);
		V3 er = rr;
		interpolate(er, g.rot_to[2], dt / g.rotate_time);
		CHECK(same(g.rrot, er) && state_zero(g.s));
	}
	// the adjust tool mid-bump: folded, the slide
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) h.frame(1, true, dt);
		const V3 rp = h.rpos;
		h.adjust = true;
		h.frame(1, true, dt);
		V3 ep = rp;
		interpolate(ep, AIM_POS, dt / h.rotate_time);
		CHECK(same(h.rpos, ep) && state_zero(h.s));
	}
	// V (1 <-> 3) while aimed: no hold, no new bump, the speed kept, steering to the new target
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 12; ++i) h.frame(1, true, dt);
		const float kt = h.s.kt;
		const float v0 = h.s.tv[0][0];
		h.frame(3, true, dt);
		CHECK(h.s.hold[0] == 0.f && h.s.hold[1] == 0.f && h.s.kt > kt);
		CHECK(v0 != 0.f);
		for (int i = 0; i < 120; ++i) h.frame(3, true, dt);
		CHECK(maxdiff(h.pos, ALT_POS) < 1e-4f && maxdiff(h.rot, ALT_ROT) < 1e-4f);
		CHECK(!h.s.kick); // the bump is over and zeroed
	}
	// show / hide / Load / net_Destroy: ResetAimSettle = fold, then everything zero
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) h.frame(1, true, dt);
		const V3 rp = h.rpos;
		h.s.fold(h.pos, h.rot);
		h.s.reset();
		CHECK(same(h.pos, rp) && state_zero(h.s) && !h.s.zoomed && !h.s.hip);
	}
	// dt <= 0 and NaN: no step
	{
		Hud h = ak_hud(c, 1.f);
		for (int i = 0; i < 7; ++i) h.frame(1, true, dt);
		const V3 p = h.pos, r = h.rot;
		const float ky = h.s.ky[1][2];
		h.frame(1, true, 0.f);
		h.frame(1, true, -1.f);
		h.frame(1, true, std::numeric_limits<float>::quiet_NaN());
		CHECK(same(h.pos, p) && same(h.rot, r) && h.s.ky[1][2] == ky);
		// a huge dt: finite and settled
		h.frame(1, true, 30.f);
		CHECK(std::isfinite(h.pos.x) && std::isfinite(h.rot.z) && maxdiff(h.pos, AIM_POS) < 1e-4f);
	}
}

// ------------------------------------------------------------------------------------------- fitted curve shapes
struct Shape
{
	double overshoot_pct, t_within5, peak, t_peak;
};
// the travel of one vector (rot or pos) alone, from the hip at rest, fine frames; 5 % of the travel
static Shape travel_shape(const std::map<std::string, std::string>& keys, bool rot, float rotate_time = .28f)
{
	Hud h = ak_hud(load_cfg(keys), 1.f);
	h.rotate_time = rotate_time;
	const V3 to = rot ? AIM_ROT : AIM_POS, from = rot ? HIP_ROT : HIP_POS;
	const double dist = maxdiff(to, from);
	const int ax = 0; // x axis: the largest component of the position travel, a representative one of the rotation
	auto comp = [&](const V3& v) { return ax == 0 ? double(v.x) : 0.0; };
	Shape s{0, 0, 0, 0};
	const double total = double(comp(to)) - comp(from);
	const float dt = .0001f;
	for (int i = 0; i < 20000; ++i)
	{
		h.frame(1, true, dt);
		const double x = comp(rot ? h.rot : h.pos);
		const double past = (x - comp(to)) / total * 100.0; // > 0 = past the target
		s.overshoot_pct = std::fmax(s.overshoot_pct, past);
		if (std::fabs(x - comp(to)) > .05 * std::fabs(total)) s.t_within5 = h.t;
	}
	(void)dist;
	return s;
}
static Shape kick_shape(float K, float w, float z, float falloff)
{
	char buf[64];
	std::snprintf(buf, sizeof buf, "0, 0, %g", K);
	char wb[16], zb[16], fb[16];
	std::snprintf(wb, sizeof wb, "%g", w);
	std::snprintf(zb, sizeof zb, "%g", z);
	std::snprintf(fb, sizeof fb, "%g", falloff);
	const as::config c = load_cfg({{"aim_settle_rot_kick", buf}, {"aim_settle_rot_freq", wb}, {"aim_settle_rot_damping", zb},
	                               {"aim_settle_falloff", fb}});
	Hud h = ak_hud(c, 1.f);
	Shape s{0, 0, 0, 0};
	for (int i = 0; i < 10000; ++i)
	{
		h.frame(1, true, .0001f);
		const double y = h.s.ky[1][2] * DEG;
		if (std::fabs(y) > std::fabs(s.peak)) { s.peak = y; s.t_peak = h.t; }
	}
	return s;
}
static void test_shapes()
{
	const std::map<std::string, std::string> rot_only = {
		{"aim_travel_rot_freq", "10.0"}, {"aim_travel_rot_damping", "0.64"}, {"aim_travel_rot_hold", "0.135"}};
	const std::map<std::string, std::string> pos_only = {
		{"aim_travel_pos_freq", "15.5"}, {"aim_travel_pos_damping", "0.76"}, {"aim_travel_pos_hold", "0.045"}};
	const Shape r = travel_shape(rot_only, true), p = travel_shape(pos_only, false);
	std::printf("  travel rot (10, 0.64, hold 0.135): overshoot %.2f %% (7.4 +-0.5), within 5 %% from %.4f s (0.645 +-0.02)\n",
	            r.overshoot_pct, r.t_within5);
	std::printf("  travel pos (15.5, 0.76, hold 0.045): overshoot %.2f %% (2.5 +-0.3), within 5 %% from %.4f s (0.251 +-0.01)\n",
	            p.overshoot_pct, p.t_within5);
	CHECK(std::fabs(r.overshoot_pct - 7.4) <= .5 && std::fabs(r.t_within5 - .645) <= .02);
	CHECK(std::fabs(p.overshoot_pct - 2.5) <= .3 && std::fabs(p.t_within5 - .251) <= .01);
	// the MP-153 rotation kick: roll K 1143 deg/s^2, w 19, zeta 0.21, falloff 0.37
	const Shape k = kick_shape(1143.f, 19.f, .21f, .37f), k2 = kick_shape(1143.f / 2, 19.f, .21f, .37f);
	std::printf("  MP-153 rotation kick (19, 0.21, falloff 0.37, K 1143): peak %.3f deg at %.3f s (3.75 +-0.15 near 0.14); half K: %.3f deg (ratio %.5f)\n",
	            k.peak, k.t_peak, k2.peak, k2.peak / k.peak);
	CHECK(std::fabs(k.peak - 3.75) <= .15 && k.t_peak > .11 && k.t_peak < .17);
	CHECK(std::fabs(k2.peak / k.peak - .5) < 1e-4);
	// the hold: the offset does not move until it ends (rotation 0.135 s, position 0.045 s), then it does
	{
		Hud h = ak_hud(load_cfg(ak_keys()), 1.f); // ref 0.28 = rotate_time 0.28: scale 1
		bool still_r = true, still_p = true, moved_r = false, moved_p = false;
		for (int i = 0; i < 2000; ++i)
		{
			h.frame(1, true, .0001f);
			const bool rr = !same(h.rot, HIP_ROT), pp = !same(h.pos, HIP_POS);
			if (h.t <= .135f - 1e-5f && rr) still_r = false;
			if (h.t <= .045f - 1e-5f && pp) still_p = false;
			if (h.t >= .1352f && rr) moved_r = true;
			if (h.t >= .0452f && pp) moved_p = true;
		}
		std::printf("  hold: rotation still until 0.135 s %s, position until 0.045 s %s, moving after: %s / %s\n",
		            still_r ? "yes" : "NO", still_p ? "yes" : "NO", moved_r ? "yes" : "NO", moved_p ? "yes" : "NO");
		CHECK(still_r && still_p && moved_r && moved_p);
		// odd frames across the hold end: the same state as fine frames (the step is split at the hold end)
		const as::config c = load_cfg(rot_only);
		const Sample a = run(c, 1.f, {.1f, .1f}).back(), b = exact(c, 1.f, .2f);
		CHECK(maxdiff(a.rot, b.rot) < 1e-7f);
	}
	// the time base: ref 0.28; T_eff 0.14 instead of 0.28 halves the time to 5 % (hold included)
	{
		std::map<std::string, std::string> kv = rot_only;
		kv["aim_travel_ref_time"] = "0.28";
		const Shape a = travel_shape(kv, true, .28f), b = travel_shape(kv, true, .14f), fixed = travel_shape(rot_only, true, .14f);
		std::printf("  time base (ref 0.28): T_eff 0.28 -> %.4f s, T_eff 0.14 -> %.4f s (ratio %.4f); without ref_time T_eff 0.14 -> %.4f s\n",
		            a.t_within5, b.t_within5, b.t_within5 / a.t_within5, fixed.t_within5);
		CHECK(std::fabs(b.t_within5 / a.t_within5 - .5) < .002 && std::fabs(fixed.t_within5 - r.t_within5) < 1e-3);
	}
}

// ------------------------------------------------------------------------------------------- SetAimSettleK
static void test_k()
{
	float k = 1.f;
	CHECK(!as::set_k(k, std::numeric_limits<float>::quiet_NaN()) && k == 1.f);
	CHECK(!as::set_k(k, std::numeric_limits<float>::infinity()) && k == 1.f);
	CHECK(!as::set_k(k, -std::numeric_limits<float>::infinity()) && k == 1.f);
	CHECK(!as::set_k(k, -1.f) && k == 1.f);
	CHECK(as::set_k(k, 10.f) && k == 4.f);
	CHECK(as::set_k(k, 2.5f) && k == 2.5f);
	CHECK(as::set_k(k, 0.f) && k == 0.f);
	// k = 0: no bump, the travel still runs (and the same travel as k = 1)
	const as::config c = load_cfg(ak_keys());
	const std::vector<Sample> s0 = run(c, 0.f, grid(1.f / 60.f, 1.f)), s1 = run(c, 1.f, grid(1.f / 60.f, 1.f));
	bool nobump = true, travel_same = true;
	for (size_t i = 0; i < s0.size(); ++i)
	{
		nobump = nobump && maxabs(s0[i].kpos) == 0.f && maxabs(s0[i].krot) == 0.f;
		travel_same = travel_same && same(s0[i].pos, s1[i].pos) && same(s0[i].rot, s1[i].rot);
	}
	CHECK(nobump && travel_same && maxdiff(s0.back().pos, AIM_POS) < 1e-4f);
	// k scales the bump linearly
	const std::vector<Sample> s2 = run(c, 2.f, grid(1.f / 60.f, 1.f));
	CHECK(std::fabs(s2[6].krot.z / s1[6].krot.z - 2.f) < 1e-4f);
	// the travel does not depend on the kick, also when the bump is over long before the travel (a slow raise)
	{
		const as::config q = load_cfg({{"aim_travel_pos_freq", "2"}, {"aim_travel_pos_damping", "0.5"}, {"aim_travel_rot_freq", "2"},
		                               {"aim_travel_rot_damping", "0.5"}, {"aim_settle_rot_kick", "1, 0, 1"}, {"aim_settle_rot_freq", "100"},
		                               {"aim_settle_rot_damping", "2"}, {"aim_settle_falloff", "0.01"}});
		const std::vector<Sample> a = run(q, 1.f, grid(1.f / 60.f, 2.f)), b = run(q, 0.f, grid(1.f / 60.f, 2.f));
		bool same_travel = true, bump_over = false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			same_travel = same_travel && same(a[i].pos, b[i].pos) && same(a[i].rot, b[i].rot);
			if (a[i].t < 1.f && maxabs(a[i].krot) == 0.f && i > 0) bump_over = true;
		}
		CHECK(bump_over && same_travel);
	}
}

// ------------------------------------------------------------------------------------------- the key reader
static void test_load()
{
	MapReader r;
	const as::config c = load_cfg(ak_keys(), &r);
	CHECK(r.bads.empty());
	CHECK(c.travel_w[0] == 15.5f && c.travel_zeta[0] == .76f && c.travel_hold[0] == .045f);
	CHECK(c.travel_w[1] == 10.f && c.travel_zeta[1] == .64f && c.travel_hold[1] == .135f);
	CHECK(c.ref_time == .28f && c.falloff == .35f);
	CHECK(c.kick_w[1] == 19.f && c.kick_zeta[1] == .21f && c.kick_w[0] == 23.5f && c.kick_zeta[0] == .2f);
	CHECK(std::fabs(c.kick[1][0] - 85.0 / DEG) < 1e-7 && c.kick[1][1] == 0.f && std::fabs(c.kick[1][2] - 570.0 / DEG) < 1e-6);
	CHECK(std::fabs(c.kick[0][0] + .870) < 1e-6 && std::fabs(c.kick[0][1] + .670) < 1e-6 && std::fabs(c.kick[0][2] + 2.295) < 1e-6);
	struct Case { const char* key; const char* val; bool erase; const char* logkey; int which; };
	// which: 0 travel pos off, 1 travel rot off, 2 both kicks off, 3 rot kick off, 4 pos kick off, 5 time base off
	const Case cases[] = {
		{"aim_travel_pos_freq", "nan", false, "aim_travel_pos_freq", 0},
		{"aim_travel_pos_freq", "abc", false, "aim_travel_pos_freq", 0},
		{"aim_travel_pos_damping", "", true, "aim_travel_pos_damping", 0},
		{"aim_travel_pos_damping", "inf", false, "aim_travel_pos_damping", 0},
		{"aim_travel_pos_damping", "-0.5", false, "aim_travel_pos_damping", 0},
		{"aim_travel_rot_hold", "2", false, "aim_travel_rot_hold", 1},
		{"aim_travel_rot_hold", "-0.1", false, "aim_travel_rot_hold", 1},
		{"aim_settle_falloff", "", true, "aim_settle_falloff", 2},
		{"aim_settle_falloff", "0", false, "aim_settle_falloff", 2},
		{"aim_settle_falloff", "3", false, "aim_settle_falloff", 2},
		{"aim_settle_rot_kick", "85, 0", false, "aim_settle_rot_kick", 3},
		{"aim_settle_rot_kick", "85, nan, 570", false, "aim_settle_rot_kick", 3},
		{"aim_settle_rot_kick", "85, 0, 570, 1", false, "aim_settle_rot_kick", 3},
		{"aim_settle_rot_kick", "85, 0, 30000", false, "aim_settle_rot_kick", 3},
		{"aim_settle_pos_kick", "", true, "aim_settle_pos_kick", 4},
		{"aim_settle_pos_kick", "-870, -670, -200000", false, "aim_settle_pos_kick", 4},
		{"aim_travel_ref_time", "-1", false, "aim_travel_ref_time", 5},
		{"aim_travel_ref_time", "9", false, "aim_travel_ref_time", 5},
	};
	for (const Case& cs : cases)
	{
		auto kv = ak_keys();
		if (cs.erase) kv.erase(cs.key);
		else kv[cs.key] = cs.val;
		MapReader rr;
		const as::config b = load_cfg(kv, &rr);
		const bool logged = rr.logged(cs.logkey) && rr.bads.size() == 1;
		bool ok = false;
		switch (cs.which)
		{
		case 0: ok = !b.travel_on(0) && b.travel_on(1) && b.kick_on(0) && b.kick_on(1); break;
		case 1: ok = b.travel_on(0) && !b.travel_on(1) && b.kick_on(0) && b.kick_on(1); break;
		case 2: ok = b.travel_on(0) && b.travel_on(1) && !b.kick_on(0) && !b.kick_on(1) && b.kick[1][2] == 0.f; break;
		case 3: ok = b.travel_on(0) && b.travel_on(1) && b.kick_on(0) && !b.kick_on(1); break;
		case 4: ok = b.travel_on(0) && b.travel_on(1) && !b.kick_on(0) && b.kick_on(1); break;
		case 5: ok = b.travel_on(0) && b.travel_on(1) && b.ref_time == 0.f && b.kick_on(1); break;
		}
		if (!ok || !logged) std::printf("  load case %s = '%s'%s: on/off %s, log %s\n", cs.key, cs.val, cs.erase ? " (removed)" : "", ok ? "ok" : "WRONG", logged ? "ok" : "WRONG");
		CHECK(ok && logged);
	}
	// clamps: w to 1..100, zeta to 0.05..2, each with a log line, the spring stays on
	{
		auto kv = ak_keys();
		kv["aim_travel_pos_freq"] = "500";
		kv["aim_travel_rot_damping"] = "3";
		kv["aim_settle_rot_freq"] = "0.5";
		kv["aim_settle_pos_damping"] = "0.01";
		MapReader rr;
		const as::config b = load_cfg(kv, &rr);
		CHECK(b.travel_w[0] == 100.f && b.travel_zeta[1] == 2.f && b.kick_w[1] == 1.f && b.kick_zeta[0] == .05f);
		CHECK(rr.bads.size() == 4);
	}
	// a zero kick vector: that kick off, silently; spaces anywhere
	{
		auto kv = ak_keys();
		kv["aim_settle_pos_kick"] = "0,0,0";
		kv["aim_settle_rot_kick"] = "  85 ,0,   570  ";
		MapReader rr;
		const as::config b = load_cfg(kv, &rr);
		CHECK(!b.kick_on(0) && b.kick_on(1) && rr.bads.empty());
	}
	std::printf("  key reader: AK values, %zu bad-value cases (each: its spring off, one log line), clamps\n",
	            sizeof cases / sizeof cases[0]);
}

// ---------------------------------------------------------------------------------------------------------------- Part 2
static std::string slurp(const std::string& p, bool must = true)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
	{
		if (must) { std::printf("FAIL cannot open %s\n", p.c_str()); ++g_fail; }
		return "";
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}
static size_t count(const std::string& s, const std::string& k)
{
	size_t n = 0;
	for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + 1)) ++n;
	return n;
}
// the text from the line holding `head` to the next line that starts with '}' (a function body at column 0)
static std::string body(const std::string& s, const std::string& head)
{
	size_t a = s.find(head);
	if (a == std::string::npos) return "";
	size_t b = s.find("\n}", a);
	return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static bool in_order(const std::string& s, const char* a, const char* b)
{
	size_t pa = s.find(a), pb = pa == std::string::npos ? pa : s.find(b, pa);
	return pa != std::string::npos && pb != std::string::npos;
}

static void part2(const std::string& root, const std::string& dir)
{
	const std::string cpp = slurp(dir + "/Weapon.cpp"), h = slurp(dir + "/Weapon.h"), ak = slurp(dir + "/WeaponAK74.cpp");
	// the header the engine includes is the one this test ran
	const std::string tested = slurp(root + "/src/xrGame/EftAimSettle.h"), there = slurp(dir + "/EftAimSettle.h", false);
	CHECK(!tested.empty() && there == tested);

	// Weapon.h: the include, the state, the setter/getter, the reset
	CHECK(h.find("#include \"EftAimSettle.h\"") != std::string::npos);
	CHECK(h.find("eft_aim_settle::config m_aim_settle_cfg;") != std::string::npos);
	CHECK(h.find("eft_aim_settle::state m_aim_settle;") != std::string::npos);
	CHECK(h.find("float m_aim_settle_k = 1.f;") != std::string::npos);
	CHECK(h.find("bool SetAimSettleKScript(float k) { return eft_aim_settle::set_k(m_aim_settle_k, k); }") != std::string::npos);
	CHECK(h.find("float GetAimSettleKScript() const { return m_aim_settle_k; }") != std::string::npos);
	CHECK(in_order(h, "void ResetAimSettle()", "m_aim_settle.fold(m_hud_offset[0], m_hud_offset[1]);"));
	CHECK(in_order(h, "m_aim_settle.fold(m_hud_offset[0], m_hud_offset[1]);", "m_aim_settle.reset();"));

	// the keys: read from the HUD section at the end of Load, through the header's reader
	CHECK(in_order(body(cpp, "void CWeapon::Load(LPCSTR section)"), "m_aimpos = READ_IF_EXISTS", "LoadAimSettle();"));
	const std::string ld = body(cpp, "void CWeapon::LoadAimSettle()");
	CHECK(ld.find("return !!pSettings->line_exist(sect, key);") != std::string::npos);
	CHECK(ld.find("return pSettings->r_string(sect, key);") != std::string::npos);
	CHECK(ld.find("Msg(\"! [EFT aim settle] [%s] %s = %s: %s\", sect, key, v ? v : \"\", why);") != std::string::npos);
	CHECK(in_order(ld, "reader r = {hud_sect.c_str()};", "eft_aim_settle::load(m_aim_settle_cfg, r);"));
	CHECK(in_order(ld, "eft_aim_settle::load(m_aim_settle_cfg, r);", "ResetAimSettle();"));
	// zeroed on show, hide, drop, net_Destroy
	CHECK(body(cpp, "void CWeapon::OnActiveItem()").find("ResetAimSettle();") != std::string::npos);
	CHECK(in_order(body(cpp, "void CWeapon::OnHiddenItem()"), "OnZoomOut();", "ResetAimSettle();"));
	CHECK(body(cpp, "void CWeapon::OnH_B_Independent(bool just_before_destroy)").find("ResetAimSettle();") != std::string::npos);
	CHECK(body(cpp, "void CWeapon::net_Destroy()").find("ResetAimSettle();") != std::string::npos);

	// InterpolateOffset is the slide the test copies
	const std::string io = body(cpp, "void CWeapon::InterpolateOffset(Fvector& current, const Fvector& target, const float factor) const");
	CHECK(in_order(io, "if (target.similar(current, EPS))", "current.set(target);"));
	CHECK(in_order(io, "diff.sub(current);", "diff.mul(factor * 2.5f);"));

	// UpdateHudAdditional: the springs with keys, today's two lines without, the pending test as it was, the kick in the
	// matrix only
	const std::string hud = body(cpp, "void CWeapon::UpdateHudAdditional(Fmatrix& trans)");
	CHECK(in_order(hud, "curr_offs = hi->m_measures.m_hands_offset[0][idx];", "if (m_aim_settle_cfg.any())"));
	CHECK(in_order(hud, "if (m_aim_settle_cfg.any())",
	               "const bool aim = (idx == 1 || idx == 3) && last_idx != 4 && !g_player_hud->m_adjust_mode;"));
	CHECK(in_order(hud, "const bool aim =", "const float rotate_time = m_zoom_params.m_fZoomRotateTime * cur_silencer_koef.zoom_rotate_time *\n\t\t\t\tcur_scope_koef.zoom_rotate_time * cur_launcher_koef.zoom_rotate_time;"));
	CHECK(in_order(hud, "const float rotate_time =", "eft_aim_settle::frame(m_aim_settle_cfg, m_aim_settle, aim, idx == 0, IsZoomed(), Device.fTimeDelta,\n\t\t\t\trotate_time, m_aim_settle_k, m_hud_offset[0], curr_offs, m_hud_offset[1], curr_rot,"));
	CHECK(in_order(hud, "eft_aim_settle::frame(", "{ InterpolateOffset(current, target, factor); });"));
	CHECK(in_order(hud, "{ InterpolateOffset(current, target, factor); });", "else\n\t\t{\n\t\t\tInterpolateOffset(m_hud_offset[0], curr_offs, factor);\n\t\t\tInterpolateOffset(m_hud_offset[1], curr_rot, factor);\n\t\t}\n\t\tInterpolateOffset(m_hud_aim_rot, curr_aim_rot, factor);"));
	CHECK(in_order(hud, "InterpolateOffset(m_hud_aim_rot, curr_aim_rot, factor);", "if (curr_offs.similar(m_hud_offset[0], .02f) && curr_rot.similar(m_hud_offset[1], .02f))"));
	CHECK(in_order(hud, "if (curr_offs.similar(m_hud_offset[0], .02f)", "Fvector hud_offs = m_hud_offset[0], hud_rot = m_hud_offset[1];"));
	CHECK(in_order(hud, "Fvector hud_offs = m_hud_offset[0], hud_rot = m_hud_offset[1];", "if (m_aim_settle.kick)\n\t\t\tm_aim_settle.add_kick(hud_offs, hud_rot);"));
	CHECK(in_order(hud, "m_aim_settle.add_kick(hud_offs, hud_rot);", "hud_rotation.rotateX(hud_rot.x);"));
	CHECK(in_order(hud, "hud_rotation.rotateX(hud_rot.x);", "hud_rotation_y.rotateY(hud_rot.y);"));
	CHECK(in_order(hud, "hud_rotation_y.rotateY(hud_rot.y);", "hud_rotation_y.rotateZ(hud_rot.z);"));
	CHECK(in_order(hud, "hud_rotation_y.rotateZ(hud_rot.z);", "hud_rotation.translate_over(hud_offs);"));
	CHECK(count(hud, "rotateX(m_hud_offset[1].x)") == 0 && count(hud, "translate_over(m_hud_offset[0])") == 0);
	// the kick is never written into m_hud_offset by the weapon (only ResetAimSettle's fold) and read nowhere else
	CHECK(count(cpp, "add_kick") == 1 && count(cpp, "m_aim_settle.ky") == 0 && count(cpp, "m_aim_settle.fold") == 0);
	CHECK(count(cpp, "eft_aim_settle::frame(") == 1);
	// the zoom-rotation factor and the inertia roll are untouched
	CHECK(in_order(hud, "if (pActor->IsZoomAimingMode())", "m_zoom_params.m_fZoomRotationFactor += factor;"));
	CHECK(cpp.find("R.rotateZ(-m_hud_offset[1].z);") != std::string::npos);

	// WeaponAK74.cpp: bit 1024 and the two exports on CWeapon, after GetAltAim
	CHECK(ak.find("static int eft_weapon_api() { return 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024; }") != std::string::npos);
	CHECK(ak.find("//   1024 = Get/SetAimSettleK") != std::string::npos);
	CHECK(in_order(ak, "class_<CWeapon,", ".def(\"GetAltAim\", &CWeapon::GetAltAimScript)"));
	CHECK(in_order(ak, ".def(\"GetAltAim\", &CWeapon::GetAltAimScript)", ".def(\"SetAimSettleK\", &CWeapon::SetAimSettleKScript)"));
	CHECK(in_order(ak, ".def(\"SetAimSettleK\", &CWeapon::SetAimSettleKScript)", ".def(\"GetAimSettleK\", &CWeapon::GetAimSettleKScript)"));
}

int main(int argc, char** argv)
{
	std::string root = argc > 1 ? argv[1] : ".";
	std::string dir = argc > 2 ? argv[2] : root + "/src/xrGame";
	struct { const char* name; void (*fn)(); } parts[] = {
		{"math", test_math}, {"frame rate", test_framerate}, {"off by default", test_off}, {"off states, reset", test_off_states},
		{"curve shapes", test_shapes}, {"SetAimSettleK", test_k}, {"key reader", test_load}};
	int f0 = g_fail;
	for (auto& p : parts)
	{
		const int before = g_fail;
		std::printf("%s:\n", p.name);
		p.fn();
		std::printf("  -> %s\n", g_fail > before ? "FAIL" : "PASS");
	}
	const int f1 = g_fail;
	std::printf("part 1 (model): %s\n", f1 > f0 ? "FAIL" : "PASS");
	part2(root, dir);
	std::printf("part 2 (sources in %s): %s\n", dir.c_str(), g_fail > f1 ? "FAIL" : "PASS");
	std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
	return g_fail ? 1 : 0;
}
