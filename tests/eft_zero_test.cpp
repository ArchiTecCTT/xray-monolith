// Standalone regression for the EFT parts sight zero (src/xrGame/EftZero.h, used by CWeapon::FireTrace; script setter
// SetZero, API bit 2048); not an engine build.
//
// Part 1 runs the header the engine includes: the setter's limits, the turn of the shot direction (up by the elevation in
// the vertical plane, right by the windage, unit length, untouched when off or vertical), a model of FireTrace's gate
// (the actor's own shot, not a blocked barrel), the dispersion AFTER the zero (a copy of random_dir: the group moves, its
// spread stays), and the engine's bullet flight (a float copy of trajectory_position, Level_Bullet_Manager.cpp) with the
// zero for 100 m: the round comes back to the sight line at 100 m. The angle is the one scripts/porting/tarkov_zeroing.py
// (anomaly-devkit) writes into the zero tables (0.000620994 rad for 904 m/s, drag 0.3, gravity 9.81).
// Part 2 reads the engine sources and checks that the change is what is written there (it fails on c1dda26's sources).
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/eft_zero_test.cpp -o /tmp/zero_test && /tmp/zero_test .
// The argument is the repository root (default "."); a second argument is a directory holding WeaponFire.cpp, Weapon.h,
// Weapon.cpp, WeaponAK74.cpp, ShootingObject.cpp, WeaponMagazinedWGrenade.cpp, Level_Bullet_Manager.cpp and EftZero.h to
// check instead of the ones under src/xrGame (e.g. the old sources from git show).
#include "../src/xrGame/EftZero.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace ez = eft_zero;

