# R1: telling a `.s` / `.ps` which engine it runs on (`r1_sm3_postprocess`, `R1_SM3`)

Branch `r1-sm3-detect` (based on `r1-ps3-lum-adapt`, see `R1-PS3-LUM-ADAPT-NOTES.md` for the ps_3_0 post-process
itself). Lets one set of post-process shader files use ps_3_0 on this engine and stay vanilla-compatible on every
older exe. Vanilla shaders compile and draw exactly as before.

Ready-to-copy files: `examples/r1-sm3-detect/shaders/r1/postprocess.s` and `postprocess.ps`, plus
`examples/r1-ps3-lum-adapt/shaders/r1/postprocess_vs3.vs` (unchanged).

## What the engine provides

| | where | value |
|---|---|---|
| `r1_sm3_postprocess` | global in the shader Lua state that runs the `.s` files (`CResourceManager::LS_Load`, R1 only) | `true`: this exe compiles `main_ps_3_0` / `main_vs_3_0` **and** the GPU has vs 3.0 + ps 3.0, so a post-process pass that names a vertex shader gets the clip-space quad. `false`: this exe, GPU without shader model 3. **`nil`**: any older exe (never set). |
| `R1_SM3` | HLSL define (R1 `shader_compile`) | defined (`1`) only while compiling for `ps_3_0` / `vs_3_0`, i.e. when the engine picked `main_ps_3_0` / `main_vs_3_0`. Never defined for a ps_2_0 / vs_2_0 build, and older exes never define it. |

The engine logs `* R1 shaders: r1_sm3_postprocess = true|false` once per device creation (game start,
`vid_restart`). The global is set before any `.s` runs, so a file's top level and its `normal(...)` function can both
read it. Same test as the sm3 entry pick: `HW.Caps` pixel shader major >= 3 and vertex shader major >= 3.

### Why the old-exe path is safe

* `.s` files run in their own Lua state (not the game script VM). It opens base/table/string/math/jit plus
  lfs/marshal and luabind, and sets **no metatable on `_G`**: there is no "strict" mode, so reading an undefined
  global returns `nil` without an error. Each `.s` is loaded as a plain chunk (with `local this = <namespace>`
  prepended); new globals it defines are moved into its namespace afterwards, but globals that already existed (like
  `r1_sm3_postprocess`) stay in `_G` for every file. Checked against `ai_script_lua_extension.cpp`
  (`bfLoadFileIntoNamespace`, `vfCopyGlobals`, `vfSetNamespace`) and run through a Lua copy of that load sequence.
* Older exes pick the pixel shader entry by searching the source text for `main_ps_1_1` .. `main_ps_2_0`, otherwise
  `main` with target `ps_2_0`; they never look for `main_ps_3_0`. The whole file still goes through
  `D3DXCompileShader`. That compiler only generates code for functions reachable from the entry, so most ps_3_0-only
  things (loops, `tex2Dlod`, instruction count, extra samplers) in an unused `main_ps_3_0` would not fail it, but
  it does parse and type-check every function, and explicit register bindings or other target-specific details are
  edge cases. Wrapping the sm3 part in `#ifdef R1_SM3` removes it before the compiler sees it, so the ps_2_0 build
  is exactly the ps_2_0 code: use that form. (Not run against D3DX here: there is no Windows/D3DX in the build
  container. The `#ifdef` form does not depend on that compiler behaviour.)
* A `.s` that takes the old path names vertex shader `"null"`, so `postprocess_vs3.vs` is never compiled there. A
  `.vs` / `.ps` file nobody references is never compiled.

## The pattern

### `.s` (do the same in `postprocess_d.s`, and `postprocess_cm.s` / `postprocess_dcm.s` if you override them)

```lua
function normal(shader, t_base, t_second, t_detail)
	if r1_sm3_postprocess then
		shader:begin("postprocess_vs3", "postprocess")   -- pass-through vs_3_0 + main_ps_3_0
	else
		shader:begin("null", "postprocess")              -- vanilla: XYZRHW quad + main (ps_2_0)
	end
	shader:fog(false)
	shader:zb(false, false)

	shader:sampler("s_base0") :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_base1") :texture("$user$rendertarget") :clamp() :f_linear()
	shader:sampler("s_noise") :texture("fx\\fx_noise2")      :f_linear()
	if r1_sm3_postprocess then
		shader:sampler("s_lum_adapt") :texture("$user$lum_adapt") :clamp() :f_none()
	end
end
```

(`shader:begin` returns `shader`, so `:fog` / `:zb` on separate lines is the same as chaining them. Binding a sampler
the compiled pixel shader does not declare is a no-op, so the `if` around sm3-only samplers is optional.)

### `.ps` (`postprocess.ps`, `postprocess_d.ps`, ...)

```hlsl
struct v2p { float4 color:COLOR0; float4 gray:COLOR1; float2 tc0:TEXCOORD0; float2 tc1:TEXCOORD1; float2 tc2:TEXCOORD2; };
uniform sampler2D s_base0;                     // shared by both entries
uniform sampler2D s_base1;
uniform sampler2D s_noise;
uniform float4 c_brightness;

float4 main(v2p I) : COLOR        // ps_2_0: older exes, and this exe on a GPU without shader model 3
{
	...vanilla-level effect...
}

#ifdef R1_SM3                     // only a ps_3_0 build of this exe sees anything below
uniform sampler2D s_lum_adapt;    // sm3-only samplers / constants / helpers go in here too

float4 main_ps_3_0(v2p I) : COLOR
{
	...halo, soft warped tube edges, honeycomb...
}
#endif
```

Rules:

* Keep the text `main_ps_3_0` in the file (inside the `#ifdef` is fine: the engine searches the raw text). If the
  `.s` names `postprocess_vs3` but the `.ps` has no `main_ps_3_0`, the pass gets a vs_3_0 with a ps_2_0 and D3D9
  refuses to draw it.
* Use plain `main` for the ps_2_0 entry. Do not write the text `main_ps_2_0` / `main_ps_1_x` anywhere in the file
  (not even in a comment) unless that is the entry you mean: both old and new exes would pick it instead of `main`.
* `postprocess_vs3.vs` stays as in `examples/r1-ps3-lum-adapt/` (it may keep its `main_vs_2_0`; it is only ever
  compiled when `r1_sm3_postprocess` is true, so `main_vs_3_0` is what runs).
* Every `.s` whose `.ps` carries `main_ps_3_0` needs the same `if r1_sm3_postprocess` branch, `_d` variant included.

## What happens where

| exe / GPU | `r1_sm3_postprocess` | `.s` takes | `.ps` compiled as | quad |
|---|---|---|---|---|
| older exe (any GPU) | `nil` | `"null"` | `main`, ps_2_0, `R1_SM3` undefined (sm3 block gone) | XYZRHW, as vanilla |
| this exe, GPU without vs/ps 3.0 | `false` | `"null"` | `main`, ps_2_0 (log: `~ postprocess.ps: GPU has no vs_3_0/ps_3_0, using entry 'main'`) | XYZRHW, as vanilla |
| this exe, GPU with vs/ps 3.0 | `true` | `"postprocess_vs3"` | `main_ps_3_0`, ps_3_0, `R1_SM3` = 1; vs: `main_vs_3_0` | clip space (same pixels) |

## Shader cache

Shader-model-3 builds are cached under a key that now also includes `R1_SM3`, so ps_3_0 / vs_3_0 blobs cached by
the `r1-ps3-lum-adapt` exe are rebuilt once (they were compiled without the define). ps_2_0 / vs_2_0 / ps_1_x keys
are unchanged: vanilla shaders are not recompiled.
