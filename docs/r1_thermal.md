# R1 (DX8) thermal: the heat mask

For mod authors. The R1 renderer can tell warm bodies from everything else and hands that to
shaders as a texture. The engine does not ship a thermal look (only a plain test look, see the end).
A mod's own post-process shader or lens shader makes the look.

How it works: while thermal is on, the warm visuals are drawn once more, flat, into a mask, against
the depth buffer of the scene that was just drawn. The scene is not rendered a second time.
Off, nothing is queued, drawn or created, and the picture is exactly what it was.

## Switching it on and off

| way | on | off | notes |
|---|---|---|---|
| console | `r__heatvision 1` | `r__heatvision 0` | saved in `user.ltx`, so it survives a restart |
| shader bus lane `thermal` | `x > 0` | `x < 0` | `x = 0` (or no such lane) leaves it to the console. Not saved. Takes effect on the next frame |

From a script, prefer the lane:

```lua
local token = shader_bus and shader_bus.try_register
    and shader_bus.try_register("thermal", "My Mod", "R1 heat mask: x>0 on, x<0 off")
local function thermal(on)
    if token then shader_bus.set(token, on and 1 or 0, 0, 0, 0)
    else get_console():execute(on and "r__heatvision 1" or "r__heatvision 0") end
end
```

The first time it goes on (per game start or `vid_restart`) the engine compiles its heat shaders and
creates the mask target, which can take a moment. If a shader file is missing the log says
`! r__heatvision: shaders\r1\<file> is missing` and thermal stays off until `vid_restart`.

## Textures

| name | size | when it is filled |
|---|---|---|
| `$user$heat` | the scene target (`$user$rendertarget`) | every normal frame while thermal is on |
| `$user$heat_vp2` | `$user$viewport2` | every small SVP (scope lens) frame while thermal is on; needs `r1_svp_size > 0` |

Channels, both textures:

| channel | holds |
|---|---|
| `r` | hotness, `0..1`. `0` = nothing warm visible at this pixel. A living body is `1`, a corpse fades from `1` to `0` |
| `g` | `1` where the warm pixel is the player's own hands (HUD), else `0` |
| `b` | `0` (reserved) |
| `a` | do not use (the target may have no alpha) |

While thermal is off both textures are black (cleared once when it goes off; before first use they
do not exist and a sampler reads black). Still, switch your look with your own flag, not with the mask.

Sampling in a post-process pass (`postprocess.s` and friends): add a sampler and read it with the
same texture coordinate as the scene (`tc0` of `s_base0`).

```lua
shader:sampler("s_heat"):texture("$user$heat"):clamp():f_linear()
```
```hlsl
uniform sampler2D s_heat;
float hot = tex2D(s_heat, I.tc0).r;
```

Sampling in a lens shader: `$user$heat_vp2` is laid out exactly like `$user$viewport2` (same size,
same lens square, moved and zoomed by the same copies), so read it with the texture coordinate you
use for `s_vp2`. This is the way to do a thermal scope: the look lives in the lens shader, with
`r1_thermal_targets 2`. The screen mask is no use inside a lens: there it shows bodies where the
main camera sees them through the tube, small and unmagnified, not where the lens picture has them.

Post-process constant `r1_thermal_info` (set on the post-process pass only, declare it as
`uniform float4`): `x` = `1` while `$user$heat` holds this frame's mask, `y` = `1` while
`r1_thermal_demo` is on, `z`, `w` = `0`.

## Console vars

| var | range | default | meaning |
|---|---|---|---|
| `r__heatvision` | `0..1` | `0` | the switch |
| `r1_thermal_targets` | `1..3` | `3` | which masks are built: `1` screen, `2` SVP lens, `3` both. A thermal scope only needs `2` |
| `r1_thermal_demo` | `0..1` | `0` | the engine's test look (below) |
| `r1_thermal_bias` | `0..4096` | `8` | depth bias of the redraw, in steps of a 24-bit depth buffer. Raise it if warm bodies show speckles or holes; too high and bodies show through thin cover at long range |
| `heat_vision_cooldown` | `0..1` | `1` | corpses cool down (`0` = corpses stay at `1`) |
| `heat_vision_cooldown_time` | `0..300000` ms | `20000` | how long a corpse takes to reach `0` |
| `heat_vision_zombie_cold` | `0..1` | `0` | `1` = zombies have no heat |

The other `heat_vision_*` vars (`mode`, `steps`, `blurring`, `args_1`, `args_2`) only feed the
DX10/DX11 heat shader. R1 does not read them.

## Shader bus

| name | kind | values |
|---|---|---|
| `thermal` | lane | `x > 0` on, `x < 0` off, `0` = console |
| `obj_hotness` | object lane (see `shader_bus.md`, "Object hotness") | `shader_bus.set_object(token, obj, x, 0, 0, 0)` gives that object hotness `x` (`0..1`) on every surface, gear included. `bus_force obj_hotness x 0 0 0` gives every object `x` on its marked textures |

On R1 `obj_hotness` only works for objects drawn as dynamic models (creatures, items, physics
objects). It is read when the object is queued, so it follows the object from the next frame.

## What is hot

Hot: living stalkers and mutants (`1`), their corpses for `heat_vision_cooldown_time` after death
(fading linearly), the player's hands on the HUD (`1`), and any object a script gave `obj_hotness`.

Not hot: the world, weapons (in hand, on the ground, held by NPCs), detectors and other held
devices, script attachments, particles, fires, lamps, the sky. Fires, lamps and anomalies have no
owner object with hotness, so `obj_hotness` cannot reach them either.

The rule is the DX10/DX11 one: the object has hotness, and the texture of the drawn part is marked
hot (the engine marks the textures of every creature, the player and the HUD hands).

## Limits

- A warm body is hidden by anything that writes depth in front of it (walls, terrain, the gun,
  grass, leaves). It is not hidden by things that do not write depth: glass, smoke, particles.
- The mask is a flat silhouette: hair or fur drawn with see-through textures shows as the full
  polygon.
- A distant creature drawn as its far LOD model has no heat (the LOD model's texture is not marked).
- A corpse that was already dead when it came online (loading a save, walking up to it) starts
  cooling at that moment.
- Mirrors of the scene that are not the main view or the small SVP lens have no mask: a full-size
  second viewport (`r1_svp_size 0`), light and shadow passes.
- The lens mask is as old as the lens picture (SVP frames are not rendered every frame).

## Cost

Off: one flag test per frame. On: one clear of a screen-size target and one extra draw call per
visible warm mesh part with a one-instruction pixel shader, plus the same for the lens target on
SVP frames. Memory: one target the size of the scene target (about 8 MB at 1920x1080), created on
first use and kept until `vid_restart`; two small lens targets if a lens mask was ever built.
While the screen mask is on, the frame goes through the post-process quad (as with any active
post-process effect).

## The test look

`r1_thermal_demo 1` (with thermal on) replaces the post-process shader with a plain picture: the
world in grey, warm bodies white, hotness as brightness. On small SVP frames it paints warm bodies
white straight into the lens picture. It is for checking the engine, not a design. It needs no
mod work and does not touch `postprocess.ps`.

## Files

The engine's game data must be installed: `gamedata\shaders\r1\heat_mask.ps`, `heat_mask_lq.vs`,
`thermal_demo.ps`, `thermal_demo_paint.ps`, and `ui_icons.vs` (already there for the 3D icons).
No other mod ships files with these names, so their place in the load order does not matter.
