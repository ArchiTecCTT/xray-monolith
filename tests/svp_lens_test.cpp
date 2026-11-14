// Standalone check of the R1 scope lens fit and reprojection math (r1_svp_fit / r1_svp_margin / r1_svp_reproject,
// see SVP-LITE-NOTES.md "Lens fit and reprojection"); not an engine build.
//
// Part 1 models, in plain C++ copies of the engine formulas:
//   * the lens shaders (vanilla model_scope_lense.ps, Boomsticks-and-Sharpsticks models_scope_nv_1..3.ps):
//       tc.x = (tc.x - 0.5) / (screen_res.x / screen_res.y) + 0.5;  t = tex2D(s_vp2, tc)   ($user$viewport2)
//   * the old small-target path: square work image with the screen-aspect projection, copied 1:1 to viewport2;
//   * the new path: the lens-fitted projection (CCameraManager::ApplyDevice), the margin (LensMarginPx), the
//     lens-shaped viewport2 (CRenderTarget::Create) and its lens rect (svp_lens_rect), the shifted copy (svp_publish).
//   For lens UVs it checks that the new image shows the same direction as the old one (same direction, same zoom),
//   that a turned camera puts the lens centre on the new aim after svp_reproject's shift, and the forced SVP frame
//   schedule of CSecondVPParams::IsSVPFrame / RequestSVPFrame.
// Part 2 reads the engine sources and checks that the formulas modelled here are the ones written there.
//
//   g++ -std=c++17 -g -Wall -fsanitize=address,undefined tests/svp_lens_test.cpp -o /tmp/svp_lens_test && /tmp/svp_lens_test .
// The argument is the repository root (default ".").
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const float PI = 3.14159265358979f;
static float deg2rad(float d) { return d * PI / 180.f; }
static int iFloor(float x) { return int(std::floor(x)); }

// ---- copies of the engine formulas ---------------------------------------------------------------------------------

// CRenderDevice::CSecondVPParams::LensMarginPx
static unsigned LensMarginPx(unsigned work, float margin)
{
	const float m = std::min(std::max(margin, 0.f), 0.5f);
	unsigned px = unsigned(iFloor(float(work) * m / (1.f + 2.f * m) + .5f));
	if (2 * px > work / 2)
		px = work / 4;
	return px;
}

struct Proj // Fmatrix::build_projection_HAT: x_ndc = aspect * cot * x / z, y_ndc = cot * y / z
{
	float hat, aspect;
	float ndc_x(float tx) const { return aspect / hat * tx; }
	float ndc_y(float ty) const { return ty / hat; }
	float tan_x(float nx) const { return nx * hat / aspect; }
	float tan_y(float ny) const { return ny * hat; }
};

struct Layout // render target sizes (CRenderTarget::Create)
{
	unsigned S; // work target
	unsigned vp2_w, vp2_h, vp2_lens;
};

static Layout make_layout(unsigned S, bool fit, float margin, unsigned W, unsigned H)
{
	Layout l = {S, S, S, 0};
	if (fit && W > H)
	{
		const unsigned lens = S - 2 * LensMarginPx(S, margin);
		const float wide = float(lens) * float(W) / float(H);
		const unsigned w = lens + 2 * unsigned(iFloor((wide - float(lens)) * .5f + .5f));
		if (w <= 4096)
		{
			l.vp2_w = w;
			l.vp2_h = lens;
			l.vp2_lens = lens;
		}
	}
	return l;
}

struct Rect { int left, top, right, bottom; };

// CRenderTarget::svp_lens_rect
static Rect lens_rect(const Layout& l, unsigned lens, unsigned W, unsigned H)
{
	const unsigned VW = l.vp2_w, VH = l.vp2_h;
	if (l.vp2_lens && lens == l.vp2_lens)
		return Rect{int((VW - lens) / 2), 0, int((VW - lens) / 2 + lens), int(lens)};
	unsigned w = unsigned(std::min(std::max(iFloor(float(VW) * float(H) / float(W) + .5f), 1), int(VW)));
	if ((VW - w) & 1)
		w += (w < VW) ? 1 : unsigned(-1);
	return Rect{int((VW - w) / 2), 0, int((VW - w) / 2 + w), int(VH)};
}

// the lens shader: lens mesh UV -> viewport2 UV
static void lens_uv(float u, float v, unsigned W, unsigned H, float& tu, float& tv)
{
	tu = (u - 0.5f) / (float(W) / float(H)) + 0.5f;
	tv = v;
}

// ---- part 1 ----------------------------------------------------------------------------------------------------------

