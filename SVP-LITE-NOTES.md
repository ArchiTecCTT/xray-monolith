# SVP-lite notes (scope "second viewport" speed-ups)

Branches `svp-lite` (safe changes) and `svp-lite-smallrt` (= svp-lite + small render target for R1). Built by `.github/workflows/svp-lite.yml` for DX8 (R1) and DX8-AVX only.
Upstream `msbuild.yml` (which publishes releases) was deleted on this branch on purpose.

The scope second viewport (SVP) renders a whole extra frame every N-th frame. These changes make
that extra frame cheaper. Nothing changes while no scope SVP is active.

## How to set a cvar

* In the console (`~`): `r__svp_delay 4` (type the name alone to see the current value / help).
* Permanently: add the line `r__svp_delay 4` to `user.ltx` (in the game's `appdata` folder). Values from
  `user.ltx` are applied at startup.

## Cvars

| cvar | range | default | what it does |
|---|---|---|---|
| `r__svp_delay` | 2..8 (integer) | **3** | The SVP frame is rendered every N-th frame. 2 = old behaviour (every 2nd frame). Higher = fewer extra frames (faster) but a staler scope image. Applies immediately. |
| `r__svp_far` | 0..5000 (metres, float) | **0** (off) | On SVP frames the far plane is cut to `min(normal far, r__svp_far)`. Less geometry, portals and HOM work for the scope image. Anything beyond is not drawn in the lens (fog is not tied to it, so a hard cut can be visible). Values smaller than the camera near plane are ignored. Try 300-500. Works in every renderer. |
| `r1_svp_size` | 0, 64..2048 (pixels) | **512** | *`svp-lite-smallrt` branch, R1 only.* The scope SVP frame is rendered into a small square offscreen target (size x size) instead of the full-screen backbuffer. 0 = off = exactly the `svp-lite` behaviour. **Needs `vid_restart` (or a game restart) to take effect** because the targets are created with the renderer. If the target or its depth buffer cannot be created the engine logs `! SVP-lite: can't create ...` and silently uses the old full-size path. Try 384 / 512 / 768 / 1024: bigger = sharper lens, slower. |
| `r__svp_skip_extras` | 0 / 1 | **1** | R1 only. On SVP frames skip the attachment/HUD/camera-attached 3D UI passes (grass is NOT skipped in this multithread build, see below), `HUD().RenderUI()` and the reshade call (SVP frames are never presented, so they were wasted). 0 = old behaviour. |

## Other changes

* `CSecondVPParams::SetSVPFrameDelay` (device.h): the old `clamp<u8>(...)` call was actually fine (`clamp` takes a
  reference and modifies in place); it is rewritten as an explicit "minimum 2" for clarity. No behaviour change.
* R1 `CRender::RenderToTarget(rtSVP)`: copies the backbuffer into `$user$viewport2` with GPU `StretchRect`
  (both are same-format render-target surfaces, default pool, no multisampling). If `StretchRect` fails the code falls
  back to the old `D3DXLoadSurfaceFromSurface` copy. The PDA copy (`rtPDA`) is untouched.

## Risk of each change

* `r__svp_delay`: low. Only a staler lens image at higher values. R2/R4 HDR flicker hack assumes "frame - 1"
  (see report), so re-test HDR in R2/R4 if you raise it there.
* `r__svp_far`: low, off by default. Visible far cut-off in the lens when on.
* `r__svp_skip_extras`: low-medium. Weapon/attachment UI is missing from the scope image only. The queued
  attachment-UI list is still cleared on skipped frames. Multithread build only: grass is deliberately not skipped on SVP frames, because the detail worker thread's frame gating would otherwise build grass visibility from the scope's narrow frustum and cull grass on the next normal frame. The LOD size (`g_fSCREEN`) also stays tied to the full-size target for the same reason (a worker thread reads `r_ssaDISCARD`).
  Reshade / `HUD().RenderUI()` run less often (never on SVP frames); effects tied to a call on each frame would
  update 1 frame in N later.
* `StretchRect` copy: low (automatic fallback). If the lens image looked wrong/black this is the first thing to suspect.

## Deliberately NOT done

* Skipping `Wallmarks->Render()` on SVP frames: its render pass is also what clears `mapWmark` and the per-frame
  skeleton wallmark list, so skipping it would accumulate stale entries (dangling visuals / doubled decals).
* Skipping the PDA block in `CLevel::OnRender`: no gain when no PDA is shown, and it has cursor side effects.

## Small render target (`svp-lite-smallrt` branch, R1 only)

What happens on an SVP frame when `r1_svp_size` > 0:

* `CRenderTarget::Begin()` renders into a scratch target (`$user$viewport2_work`, size x size) with its own depth
  buffer (same size, same format as the main one, no multisampling); viewport / `curWidth` / `curHeight` are set to
  that size, so LOD scale (`g_fSCREEN`) is computed from the small size too (it used to lag one frame behind).
* `CRenderTarget::End()` skips PP-UI, distortion and the post-process quad (so no supersample / noise / colour grading
  in the lens), still empties `mapDistort` / `mapHUDDistort`, restores the backbuffer + depth + viewport, and copies the
  scratch target into `$user$viewport2` (same-size GPU `StretchRect`, D3DX fallback). A scratch target is used so the
  lens shader never samples a texture that is bound as the render target.
* `CRender::RenderToTarget(rtSVP)` returns immediately (the full-screen backbuffer copy is gone for R1). The PDA copy is unchanged.
* The image is square: the lens image is squashed to size x size while the projection aspect stays the screen aspect.
  Shaders that sample `$user$viewport2` with normalised screen UVs (the 3dss shaders) un-squash it automatically.
  Only the centre of the image is used by the lens, so texel density is lower than the full-size image: raise
  `r1_svp_size` if the lens looks blurry.
* Bullet tracers and `Game().OnRender()` draw after the render pass on the backbuffer, so they are no longer in the lens image.
* Supersampling / post-process (`Perform()`) is untouched for normal frames. If the previous normal frame had distortion
  the "had distortion" marker is carried over the SVP frame so the next frame keeps using the offscreen path (no flicker).

Reviewed and left alone (all cosmetic, sub-texel): the half-texel offsets from `Device.dwWidth/dwHeight` in
`cl_texgen` / `cl_VPtexgen` / particle `ApplyTexgen`, the `screen_res` shader constant, post-process UVs (skipped on SVP frames).

Risks: medium. The R1 render-target switching is the new part: look for a black or stuck lens image, a corrupted or
shifted main image right after an SVP frame, or flickering distortion. `r1_svp_size 0` + `vid_restart` returns to the `svp-lite` behaviour.

## Lens fit and reprojection (`r1-svp-lens` branch, R1 only)

Builds on the small render target above (`r1_svp_size` > 0). Nothing here runs while no scope SVP is active, in
R2/R3/R4, or with `r1_svp_size 0`. Each part has its own switch.

### Cvars

| cvar | range | default | what it does |
|---|---|---|---|
| `r1_svp_fit` | 0 / 1 | **1** | The SVP frame renders only the square the lens shows (plus the margin) into the `r1_svp_size` work target, with a square frustum of the same vertical FOV. The lens square is copied 1:1 into `$user$viewport2`. Applies immediately, but the shape of `$user$viewport2` is chosen at `vid_restart` (see below). 0 = the image of the `svp-lite-smallrt` path. |
| `r1_svp_margin` | 0..0.5 | **0.15** | Extra view rendered around the lens square, as a fraction of the lens width on each side. Room for `r1_svp_reproject` to shift into. Costs lens sharpness: the lens square is `size / (1 + 2*margin)` pixels (256 -> 196 at 0.15). 0 turns reprojection off. |
| `r1_svp_reproject` | 0 / 1 | **1** | On every normal frame the last SVP image is shifted by the aim's yaw/pitch change since it was rendered, so the lens follows the aim at the full frame rate while the world is only rendered every `r__svp_delay` frames. Needs `r1_svp_fit 1` and a margin > 0. |
| `r1_svp_stats` | 0 / 1 | **0** | Every ~2 s writes a line to the log/console: shown fps, normal vs SVP frames (count, average frame time, scene CPU time, draw calls, triangles), SVP frames per second, forced SVP frames per second, lens shifts per second. |

### How the lens reads the image (unchanged shaders)

Vanilla `model_scope_lense.ps` and the Boomsticks-and-Sharpsticks `models_scope_nv_1..3.ps` sample `$user$viewport2` at
`tc.x = (tc.x - 0.5) / (screen_res.x / screen_res.y) + 0.5`, `tc.y` unchanged: the lens shows all rows and the middle
`h/w` of the columns (`screen_res` = the screen, not the target). With the old projection (`fAspect = h/w`) that is the
angular square `|x/z|, |y/z| <= tan(fov/2)`; the sides of the image (44 % at 16:9) were rendered and never seen.

### What `r1_svp_fit` does

* `CCameraManager::ApplyDevice` (SVP frame, R1 with the small target): `build_projection_HAT(hat, aspect)` with
  `lens = size - 2*margin_px`, `hat = tan(fov/2) * size / lens` and `aspect = fAspect * w/h` (= 1 normally). The FOV is
  vertical (`Fmatrix::build_projection`: `_22 = cot`, `_11 = aspect * cot`), so the lens square is exactly the middle
  `lens x lens` pixels of the work image and the whole image covers the square plus `margin_px` on each side.
  `Device.fFOV` / `fASPECT` keep their old SVP values, so LOD selection and shader constants do not change.
* `$user$viewport2` is created `round(lens * w/h) x lens` (16:9, size 256, margin 0.15: 348 x 196) so the columns the lens
  reads are exactly the lens square: the square is copied 1:1 (plus a guard column on each side) into the middle, the
  side columns are left black and are never read by the lens shaders.
* Mapping check: `tests/svp_lens_test.cpp` (`g++ -std=c++17 tests/svp_lens_test.cpp -o t && ./t .`) models the shader,
  the old path and the new path and checks that every lens UV shows the same direction as before within one work texel
  (several screens, sizes, margins and FOVs), that the reprojected lens centre is the new aim, and the forced-frame schedule.
* If `r1_svp_fit` is switched at runtime without `vid_restart`, the copy is scaled (linear `StretchRect`) into whatever
  shape `$user$viewport2` has, so the lens still lines up; only the resolution is not the best one until `vid_restart`.
* Multithread caveats (see above) hold: grass still renders on SVP frames, so the detail worker's gating does not
  build the next normal frame's grass from the SVP frustum; `g_fSCREEN` is not touched. The SVP frustum is narrower and
  taller than before (still the zoomed one); the next normal frame rebuilds its own projection in `ApplyDevice`.

### What `r1_svp_reproject` does

* At the start of each normal frame (`CRenderTarget::Begin`) the current camera direction is put into the view space of
  the last SVP frame; its position in that image (`x/z * aspect / hat`, `y/z / hat`, times half the size) is the shift
  in whole work pixels. The lens square is copied from the work image at that offset (only when the shift changed).
* A fresh SVP frame is requested for the next frame (`CSecondVPParams::RequestSVPFrame`) when the shift would leave the
  margin (it is clamped to the edge for this frame), when the lens FOV (`hud_params.y`) changed, or when the last image
  is older than `2 * r__svp_delay + 2` frames (scope raised again). A regular SVP frame that falls within
  `r__svp_delay / 2` frames after a forced one is skipped. `IsSVPFrame()` gives the same answer all through a frame.
* What it cannot fix: things that move (NPCs, foliage in wind, particles) still update at the SVP rate; moving the
  camera (walking, strafing) is not compensated, so close objects show a small parallax error until the next SVP frame;
  leaning (roll) is not compensated; the shift is in whole work pixels (steps of a few screen pixels on a big lens).
  Fast panning past the margin renders extra (forced) SVP frames: at worst every second frame, like `r__svp_delay 2`.

### What to test in game

1. Lens alignment vs today: `r1_svp_fit 0` + `vid_restart`, aim at a far edge (antenna, tree line) with the reticle on it,
   screenshot; `r1_svp_fit 1` + `vid_restart`, same spot. The picture should be the same direction and zoom, sharper
   left-right. Also check a night scope (BaS `models_scope_nv_*`).
2. Panning lag: pan slowly and quickly with `r1_svp_reproject 1` vs `0`. With 1 the lens should track the aim every
   frame; a brief edge smear/jump when panning fast is the margin running out (raise `r1_svp_margin` to 0.2-0.25 if
   it happens often; raising it lowers sharpness, raise `r1_svp_size` to compensate).
3. FPS: `r1_svp_stats 1`, aim through a scope at a busy view for ~10 s, compare `SVP ... ms / calls` and `shown fps`
   for `r1_svp_fit 0/1` (needs `vid_restart` between) and for `r__svp_delay 4` vs higher values with reprojection on
   (with reprojection, a higher delay costs less in panning lag; moving targets still update at the SVP rate).
4. The usual SVP-lite checks: black/stuck lens, a corrupted main image right after an SVP frame, flicker.

### Risks and how to switch each part off

* `r1_svp_fit`: medium. New projection on SVP frames and a differently shaped `$user$viewport2`. A wrong-looking lens
  (offset, stretched, black edge): `r1_svp_fit 0` (+ `vid_restart` to get the old square viewport2 back). Mods that read
  `$user$viewport2` outside the lens square (e.g. 3D-shader-scope packs that sample it with screen UVs far from the
  middle) see black there with fit on: use `r1_svp_fit 0` with them.
* `r1_svp_margin`: low. Only sharpness vs room to shift.
* `r1_svp_reproject`: low-medium. Extra small GPU copies per normal frame and extra (forced) SVP frames while panning.
  `r1_svp_reproject 0` returns to "update every N frames". If forced frames make panning hitchy, raise the margin.
* `r1_svp_stats`: none (off by default, a log line every 2 s when on).
* Everything: `r1_svp_size 0` + `vid_restart` is the plain full-size path as before.

### Possible next step (not done)

For low zoom (~1.5-2x) skip the SVP render entirely and magnify the middle of the main frame into the lens instead
(no second scene at all; less detail than a real zoomed render, and the main frame's post-process/HUD would need care).
