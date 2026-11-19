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

While the main menu or the pause menu is open there is no mask and no thermal picture, whatever the
switch says. It comes back by itself when the menu closes.

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

## The heat ladder: what is hot, and how hot

Heat is a number from `0` to `1`. The picture keeps the levels apart in all three colour modes: only
heat `1` reaches the top level (pure white; white-yellow in colour mode), a living body sits a step
under it (light grey-white; orange).

| thing | heat | rule |
|---|---|---|
| open fire: campfires, burning barrels, flamethrowers, explosions, a fire anomaly going off | `1` | particle effects, by name (below). Pure white, and it blooms |
| a creature or the ground burning | `0.9` | particle effects |
| the muzzle of a gun that was fired, the player's or an NPC's | up to `r1_thermal_barrel` (`0.9`), cooling | hottest at the muzzle, falling off over `r1_thermal_barrel_len` (`0.45` m) of barrel. The rest of the gun warms late and little, up to `r1_thermal_gun` (`0.25`) after a long burst. From the silencer-overheat counter (`sil_glow_*`): one shot is about `0.15` at the muzzle for half a second, a 30-round burst about `0.8` fading over 12 s. Never fired = cold |
| living stalker, mutant, bird, the player's hands | `0.85` | the creature's whole own model, whatever model, shader or texture it has. The value comes from config, see below |
| gear a script attached to a warm creature (`add_attachment`, world type: helmets, headsets, packs) | the wearer's heat x `r1_thermal_gear` (`0.9`) | follows the wearer, cools with a corpse |
| zombified stalker (community `zombied`, the zombies that shoot) | `0.65` | like a person, dimmer |
| a lit lamp: lamp objects, searchlights, and lamps built into the level that have a glow | `r1_thermal_lamp` (`0.45`) | a lamp object: its model while its light is on. A level lamp: a small spot where its glow is, `r1_thermal_lamp_size` (`0.25`) of the glow's size. Off or shot out = cold |
| a dormant burner anomaly | `0.25` | its idle heat haze, a faint soft patch |
| shambling zombie (class `SM_ZOMBI`) | `0.2` | barely above the world: easy to miss |
| corpse | its living heat fading to `0` in `heat_vision_cooldown_time` | stays warm through death, no gap |
| what a fire warms around it | up to `r1_thermal_fire_spread` (`0.6`) at the fire | see "Heat that follows physics" |
| an object a script gave `obj_hotness` | that value | every surface of it |

Not hot: the world, level lamps without a glow, guns never fired, detectors and other held devices,
the sky, smoke, dust.

`r1_thermal_ladder 0` switches the ladder off: every creature is `1` again and the top level is
`r1_thermal_body`, as in round 2.

### A creature's heat comes from config

The value of a living creature is looked up in this order, the first match wins:

1. `thermal_heat = <0..1>` in the creature's own section (its `.ltx`);
2. `configs\r1_thermal.ltx`, `[creature_sections]`: `<section name> = <0..1>`;
3. `[creature_communities]`: `<community> = <0..1>` (stalkers only);
4. `[creature_classes]`: `<the class of the section> = <0..1>`, for example `SM_ZOMBI`;
5. `[creatures]` `default` (`0.85`).

A mod changes any of it without engine changes, with a DLTX file next to the engine's one,
`mod_r1_thermal_<name>.ltx`:

```ini
![creature_sections]
my_ice_golem = 0.1
![creature_communities]
monolith = 0.75
```