// the direction (x/z, y/z in camera space) the old path shows at lens UV (u, v)
static void old_dir(float u, float v, float fov, unsigned W, unsigned H, float& tx, float& ty)
{
	const Proj p = {std::tan(deg2rad(fov) * .5f), float(H) / float(W)}; // fAspect = h/w
	float tu, tv;
	lens_uv(u, v, W, H, tu, tv);
	// viewport2 = the work image (texture UV 0..1 <-> ndc -1..1, v down)
	tx = p.tan_x(tu * 2.f - 1.f);
	ty = p.tan_y(1.f - tv * 2.f);
}

// the direction the new path shows at lens UV (u, v): viewport2 texel -> work texel -> fitted projection
static bool new_dir(float u, float v, float fov, unsigned W, unsigned H, unsigned S, float margin, bool vp2_fit,
	int sx, int sy, const Proj& work_proj, float& tx, float& ty)
{
	const Layout l = make_layout(S, vp2_fit, margin, W, H);
	const unsigned M = LensMarginPx(S, margin);
	const unsigned lens = S - 2 * M;
	const Rect dst = lens_rect(l, lens, W, H);
	float tu, tv;
	lens_uv(u, v, W, H, tu, tv);
	const float px = tu * float(l.vp2_w), py = tv * float(l.vp2_h); // continuous texel coordinates in viewport2
	// svp_publish copies one guard column on each side of a 1:1 lens square (when the shift leaves room): the lens
	// edge may read up to half a texel past the square
	const bool same_size = unsigned(dst.right - dst.left) == lens;
	const bool guard = same_size && dst.left >= 1 && unsigned(dst.right) + 1 <= l.vp2_w && int(M) + sx >= 1 &&
		int(M) + sx + int(lens) + 1 <= int(S);
	// without a guard column (r1_svp_margin 0) the rounding of viewport2's width leaves < half a texel at the rim
	const float slack = guard ? 1.f : (same_size ? .5f : 1e-3f);
	if (px < dst.left - slack || px > dst.right + slack || py < dst.top - 1e-3f || py > dst.bottom + 1e-3f)
		return false;
	// StretchRect maps dst rect onto src rect linearly (1:1 when the sizes match)
	const float wx = float(M + sx) + (px - dst.left) * float(lens) / float(dst.right - dst.left);
	const float wy = float(M + sy) + (py - dst.top) * float(lens) / float(dst.bottom - dst.top);
	tx = work_proj.tan_x(wx / float(S) * 2.f - 1.f);
	ty = work_proj.tan_y(1.f - wy / float(S) * 2.f);
	(void)fov;
	return true;
}

// CCameraManager::ApplyDevice, lens-fitted branch
static Proj fit_proj(float fov, unsigned S, float margin, unsigned W, unsigned H, float fAspect)
{
	const unsigned M = LensMarginPx(S, margin);
	const unsigned lens = S - 2 * M;
	const float hat = std::tan(deg2rad(fov) * .5f) * float(S) / float(lens);
	const float aspect = fAspect * (float(W) * .5f / (float(H) * .5f));
	return Proj{hat, aspect};
}

struct V3 { float x, y, z; };
static V3 dir_hp(float h, float p) { return V3{std::sin(h) * std::cos(p), std::sin(p), std::cos(h) * std::cos(p)}; }
// camera basis for heading h, pitch p (no roll): view-space coordinates of a world direction d
static V3 to_view(float h, float p, V3 d)
{
	const V3 z = dir_hp(h, p);
	const V3 x = V3{std::cos(h), 0.f, -std::sin(h)}; // right
	const V3 y = V3{z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x}; // up = z cross x
	return V3{d.x * x.x + d.y * x.y + d.z * x.z, d.x * y.x + d.y * y.y + d.z * y.z, d.x * z.x + d.y * z.y + d.z * z.z};
}

// svp_reproject: work-image shift that puts the lens centre on the current aim
static bool reproject_shift(const Proj& pr, unsigned S, unsigned M, V3 v, int& sx, int& sy)
{
	const float half = float(S) * .5f;
	const float fx = (v.x / v.z) * pr.aspect / pr.hat * half;
	const float fy = -(v.y / v.z) / pr.hat * half;
	const float lim = float(M) + 1.f;
	sx = iFloor(std::min(std::max(fx, -lim), lim) + .5f);
	sy = iFloor(std::min(std::max(fy, -lim), lim) + .5f);
	const bool out = sx < -int(M) || sx > int(M) || sy < -int(M) || sy > int(M);
	sx = std::min(std::max(sx, -int(M)), int(M));
	sy = std::min(std::max(sy, -int(M)), int(M));
	return out;
}

// CSecondVPParams::IsSVPFrame / RequestSVPFrame
struct Sched
{
	unsigned delay = 4, forceFrame = 0, prevForceFrame = 0, frame = 0;
	bool active = true;
	bool is_svp() const
	{
		if (!active) return false;
		const unsigned f = frame;
		if (forceFrame == 0) return f % delay == 0;
		if (f == forceFrame) return true;
		if (f % delay != 0) return false;
		const unsigned last_forced = (forceFrame <= f) ? forceFrame : prevForceFrame;
		return !(last_forced && last_forced < f && f - last_forced <= unsigned(delay / 2));
	}
	void request()
	{
		const unsigned next = frame + 1;
		if (forceFrame == next) return;
		prevForceFrame = forceFrame;
		forceFrame = next;
	}
};

