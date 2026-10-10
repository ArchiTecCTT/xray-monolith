#pragma once
#include <cmath>
#include <cstdlib>

// EFT parts: the aim-in springs of CWeapon::UpdateHudAdditional (HUD section keys aim_travel_* / aim_settle_*).
//
// Travel: for HUD idx 1/3 the hip->aim offset (m_hud_offset[0] position, [1] rotation) arrives as a damped spring,
//   x'' + 2 zeta w x' + w^2 (x - target) = 0 per axis, instead of the exponential slide (InterpolateOffset). After a
//   zoom-in from rest at the hip target it first holds still for `hold` seconds.
// Kick: a bump added on top (only in the HUD matrix), y'' + 2 zeta w y' + w^2 y = K k b(t), with b falling linearly
//   1 -> 0 over `falloff` seconds from the zoom-in. Tarkov: on aim, SwaySpring.ApplyVelocity(direction * blend) every
//   frame with the blend falling linearly (ProceduralWeaponAnimation.cs:666-670, :2530-2537); here per second.
//
// Every step is the exact solution over dt (Tarkov's BetterSpring.Process, BetterSpring.cs:17-20, :60-190, written as the
// 2x2 transition of (x - target, x'); the kick adds the particular solution of an input linear in time), so the motion
// does not depend on the frame rate. Self-contained (no engine types): tests/eft_aimsettle_test.cpp runs this same code.
namespace eft_aim_settle
{
// Load clamps w and zeta into these; any other key outside its range switches its spring off (one log line).
const float W_MIN = 1.f, W_MAX = 100.f;    // rad/s
const float ZETA_MIN = .05f, ZETA_MAX = 2.f;
const float HOLD_MAX = 1.f;                // s
const float REF_TIME_MAX = 5.f;            // s
const float FALLOFF_MAX = 2.f;             // s
const float ROT_KICK_MAX = 20000.f;        // deg/s^2 per axis
const float POS_KICK_MAX = 100000.f;       // mm/s^2 per axis
const float K_MAX = 4.f;                   // SetAimSettleK
// settled: every axis within SNAP_X of its target (the EPS of CWeapon::InterpolateOffset) and slower than SNAP_V per s
const float SNAP_X = .00001f, SNAP_V = .0001f;

struct config
{
	// [0] position (m), [1] rotation (rad, m_hud_offset[1]: rotateX, rotateY, rotateZ); w <= 0 = that spring off
	float travel_w[2], travel_zeta[2], travel_hold[2];
	float ref_time;   // > 0: w * ref_time / T_eff and hold * T_eff / ref_time (T_eff = zoom_rotate_time x addon factors)
	float kick_w[2], kick_zeta[2];
	float kick[2][3]; // input at full blend: m/s^2 / rad/s^2
	float falloff;    // s
	config() { clear(); }
	void clear()
	{
		for (int i = 0; i < 2; ++i)
		{
			travel_w[i] = travel_zeta[i] = travel_hold[i] = 0.f;
			kick_w[i] = kick_zeta[i] = 0.f;
			kick[i][0] = kick[i][1] = kick[i][2] = 0.f;
		}
		ref_time = falloff = 0.f;
	}
	bool travel_on(int i) const { return travel_w[i] > 0.f; }
	bool kick_on(int i) const { return kick_w[i] > 0.f; }
	bool any() const { return travel_on(0) || travel_on(1) || kick_on(0) || kick_on(1); }
};

// ------------------------------------------------------------------------------------------------- reading the keys
// R: bool has(const char* key), const char* str(const char* key) (may be null), void bad(const char* key, const char* why)
template <class R>
bool read_num(R& r, const char* key, float& out)
{
	const char* s = r.str(key);
	if (!s) return false;
	char* end = nullptr;
	const double v = std::strtod(s, &end);
	if (end == s) return false;
	while (*end == ' ' || *end == '\t') ++end;
	if (*end || !std::isfinite(v)) return false;
	out = float(v);
	return std::isfinite(out);
}

template <class R>
bool read_vec(R& r, const char* key, float out[3])
{
	const char* s = r.str(key);
	if (!s) return false;
	for (int i = 0; i < 3; ++i)
	{
		char* end = nullptr;
		const double v = std::strtod(s, &end);
		if (end == s || !std::isfinite(v)) return false;
		out[i] = float(v);
		while (*end == ' ' || *end == '\t') ++end;
		if (i < 2)
		{
			if (*end != ',') return false;
			++end;
		}
		s = end;
	}
	return *s == 0;
}

// freq + damping of one spring; false = off (freq absent or <= 0 silently, anything wrong with a log line)
template <class R>
bool read_spring(R& r, const char* wkey, const char* zkey, float& w, float& zeta)
{
	w = zeta = 0.f;
	if (!r.has(wkey)) return false;
	float fw, fz;
	if (!read_num(r, wkey, fw)) { r.bad(wkey, "not a finite number, spring off"); return false; }
	if (fw <= 0.f) return false;
	if (!r.has(zkey)) { r.bad(zkey, "missing (needed with a frequency), spring off"); return false; }
	if (!read_num(r, zkey, fz) || fz <= 0.f) { r.bad(zkey, "not a finite number > 0, spring off"); return false; }
	if (fw < W_MIN || fw > W_MAX)
	{
		r.bad(wkey, "outside 1..100 rad/s, clamped");
		fw = fw < W_MIN ? W_MIN : W_MAX;
	}
	if (fz < ZETA_MIN || fz > ZETA_MAX)
	{
		r.bad(zkey, "outside 0.05..2, clamped");
		fz = fz < ZETA_MIN ? ZETA_MIN : ZETA_MAX;
	}
	w = fw;
	zeta = fz;
	return true;
}

template <class R>
void load(config& c, R& r)
{
	c.clear();
	static const char* const tw[2] = {"aim_travel_pos_freq", "aim_travel_rot_freq"};
	static const char* const tz[2] = {"aim_travel_pos_damping", "aim_travel_rot_damping"};
	static const char* const th[2] = {"aim_travel_pos_hold", "aim_travel_rot_hold"};
	static const char* const kw[2] = {"aim_settle_pos_freq", "aim_settle_rot_freq"};
	static const char* const kz[2] = {"aim_settle_pos_damping", "aim_settle_rot_damping"};
	static const char* const kk[2] = {"aim_settle_pos_kick", "aim_settle_rot_kick"};
	static const float kmax[2] = {POS_KICK_MAX, ROT_KICK_MAX};
	static const double kunit[2] = {.001, 3.14159265358979323846 / 180.0}; // mm -> m, deg -> rad
	for (int i = 0; i < 2; ++i)
	{
		if (read_spring(r, tw[i], tz[i], c.travel_w[i], c.travel_zeta[i]) && r.has(th[i]))
		{
			float h;
			if (!read_num(r, th[i], h) || h < 0.f || h > HOLD_MAX)
			{
				r.bad(th[i], "not a time in 0..1 s, spring off");
				c.travel_w[i] = c.travel_zeta[i] = 0.f;
			}
			else
				c.travel_hold[i] = h;
		}
		if (read_spring(r, kw[i], kz[i], c.kick_w[i], c.kick_zeta[i]))
		{
			float v[3];
			if (!r.has(kk[i]))
			{
				r.bad(kk[i], "missing (needed with a frequency), spring off");
				c.kick_w[i] = c.kick_zeta[i] = 0.f;
			}
			else if (!read_vec(r, kk[i], v) || std::fabs(v[0]) > kmax[i] || std::fabs(v[1]) > kmax[i] ||
				std::fabs(v[2]) > kmax[i])
			{
				r.bad(kk[i], i ? "not 3 finite numbers within +-20000 deg/s^2, spring off"
				               : "not 3 finite numbers within +-100000 mm/s^2, spring off");
				c.kick_w[i] = c.kick_zeta[i] = 0.f;
			}
			else if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f)
				c.kick_w[i] = c.kick_zeta[i] = 0.f;
			else
				for (int a = 0; a < 3; ++a)
					c.kick[i][a] = float(v[a] * kunit[i]);
		}
	}
	if (c.kick_on(0) || c.kick_on(1))
	{
		float f;
		if (!r.has("aim_settle_falloff") || !read_num(r, "aim_settle_falloff", f) || f <= 0.f || f > FALLOFF_MAX)
		{
			r.bad("aim_settle_falloff", "missing or not a time in 0..2 s (> 0), kick springs off");
			for (int i = 0; i < 2; ++i)
			{
				c.kick_w[i] = c.kick_zeta[i] = 0.f;
				c.kick[i][0] = c.kick[i][1] = c.kick[i][2] = 0.f;
			}
		}
		else
			c.falloff = f;
	}
	if ((c.travel_on(0) || c.travel_on(1)) && r.has("aim_travel_ref_time"))
	{
		float t;
		if (!read_num(r, "aim_travel_ref_time", t) || t < 0.f || t > REF_TIME_MAX)
			r.bad("aim_travel_ref_time", "not a time in 0..5 s, fixed time (no time base)");
		else
			c.ref_time = t;
	}
}

