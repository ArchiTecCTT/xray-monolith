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
| `r__svp_skip_extras` | 0 / 1 | **1** | R1 only. On SVP frames skip grass (details), the attachment/HUD/camera-attached 3D UI passes, `HUD().RenderUI()` and the reshade call (SVP frames are never presented, so they were wasted). 0 = old behaviour. |

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
* `r__svp_skip_extras`: low-medium. Grass and weapon/attachment UI are missing from the scope image only. The queued
  attachment-UI list is still cleared on skipped frames. Grass cache updates are skipped for one frame after an SVP frame.
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