Read once per game start. A creature's value is looked up again every 3 seconds, so a script that
changes a stalker's community changes his heat. An object that is not a creature: the `obj_hotness`
object lane (below).

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
| a kind of creature | a line in a `mod_r1_thermal_<name>.ltx` (`![creature_sections]`, `![creature_communities]`, `![creature_classes]`), or `thermal_heat` in its section |
| one object (item, creature, physics object) | `obj_hotness` object lane: `shader_bus.set_object(token, obj, x, 0, 0, 0)`, see `shader_bus.md`. It replaces the engine's value and is not changed by sun or rain |
| a particle effect | a line in a `mod_r1_thermal_<name>.ltx` (`![particles]`). Hotness `0.75` and up counts as an open fire and spreads heat |
| a script attachment on a creature | nothing to do, it follows the wearer |
| engine code that draws a model | `IDSGraphManager::set_ItemHeat(x)` around its `add_Dynamic` calls; with a point (`set_ItemHeat(at_point, far_from_it, point, reach)`) the heat sits there and falls off |

## Heat that follows physics

Not a global dial: each part depends on what the thing was actually exposed to. Every strength is a
console var, and `0` is its neutral value (the part is off).

**Heat spreads from fires** (`r1_thermal_fire_spread`, default `0.6`; `r1_thermal_fire_reach`, default
`0.6` m). Every open fire the frame draws (a particle effect of hotness `0.75` and up) warms what is
around it:

- the ground, the container and the air around it: a soft round spot in the mask, strongest at the
  fire and gone at the flames' size plus the reach (a muzzle flash gets a fraction of the reach, an
  explosion more). It is tested against depth, so a wall in front of the fire hides it;
- objects near it (a crate, a gun on the ground, a stalker sitting at it) warm as a whole, by their
  distance to the fire: a cold object up to the spread value, a warm creature by a third of that.

Lit lamps do not spread heat in the world; the glow of the look around every warm thing is their
small version.

**Sun and rain on a creature** (`r1_thermal_sun`, default `0.07`; `r1_thermal_wet`, default `0.2`).
Each creature drawn warm (and the gear it wears) keeps two numbers: how wet its surface is and how
much the sun has warmed it.

- Rain only wets what it reaches: rain density x how much open sky the creature has above it (the
  same sky value the engine uses for its lighting). Under a roof it stays dry. Soaked in about 20 s,
  dry again in about 90 s. A soaked creature loses `r1_thermal_wet` of its heat (`0.85` reads `0.68`).
- The sun only warms what it shines on: the sun's strength from the weather (nothing at night) x the
  creature's own sun visibility. Warm in about 45 s, cool again in 60 s. Full sun adds
  `r1_thermal_sun` (`0.85` reads `0.92`). A wet surface is not warmed.
- A creature is never hidden by this: the shift is a step of the ladder at most.
- A creature seen for the first time starts at what it would have settled at where it stands.

For testing, without waiting for the weather: `r1_thermal_force_sun` and `r1_thermal_force_rain`
(`0..1`; `-1` = the weather, the default). While one is forced the state follows within a second or two.

**Lightning** (`r1_thermal_lightning`, default `0.6`): while a bolt is lit the thermal sky flashes and
the bolt itself shows.

**Not built: world surfaces.** Walls, ground and roofs are as in round 2 (a grey from the scene's
brightness). A wall in the sun is not warmer than one in the shade, and rain does not cool the open
ground. See "Limits" for what that would take.

## Generations: an old, blurry thermal and a modern, sharp one

The engine gives a device three things to differ by. Which colour modes a device offers is a rule
for the mod, not the engine: the mod simply never sets the others.

| setting | console var | bus lane `thermal_sensor` | meaning |
|---|---|---|---|
| sharpness | `r1_thermal_soft` (`1..8`, default `3`) | `x` (`> 0`) | screen pixels per sensor pixel. `1` = as sharp as the screen, `8` = a 240x135 sensor on 1080p |
| grain | `r1_thermal_grain` (`0..0.3`, default `0.027`) | `y` (`> 0` the value, `< 0` none) | moving sensor noise |
| fixed pattern | `r1_thermal_fpn` (`0..0.1`, default `0.006`) | `z` (`> 0` the value, `< 0` none) | faint columns and lines that do not move |
| contrast | `r1_thermal_world` (default `0.45`) | `w` (`> 0`) = a factor on it | contrast of the world; low = washed |