// SetAimSettleK: refuses a non-finite or negative k (false, nothing changes), clamps to K_MAX
inline bool set_k(float& dst, float k)
{
	if (!std::isfinite(k) || k < 0.f) return false;
	dst = k > K_MAX ? K_MAX : k;
	return true;
}

// ------------------------------------------------------------------------------------------------------- the math
// (e, e') after dt of e'' + 2 zeta w e' + w^2 e = 0: e1 = xx e + xv e', e1' = vx e + vv e'. BetterSpring's three cases;
// as there, zeta within 1e-4 of 1 is solved as 1 (critical).
struct step_matrix
{
	double xx, xv, vx, vv;
};

inline bool critical(double zeta) { return zeta > 0.9999 && zeta <= 1.0001; }

inline step_matrix transition(double w, double zeta, double dt)
{
	step_matrix m;
	if (zeta > 1.0001) // overdamped: c1 e^(z1 t) + c2 e^(z2 t)
	{
		const double zb = w * std::sqrt(zeta * zeta - 1.0), z1 = -w * zeta - zb, z2 = -w * zeta + zb;
		const double e1 = std::exp(z1 * dt), e2 = std::exp(z2 * dt), d = z1 - z2;
		m.xx = e2 - z2 * (e1 - e2) / d;
		m.xv = (e1 - e2) / d;
		m.vx = z2 * e2 - z2 * (z1 * e1 - z2 * e2) / d;
		m.vv = (z1 * e1 - z2 * e2) / d;
	}
	else if (critical(zeta)) // critical: (e + (e' + w e) t) e^(-w t)
	{
		const double ex = std::exp(-w * dt);
		m.xx = ex * (1.0 + w * dt);
		m.xv = ex * dt;
		m.vx = -w * w * dt * ex;
		m.vv = ex * (1.0 - w * dt);
	}
	else // underdamped: e^(-zeta w t) (e cos(a t) + (e' + zeta w e) / a sin(a t)), a = w sqrt(1 - zeta^2)
	{
		const double wz = w * zeta, a = w * std::sqrt(1.0 - zeta * zeta);
		const double ex = std::exp(-wz * dt), c = std::cos(a * dt), s = std::sin(a * dt);
		m.xx = ex * (c + wz / a * s);
		m.xv = ex * s / a;
		m.vx = -ex * s * w * w / a;
		m.vv = ex * (c - wz / a * s);
	}
	return m;
}

