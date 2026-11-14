// Standalone check of the scroll-wheel lens zoom math (scope_lense_fov_max, see SVP-LITE-NOTES.md "Scroll-wheel lens
// zoom"); not an engine build.
//
// Part 1 uses src/xrEngine/svp_lens_zoom.h as it is (the engine includes the same file) and checks:
//   * StepFov / NearestStep: geometric steps from scope_lense_fov to scope_lense_fov_max, start-step rounding;
//   * Ease: about 95% of the way after 0.15 s at any frame rate, no overshoot, snaps to the target;
//   * LensGrowPx + the scaled copy of svp_publish / svp_reproject (R1): while the lens FOV eases, the last SVP image
//     scaled into the lens shows the new FOV's view within about one work texel, and the source rect (shifted and
//     grown) always stays inside the work image.
// Part 2 reads the engine sources and checks that they call these functions where the notes say.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/svp_lens_zoom_test.cpp -o /tmp/svp_lens_zoom_test && /tmp/svp_lens_zoom_test .
// The argument is the repository root (default ".").
#include "../src/xrEngine/svp_lens_zoom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace svp_lens_zoom;

static const float PI = 3.14159265358979f;
static float deg2rad(float d) { return d * PI / 180.f; }
static int iFloor(float x) { return int(std::floor(x)); }

// CRenderDevice::CSecondVPParams::LensMarginPx (copy, see svp_lens_test.cpp)
static unsigned LensMarginPx(unsigned work, float margin)
{
	const float m = std::min(std::max(margin, 0.f), 0.5f);
	unsigned px = unsigned(iFloor(float(work) * m / (1.f + 2.f * m) + .5f));
	if (2 * px > work / 2)
		px = work / 4;
	return px;
}

static void test_steps()
{
	// the Steyr Scout mod: scope_lense_fov 3.66 (x25 of the old 2D view), e.g. max 3.66 * 4 = 14.64, 6 steps
	const float lo = 3.66f, hi = 14.64f;
	const int n = 6;
	CHECK(StepFov(lo, hi, n, 0) == lo);
	CHECK(StepFov(lo, hi, n, n) == hi);
	CHECK(StepFov(lo, hi, n, -3) == lo);
	CHECK(StepFov(lo, hi, n, n + 5) == hi);
	// geometric: every step has the same ratio
	const float r = std::pow(hi / lo, 1.f / float(n));
	for (int s = 0; s < n; ++s)
	{
		const float a = StepFov(lo, hi, n, s), b = StepFov(lo, hi, n, s + 1);
		CHECK(b > a);
		CHECK(std::fabs(b / a - r) < 1e-4f);
	}
	// nearest step of each step's FOV is that step; ends and outside values clamp
	for (int s = 0; s <= n; ++s)
		CHECK(NearestStep(lo, hi, n, StepFov(lo, hi, n, s)) == s);
	CHECK(NearestStep(lo, hi, n, 1.f) == 0);
	CHECK(NearestStep(lo, hi, n, 100.f) == n);
	CHECK(NearestStep(lo, hi, n, lo * std::pow(r, 2.4f)) == 2);
	CHECK(NearestStep(lo, hi, n, lo * std::pow(r, 2.6f)) == 3);
	// disabled / bad input: always the base FOV, step 0
	CHECK(StepFov(lo, 0.f, n, 3) == lo);
	CHECK(StepFov(lo, lo, n, 3) == lo);
	CHECK(StepFov(lo, hi, 0, 3) == lo);
	CHECK(NearestStep(lo, 0.f, n, 10.f) == 0);
	CHECK(NearestStep(0.f, hi, n, 10.f) == 0);
	// one step
	CHECK(StepFov(lo, hi, 1, 0) == lo && StepFov(lo, hi, 1, 1) == hi);
}

static void test_ease()
{
	const float rates[] = {20.f, 30.f, 60.f, 144.f, 300.f};
	float left_at_015[5];
	for (int i = 0; i < 5; ++i)
	{
		const float dt = 1.f / rates[i];
		float cur = 10.f;
		const float target = 4.f;
		float t = 0.f, prev = cur;
		bool snapped = false;
		while (t < 2.f)
		{
			cur = Ease(cur, target, dt);
			t += dt;
			CHECK(cur <= prev + 1e-6f); // monotone toward the target
			CHECK(cur >= target); // no overshoot
			prev = cur;
			if (std::fabs(t - 0.15f) < dt * .5f)
				left_at_015[i] = (cur - target) / (10.f - target);
			if (cur == target) { snapped = true; break; }
		}
		CHECK(snapped);
		CHECK(t < 0.5f); // done (snapped) well within half a second
	}
	// ~5% left after 0.15 s at every frame rate (the sample time is within half a frame of 0.15 s)
	for (int i = 0; i < 5; ++i)
	{
		if (!(left_at_015[i] > 0.02f && left_at_015[i] < 0.10f))
			std::printf("  %.0f fps: %.3f left at 0.15 s\n", rates[i], left_at_015[i]);
		CHECK(left_at_015[i] > 0.02f && left_at_015[i] < 0.10f);
	}
	// the other way, and no time = no change (pause)
	CHECK(Ease(4.f, 10.f, 1.f / 60.f) > 4.f);
	CHECK(Ease(4.f, 10.f, 0.f) == 4.f);
	CHECK(Ease(4.f, 10.f, -1.f) == 4.f);
	CHECK(Ease(9.999f, 10.f, 1.f / 60.f) == 10.f);
	// a long frame lands on (or very near) the target, never past it
	CHECK(std::fabs(Ease(4.f, 10.f, 5.f) - 10.f) < 1e-4f);
}

