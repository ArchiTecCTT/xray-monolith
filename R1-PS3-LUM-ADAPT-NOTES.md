# R1: shader model 3 and luminance adaptation

Branch `r1-ps3-lum-adapt` (based on `svp-lite-mt-parts-zero-texfix`). Two engine changes for the R1 renderer
(`renderer_r1`, `AnomalyDX8AVX.exe`, D3D9 underneath). Both do nothing for vanilla shaders: a shader that does not
ask for them compiles and draws exactly as before.

Ready-to-copy shader files: `examples/r1-ps3-lum-adapt/shaders/r1/` (they live outside `gamedata/` on purpose, so
nothing in this repository overrides a vanilla file).

---

## 1. ps_3_0 / vs_3_0 on R1

### Entry names

The engine picks the shader target from the entry-point name it finds in the file (unchanged rule, two new names):

| entry in the file | target |
|---|---|
| `main_ps_1_1` .. `main_ps_1_4`, `main_ps_2_0` | as before |
| plain `main` (pixel shader) | `ps_2_0`, as before |
| **`main_ps_3_0`** | **`ps_3_0`** |
| `main_vs_1_1`, `main_vs_2_0`, plain `main` (vertex shader) | as before |
| **`main_vs_3_0`** | **`vs_3_0`** |

`main_ps_3_0` / `main_vs_3_0` win over any other entry in the same file, but only when the GPU reports both
vs 3.0 and ps 3.0 (D3D9 only runs a vs_3_0 together with a ps_3_0). Otherwise the engine logs
`~ name.ps: GPU has no vs_3_0/ps_3_0, using entry ...` and falls back to the entry it would have picked before
(`main_ps_2_0`, then `main_ps_1_x`, then plain `main`). A file with only `main_ps_3_0` fails to compile on such a GPU,
so offer a `main_ps_2_0` (and `main_vs_2_0` in the vertex shader) if you care about pre-2006 hardware.

The compile still goes through `D3DXCompileShader` with `DEBUG | PACKMATRIX_ROWMAJOR | USE_LEGACY_D3DX9_31_DLL`.
The shader cache (`appdata\shaders_cache\r1\`) keys sm3 builds on the target as well; every other shader keeps its
old cache key, so nothing is recompiled for the new exe.

### Post-process passes need a vertex shader

D3D9 does not run a ps_3_0 with fixed-function vertex processing or with pre-transformed (XYZRHW) vertices, and the
vanilla R1 post-process `.s` files use vertex shader `"null"` with an XYZRHW quad. The engine now looks at the pass:

* vertex shader `"null"` (vanilla): the XYZRHW quad, exactly as before;
* any named vertex shader: the **same quad in clip space** (`POSITION`, w = 1) with the same `COLOR0`, `COLOR1`,
  `TEXCOORD0..2`, placed so it lands on exactly the same pixels (D3D9 half-pixel rule included).

So a mod's post-process needs three things:

1. `shaders\r1\postprocess_vs3.vs`: a pass-through vertex shader (copy it from the examples folder). It must output
   the clip position it receives unchanged.
2. In `postprocess.s` **and** `postprocess_d.s` (and `postprocess_cm.s` / `postprocess_dcm.s` if you override those):
   `shader:begin("postprocess_vs3", "postprocess")` instead of `shader:begin("null", "postprocess")`.
   Keep `:fog(false)` and `:zb(false,false)`; keep your samplers.
3. In `postprocess.ps` / `postprocess_d.ps`: the entry `main_ps_3_0` (same `v2p` input struct as before:
   `COLOR0`, `COLOR1`, `TEXCOORD0..2`; no `POSITION` input).

To ship one set of files that uses this on new exes and stays vanilla on older exes / older GPUs, branch the `.s` on
the global `r1_sm3_postprocess` and keep `main_ps_3_0` inside `#ifdef R1_SM3` (branch `r1-sm3-detect`; see
`R1-SM3-DETECT-NOTES.md`).

Pitfalls:

* A `.s` that names a vertex shader always gets the clip-space quad, whatever its target. A vertex shader that
  transforms the position (like the world shaders do) draws garbage here; use the pass-through.
* Do not mix: a `vs_3_0` with a `ps_2_0` (or the other way round) fails at draw time on D3D9. With the two files above
  and the GPU check, the engine always picks a matching pair.
* `postprocess_cm` uses its `l_special` element for the colour-map pass; the rule above applies to it per element.

---

## 2. Luminance adaptation (`s_lum_adapt`)

Eye-like auto-gain: the engine meters the scene, averages it to one pixel and eases a 1x1 history towards it every
frame (`k = 1 - exp(-dt * speed)`, separate speeds for a brighter and a darker scene). Post-process shaders read the
result through a sampler.

### When does it run?

