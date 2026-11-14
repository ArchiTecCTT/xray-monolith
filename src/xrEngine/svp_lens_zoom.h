#pragma once
// Scroll-wheel zoom of a 3D scope lens (second viewport, "SVP"): the step and easing math, and the R1 helper that
// scales the last SVP image while the lens FOV changes. Plain functions (only <cmath>) so tests/svp_lens_test.cpp can
// include this file as it is. See SVP-LITE-NOTES.md "Scroll-wheel lens zoom".
#include <cmath>

namespace svp_lens_zoom
{
// Lens FOV (vertical degrees) of step 'step' (0 = fov_min = most zoom, 'steps' = fov_max = least zoom): geometric
// steps, so each wheel notch changes the magnification by the same factor.
inline float StepFov(float fov_min, float fov_max, int steps, int step)
{
	if (steps < 1 || !(fov_max > fov_min) || !(fov_min > 0.f))
		return fov_min;
	if (step <= 0)
		return fov_min;
	if (step >= steps)
		return fov_max;
	return fov_min * std::pow(fov_max / fov_min, float(step) / float(steps));
}

// The step whose FOV is nearest (in log space) to 'fov'; clamped to 0..steps.
inline int NearestStep(float fov_min, float fov_max, int steps, float fov)
{
	if (steps < 1 || !(fov_max > fov_min) || !(fov_min > 0.f) || !(fov > fov_min))
		return 0;
	if (fov >= fov_max)
		return steps;
	const float t = std::log(fov / fov_min) / std::log(fov_max / fov_min);
	int s = int(std::floor(t * float(steps) + .5f));
	return s < 0 ? 0 : (s > steps ? steps : s);
}

// Ease time: after kEaseTime seconds 95% of the way is done (exp(-3) = 0.05).
static const float kEaseTime = 0.15f;

// One frame of easing 'cur' toward 'target' (frame-rate independent: the same total time at any frame rate). Snaps
// to the target once closer than 0.1% of it.
inline float Ease(float cur, float target, float dt)
{
	if (!(dt > 0.f))
		return cur;
	const float k = std::exp(-dt * 3.f / kEaseTime);
	float v = target + (cur - target) * k;
	if (std::fabs(v - target) <= 0.001f * std::fabs(target))
		v = target;
	return v;
}

// R1 (r1_svp_reproject): how many work pixels to grow ('k' > 0, zoom out) or shrink ('k' < 0, zoom in) the lens
// square on each side so the last SVP image, rendered with lens FOV 'fov_img', shows the view of lens FOV 'fov_now'.
// The lens square is 'lens' pixels for tan(fov_img / 2); the new one is lens * tan(fov_now / 2) / tan(fov_img / 2).
// Clamped to what the image has: the square stays at least 2 pixels and at most lens + 2 * margin.
inline int LensGrowPx(unsigned lens, unsigned margin, float fov_img, float fov_now)
{
	const float d2r = 3.14159265358979f / 360.f; // degrees -> radians, halved
	if (!(fov_img > 0.f) || !(fov_now > 0.f) || fov_img >= 179.f || fov_now >= 179.f || lens < 2)
		return 0;
	const float scale = std::tan(fov_now * d2r) / std::tan(fov_img * d2r);
	int k = int(std::floor((float(lens) * scale - float(lens)) * .5f + .5f));
	const int kmin = -int((lens - 2) / 2);
	if (k < kmin) k = kmin;
	if (k > int(margin)) k = int(margin);
	return k;
}
}