// one axis toward a fixed target
inline void free_axis(const step_matrix& m, float& x, double to, float& v)
{
	const double e = double(x) - to, ev = v;
	x = float(to + m.xx * e + m.xv * ev);
	v = float(m.vx * e + m.vv * ev);
}

// one axis of y'' + 2 zeta w y' + w^2 y = a + c t (t = 0 at the start of the step): the particular solution
// (a + c t) / w^2 - 2 zeta c / w^3 taken out, the rest stepped free (m = transition(w, zeta, dt)), put back
inline void forced_axis(const step_matrix& m, double w, double zeta, double dt, double a, double c, float& y, float& v)
{
	if (critical(zeta)) zeta = 1.0; // the same zeta as the free part
	const double w2 = w * w, p0 = a / w2 - 2.0 * zeta * c / (w2 * w), pv = c / w2;
	const double e = double(y) - p0, ev = double(v) - pv;
	y = float(m.xx * e + m.xv * ev + p0 + pv * dt);
	v = float(m.vx * e + m.vv * ev + pv);
}

template <class V>
bool close3(const V& a, const V& b, float eps) // Fvector::similar
{
	return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// travel: x (3 axes) toward `to`; holds still while hold > 0 (the step is split exactly at its end)
template <class V>
void travel(float w, float zeta, float dt, V& x, const V& to, float v[3], float& hold)
{
	if (!(dt > 0.f)) return;
	if (hold > 0.f)
	{
		if (dt <= hold)
		{
			hold -= dt;
			return;
		}
		dt -= hold;
		hold = 0.f;
	}
	if (close3(x, to, SNAP_X) && std::fabs(v[0]) < SNAP_V && std::fabs(v[1]) < SNAP_V && std::fabs(v[2]) < SNAP_V)
	{
		x = to; // settled: no math
		v[0] = v[1] = v[2] = 0.f;
		return;
	}
	const step_matrix m = transition(w, zeta, dt);
	free_axis(m, x.x, to.x, v[0]);
	free_axis(m, x.y, to.y, v[1]);
	free_axis(m, x.z, to.z, v[2]);
}

inline float time_scale(const config& c, float t_eff)
{
	if (!(c.ref_time > 0.f) || !(t_eff > 0.f) || !std::isfinite(t_eff)) return 1.f;
	return c.ref_time / t_eff;
}

struct state
{
	float tv[2][3];           // travel speed ([0] m/s, [1] rad/s)
	float hold[2];            // s the travel still holds still
	float ky[2][3], kv[2][3]; // kick offset ([0] m, [1] rad) and its speed
	float kt;                 // s since the zoom-in (blend = 1 - kt / falloff)
	bool kick;                // ky is live: the HUD matrix adds it
	bool zoomed;              // IsZoomed() last frame
	bool hip;                 // last frame was HUD idx 0 with the offset on its target
	state() { reset(); }
	void stop_kick()
	{
		for (int i = 0; i < 2; ++i)
			for (int a = 0; a < 3; ++a)
				ky[i][a] = kv[i][a] = 0.f;
		kt = 0.f;
		kick = false;
	}
	void stop_motion()
	{
		for (int i = 0; i < 2; ++i)
		{
			hold[i] = 0.f;
			tv[i][0] = tv[i][1] = tv[i][2] = 0.f;
		}
		stop_kick();
	}
	void reset()
	{
		stop_motion();
		zoomed = hip = false;
	}
	// the kick goes into the offset (nothing pops; the slide then carries it away), every speed and hold to zero
	template <class V>
	void fold(V& pos, V& rot)
	{
		if (kick)
		{
			pos.x += ky[0][0]; pos.y += ky[0][1]; pos.z += ky[0][2];
			rot.x += ky[1][0]; rot.y += ky[1][1]; rot.z += ky[1][2];
		}
		stop_motion();
	}
	// the HUD matrix only
	template <class V>
	void add_kick(V& pos, V& rot) const
	{
		pos.x += ky[0][0]; pos.y += ky[0][1]; pos.z += ky[0][2];
		rot.x += ky[1][0]; rot.y += ky[1][1]; rot.z += ky[1][2];
	}
};

inline void kick_step(const config& c, state& s, float dt, float k)
{
	if (!(dt > 0.f)) return;
	if (s.kt >= c.falloff)
	{
		bool quiet = true;
		for (int i = 0; i < 2; ++i)
			for (int a = 0; a < 3; ++a)
				quiet = quiet && std::fabs(s.ky[i][a]) < SNAP_X && std::fabs(s.kv[i][a]) < SNAP_V;
		if (quiet)
		{
			s.stop_kick(); // the bump is over: zero, no math (the travel goes on)
			return;
		}
	}
	double rest = dt;
	if (s.kt < c.falloff) // the input part, split where the blend reaches 0
	{
		const double left = double(c.falloff) - s.kt, d = rest < left ? rest : left;
		const double b0 = 1.0 - s.kt / double(c.falloff);
		for (int i = 0; i < 2; ++i)
		{
			if (!c.kick_on(i)) continue;
			const step_matrix m = transition(c.kick_w[i], c.kick_zeta[i], d);
			for (int a = 0; a < 3; ++a)
			{
				const double K = double(c.kick[i][a]) * k;
				forced_axis(m, c.kick_w[i], c.kick_zeta[i], d, K * b0, -K / c.falloff, s.ky[i][a], s.kv[i][a]);
			}
		}
		s.kt = rest < left ? float(s.kt + d) : c.falloff;
		rest -= d;
	}
	if (rest > 0.0)
		for (int i = 0; i < 2; ++i)
		{
			if (!c.kick_on(i)) continue;
			const step_matrix m = transition(c.kick_w[i], c.kick_zeta[i], rest);
			for (int a = 0; a < 3; ++a)
				free_axis(m, s.ky[i][a], 0.0, s.kv[i][a]);
		}
}

// One frame of the offset part of CWeapon::UpdateHudAdditional with the springs configured (c.any()).
//   aim: HUD idx 1 or 3, not the adjust tool, not coming back from the lowered pose (last_idx 4); otherwise the springs
//        are off for this frame: the kick is folded into the offset, speeds zero, and the slide runs as today
//   at_hip: HUD idx 0;  zoomed: IsZoomed();  t_eff: zoom_rotate_time x silencer/scope/launcher factors;  k: the strength
//   pos/rot: m_hud_offset[0]/[1] (never holding the kick), going to pos_to/rot_to;  slide(cur, to): InterpolateOffset
// Afterwards, while s.kick, the HUD matrix adds the kick (state::add_kick).
template <class V, class Slide>
void frame(const config& c, state& s, bool aim, bool at_hip, bool zoomed, float dt, float t_eff, float k, V& pos,
           const V& pos_to, V& rot, const V& rot_to, Slide&& slide)
{
	const bool zoom_in = zoomed && !s.zoomed;
	s.zoomed = zoomed;
	if (!aim)
	{
		s.fold(pos, rot);
		slide(pos, pos_to);
		slide(rot, rot_to);
		s.hip = at_hip && close3(pos, pos_to, SNAP_X) && close3(rot, rot_to, SNAP_X);
		return;
	}
	const float sc = time_scale(c, t_eff);
	if (zoom_in) // from rest (the off frames zeroed the speeds); a hold only when it starts settled at the hip
	{
		for (int i = 0; i < 2; ++i)
		{
			s.tv[i][0] = s.tv[i][1] = s.tv[i][2] = 0.f;
			s.hold[i] = s.hip && c.travel_on(i) ? c.travel_hold[i] / sc : 0.f;
		}
		if ((c.kick_on(0) || c.kick_on(1)) && k > 0.f)
		{
			s.stop_kick();
			s.kick = true;
		}
	}
	s.hip = false;
	if (c.travel_on(0))
		travel(c.travel_w[0] * sc, c.travel_zeta[0], dt, pos, pos_to, s.tv[0], s.hold[0]);
	else
		slide(pos, pos_to);
	if (c.travel_on(1))
		travel(c.travel_w[1] * sc, c.travel_zeta[1], dt, rot, rot_to, s.tv[1], s.hold[1]);
	else
		slide(rot, rot_to);
	if (s.kick)
		kick_step(c, s, dt, k);
}
} // namespace eft_aim_settle