Only when it is switched on **and** the R1 post-process quad is drawn that frame (the only pass that can read it;
R1's `if (!bPerform) return;` is untouched). Switched off it creates no render target, compiles no shader and draws
nothing. On: four tiny passes (64x64, 8x8, 1x1, 1x1; about 70k texture reads in total), well below a millisecond even
on an iGPU.

The first frame after it was off (or after the post-process quad was not drawn for more than 3 frames) starts over
at the metered value instead of easing from a stale one. Scope frames of a full-size second viewport are not metered.

### Turning it on and off

| control | meaning |
|---|---|
| console `r1_lum_adapt 0/1` | default 0. Saved to `user.ltx` like any console var. |
| console `r1_lum_adapt_speed` | rate (1/s) towards a brighter scene, default 4 (90% in ~0.6 s), range 0.05..60 |
| console `r1_lum_adapt_speed_down` | rate (1/s) towards a darker scene, default 2 (90% in ~1.2 s) |
| shader bus lane `lum_adapt` | `x > 0` on, `x < 0` off, `x = 0` leave it to `r1_lum_adapt`; `y > 0` / `z > 0` replace the two speeds |

The bus lane is the better switch for a mod: nothing is written to `user.ltx`, and uninstalling the mod turns it off.
This branch has shader bus version 2 (`shader_bus.register`, `try_register`, `set`, `get`, ...; no `set_texture`,
no `SHADER_BUS` define):

```lua
local lum_token = nil
function on_game_start()
	if shader_bus and shader_bus.try_register then
		lum_token = shader_bus.try_register("lum_adapt", "zz NVG Overhaul", "R1 luminance adaptation")
	end
end
-- NVG on:  shader_bus.set(lum_token, 1, 6, 2, 0)   -- on, speed up 6/s, speed down 2/s
-- NVG off: shader_bus.set(lum_token, 0, 0, 0, 0)   -- back to r1_lum_adapt (default off)
-- no bus:  get_console():execute("r1_lum_adapt 1")
```

The bus latches once per frame, so a change shows one frame later. Full example:
`examples/r1-ps3-lum-adapt/lum_adapt_bus.script.example`.

### Reading it in a post-process shader

`.s`: `shader:sampler("s_lum_adapt"):texture("$user$lum_adapt"):clamp():f_none()` (point sampling: it is a 1x1
float texture, and some D3D9 GPUs cannot filter 32-bit float).

`.ps`:

```hlsl
uniform sampler2D s_lum_adapt;  // 1x1: rgb = adapted mean colour, a = adapted value of what the meter wrote to alpha
uniform float4 lum_adapt_info;  // x = 1 when s_lum_adapt is live this frame (0 = off, never ran, or unavailable)
                                // y = seconds since the adaptation (re)started (for a power-on ramp)
                                // z, w = current speeds up / down (1/s)
float lum = tex2D(s_lum_adapt, float2(0.5, 0.5)).a;
float gain = (lum_adapt_info.x > 0.5) ? clamp(0.35 / max(lum, 0.02), 0.5, 8.0) : 1.0;
```

When the adaptation is off the texture holds an old value or nothing (reads as 0); always check
`lum_adapt_info.x`. Works from `ps_2_0` too, it costs one texture read and a few instructions.

### The metering shaders (mod-overridable)

Vanilla has none of these files; the mod ships them in `gamedata\shaders\r1\`. If one of the three `.ps` files is
missing the engine logs `! r1_lum_adapt: shaders\r1\... is missing` once and stays off until `vid_restart`.
A file that fails to compile ends the game with the usual shader error, like any other shader.

| file | pass | inputs | output |
|---|---|---|---|
| `lum_adapt_meter.ps` (required) | scene -> 64x64 | `s_image` = `$user$rendertarget`, `c_lum_step` | rgb = mean colour, a = what the adaptation follows (luminance in the example) |
| `lum_adapt_meter.s` (optional) | same pass | anything you bind, e.g. a tube-mask texture | |
| `lum_adapt_reduce.ps` (required) | 64x64 -> 8x8, also the 1x1 history copy | `s_image`, `c_lum_step` | average of the taps |
| `lum_adapt_blend.ps` (required) | 8x8 -> 1x1, eased with the history | `s_image`, `s_lum_prev`, `c_lum_step`, `c_lum_adapt` | the new adapted value |

Constants the engine sets (any of the passes may declare them):

* `c_lum_step`: `xy` = one output pixel's footprint in source texture coordinates (meter 1/64, reduce 1/8, blend 1,
  copy 0), `zw` = output size in pixels. Texture coordinate `TEXCOORD0` (also `1` and `2`) is the centre of the
  footprint, 0..1 over the whole source.
* `c_lum_adapt` (blend): `x` = blend factor towards a brighter scene, `y` = towards a darker one (both
  `1 - exp(-dt * speed)`), `z` = dt in seconds, `w` = 1 on a start-over frame (return the metered value then).

Rules:

* `lum_adapt_reduce.ps` must stay "average of taps at `tc + offset * c_lum_step.xy`": the engine also runs it with
  a zero step (point sampled) to copy the 1x1 result into the history.
* The history decides brighter/darker by comparing **alpha**, so write the value you adapt on into alpha.
* Formats: 64x64 and 8x8 are `A16B16G16R16F` (or `A8R8G8B8` on a GPU that cannot filter FP16), the two 1x1 targets
  `A32B32G32R32F` (or `A16B16G16R16F`). Values above 1 survive only in the float formats.
* To weight what is metered (e.g. only inside the NVG tubes), ship `lum_adapt_meter.s` with an extra sampler and
  weight the taps in `lum_adapt_meter.ps`: `sum += c * m; wsum += m; ... avg = sum / max(wsum, 1e-4)`.
* The meter sees the scene before the post-process quad (no NVG tint, noise or tubes yet), HUD included.

### Render targets

`$user$lum_64`, `$user$lum_8`, `$user$lum_adapt` (read this one), `$user$lum_prev`. All are created on first use
and released with the rest of R1's targets on `vid_restart` / device reset.
