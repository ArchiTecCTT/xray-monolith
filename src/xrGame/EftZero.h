#pragma once
#include <cmath>

// EFT parts: the sight's zero (CWeapon::SetZeroScript, API bit 2048).
//
// A zeroed rifle shoots a little upward relative to its sight line, so the falling bullet crosses that line again at the
// zero range. The sight picture does not move: the shot leaves `elev` radians above the aim (and `wind` radians to the
// right of it) in CWeapon::FireTrace, before the dispersion (CShootingObject::FireBullet -> random_dir). The shot's start,
// the HUD/eye pick, firepos/aimpos and the blocked-barrel decision stay as they are; the caller skips this for a blocked
// barrel and for shots that are not the actor's. "Up" is the world's up in the plane of the shot (gravity's plane), so a
// rolled rifle (V, a canted sight) is zeroed against the same drop.
//
// Self-contained (no engine types): tests/eft_zero_test.cpp runs this same code.
namespace eft_zero
{
const float ELEV_MAX = .1f;    // rad (5.7 deg): far beyond any rifle zero (1000 m at 900 m/s needs ~.01)
const float WIND_MAX = .01f;   // rad
const float FLAT_MIN = 1e-4f;  // horizontal length of the unit shot direction below which the shot is vertical: unchanged

// The values SetZeroScript takes: both finite, |elev| <= ELEV_MAX, |wind| <= WIND_MAX.
inline bool accept(float elev, float wind)
{
	return std::isfinite(elev) && std::isfinite(wind) && std::fabs(elev) <= ELEV_MAX && std::fabs(wind) <= WIND_MAX;
}

// Turns the direction (x right, y up, z forward; made unit first) up by elev and then right by wind, unit length out.
// False (dir untouched) when both are 0, the direction is zero or the shot is (nearly) vertical.
// V: anything with float members x, y, z (Fvector).
template <class V>
bool apply(V& dir, float elev, float wind)
{
	if (elev == 0.f && wind == 0.f)
		return false;
	const float l0 = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (!(l0 > 0.f))
		return false;
	const float dx = dir.x / l0, dy = dir.y / l0, dz = dir.z / l0;
	const float flat = std::sqrt(dx * dx + dz * dz);
	if (!(flat >= FLAT_MIN))
		return false;
	// right: horizontal, perpendicular to the shot; up: perpendicular to the shot in the vertical plane through it
	const float rx = dz / flat, rz = -dx / flat;
	const float ux = -dx * dy / flat, uy = flat, uz = -dz * dy / flat;
	const float ce = std::cos(elev), se = std::sin(elev), cw = std::cos(wind), sw = std::sin(wind);
	const float ex = dx * ce + ux * se, ey = dy * ce + uy * se, ez = dz * ce + uz * se;
	float x = ex * cw + rx * sw, y = ey * cw, z = ez * cw + rz * sw;
	const float len = std::sqrt(x * x + y * y + z * z);
	if (!(len > 0.f))
		return false;
	dir.x = x / len;
	dir.y = y / len;
	dir.z = z / len;
	return true;
}
} // namespace eft_zero
