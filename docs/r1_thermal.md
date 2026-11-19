# R1 (DX8) thermal

For mod authors. The R1 renderer can tell warm things from everything else, and it ships a thermal look
built on that: thermal on shows a thermal picture, with no mod work. A mod can also read the raw heat
mask and make its own look.

How it works: while thermal is on, the warm things are drawn once more into a mask, against the depth
buffer of the scene that was just drawn. The scene is not rendered a second time. The look is made
from the scene and the mask in a few small passes. Off, nothing is queued, drawn or created, and the
picture is exactly what it was.

## Switching it on and off

| way | on | off | notes |
|---|---|---|---|
| console | `r__heatvision 1` | `r__heatvision 0` | saved in `user.ltx`, so it survives a restart |
| shader bus lane `thermal` | `x > 0` | `x < 0` | `x = 0` (or no such lane) leaves it to the console. Not saved. Takes effect on the next frame |

From a script, prefer the lane:

```lua
local token = shader_bus and shader_bus.try_register
    and shader_bus.try_register("thermal", "My Mod", "R1 thermal: x on/off, y look, z colour mode, w targets")
local function thermal(on)
    if token then shader_bus.set(token, on and 1 or -1, 0, 0, 0)
    else get_console():execute(on and "r__heatvision 1" or "r__heatvision 0") end
end
```

The first time it goes on (per game start or `vid_restart`) the engine compiles its shaders and creates
its targets, which can take a moment. If a shader file is missing the log says
`! r__heatvision: shaders\r1\<file> is missing` and thermal stays off until `vid_restart`.

## What you get with thermal on

`r1_thermal_look` (default `1`) decides what the engine does with the mask:

| `r1_thermal_look` | screen | SVP scope lens |
|---|---|---|
| `0` | nothing: only the masks are built, a mod's shaders make the look | nothing |
| `1` | the thermal picture, whole screen or inside the region circle (below) | the lens picture is the thermal picture |
| `2` | not shown; the picture is built into `$user$thermal` for a mod's post-process shader | as `1` |

`r1_thermal_targets` picks the views: `1` screen, `2` SVP lens, `3` both (default).

The SVP lens only exists for guns with `scope_lense_fov` (a real second picture inside the glass).
A plain 2D scope (a ring texture over the zoomed main view) and a helmet sight are the SCREEN:
use targets `1` or `3`, and the region circle to keep the picture inside the glass or tube. The 2D
scope texture, the HUD and all other UI are drawn after the thermal picture, so they stay on top
of it with their own colours.

While the whole-screen look is shown it replaces the post-process pass (`postprocess.ps` and
friends), so screen effects that pass makes (hit blur, noise, NVG shaders) do not show. With a
region, the post-process pass runs as always and the circle is drawn over it.

### Region: thermal only inside a circle

For a helmet-mounted sight with a round tube, or a 2D scope: the thermal picture inside a circle,
the normal frame (colours, brightness, post-process) around it.

| way | value |
|---|---|
| console `r1_thermal_region x,y,r` | three numbers with commas, no spaces: centre `x`, `y` in parts of the screen (`0.5,0.5` = middle), radius `r` in screen heights. `r = 0` = whole screen (default `0.5,0.5,0`). Example: `r1_thermal_region 0.5,0.5,0.35` |
| console `r1_thermal_region_soft s` | width of the soft edge, in screen heights, inside the radius (default `0.02`) |
| bus lane `thermal_region` | `x`, `y`, `z` = radius, `w` = soft edge. `z > 0` uses the lane, `z < 0` forces whole screen, `z = 0` (or no lane) leaves it to the console. Per frame, not saved |

```lua
local region = shader_bus.try_register("thermal_region", "My Mod", "R1 thermal circle: x, y, radius, soft edge")
shader_bus.set(region, 0.5, 0.5, 0.35, 0.02)   -- a tube in the middle, 0.35 screen heights in radius
```

The circle is round on any aspect ratio. A shape that is not a circle (a baked overlay mask) is
`r1_thermal_look 2` plus the mod's own post-process shader:

```lua
shader:sampler("s_thermal"):texture("$user$thermal"):clamp():f_linear()
```
```hlsl
uniform sampler2D s_thermal;
uniform float4 r1_thermal_info;                 // z = 1 while $user$thermal holds this frame's picture
float3 t = tex2D(s_thermal, I.tc0).rgb;
image = lerp(image, t, my_mask * r1_thermal_info.z);
```

### Colour modes

