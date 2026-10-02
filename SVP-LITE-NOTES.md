# SVP-lite notes (scope "second viewport" speed-ups)

Branch `svp-lite`. Built by `.github/workflows/svp-lite.yml` for DX8 (R1) and DX8-AVX only.
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