struct V3
{
	float x = 0, y = 0, z = 0;
};
static V3 v3(float x, float y, float z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
static float len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 dir_hp(float heading, float pitch) // x right, y up, z forward; heading turns right
{
	return v3(std::sin(heading) * std::cos(pitch), std::sin(pitch), std::cos(heading) * std::cos(pitch));
}
static float pitch_of(const V3& d) { return std::asin(std::fmax(-1.f, std::fmin(1.f, d.y / len(d)))); }
static float heading_of(const V3& d) { return std::atan2(d.x, d.z); }
static bool same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// ---------------------------------------------------------------------------------------------------------------- Part 1
// CWeapon::FireTrace's gate, as written in WeaponFire.cpp (Part 2 checks the line)
static V3 fire_trace_dir(V3 D, float elev, float wind, bool parent_is_actor, bool barrel_blocked)
{
	V3 shot_dir = D;
	if ((elev != 0.f || wind != 0.f) && parent_is_actor && !barrel_blocked)
		ez::apply(shot_dir, elev, wind);
	return shot_dir;
}

// WeaponFire.cpp random_dir / _nrand (deterministic generator in place of the engine's Random)
static std::mt19937 rng(12345);
static float randF(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
static float nrand(float sigma)
{
	const float ONE_OVER_SIGMA_EXP = 1.0f / 0.7975f;
	if (sigma == 0) return 0;
	float y;
	do { y = -std::log(randF(0.f, 1.f)); } while (randF(0.f, 1.f) > std::exp(-(y - 1.0f) * (y - 1.0f) * 0.5f));
	return (rng() & 1) ? y * sigma * ONE_OVER_SIGMA_EXP : -y * sigma * ONE_OVER_SIGMA_EXP;
}
static void basis(const V3& d, V3& u, V3& v) // Fvector::generate_orthonormal_basis
{
	V3 up = std::fabs(d.y) < .99f ? v3(0, 1, 0) : v3(1, 0, 0);
	u = v3(up.y * d.z - up.z * d.y, up.z * d.x - up.x * d.z, up.x * d.y - up.y * d.x);
	float l = len(u);
	u = v3(u.x / l, u.y / l, u.z / l);
	v = v3(d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x);
}
static V3 random_dir(const V3& src, float dispersion)
{
	float sigma = dispersion / 3.f;
	float alpha = std::fmax(-dispersion, std::fmin(dispersion, nrand(sigma)));
	float theta = randF(0, 3.14159265f);
	float r = std::tan(alpha);
	V3 U, V;
	basis(src, U, V);
	V3 t = v3(src.x + U.x * r * std::sin(theta) + V.x * r * std::cos(theta), src.y + U.y * r * std::sin(theta) + V.y * r * std::cos(theta),
		src.z + U.z * r * std::sin(theta) + V.z * r * std::cos(theta));
	float l = len(t);
	return v3(t.x / l, t.y / l, t.z / l);
}

// Level_Bullet_Manager.cpp trajectory_position (float, as the engine), y up; x down range in the shot's vertical plane
static const float AIR_EPS = .1f;
static void trajectory(float t, float vx, float vy, float g, float a, float& x, float& y)
{
	float pt = std::fmax(0.f, 1.f / a - AIR_EPS);
	if (t - pt < 0.f)
	{
		float h = t * t * .5f;
		x = vx * t - vx * a * h;
		y = vy * t - vy * a * h - g * h;
		return;
	}
	float h = pt * pt * .5f;
	float px = vx * pt - vx * a * h, py = vy * pt - vy * a * h - g * h;
	float f = std::fmax(0.f, 1.f - a * pt);
	float ux = vx * f, uy = vy * f - g * pt, d = t - pt;
	x = px + ux * d;
	y = py + uy * d - g * d * d * .5f;
}
// the height at range X of a shot along `dir` at v0 from the origin (fine time steps, then the chord: like the engine)
static float height_at(float X, const V3& dir, float v0, float g, float a)
{
	float flat = std::sqrt(dir.x * dir.x + dir.z * dir.z);
	float vx = v0 * flat, vy = v0 * dir.y;
	float t0 = 0.f, x0 = 0.f, y0 = 0.f;
	for (int i = 1; i < 2000000; ++i)
	{
		float t = i * .0005f, x, y;
		trajectory(t, vx, vy, g, a, x, y);
		if (x >= X)
			return y0 + (y - y0) * (X - x0) / (x - x0);
		t0 = t, x0 = x, y0 = y;
	}
	(void)t0;
	return -std::numeric_limits<float>::infinity();
}
static float solve(float X, float v0, float g, float a)
{
	float lo = -.01f, hi = .05f;
	for (int i = 0; i < 60; ++i)
	{
		float m = (lo + hi) * .5f;
		if (height_at(X, dir_hp(0.f, m), v0, g, a) < 0.f) lo = m; else hi = m;
	}
	return (lo + hi) * .5f;
}

static void part1()
{
	const float NaN = std::numeric_limits<float>::quiet_NaN(), INF = std::numeric_limits<float>::infinity();
	// the setter's limits
	CHECK(ez::accept(0.f, 0.f));
	CHECK(ez::accept(ez::ELEV_MAX, ez::WIND_MAX) && ez::accept(-ez::ELEV_MAX, -ez::WIND_MAX));
	CHECK(!ez::accept(ez::ELEV_MAX * 1.001f, 0.f) && !ez::accept(0.f, ez::WIND_MAX * 1.001f));
	CHECK(!ez::accept(NaN, 0.f) && !ez::accept(0.f, NaN) && !ez::accept(INF, 0.f) && !ez::accept(0.f, -INF));
	CHECK(ez::accept(.0099f, 0.f)); // 1000 m with the owner's ballistics needs ~.0096

	// off: untouched, false
	V3 d = dir_hp(.3f, .1f), d0 = d;
	CHECK(!ez::apply(d, 0.f, 0.f) && same(d, d0));
	// a vertical shot: untouched, false
	V3 up = v3(0, 1, 0), down = v3(0, -1, 0), nearly = v3(0.00005f, 0.99999999f, 0.f);
	CHECK(!ez::apply(up, .01f, 0.f) && same(up, v3(0, 1, 0)));
	CHECK(!ez::apply(down, .01f, .001f) && same(down, v3(0, -1, 0)));
	CHECK(!ez::apply(nearly, .01f, 0.f));
	V3 zl = v3(0, 0, 0);
	CHECK(!ez::apply(zl, .01f, 0.f) && same(zl, v3(0, 0, 0)));

	// a level shot: up by exactly the elevation, the heading unchanged, unit length
	V3 lvl = v3(0, 0, 1);
	CHECK(ez::apply(lvl, .002f, 0.f));
	CHECK(std::fabs(pitch_of(lvl) - .002f) < 1e-6f && std::fabs(heading_of(lvl)) < 1e-7f && std::fabs(len(lvl) - 1.f) < 1e-6f);
	// windage: to the right (+x for a shot along +z), the pitch unchanged
	V3 wnd = v3(0, 0, 1);
	CHECK(ez::apply(wnd, 0.f, .001f));
	CHECK(wnd.x > 0.f && std::fabs(heading_of(wnd) - .001f) < 1e-6f && std::fabs(pitch_of(wnd)) < 1e-7f);
	// a direction a little off unit length (float drift) comes back unit and turned as the unit one
	V3 unit = dir_hp(.7f, -.2f), longer = v3(unit.x * 1.5f, unit.y * 1.5f, unit.z * 1.5f);
	ez::apply(unit, .004f, .0005f);
	ez::apply(longer, .004f, .0005f);
	CHECK(std::fabs(len(longer) - 1.f) < 1e-6f && std::fabs(longer.x - unit.x) < 1e-6f && std::fabs(longer.y - unit.y) < 1e-6f &&
		std::fabs(longer.z - unit.z) < 1e-6f);
	V3 neg = v3(0, 0, 1);
	ez::apply(neg, -.003f, -.001f);
	CHECK(pitch_of(neg) < 0.f && neg.x < 0.f);

	// any heading and pitch: the pitch grows by the elevation (the turn stays in the vertical plane), the heading stays
	float worst_p = 0.f, worst_h = 0.f, worst_l = 0.f, worst_a = 0.f;
	for (int i = 0; i < 4000; ++i)
	{
		float h = randF(-3.1f, 3.1f), p = randF(-1.2f, 1.2f), e = randF(-.05f, .05f), w = randF(-.01f, .01f);
		V3 a = dir_hp(h, p), b = a;
		CHECK(ez::apply(b, e, 0.f));
		worst_p = std::fmax(worst_p, std::fabs(pitch_of(b) - (p + e)));
		float dh = std::fabs(heading_of(b) - h);
		worst_h = std::fmax(worst_h, std::fmin(dh, 6.2831853f - dh));
		worst_l = std::fmax(worst_l, std::fabs(len(b) - 1.f));
		V3 c = a;
		ez::apply(c, e, w); // up then right: the angle to the aim is acos(cos e cos w)
		V3 x = v3(a.y * c.z - a.z * c.y, a.z * c.x - a.x * c.z, a.x * c.y - a.y * c.x);
		// sin^2 = 1 - cos^2 e cos^2 w = sin^2 e + cos^2 e sin^2 w (no cancellation), in double
		double se = std::sin((double)e), ce = std::cos((double)e), sw = std::sin((double)w), cw = std::cos((double)w);
		float want = (float)std::atan2(std::sqrt(se * se + ce * ce * sw * sw), ce * cw);
		worst_a = std::fmax(worst_a, std::fabs(std::atan2(len(x), dot(a, c)) - want));
	}
	CHECK(worst_p < 2e-5f);
	CHECK(worst_h < 2e-5f);
	CHECK(worst_l < 2e-6f);
	CHECK(worst_a < 2e-5f);
	std::printf("turn: worst pitch error %.2g rad, heading %.2g rad, length %.2g, combined angle %.2g rad\n", worst_p, worst_h,
		worst_l, worst_a);

	// FireTrace's gate: only the actor's own shot with a free barrel
	V3 D = dir_hp(.5f, .02f);
	CHECK(same(fire_trace_dir(D, 0.f, 0.f, true, false), D));
	CHECK(!same(fire_trace_dir(D, .001f, 0.f, true, false), D));
	CHECK(!same(fire_trace_dir(D, 0.f, .001f, true, false), D));
	CHECK(same(fire_trace_dir(D, .001f, .001f, false, false), D)); // an NPC's shot
	CHECK(same(fire_trace_dir(D, .001f, .001f, true, true), D));   // a blocked barrel (the eye -> barrel trace)

	// the dispersion comes after the zero: the group's centre moves up by the elevation, its spread stays
	const int N = 20000;
	const float disp = .004f, e = .0025f;
	double mp0 = 0, mp1 = 0, s0 = 0, s1 = 0;
	V3 aim = dir_hp(0.f, 0.f), zeroed = aim;
	ez::apply(zeroed, e, 0.f);
	for (int i = 0; i < N; ++i)
	{
		float a = pitch_of(random_dir(aim, disp)), b = pitch_of(random_dir(zeroed, disp));
		mp0 += a, mp1 += b, s0 += a * a, s1 += b * b;
	}
	mp0 /= N, mp1 /= N;
	double sd0 = std::sqrt(s0 / N - mp0 * mp0), sd1 = std::sqrt(s1 / N - mp1 * mp1);
	CHECK(std::fabs(mp0) < 5e-5);
	CHECK(std::fabs(mp1 - e) < 5e-5);
	CHECK(std::fabs(sd1 / sd0 - 1.0) < .05);
	std::printf("group: centre %.5f -> %.5f rad (zero %.4f), spread %.5f -> %.5f rad\n", mp0, mp1, e, sd0, sd1);

	// the engine's flight: zeroed for 100 m (904 m/s, drag 0.3, gravity 9.81), the round is on the sight line at 100 m
	const float v0 = 904.f, g = 9.81f, A = .3f;
	float th = solve(100.f, v0, g, A);
	CHECK(std::fabs(th - .000620994f) < 2e-8f); // tarkov_zeroing.py's table angle (double solve, exact closed form)
	V3 shot = v3(0, 0, 1);
	ez::apply(shot, th, 0.f);
	float y100 = height_at(100.f, shot, v0, g, A), y50 = height_at(50.f, shot, v0, g, A), y200 = height_at(200.f, shot, v0, g, A);
	CHECK(std::fabs(y100) < .001f);
	CHECK(std::fabs(y50 - .016f) < .001f && std::fabs(y200 + .133f) < .002f);
	CHECK(height_at(100.f, v3(0, 0, 1), v0, g, A) < -.05f); // no zero: 6 cm low at 100 m
	std::printf("flight: zero %.9f rad; at 50 / 100 / 200 m: %+.4f / %+.4f / %+.4f m\n", th, y50, y100, y200);
}

// ---------------------------------------------------------------------------------------------------------------- Part 2
static std::string slurp(const std::string& p)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) { std::printf("FAIL cannot open %s\n", p.c_str()); ++g_fail; return ""; }
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}
// the text from the line holding `head` to the next line that starts with '}' (a function body at column 0)
static std::string body(const std::string& s, const std::string& head)
{
	size_t a = s.find(head);
	if (a == std::string::npos) return "";
	size_t b = s.find("\n}", a);
	return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
static bool in_order(const std::string& s, const char* a, const char* b)
{
	size_t pa = s.find(a), pb = pa == std::string::npos ? pa : s.find(b, pa);
	return pa != std::string::npos && pb != std::string::npos;
}

static void part2(const std::string& dir)
{
	const std::string fire = slurp(dir + "/WeaponFire.cpp"), h = slurp(dir + "/Weapon.h"), cpp = slurp(dir + "/Weapon.cpp"),
		ak = slurp(dir + "/WeaponAK74.cpp"), so = slurp(dir + "/ShootingObject.cpp"), gl = slurp(dir + "/WeaponMagazinedWGrenade.cpp"),
		bm = slurp(dir + "/Level_Bullet_Manager.cpp"), ez_h = slurp(dir + "/EftZero.h");

	// WeaponFire.cpp: the zero in FireTrace, before the shot loop; P unchanged; the setter
	CHECK(has(fire, "#include \"EftZero.h\""));
	const std::string ft = body(fire, "void CWeapon::FireTrace(const Fvector& P, const Fvector& D)");
	CHECK(has(ft, "\tFvector shot_dir = D;\n"));
	CHECK(has(ft, "\tif ((m_eft_zero_elev != 0.f || m_eft_zero_wind != 0.f) && ParentIsActor() && !GetPick().barrel_blocked)\n"
		"\t\teft_zero::apply(shot_dir, m_eft_zero_elev, m_eft_zero_wind);\n"));
	CHECK(in_order(ft, "eft_zero::apply(shot_dir", "for (int i = 0; i < l_cartridge.param_s.buckShot; ++i)"));
	CHECK(has(ft, "FireBullet(P, shot_dir, fire_disp, l_cartridge, H_Parent()->ID(), ID(), SendHit, iAmmoElapsed);"));
	CHECK(!has(ft, "FireBullet(P, D,"));
	const std::string sz = body(fire, "bool CWeapon::SetZeroScript(float elev, float wind)");
	CHECK(has(sz, "if (!eft_zero::accept(elev, wind))\n\t\treturn false;"));
	CHECK(in_order(sz, "return false;", "m_eft_zero_elev = elev;") && has(sz, "m_eft_zero_wind = wind;"));

	// ShootingObject.cpp: the dispersion turns the direction FireTrace passed (after the zero), then the bullet starts
	const std::string fb = body(so, "void CShootingObject::FireBullet(const Fvector& pos,");
	CHECK(in_order(fb, "random_dir(dir, shot_dir, fire_disp);", "Level().BulletManager().AddBullet(pos,"));

	// the launcher does not go through FireTrace, and nothing else reads the zero
	const std::string lg = body(gl, "void CWeaponMagazinedWGrenade::LaunchGrenade()");
	CHECK(!lg.empty() && !has(lg, "FireTrace") && !has(lg, "m_eft_zero"));
	CHECK(!has(gl, "m_eft_zero") && !has(so, "m_eft_zero"));

	// Weapon.h: declaration, getters, the two fields (0 = off); not saved or sent (Weapon.cpp never names them)
	CHECK(has(h, "\tbool SetZeroScript(float elev, float wind);\n"));
	CHECK(has(h, "float GetZeroElevationScript() const { return m_eft_zero_elev; }"));
	CHECK(has(h, "float GetZeroWindageScript() const { return m_eft_zero_wind; }"));
	CHECK(has(h, "\tfloat m_eft_zero_elev = 0.f;\n") && has(h, "\tfloat m_eft_zero_wind = 0.f;\n"));
	CHECK(!has(cpp, "m_eft_zero"));

	// WeaponAK74.cpp: bit 2048 and the three exports on CWeapon, after GetAimSettleK
	CHECK(has(ak, "static int eft_weapon_api() { return 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048"));
	CHECK(has(ak, "//   2048 = SetZero / GetZeroElevation / GetZeroWindage"));
	CHECK(in_order(ak, ".def(\"GetAimSettleK\", &CWeapon::GetAimSettleKScript)", ".def(\"SetZero\", &CWeapon::SetZeroScript)"));
	CHECK(in_order(ak, ".def(\"SetZero\", &CWeapon::SetZeroScript)", ".def(\"GetZeroElevation\", &CWeapon::GetZeroElevationScript)"));
	CHECK(in_order(ak, ".def(\"GetZeroElevation\", &CWeapon::GetZeroElevationScript)",
		".def(\"GetZeroWindage\", &CWeapon::GetZeroWindageScript)"));

	// the flight the calibration copies (tarkov_zeroing.py): one drag in single player, the parabola's split, the epsilon
	CHECK(has(bm, "static float const air_resistance_epsilon = .1f;"));
	CHECK(has(bm, "float const air_resistance = (GameID() == eGameIDSingle) ? m_fAirResistanceK : bullet.air_resistance;"));
	const std::string tp = body(bm, "static Fvector trajectory_position(");
	CHECK(has(tp, "float const parabolic_time = _max(0.f, 1.f / air_resistance - air_resistance_epsilon);"));
	const std::string pp = body(bm, "static Fvector parabolic_position(");
	CHECK(has(pp, "Fvector(start_velocity).mul(-air_resistance),\n\t\t\tsqr_t_div_2"));
	CHECK(has(bm, "starting_speed *= cartridge.param_s.kBulletSpeed;"));

	// EftZero.h is the header Part 1 ran
	CHECK(has(ez_h, "namespace eft_zero") && has(ez_h, "const float ELEV_MAX = .1f;"));
}

int main(int argc, char** argv)
{
	std::string root = argc > 1 ? argv[1] : ".";
	std::string dir = argc > 2 ? argv[2] : root + "/src/xrGame";
	part1();
	int f1 = g_fail;
	std::printf("part 1 (model): %s\n", f1 ? "FAIL" : "PASS");
	part2(dir);
	std::printf("part 2 (sources in %s): %s\n", dir.c_str(), g_fail > f1 ? "FAIL" : "PASS");
	std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
	return g_fail ? 1 : 0;
}