`r1_thermal_palette`: `0` white hot (default), `1` black hot, `2` colour (dark blue, purple, orange,
white). Black hot is the inverse held a little under white, so the sky does not glare. At run time by the bus: lane `thermal`, `z` = `1` white hot, `2` black hot, `3` colour
(`0` = the console var).

### Lane `thermal`, all four values

| value | meaning |
|---|---|
| `x` | `> 0` on, `< 0` off, `0` = `r__heatvision` |
| `y` | `1` show the look, `2` build it only (`$user$thermal`), `< 0` masks only, `0` = `r1_thermal_look` |
| `z` | `1` white hot, `2` black hot, `3` colour, `0` = `r1_thermal_palette` |
| `w` | `1` screen, `2` SVP lens, `3` both, `0` = `r1_thermal_targets` |

## What is hot, and how hot

| thing | hotness | rule |
|---|---|---|
| living stalker, mutant, bird | `1` | the creature's whole own model, whatever model, shader or texture it has (so a body a mod swapped with `set_visual_name` / `set_shader` is warm) |
| corpse | `1` fading to `0` in `heat_vision_cooldown_time` | same |
| gear a script attached to a warm creature (`add_attachment`, world type: helmets, headsets, packs) | the wearer's hotness x `r1_thermal_gear` (`0.8`) | follows the wearer, cools with a corpse |
| the player's hands in first person | `1` | the hands' textures are marked hot by the engine |
| a gun that was fired, in the player's hands or an NPC's | up to `r1_thermal_barrel` (`0.8`) | the whole gun model. From the silencer-overheat counter: each shot adds `sil_glow_shot_temp`, it cools by `sil_glow_cool_temp_rate` per second, full at `sil_glow_max_temp`; shown as the square root, so the first shots count. One shot is about `0.13` for half a second, a 30-round burst about `0.7` fading over 12 s. Never fired = cold |
| a lit lamp that is an object (hanging lamps and other `hanging_lamp` class lights) | `r1_thermal_lamp` (`0.65`) | the lamp's model, while its light is on; switched off or shot out = cold |
| fire: campfires, burning barrels, flamethrowers, fire anomalies going off, explosions | `1` | particle effects, by name (below) |
| a dormant burner anomaly | `0.25` | its idle heat haze, a faint soft patch |
| an object a script gave `obj_hotness` | that value | every surface of it |

Not hot: the world, lamps that are part of the level geometry, guns never fired, detectors and other
held devices, the sky, smoke, dust.

### Warm particle effects: `configs\r1_thermal.ltx`

Fires and anomalies are particle effects. The file lists parts of effect names with a hotness and a
gain; an effect whose name contains one is drawn once more into the mask, with its own sprites.

```ini
[particles]
flame          = 1.0, 3      ; hotness, gain
buble_distort  = 0.25, 3     ; the idle haze of a burner anomaly
smoke          = 0           ; never warm, whatever else the name says
```

- If any matching pattern has hotness `0` the effect is cold; otherwise the longest matching pattern counts.
- Hotness is a cap: sprites on top of each other never go above it. Gain is how fast a sprite's own
  picture (brightness x opacity) reaches the cap.
- For a particle group the names of the effects inside it count, not the group's name.
- A mod adds its own lines with a DLTX file next to it, `mod_r1_thermal_<name>.ltx`, section `![particles]`.
- Read when thermal first goes on, and after `vid_restart`. `r1_thermal_particles 0` switches all of it off.

### Marking your own heat source

| what you have | how |
|---|---|
| an object (item, creature, physics object) | `obj_hotness` object lane: `shader_bus.set_object(token, obj, x, 0, 0, 0)`, see `shader_bus.md` |
| a particle effect | a line in a `mod_r1_thermal_<name>.ltx` |
| a script attachment on a creature | nothing to do, it follows the wearer |
| engine code that draws a model | `IDSGraphManager::set_ItemHeat(x)` around its `add_Dynamic` calls |

## The look

Built at "sensor size": the view's size divided by `r1_thermal_soft` (default `3`, so 640x360 on a
1080p screen), then stretched over the view. That is what makes it soft, and cheap.