`0` in a lane value (or no such lane) leaves that setting to the console var. The lane is read every
frame and not saved, so a mod sets it when the device goes on:

```lua
local sensor = shader_bus.try_register("thermal_sensor", "My Mod", "R1 thermal sensor: x size, y grain, z pattern, w contrast")
shader_bus.set(sensor, 6, 0.07, 0.02, 0.7)     -- an old device
shader_bus.set(sensor, 1.5, 0.006, 0.001, 1.25) -- a modern one
shader_bus.set(sensor, 0, 0, 0, 0)              -- back to the console vars
```

Changing the sensor size re-creates five small targets, so set it per device, do not animate it.
What sharper costs: the look's passes work on sensor pixels, so `r1_thermal_soft 1.5` is four times
the work of the default `3`, and `1` is nine times (about as much as two more full passes over the
screen with a long shader). Softer is cheaper.

Three ready settings for testing (`cfg_load` files in the exe zip's `appdata\`):

| file | sensor on 1080p | grain | pattern | contrast |
|---|---|---|---|---|
| `thermal_gen_old` | 320x180 (`soft 6`) | `0.07` | `0.02` | `0.32` |
| `thermal_gen_mid` | 640x360 (`soft 3`) | `0.027` | `0.006` | `0.45` (exactly the default) |
| `thermal_gen_modern` | 1280x720 (`soft 1.5`) | `0.006` | `0.001` | `0.56` |

There is no choppy or laggy picture: no frame is skipped.

## The look

Built at "sensor size": the view's size divided by `r1_thermal_soft` (default `3`, so 640x360 on a
1080p screen), then stretched over the view. That is what makes it soft, and cheap.

| var | range | default | what it does |
|---|---|---|---|
| `r1_thermal_soft` | `1..8` | `3` | screen pixels per sensor pixel. Higher = softer, blockier and cheaper |
| `r1_thermal_top` | `0..1` | `1` | grey level of heat `1` (fire) where it faces the camera. A thing of heat `h` is `h` of the way from the world's grey to it |
| `r1_thermal_bloom` | `0..2` | `0.5` | extra glow around the very hot (heat above `0.93`: fire). `0` = none |
| `r1_thermal_body` | `0..1` | `0.95` | only with `r1_thermal_ladder 0`: the level of every warm body, as in round 2 |
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
| `r` | heat, `0..1`, the ladder's values. `0` = nothing warm visible at this pixel. A living body is `0.85`, fire `1`; low values around a fire are what it warms |
| `g` | `1` where the warm pixel is the player's own hands (HUD), else `0` |
| `b` | sky flag: `1` where nothing but the sky was drawn (also behind smoke and glass against the sky), else `0` |
| `a` | edge term of a warm model: `0` where its surface faces the camera, towards `1` where it is seen edge-on; `0` elsewhere |