static void part1()
{
	const unsigned screens[][2] = {{1920, 1080}, {1366, 768}, {2560, 1080}, {1280, 1024}, {1600, 900}};
	const unsigned sizes[] = {256, 384, 512, 1024};
	const float margins[] = {0.f, 0.05f, 0.15f, 0.3f};
	const float fovs[] = {3.f, 8.f, 15.f, 30.f};

	for (const auto& sc : screens)
		for (unsigned S : sizes)
			for (float m : margins)
				for (float fov : fovs)
					for (int vp2_fit = 0; vp2_fit < 2; ++vp2_fit)
					{
						const unsigned W = sc[0], H = sc[1];
						const Proj wp = fit_proj(fov, S, m, W, H, float(H) / float(W));
						const unsigned M = LensMarginPx(S, m);
						const unsigned lens = S - 2 * M;
						CHECK(lens >= S / 2);
						CHECK((S - lens) % 2 == 0);
						const Layout l = make_layout(S, vp2_fit != 0, m, W, H);
						if (l.vp2_lens)
						{
							CHECK(l.vp2_h == lens);
							CHECK((l.vp2_w - lens) % 2 == 0);
							// the lens shader reads vp2_w * h/w columns as the lens square: within a pixel of 'lens'
							CHECK(std::fabs(float(l.vp2_w) * float(H) / float(W) - float(lens)) < 1.f);
						}
						// one work texel, as a tangent: the tolerance of the copy (integer rects)
						const float texel = 2.f * wp.hat / float(S);
						float worst = 0.f;
						for (int iu = 0; iu <= 20; ++iu)
							for (int iv = 0; iv <= 20; ++iv)
							{
								const float u = iu / 20.f, v = iv / 20.f;
								float ox, oy, nx, ny;
								old_dir(u, v, fov, W, H, ox, oy);
								const bool in = new_dir(u, v, fov, W, H, S, m, vp2_fit != 0, 0, 0, wp, nx, ny);
								CHECK(in);
								if (!in) continue;
								worst = std::max(worst, std::max(std::fabs(ox - nx), std::fabs(oy - ny)));
							}
						if (!(worst <= texel))
							std::printf("  %ux%u S=%u m=%.2f fov=%.0f vp2_fit=%d: worst %.3g texels\n", W, H, S, m, fov,
								vp2_fit, worst / texel);
						CHECK(worst <= texel);

						// the square the lens shows in the old path: |x/z|, |y/z| <= tan(fov/2): same in the new one
						float ox, oy;
						old_dir(1.f, 0.f, fov, W, H, ox, oy);
						CHECK(std::fabs(ox - std::tan(deg2rad(fov) * .5f)) < 1e-4f);
						CHECK(std::fabs(oy - std::tan(deg2rad(fov) * .5f)) < 1e-4f);

						// reprojection: turn the camera, shift, the lens centre shows the new aim
						if (M > 0)
						{
							const float h0 = 0.3f, p0 = -0.1f;
							const float steps[][2] = {{0.2f, 0.f}, {-0.35f, 0.1f}, {0.f, -0.4f}, {0.05f, 0.05f}};
							for (const auto& st : steps)
							{
								const float half_t = std::tan(deg2rad(fov) * .5f);
								const float dh = std::atan(st[0] * half_t), dp = std::atan(st[1] * half_t);
								const V3 aim = dir_hp(h0 + dh, p0 + dp);
								const V3 v = to_view(h0, p0, aim);
								int sx, sy;
								const bool out = reproject_shift(wp, S, M, v, sx, sy);
								// the shift is limited by the margin: 0.2 of the half lens fits a 0.15 margin? m * lens per side
								float cx, cy;
								CHECK(new_dir(.5f, .5f, fov, W, H, S, m, vp2_fit != 0, sx, sy, wp, cx, cy));
								const float ex = v.x / v.z, ey = v.y / v.z;
								if (!out)
								{
									// within one work texel (rounding to whole pixels + the lens-rect rounding)
									CHECK(std::fabs(cx - ex) <= texel);
									CHECK(std::fabs(cy - ey) <= texel);
								}
								else
								{
									// clamped: the image moved as far as it can, in the right direction
									CHECK(std::abs(sx) == int(M) || std::abs(sy) == int(M));
									CHECK((ex > 0) == (cx > -1e-6f) || std::fabs(ex) < texel);
									CHECK((ey > 0) == (cy > -1e-6f) || std::fabs(ey) < texel);
								}
								// turning right moves the source rect right (the world moves left in the lens)
								if (ex > texel) CHECK(sx > 0);
								if (ey > texel) CHECK(sy < 0);
							}
						}
					}

	// schedule: no forcing = the old modulo schedule
	{
		Sched s;
		for (s.frame = 1; s.frame < 100; ++s.frame)
			CHECK(s.is_svp() == (s.frame % s.delay == 0));
	}
	// a request during frame f makes f+1 an SVP frame, the answer for f does not change, and a regular frame right
	// after a forced one is skipped
	{
		Sched s;
		unsigned svp_count = 0;
		for (s.frame = 1; s.frame < 400; ++s.frame)
		{
			const bool before = s.is_svp();
			if (!before && (s.frame % 7 == 3 || s.frame % 11 == 5))
			{
				s.request();
				s.request(); // twice in one frame is harmless
				CHECK(s.is_svp() == before);
				CHECK(s.forceFrame == s.frame + 1);
			}
			if (before) ++svp_count;
			if (s.frame > 1 && s.forceFrame == s.frame)
				CHECK(s.is_svp());
			// a regular frame 1..delay/2 after a forced frame is not rendered
			if (s.frame % s.delay == 0)
			{
				const unsigned lf = (s.forceFrame <= s.frame) ? s.forceFrame : s.prevForceFrame;
				if (lf && lf < s.frame && s.frame - lf <= s.delay / 2)
					CHECK(!s.is_svp());
			}
		}
		CHECK(svp_count > 0);
		// inactive: never
		s.active = false;
		for (s.frame = 1; s.frame < 50; ++s.frame)
			CHECK(!s.is_svp());
	}
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
	const std::string dev = read_file(root + "/src/xrEngine/device.cpp");
	HAS(dev, "u32 px = u32(iFloor(float(work) * m / (1.f + 2.f * m) + .5f));");
	HAS(dev, "if (2 * px > work / 2)");
	HAS(dev, "const u32 last_forced = (forceFrame <= f) ? forceFrame : prevForceFrame;");
	HAS(dev, "return !(last_forced && last_forced < f && f - last_forced <= u32(frameDelay / 2));");
	HAS(dev, "if (forceFrame == 0) // nothing was ever forced: the plain schedule\n\t\treturn f % frameDelay == 0;");

	const std::string cam = read_file(root + "/src/xrEngine/CameraManager.cpp");
	HAS(cam, "const float hat = tanf(deg2rad(Device.fFOV) * .5f) * float(work) / float(lens);");
	HAS(cam, "const float aspect = m_cam_info.fAspect * (Device.fWidth_2 / Device.fHeight_2);");
	HAS(cam, "Device.mProject.build_projection_HAT(hat, aspect, _viewport_near, proj_far);");
	HAS(cam, "if (!svp_lens_fit)\n\t\tDevice.mProject.build_projection(deg2rad(Device.fFOV), m_cam_info.fAspect, _viewport_near, proj_far);");

	const std::string rt = read_file(root + "/src/Layers/xrRenderPC_R1/FStaticRender_RenderTarget.cpp");
	HAS(rt, "const u32 w = lens + 2 * u32(iFloor((wide - float(lens)) * .5f + .5f));");
	HAS(rt, "const float wide = float(lens) * float(Device.dwWidth) / float(Device.dwHeight);");
	HAS(rt, "r.left = LONG((W - lens) / 2);");
	HAS(rt, "u32 w = u32(clampr(iFloor(float(W) * float(Device.dwHeight) / float(Device.dwWidth) + .5f), 1, int(W)));");
	HAS(rt, "src.left = M - grow + shift_x;"); // grow = 0 unless the lens FOV changed (svp_lens_zoom_test.cpp)
	HAS(rt, "src.top = M - grow + shift_y;");
	HAS(rt, "if (same_size && dst.left >= 1 && u32(dst.right) + 1 <= svp_vp2_w && src.left >= 1 && u32(src.right) + 1 <= svp_w)");
	HAS(rt, "const float fx = (v.x / v.z) * svp_lens_aspect / svp_lens_hat * half; // + = right");
	HAS(rt, "const float fy = -(v.y / v.z) / svp_lens_hat * half; // + = down (pixel rows)");
	HAS(rt, "svp_lens_view.transform_dir(v, Device.vCameraDirection);");

	// Fmatrix::build_projection_HAT: _11 = aspect * cot, _22 = cot (the model's Proj)
	const std::string mat = read_file(root + "/src/xrCore/_matrix.h");
	HAS(mat, "T w = fAspect * cot;");
	HAS(mat, "T h = T(1) * cot;");
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : ".";
	part1();
	part2(root);
	if (g_fail)
	{
		std::printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	std::printf("svp_lens_test: all checks passed\n");
	return 0;
}