| var | range | default | what it does |
|---|---|---|---|
| `r1_thermal_soft` | `1..8` | `3` | screen pixels per sensor pixel. Higher = softer, blockier and cheaper |
| `r1_thermal_body` | `0..1` | `0.95` | grey level of a hot body where it faces the camera (`1` = pure white) |
| `r1_thermal_round` | `0..1` | `0.5` | how much darker a body is where its surface turns away from the camera (the bright core, readable form). `0` = flat |
| `r1_thermal_ring` | `0..2` | `1.5` | the thin dark ring right outside a hot thing |
| `r1_thermal_glow` | `0..1` | `0.12` | the glow around a hot thing |
| `r1_thermal_grain` | `0..0.3` | `0.027` | moving sensor grain (changes 30 times a second) |
| `r1_thermal_fpn` | `0..0.1` | `0.006` | fixed pattern: faint columns and lines |
| `r1_thermal_world` | `0..2` | `0.45` | contrast of the world |
| `r1_thermal_world_base` | `0..1` | `0.1` | grey level of a black world pixel |
| `r1_thermal_world_gamma` | `0.2..3` | `0.85` | curve of the world's brightness |
| `r1_thermal_sky` | `0..1` | `0.035` | grey level of the sky |
| `r1_thermal_sky_clouds` | `0..1` | `0.1` | how much of the clouds stays |
| `r1_thermal_palette` | `0..2` | `0` | colour mode |

The world's grey comes from the scene's brightness, so a dark night gives a dark world with bright
bodies; raise `r1_thermal_world_base` or `r1_thermal_world` for a lighter one.

## The raw mask, for a mod's own look

| name | size | when it is filled |
|---|---|---|
| `$user$heat` | the scene target (`$user$rendertarget`) | every normal frame while thermal is on and targets has `1` |
| `$user$heat_vp2` | `$user$viewport2` | every small SVP (scope lens) frame while thermal is on and targets has `2`; needs `r1_svp_size > 0` |
| `$user$thermal` | sensor size | the engine's finished thermal picture of the screen, while the look is `1` or `2` |

Mask channels, both mask textures:

| channel | holds |
|---|---|
| `r` | hotness, `0..1`. `0` = nothing warm visible at this pixel. A living body is `1` |
| `g` | `1` where the warm pixel is the player's own hands (HUD), else `0` |
| `b` | sky flag: `1` where nothing but the sky was drawn (also behind smoke and glass against the sky), else `0` |
| `a` | edge term of a warm model: `0` where its surface faces the camera, towards `1` where it is seen edge-on; `0` elsewhere |

Changed since round 1: `b` was reserved and `a` was "do not use"; `r` and `g` are as they were. The
mask is now always an `A8R8G8B8` texture (if the GPU refuses that, the log says so and `a` reads `1`).
`r1_thermal_demo` is gone: `r1_thermal_look 1` is the engine's look, `0` is "masks only".

While thermal is off the masks are black (cleared once when it goes off; before first use they do
not exist and a sampler reads black). Switch your look with your own flag, not with the mask.

Sampling in a post-process pass: add a sampler and read it with the scene's texture coordinate.

```lua
shader:sampler("s_heat"):texture("$user$heat"):clamp():f_linear()
```
```hlsl
uniform sampler2D s_heat;
float hot = tex2D(s_heat, I.tc0).r;
```

Sampling in a lens shader: `$user$heat_vp2` is laid out exactly like `$user$viewport2` (same size,
same lens square, moved and zoomed by the same copies), so read it with the texture coordinate you
use for `s_vp2`. With `r1_thermal_look 1` or `2` the lens picture itself is already thermal; use
look `0` when the lens shader makes its own. The screen mask is no use inside a lens.

Post-process constant `r1_thermal_info` (set on the post-process pass only, declare it as
`uniform float4`): `x` = `1` while `$user$heat` holds this frame's mask, `y` = `1` while the engine
shows its look on this frame (in the region circle; with the whole-screen look the post-process pass
does not run), `z` = `1` while `$user$thermal` holds this frame's picture, `w` = `0`.

## Console vars that are not the look