The mask is an `A8R8G8B8` texture (if the GPU refuses that, the log says so and `a` reads `1`).

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
| `r1_thermal_ladder` | `0..1` | `1` | the heat ladder. `0` = every creature is `1`, the top level is `r1_thermal_body` (round 2) |
| `r1_thermal_gear` | `0..1` | `0.9` | worn script attachments, as a share of the wearer's heat |
| `r1_thermal_lamp` | `0..1` | `0.45` | a lit lamp |
| `r1_thermal_lamp_size` | `0..1` | `0.25` | the warm spot of a level lamp, as a share of its glow's size. `0` = level lamps stay cold |
| `r1_thermal_barrel` | `0..1` | `0.9` | a gun's muzzle at full barrel temperature |
| `r1_thermal_barrel_len` | `0..3` m | `0.45` | how far from the muzzle the barrel's heat reaches. `0` = the whole gun at the barrel's heat (round 2) |
| `r1_thermal_gun` | `0..1` | `0.25` | the rest of the gun at full barrel temperature |
| `r1_thermal_fire_spread` | `0..1` | `0.6` | heat an open fire gives to what is right at it. `0` = off |
| `r1_thermal_fire_reach` | `0..10` m | `0.6` | how far beyond the flames that reaches |
| `r1_thermal_wet` | `0..1` | `0.2` | share of its heat a soaked creature loses. `0` = off |
| `r1_thermal_sun` | `0..1` | `0.07` | heat direct sun adds to a creature. `0` = off |
| `r1_thermal_force_rain`, `r1_thermal_force_sun` | `-1..1` | `-1` | testing: rain density / sun strength instead of the weather's. `-1` = the weather |
| `r1_thermal_lightning` | `0..2` | `0.6` | the flash of the thermal sky with a lightning bolt. `0` = off |
| `r1_thermal_particles` | `0..1` | `1` | warm particle effects on / off |
| `r1_thermal_sync` | `0..1` | `1` | wait for the frame's skeleton poses before drawing, so the heat redraw matches the scene. `0` = as round 2, for comparing if bodies flicker |
| `r1_thermal_depth` | `0..50` cm | `2` | the heat redraw of world models is pulled this far towards the camera, the same at any range. Raise it if warm bodies still show holes while they move; a body then shows through cover thinner than this |
| `r1_thermal_bias` | `0..4096` | `8` | depth bias of the redraw, in steps of a 24-bit depth buffer (the first-person hands and gun only have this one) |
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
| `thermal_sensor` | lane | the device's quality: sensor pixel size, grain, fixed pattern, contrast factor ("Generations") |
| `obj_hotness` | object lane (see `shader_bus.md`, "Object hotness") | `shader_bus.set_object(token, obj, x, 0, 0, 0)` gives that object hotness `x` (`0..1`) on every surface. `bus_force obj_hotness x 0 0 0` gives every object `x` on its own model and on marked textures |

On R1 `obj_hotness` only works for objects drawn as dynamic models (creatures, items, physics
objects). It is read when the object is queued, so it follows the object from the next frame.

## Changed for mods since round 2

- Mask `r`: a living body is `0.85`, not `1`; only fire is `1`. A look that tested `r > 0.9` for
  "a body" has to test lower (`r > 0.1` finds everything warm). Low values around fires are new.
- New files: `shaders\r1\heat_spot.ps`; `configs\r1_thermal.ltx` has four new sections for creatures.
- New lane `thermal_sensor`. New console vars: the ladder, fire spread, wet, sun, lightning, lamp
  size, barrel length, gun body, sync, depth (tables above). Defaults changed: `r1_thermal_gear`
  `0.8 -> 0.9`, `r1_thermal_lamp` `0.65 -> 0.45`, `r1_thermal_barrel` `0.8 -> 0.9`. Console vars are
  saved in `user.ltx`, so a PC that ran round 2 keeps the old three until they are set again.
- `IDSGraphManager::set_ItemHeat` has a second form with a heat point.
- A mod that ships its own `heat_mask.vs`, `heat_mask_lq.vs` or `heat_mask.ps` has to take the new
  ones: the heat of a pixel now comes from the vertex shader (`TEXCOORD1.y`; new constants
  `r1_heat_vs` = heat at the heat point, heat far from it, depth pull in metres, and `r1_heat_point` =
  the point and 1 / its reach), and `heat_mask.ps` no longer reads `r1_heat_item.x`.
- `thermal_down.ps` writes "very hot" into green, `thermal_look.ps` adds it as bloom (new constant
  `thermal_hot`). A mod's own look that reads `$user$heat` directly is not affected.
- No thermal while a menu is open. With a region, `$user$thermal` only holds the box around the
  circle (with `r1_thermal_look 2` it holds the whole picture, as before).