// svp_reproject + svp_publish with the lens FOV changed since the image was rendered: the lens (dst) shows a square of
// src_size = lens + 2 * grow work pixels around (S/2 + shift). Returns the tangent the lens edge (u = 1) shows,
// relative to the image centre direction, and checks that the source rect is inside the image.
static void test_grow()
{
	const unsigned sizes[] = {256, 384, 512, 1024};
	const float margins[] = {0.05f, 0.15f, 0.3f};
	const float fovs[] = {2.f, 3.66f, 8.f, 15.f, 30.f};
	const float ratios[] = {0.5f, 0.8f, 0.95f, 1.02f, 1.1f, 1.25f, 1.6f, 4.f};
	for (unsigned S : sizes)
		for (float m : margins)
			for (float fov_img : fovs)
				for (float ratio : ratios)
				{
					const unsigned M = LensMarginPx(S, m);
					const unsigned lens = S - 2 * M;
					const float fov_now = fov_img * ratio;
					const int grow = LensGrowPx(lens, M, fov_img, fov_now);
					CHECK(grow <= int(M));
					CHECK(int(lens) + 2 * grow >= 2);
					// the work image covers tan(hat) over S pixels; the lens square of the old image is lens pixels
					const float hat_img = std::tan(deg2rad(fov_img) * .5f) * float(S) / float(lens);
					const float texel = 2.f * hat_img / float(S);
					const float want = std::tan(deg2rad(fov_now) * .5f); // the new lens edge
					const float src_half = (float(lens) + 2.f * float(grow)) * .5f;
					const float shown = src_half / (float(S) * .5f) * hat_img; // the edge the scaled copy shows
					// fits: within one work texel (whole-pixel rect); clamped (zoom out past the margin): as far as it goes
					const bool clamped = grow == int(M) && want > shown + texel;
					if (!clamped)
					{
						if (!(std::fabs(shown - want) <= texel))
							std::printf("  S=%u m=%.2f fov %.2f -> %.2f: edge off by %.2f texels\n", S, m, fov_img,
								fov_now, std::fabs(shown - want) / texel);
						CHECK(std::fabs(shown - want) <= texel);
					}
					else
						CHECK(ratio > 1.f);
					// shift room left (svp_reproject: M - grow) keeps the shifted, grown rect inside the image
					const int room = int(M) - grow;
					CHECK(room >= 0);
					for (int sx = -room; sx <= room; sx += std::max(1, room))
					{
						const int left = int(M) - grow + sx;
						const int right = left + int(lens) + 2 * grow;
						CHECK(left >= 0);
						CHECK(right <= int(S));
					}
					// the same FOV: no grow (the old 1:1 path)
					CHECK(LensGrowPx(lens, M, fov_img, fov_img) == 0);
				}
	// bad input: no grow
	CHECK(LensGrowPx(196, 30, 0.f, 5.f) == 0);
	CHECK(LensGrowPx(196, 30, 5.f, 0.f) == 0);
	CHECK(LensGrowPx(196, 30, 5.f, 179.5f) == 0);
	CHECK(LensGrowPx(1, 30, 5.f, 6.f) == 0);
}

// ---- part 2 ----------------------------------------------------------------------------------------------------------

static std::string read_file(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
	{
		std::printf("FAIL: can't read %s\n", path.c_str());
		++g_fail;
		return std::string();
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

#define HAS(src, text) CHECK((src).find(text) != std::string::npos)

static void part2(const std::string& root)
{
	const std::string actor = read_file(root + "/src/xrGame/Actor.cpp");
	HAS(actor, "hud_params.y = pWeapon->GetSecondVPLensFov();");

	const std::string wpn = read_file(root + "/src/xrGame/Weapon.cpp");
	HAS(wpn, "if (LensZoomApplies()) { LensZoomStep(-1); return; }");
	HAS(wpn, "if (LensZoomApplies()) { LensZoomStep(+1); return; }");
	HAS(wpn, "m_lens_fov_cur = svp_lens_zoom::Ease(m_lens_fov_cur, target, Device.fTimeDelta);");
	HAS(wpn, "return svp_lens_zoom::StepFov(GetSecondVPZoomFactor(), m_zoom_params.m_fSecondVPFovMax, m_zoom_params.m_iSecondVPZoomSteps,");
	HAS(wpn, "svp_lens_zoom::NearestStep(fov_min, fov_max, m_zoom_params.m_iSecondVPZoomSteps, fov_start);");
	HAS(wpn, "if (m_zoom_params.m_bUseDynamicZoom && !LensZoomApplies())");

	const std::string rt = read_file(root + "/src/Layers/xrRenderPC_R1/FStaticRender_RenderTarget.cpp");
	HAS(rt, "grow = svp_lens_zoom::LensGrowPx(svp_w - 2 * svp_lens_margin, svp_lens_margin, svp_lens_fov, fov_now);");
	HAS(rt, "const int M = int(svp_lens_margin) - grow;");
	HAS(rt, "src.left = M - grow + shift_x;");
	HAS(rt, "const u32 src_size = u32(int(lens) + 2 * grow);");

	const std::string cam = read_file(root + "/src/xrEngine/CameraManager.cpp");
	HAS(cam, "Device.fFOV = g_pGamePersistent->m_pGShaderConstants->hud_params.y;");
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";
	test_steps();
	test_ease();
	test_grow();
	part2(root);
	if (g_fail)
	{
		std::printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	std::printf("svp_lens_zoom_test: all checks passed\n");
	return 0;
}