| var | range | default | meaning |
|---|---|---|---|
| `r__heatvision` | `0..1` | `0` | the switch |
| `r1_thermal_targets` | `1..3` | `3` | `1` screen, `2` SVP lens, `3` both |
| `r1_thermal_look` | `0..2` | `1` | `0` masks only, `1` the engine's look, `2` built into `$user$thermal` but not shown on screen |
| `r1_thermal_region` | `x,y,r` | `0.5,0.5,0` | circle for the screen look: centre x, y, radius in screen heights; radius `0` = whole screen |
| `r1_thermal_region_soft` | `0..1` | `0.02` | soft edge of the circle |
| `r1_thermal_gear` | `0..1` | `0.8` | worn script attachments, as a share of the wearer's hotness |
| `r1_thermal_lamp` | `0..1` | `0.65` | a lit lamp object |
| `r1_thermal_barrel` | `0..1` | `0.8` | a gun at full barrel temperature |
| `r1_thermal_particles` | `0..1` | `1` | warm particle effects on / off |
| `r1_thermal_bias` | `0..4096` | `8` | depth bias of the redraw, in steps of a 24-bit depth buffer. Raise it if warm bodies show speckles or holes; too high and bodies show through thin cover at long range |
| `heat_vision_cooldown` | `0..1` | `1` | corpses cool down (`0` = corpses stay at `1`) |
| `heat_vision_cooldown_time` | `0..300000` ms | `20000` | how long a corpse takes to reach `0` |
| `heat_vision_zombie_cold` | `0..1` | `0` | `1` = zombies have no heat |
| `sil_glow_shot_temp`, `sil_glow_max_temp`, `sil_glow_cool_temp_rate` | | `0.004`, `0.15`, `0.01` | the barrel temperature counter (shared with the DX10/DX11 silencer glow) |

The other `heat_vision_*` vars (`mode`, `steps`, `blurring`, `args_1`, `args_2`) only feed the
DX10/DX11 heat shader. R1 does not read them.

## Shader bus names

| name | kind | values |
|---|---|---|
| `thermal` | lane | on/off, look, colour mode, targets (table above) |
| `thermal_region` | lane | circle: `x`, `y`, radius, soft edge |
| `obj_hotness` | object lane (see `shader_bus.md`, "Object hotness") | `shader_bus.set_object(token, obj, x, 0, 0, 0)` gives that object hotness `x` (`0..1`) on every surface. `bus_force obj_hotness x 0 0 0` gives every object `x` on its own model and on marked textures |

On R1 `obj_hotness` only works for objects drawn as dynamic models (creatures, items, physics
objects). It is read when the object is queued, so it follows the object from the next frame.

## Limits

- A warm thing is hidden by anything that writes depth in front of it (walls, terrain, the gun,
  grass, leaves). It is not hidden by things that do not write depth: glass, smoke, particles.
- Hair or fur drawn with see-through textures shows as the full polygon.
- The whole gun warms up, not only the barrel.
- A corpse that was already dead when it came online starts cooling at that moment.
- Level-geometry lamps and fires without particles are cold; a fire shows as far as its particles do.
- Mirrors of the scene that are not the main view or the small SVP lens have no mask: a full-size
  second viewport (`r1_svp_size 0`), light and shadow passes.
- The lens mask and the lens look are as old as the lens picture (SVP frames are not rendered every frame).
- The region is a circle. Other shapes: `r1_thermal_look 2` and a mod shader.

## Cost

Off: one flag test and one bus lookup per frame.

On, per frame (1080p, `r1_thermal_soft 3`):

- the mask: one clear of a screen-size target, one screen quad for the sky flag (only sky pixels pass
  the depth test), one extra draw call per visible warm mesh part with a 4-instruction pixel shader,
  one extra draw per visible warm particle effect;
- the look: five passes on small targets (640x360 twice, 320x180 twice, 640x360 again), together
  about as many texture reads as two and a half screen-size ones, then one screen quad with one
  texture read (in place of the post-process pass) or a quad over the circle;
- the same on SVP frames for the lens, at the lens's size.

Memory: the mask (8 MB at 1080p), five small targets (about 4 MB), a 128x128 noise texture; for the
lens, two masks and five small targets more. Created on first use, kept until `vid_restart`.

If it is slow, lower in this order: `r1_thermal_soft 4` (or `5`), `r1_thermal_particles 0`,
`r1_thermal_targets 1` (no lens work).

## Files

The engine's game data must be installed: in `gamedata\shaders\r1\` the files `heat_mask.vs`,
`heat_mask_lq.vs`, `heat_mask.ps`, `heat_flat.ps`, `heat_particle.vs`, `heat_particle.ps`,
`thermal_noise.ps`, `thermal_down.ps`, `thermal_blur.ps`, `thermal_look.ps`, `thermal_palette.ps`,
`thermal_show.ps`, `thermal_show_region.ps`, and `gamedata\configs\r1_thermal.ltx`. No other mod ships
files with these names, so their place in the load order does not matter. `thermal_demo.ps` and
`thermal_demo_paint.ps` from round 1 are no longer used and can stay or go.