- Round 2's picture: `r1_thermal_ladder 0`, `r1_thermal_fire_spread 0`, `r1_thermal_wet 0`,
  `r1_thermal_sun 0`, `r1_thermal_lightning 0`, `r1_thermal_lamp_size 0`, `r1_thermal_barrel_len 0`
  and the three old defaults above (`cfg_load thermal_r2`; `cfg_load thermal_r3` puts round 3 back).

## Limits

- A warm thing is hidden by anything that writes depth in front of it (walls, terrain, the gun,
  grass, leaves). It is not hidden by things that do not write depth: glass, smoke, particles.
- Hair or fur drawn with see-through textures shows as the full polygon.
- A gun whose model has no usable fire point shows whole, at half the barrel's heat.
- A corpse that was already dead when it came online starts cooling at that moment.
- A lamp built into the level is only warm if the level has a glow on it; fires without particles are cold.
- A fire's warm spot is a round patch facing the camera: it lies on whatever is at the fire and
  behind it. The barrel or pit a fire burns in is level geometry and only gets that spot.
- World surfaces do not follow sun, shade or rain. The honest way needs every surface's baked sun and
  sky exposure in the thermal pass, and R1's world shaders only write colour. It would take
  replacement world pixel shaders (the `lmap*`, `vert*`, `impl*` families, about twenty files) that
  also write the sun term into the alpha of the main target, and a main target with alpha. Any mod
  that ships its own copies of those files would switch it off or break it, so it was not done here.
- A creature's wetness and sun warmth are only tracked while it is drawn with thermal on.
- Mirrors of the scene that are not the main view or the small SVP lens have no mask: a full-size
  second viewport (`r1_svp_size 0`), light and shadow passes.
- The lens mask and the lens look are as old as the lens picture (SVP frames are not rendered every frame).
- The region is a circle. Other shapes: `r1_thermal_look 2` and a mod shader.

## Cost

Off: one flag test and one bus lookup per frame.

On, per frame (1080p, `r1_thermal_soft 3`):

- the mask: one clear of a screen-size target, one screen quad for the sky flag (only sky pixels pass
  the depth test), one extra draw call per visible warm mesh part with a 5-instruction pixel shader,
  one extra draw per visible warm particle effect, and one draw call for all soft spots together (one
  quad per open fire and per visible lamp glow);
- the look: five passes on small targets (640x360 twice, 320x180 twice, 640x360 again), together
  about as many texture reads as two and a half screen-size ones, then one screen quad with one
  texture read (in place of the post-process pass) or a quad over the circle;
- with a region (the tube), the five small passes only work on the box around the circle: a circle
  of radius `0.35` is about a quarter of the sensor;
- the same on SVP frames for the lens, at the lens's size.

On the CPU, per frame: one distance test per dynamic model against the open fires of the last frame
(only while a fire is in view), one small update per warm creature, and, when the worker that
computes skeleton poses is not done yet as drawing starts, a short wait for it (`r1_thermal_sync`).

Memory: the mask (8 MB at 1080p), five small targets (about 4 MB), a 128x128 noise texture; for the
lens, two masks and five small targets more. Created on first use, kept until `vid_restart`.

If it is slow, lower in this order: `r1_thermal_soft 4` (or `5`), `r1_thermal_particles 0`,
`r1_thermal_targets 1` (no lens work).

## Files

The engine's game data must be installed: in `gamedata\shaders\r1\` the files `heat_mask.vs`,
`heat_mask_lq.vs`, `heat_mask.ps`, `heat_flat.ps`, `heat_particle.vs`, `heat_particle.ps`, `heat_spot.ps`,
`thermal_noise.ps`, `thermal_down.ps`, `thermal_blur.ps`, `thermal_look.ps`, `thermal_palette.ps`,
`thermal_show.ps`, `thermal_show_region.ps`, and `gamedata\configs\r1_thermal.ltx`. No other mod ships
files with these names, so their place in the load order does not matter. `thermal_demo.ps` and
`thermal_demo_paint.ps` from round 1 are no longer used and can stay or go.
