# Audio API (headphones): listener-side mix controls

Engine controls for a headphones feature. A Tarkov-style headset boosts other people's footsteps, lowers background
noise, lifts quiet sounds and squashes loud ones like gunshots. Scripts and items are written separately. This page
lists the engine functions those scripts call.

**What this changes:** only what the player hears, through the gain and the distance rolloff of each OpenAL source.

**What this never changes:**
- AI hearing: sound power, `p_source.volume`, `max_ai_distance`, the sound events, or the `feel_sound` path.
- Anything saved or sent over the network.

Every value goes back to its default when the game starts. Call `audio_reset(0)` to reset by hand.

Code:
- `src/xrSound/SoundRender_Mix.h`
- the "Listener-side mix" block at the end of `src/xrSound/SoundRender_Core.cpp`
- `CSoundRender_TargetA::fill_parameters`
- `CSoundRender_Emitter::update_culling` (the level each sound has at the listener, for the limiter and the compressor)
- the Lua exports in `src/xrGame/script_sound_script.cpp`

Test: `tests/audio_mix_test.cpp`. CI runs it on every push.

## Feature mask

```lua
local bits = audio_api and audio_api() or 0
```

| bit | meaning |
|---|---|
| 1 | `audio_set_category_gain`, `audio_get_category_gain`, `audio_get_category_target` |
| 2 | `audio_set_steps_others`, `audio_get_steps_range` |
| 4 | `audio_set_limiter`, `audio_set_limiter_detect`, `audio_get_limiter_enabled`, `audio_get_limiter_reduction`, `audio_get_loud_level` |
| 8 | gains above 1 really get louder (up to 4) |
| 16 | `sound_object:set_audio_category(name)`, `sound_object:get_audio_category()` |
| 32 | `audio_reset` |
| 64 | compressor: `audio_set_compressor`, `audio_set_compressor_detect`, `audio_set_compressor_category`, `audio_get_compressor_category`, `audio_get_compressor_enabled`, `audio_get_compressor_gain`, `audio_get_compressor_level` |
| 128 | distance curve: `audio_set_category_rolloff`, `audio_get_category_rolloff` |

Bits 64 and 128 are new on branch `headphones-compressor`; the `headphones-audio` exe reports 63.

Use `bit.band(audio_api(), 4) ~= 0` (or the bit library you use elsewhere) before calling a function. An exe without
this feature has no `audio_api` at all.

## Categories

Each playing sound is in exactly one category. The engine picks it in this order:

1. **A tag set where the sound is created or played.** These tags exist:

| category | what is in it | where it is tagged |
|---|---|---|
| `weather` | rain loop (`ambient\rain`) | `xrEngine/Rain.cpp` |
| `weather` | thunder claps | `xrEngine/thunderbolt.cpp` |
| `weather` | environment ambient sounds (the `sounds` of ambient channels and the `sound` of ambient effects in the weather / ambient configs) whose file path contains `wind`, `rain`, `thunder` or `storm` | `xrEngine/Environment_misc.cpp` |
| `ambient` | the other environment ambient channel / effect sounds | `xrEngine/Environment_misc.cpp` |
| `ambient` | static level sounds (`level.snd_static`) | `xrGame/level_sounds.cpp` |
| `ambient` | random level sounds (`Sounds_Random` of the level) | `xrGame/Level_load.cpp` |
| `steps_others` | footsteps of every NPC and monster (`CStepManager`, entity ID ≠ 0) | `xrGame/step_manager.cpp` |
| any | a script's `sound_object` after `snd:set_audio_category(name)` | your script |

2. **No tag:**
   - `music`: `st_Music` sounds (level music tracks) and intro / video sounds.
   - `ui`: 2D sounds with no game object (menus, UI clicks, most PDA sounds).
   - `world`: everything else. That is weapons (yours and others'), bullet impacts, explosions, voices, monsters (except their steps), physics, anomalies, items, and **your own footsteps**.

Notes:
- **Wind.** Anomaly has no separate wind sound. Wind is part of the environment ambient channels, which is why
  files with `wind` in the path go to `weather`. A wind bed whose file name has none of the four words lands in
  `ambient`.
- **Your own weapon.** Your first-person weapon sounds are in `world`. This includes HUD sounds, which play in 2D with
  you as the object.
- **The limiter** works on every category except `ui` and `music`.
- **The compressor** works on `world` and `steps_others` unless a script changes the set. It can never work on `ui`
  or `music`.

## Measurements: why footsteps were hard to hear

The owner's test of headphones 0.1: "You can feel the changes, but I feel like it's so miniscule." Before adding
anything, the levels were worked out from the sound files and the engine code. Nothing here was measured in a running
game.

**How loud a sound is at the listener** (sliders at 1, nothing in the way):

- The engine's own fade, `CSoundRender_Emitter::update_culling`: `base_volume x min(volume, (max - d) / (max - min))`,
  silent beyond `max`. The result is capped at 1. `volume` is what the caller passes; for a footstep it is the step
  power. Note that it caps the fade instead of scaling it.
- On top of that, OpenAL's own distance model for every mono 3D source. The engine never sets one, so it is OpenAL's
  default, inverse distance clamped: `min / (min + 0.75 x (d - min))` (0.75 is `psSoundRolloff`).
- `min`, `max` and `base_volume` come from each OGG file's comment. Stereo files play in 2D with no distance.

**Data:** Anomaly 1.5.3 sound files (the public unpacked game data, `Tosox/STALKER-Anomaly-gamedata`, branch
`v1.5.3`; weapon sounds from the devkit's vanilla copy). Each row is the median over the files of its group:
154 step files (`material\human\step`), 165 combat and 85 idle voice files of one voice set, 473 shot files,
75 silenced shot files, 229 nature bed files, 8 thunder files. Step powers are from `[stalker_step_manager]` in
`creatures\m_stalker.ltx`. The owner's game also runs sound mods (JSRS for guns, Soundscape Overhaul for ambience);
their files were not available and are not in this table.

Gain at the listener in dB (0 = the file at full volume; "silent" = beyond the file's maximum distance):

| Sound | min / max m, base volume | 5 m | 15 m | 30 m | 50 m | the file itself: peak / average dBFS |
|---|---|---|---|---|---|---|
| NPC step, crouching (power 0.10) | 1.5 / 40, 1.0 | -28.8 | -37.8 | -43.7 | silent | -7 / -30 |
| NPC step, walking (power 0.25) | 1.5 / 40, 1.0 | -20.8 | -29.8 | -35.7 | silent | -7 / -30 |
| NPC step, running or sprinting (power 0.8 to 2.3) | 1.5 / 40, 1.0 | -9.6 | -21.5 | -35.4 | silent | -7 / -30 |
| Voice, combat shout | 3 / 150, 1.0 | -3.6 | -12.8 | -19.5 | -25.5 | 0 / -12 |
| Voice, idle talk | 2 / 50, 1.0 | -7.1 | -18.1 | -28.8 | silent | 0 / -14 |
| Gunshot | 5 / 300, 2.0 | 0.0 | -8.0 | -13.5 | -17.8 | 0 / -17 |
| Gunshot, silenced | 2 / 35, 1.0 | -7.5 | -21.2 | -43.8 | silent | 0 / -17 |
| Rain loop (2D, no distance) | base 0.65 | -3.7 x rain strength | same | same | same | -3 / -24 |
| Thunder clap (the code sets its range to half and twice its distance) | base 1.9 | -4.9 | same | same | same | 0 / -18 |
| Wind bed | 10 / 150, 1.0 | 0.0 | -3.0 | -8.9 | -14.1 | -10 / -25 |
| Ambient bed: birds, crows, crickets. The game places them 30 to 160 m from the camera (-19.7 dB at 90 m, -25.8 dB at 140 m) | 10 / 300, 1.0 | never this near | never this near | -8.6 | -13.3 | -9 / -26 |

**Where a walking step at 15 m loses its 29.8 dB:**

| Part | Loss |
|---|---|
| Step power 0.25 (walking; running is 0.8 to 1.0, crouching 0.1) | -12.0 dB |
| OpenAL's inverse distance, with the step files' 1.5 m reference distance | -17.8 dB |
| The engine's linear fade to 40 m | 0 dB (it only bites below the power cap, from 30 m on) |

At the same 15 m the inverse distance costs a gunshot 8.0 dB (5 m reference) and a shout 12.0 dB (3 m reference).

**What this says:**

1. **It is all three: gain, distance falloff and masking, and falloff is the biggest.** A walking NPC's step is
   12 dB down by its step power and a further 18 dB down by distance at 15 m.
2. **Masking.** At 15 m that step (-29.8 dB, and its file peaks 7 dB under full scale) sits 16 to 21 dB under an
   ambient bed 30 to 50 m away, 26 dB under full rain, 22 dB under a gunshot and 17 dB under a shout at the same 15 m.
3. **Why headphones 0.1 felt minuscule.** Its footstep boost (at most x4, +12 dB) leaves the walking step at 15 m at
   -17.8 dB, still under the beds. Its range factor only stretches the engine's linear fade and the 50 m step gate,
   and neither is what makes steps quiet: most step files are silent beyond 40 m and under -35 dB from 30 m on.
4. **The limiter reads distant shots as far louder than they are.** Its detector leaves out OpenAL's distance part
   and does not cap the base volume of 2 that shot files have. A shot 150 m away reads 0 dB while it is heard at
   -27 dB, and every shot within about 260 m reads above the limiter's default -12 dB threshold. So with the limiter
   on, a firefight anywhere in earshot ducks the whole world, footsteps included. The limiter is left as it was,
   because headphones 0.1 was tuned and tested with it. The compressor's detector uses the level as heard, so a
   headset script that switches to the compressor should switch the limiter off.

**What the controls do to an NPC's walking step** (same model, median over the step files):

| Setting | 5 m | 15 m | 30 m | 50 m |
|---|---|---|---|---|
| Nothing set | -20.8 | -29.8 | -35.7 | silent |
| Step gain x4 (the most 0.1 could do) | -8.8 | -17.8 | -23.7 | silent |
| Step rolloff 0.25 | -15.2 | -20.6 | -25.2 | silent |
| Step rolloff 0 (no inverse distance at all) | -12.0 | -12.0 | -12.0 | silent |
| Step rolloff 0.25, range 1.5 | -15.2 | -20.6 | -25.2 | -32.3 |
| Step rolloff 0.25, range 1.5, gain x2.8 | -6.3 | -11.7 | -16.3 | -23.4 |
| The suggested headset settings below, compressor included | -10.4 | -11.9 | -13.1 | -17.7 |

So the answer to the measurements is a distance-curve control for footsteps and a compressor with makeup gain. The
gain cap stays where it is (see [Gain above 1](#gain-above-1)).

## Functions

All gains are linear: 1 = unchanged, 0.5 ≈ -6 dB, 2 ≈ +6 dB. Out-of-range values are clamped. NaN falls back to the
default.

### `audio_set_category_gain(category, gain, fade_ms) -> bool`

Fades one category's gain to `gain` over `fade_ms`.
- `category`: `"world"`, `"ambient"`, `"weather"`, `"steps_others"`, `"ui"` or `"music"`. An unknown name returns
  `false` and changes nothing.
- `gain`: 0 … 4. Default 1.
- `fade_ms`: 0 … 60000. 0 jumps to the new gain.

The fade is a linear ramp in real time, so it keeps running when the game is paused or time is slowed. If you call it
again during a fade, the new fade starts from the current gain.

### `audio_get_category_gain(category) -> number`

The current gain, part way through a fade. Returns 1 for an unknown name.

### `audio_get_category_target(category) -> number`

The gain the category is fading to.

### `audio_set_steps_others(gain, range_k, fade_ms)`

Footsteps of NPCs and monsters, never your own.
- `gain`: 0 … 4. Default 1. This is the `steps_others` category gain, so it is the same setter as
  `audio_set_category_gain("steps_others", gain, fade_ms)`.
- `range_k`: 0.1 … 4. Default 1. It scales two things, for **sound only**:
  1. the engine's step gate, which only plays steps within `step_sound_distance` (50 m by default) of the camera;
  2. the step sound's own maximum audible distance, which also stretches the engine's linear fade with distance.

  The `eOnFootStep` script callback still fires only within `step_sound_distance`. The AI never hears footsteps from
  this path at all: steps play through `play_no_feedback`.
- `fade_ms`: 0 … 60000. Applies to the gain only. The range applies to steps that start after the call.

### `audio_get_steps_range() -> number`

The current `range_k`.

### `audio_set_limiter(enabled, threshold_db, attack_ms, release_ms, ratio)`

The loud-sound limiter.
- `enabled`: true / false. Default false. When you disable it, the current reduction releases over `release_ms`; it
  does not jump.
- `threshold_db`: -60 … 0. Default -12. The detector level above which the limiter reduces.
- `attack_ms`: 0 … 1000. Default 5. How fast the reduction deepens. Time constant of a one-pole smoother on the
  reduction in dB.
- `release_ms`: 0 … 10000. Default 400. How fast it recovers.
- `ratio`: 1 … 100. Default 20. Each dB over the threshold becomes 1/ratio dB. 20 or more behaves as a limiter;
  2 to 6 as a gentle compressor.

**How it works.** Once per sound update, the detector takes the loudest **loud** sound playing.
- A sound is loud when its AI sound type has a bit of the detect mask. By default that is shots and explosions
  (`CExplosive` types its explosion sound as weapon shooting).
- The level of that sound at the listener is `base volume × distance falloff × occlusion × fade`, in dB. It is
  measured before the user's volume sliders. A shot next to you is about 0 dB. Half its audible distance away is
  about -6 dB.

From that level, the limiter computes:
- **Gain reduction (dB, ≤ 0):** `-(level_db - threshold_db) × (1 - 1/ratio)` when the level is over the threshold.
  Attack and release smooth it.
- **Gain:** `10^(reduction/20)`. It multiplies every source except `ui` and `music`. That includes the loud sound
  itself, so a shot is squashed along with everything else.

**Limits.**
- The engine knows the gain of a sound but not its waveform. The limiter therefore follows the whole sound: it
  ducks for as long as a shot or explosion sample plays (its tail included), then releases. Peaks inside a sample are
  not tracked.
- A real per-sample limiter would need DSP on the mixed output. Neither OpenAL Soft nor this engine exposes that (see
  [EFX](#openal-soft-efx) below).

### `audio_set_limiter_detect(game_type_mask, own_k)`

Which sounds count as loud.
- `game_type_mask`: bits of `ESoundTypes` (`src/xrServerEntities/ai_sounds.h`). A sound counts when its type has any
  of these bits. Default `0x00200100`, which is `SOUND_TYPE_SHOOTING | SOUND_TYPE_OBJECT_EXPLODING`.
  - Add `0x00080000` (bullet hits) to duck on nearby impacts too.
  - Don't use the class bits such as `SOUND_TYPE_WEAPON` (`0x80000000`). They match every weapon sound, reloads and
    clicks included.
- `own_k`: 0 … 4. Default 1. A weight on 2D loud sounds, which in practice are your own first-person shots.
  - 0: your own shots never trigger the limiter. They are still reduced while someone else's shot or an explosion
    triggers it.
  - Above 1: your shots count as louder than they are.

### `audio_get_limiter_enabled() -> bool`

Whether the limiter is on.

### `audio_get_limiter_reduction() -> number`

The current gain reduction in dB (≤ 0). Use it for a debug HUD.

### `audio_get_loud_level() -> number`

The current detector level in dB. -120 means nothing loud is playing.

### `audio_set_compressor(enabled, threshold_db, ratio, attack_ms, release_ms, makeup_db, knee_db)`

A compressor on the compressed categories (`world` and `steps_others` by default). It is what a Tarkov headset
does: quiet sounds come up, loud ones go down, and after a loud sound the boost swells back.

- `enabled`: true / false. Default false. When you disable it, the gain goes back to 0 dB over `release_ms`; it does
  not jump.
- `threshold_db`: -60 ... 0. Default -24. Sounds above this level bring the gain down.
- `ratio`: 1 ... 20. Default 4. Each dB over the threshold comes out as 1/ratio dB.
- `attack_ms`: 0 ... 1000. Default 10. How fast the gain falls when something loud starts.
- `release_ms`: 0 ... 10000. Default 250. How fast it comes back.
- `makeup_db`: -24 ... 24. Default 0. The gain while nothing is over the threshold. This is the part that lifts quiet
  sounds: steps, gear, distant voices and movement.
- `knee_db`: 0 ... 24. Default 6. The width of the soft bend around the threshold. 0 is a hard corner.

**How it works.** Once per sound update:

1. **Detector.** The engine takes the loudest counted sound in the compressed categories. Its level is the level as
   heard: `base volume x engine fade x OpenAL distance x occlusion x fade-in/out`, times its category gain and the
   limiter's gain, before the user's volume sliders. Which sounds count is set by `audio_set_compressor_detect`.
2. **Gain computer.** `reduction = (1/ratio - 1) x (level - threshold)` above the knee, 0 below it, and a smooth
   parabola inside the knee.
3. **Gain.** `makeup + reduction`, smoothed with the attack time when it falls and the release time when it rises.
4. Every source in the compressed categories is multiplied by that one gain, on top of its category gain and the
   limiter.

With threshold -24, ratio 4, knee 6 and makeup +10:

| The loudest sound playing | Its level | Compressor gain | It comes out at |
|---|---|---|---|
| Nothing loud (quiet scene) | under -27 dB | +10 dB | everything in the set is 10 dB louder |
| Idle voice at 15 m | -18.1 dB | +5.6 dB | -12.5 dB |
| Shout at 15 m | -12.8 dB | +1.6 dB | -11.2 dB |
| Gunshot at 30 m | -13.5 dB | +2.1 dB | -11.4 dB |
| Gunshot at 15 m | -8.0 dB | -2.0 dB | -10.0 dB |
| Shout at 5 m | -3.6 dB | -5.3 dB | -8.9 dB |
| Your own shot | 0 dB | -8.0 dB | -8.0 dB |

A 30 dB spread of inputs comes out within about 5 dB. While the loud sound plays, every other sound in the set gets
the same gain, so quiet sounds dip with it and come back over the release time.

**Limits.**
- Like the limiter, the detector knows a sound's gain, not its waveform. It follows a sound for as long as its sample
  plays. A sample that is quiet inside (a hum recorded at a low level) still counts at its full gain.
- The chain is category gain, then limiter, then compressor. With both on, the compressor sees a shot after the
  limiter has reduced it. Use the compressor alone: the limiter also ducks for distant shots that are not loud at
  the listener (see the measurements, point 4).
- Change the category set before you enable the compressor. A category added or removed while the gain is not 0 dB
  jumps.

### `audio_set_compressor_detect(game_type_mask, own_type_mask, own_k)`

Which sounds drive the compressor. Bits are those of `ESoundTypes` (`src/xrServerEntities/ai_sounds.h`); a sound
counts when its type has any bit of the mask. Sounds outside the compressed categories never count.

- `game_type_mask`: for 3D sounds. Default `0x07FFFF00`: every typed sound except ambient-typed ones. That is shots,
  explosions, hits, voices, steps, item and physics sounds. Untyped sounds never count.
- `own_type_mask`: for 2D sounds, which in the `world` category are your own. Default `0x00200100`: your shots and
  explosions only. Your own steps, breathing loops and item sounds are left out on purpose. They play at full gain
  for their whole sample, so they would pump the world with every step, and a looping one would hold the boost down
  for good.
- `own_k`: 0 ... 4. Default 1. A weight on those 2D sounds. Below 1 your own shots squash less.

To make your own footsteps count too (as in Tarkov, where moving costs you hearing), add `0x00008000` to
`own_type_mask`.

### `audio_set_compressor_category(category, enabled) -> bool` / `audio_get_compressor_category(category) -> bool`

Puts a category into the compressed set or takes it out. Default set: `world` and `steps_others`.
- `"ambient"` and `"weather"` can be added. Then their beds get the makeup gain too and dip with loud sounds, and
  their typed sounds can drive the detector.
- `"ui"`, `"music"` and unknown names return `false`: they are never compressed.

### `audio_get_compressor_enabled() -> bool`

Whether the compressor is on.

### `audio_get_compressor_gain() -> number`

The current compressor gain in dB: makeup plus reduction, smoothed. Use it for a debug HUD.

### `audio_get_compressor_level() -> number`

The current detector level in dB. -120 means nothing counted is playing.

### `audio_set_category_rolloff(category, k) -> bool`

The distance curve of one category.
- `category`: any of the six names. An unknown name returns `false`.
- `k`: 0 ... 2. Default 1. A factor on OpenAL's rolloff for the sources of that category.
  - 1: unchanged.
  - Below 1: sounds lose less with distance. Near sounds stay as they are, far ones get louder.
  - 0: no inverse-distance loss. Only the engine's linear fade to the file's maximum distance is left.
  - Above 1: sounds die away faster.

It applies at once, also to sounds that are already playing. It does nothing to 2D sounds. It changes only the
OpenAL source; the sound's own distances, its volume and the AI's hearing range are not touched.

For `steps_others`, 0.25 gives +5.6 dB at 5 m, +9.2 dB at 15 m and +10.5 dB at 30 m (table above). Use it together
with `audio_set_steps_others(gain, range_k, ...)`: the range factor moves where steps go silent (40 m for most step
files), the rolloff makes them audible on the way there.

### `audio_get_category_rolloff(category) -> number`

The current factor. Returns 1 for an unknown name.

### `audio_reset(fade_ms)`

Puts everything back to the defaults:
- every category fades to 1 over `fade_ms`;
- steps range goes to 1 and every category's rolloff to 1;
- the limiter is switched off (its reduction releases);
- the limiter's detect mask goes back to the default and `own_k` to 1;
- the compressor is switched off (its gain goes back to 0 dB over its release time), its category set goes back to
  `world` and `steps_others`, and its detect masks and `own_k` go back to the defaults.

### `sound_object:set_audio_category(name) -> bool` / `sound_object:get_audio_category() -> string`

Tags what a script `sound_object` plays from now on.
- `name`: one of the six categories, or `"auto"` (no tag, the default).
- `get_audio_category` returns the name or `"auto"`.

Use this for sounds a script plays itself, for example a scripted ambient bed:
`snd:set_audio_category("ambient")` before `snd:play(...)`.

## Gain above 1

**Before this change, a gain above 1 did not get louder.** Two things stopped it:
- `CSoundRender_TargetA::fill_parameters` clamped the source gain to `[EPS_S, 1]`.
- Every source was created with `AL_MAX_GAIN = 1`. OpenAL Soft clamps the source gain to `[AL_MIN_GAIN, AL_MAX_GAIN]`
  before it multiplies by the listener gain.

**Now:**
- The engine still clamps its own gain to 1, exactly as before. That gain is volume × distance × occlusion × sliders.
- The mix factor is applied after that clamp. The result is clamped to `[0, 4]`.
- `AL_MAX_GAIN` of every source is 4 (`SOUND_MIX_MAX_GAIN` in `SoundRender_Mix.h`).

With no script calls the mix factor is exactly 1, so every source gets the same gain as before.

**Clipping.** A boost can push the mixed output past full scale. OpenAL Soft's output limiter catches that for
integer output formats, the usual 16-bit device output; it is on by default for them (`output-limiter` in
`alsoft.ini`). With float output there is no limiter. Prefer moderate boosts (≤ 2 to 3), and cut what you don't need
rather than boosting.

**The cap was not raised for the compressor.** The limit is on the final gain of a source (engine gain, at most 1,
times every mix factor): it may not go over 4. The measurements say why that is enough:
- The sounds that need lifting are far below full volume. A walking step at 15 m is at -29.8 dB. The rolloff, the
  step gain and the makeup together can add well over 30 dB to it before the cap matters.
- A higher cap would only let sounds that are already near full volume go further over it, which is where clipping
  comes from.
- The compressor works against clipping: its detector reads levels after the category gains, so a boosted loud sound
  brings the gain down. With threshold -24 and ratio 4, the loudest possible source (+12 dB) comes out at
  `-15 + makeup` dB.

The product of the factors can be more than 4 for a quiet source (for example step gain 4 x makeup +12 dB). That is
fine: only the final gain is capped.

Listener `AL_GAIN` is not used for any of this. `CRenderDevice` sets it to 0 / 1 during precache, which would
overwrite it.

## OpenAL Soft EFX

The tree builds OpenAL Soft 1.23.1 from source (`src/3rd party/OpenAL-new`). It has `efx.h`. The engine already
loads the EFX entry points, but uses them only for EAX reverb on an auxiliary slot.

| EFX feature | Fits the headset? |
|---|---|
| `AL_EFFECT_COMPRESSOR` | No. It has a single on/off parameter (no threshold, ratio, attack or release). It only runs on an auxiliary effect slot (the wet send), not on the dry signal. |
| `AL_EFFECT_EQUALIZER` | No for the dry signal: it is also slot-only. Usable only for the reverb send. |
| Direct filters (`AL_FILTER_LOWPASS` / `HIGHPASS` / `BANDPASS`, through `AL_DIRECT_FILTER`) | Yes, this is the usable part. They work per source on the dry path. |

The direct filters would be the way to add a "headset tone", for example a high-pass on ambient or a band-pass for
cheap headsets. That is a possible next step; it is not in this change.

The limiter, the compressor and the category gains above are therefore done in software, on the source gains.

## Defaults and what they cost

With no script calls:
- Every factor is 1, the limiter and the compressor are off, the step range is 1 and every rolloff is 1. Each
  source gets exactly the gain and the rolloff it had before.
- Per frame: one pass over the playing emitters for the limiter and one for the compressor, each only while it is
  enabled, plus a few multiplies per source and one distance formula per playing sound.

## Suggested headset settings

A starting point for a good headset, from the measurements. Not played yet.

```lua
audio_set_limiter(false, -12, 5, 400, 20)             -- the compressor replaces it
audio_set_category_rolloff("steps_others", 0.3)       -- steps lose less with distance
audio_set_steps_others(2.0, 1.5, 750)                 -- +6 dB, audible to about 60 m
audio_set_compressor(true, -24, 4, 10, 250, 10, 6)    -- squash loud, lift quiet by 10 dB
```

What an NPC's footsteps then come out at (dB, quiet scene):

| | 5 m | 15 m | 30 m | 50 m |
|---|---|---|---|---|
| Crouching, nothing set | -28.8 | -37.8 | -43.7 | silent |
| Crouching, these settings | -12.4 | -14.3 | -18.4 | -22.3 |
| Walking, nothing set | -20.8 | -29.8 | -35.7 | silent |
| Walking, these settings | -10.4 | -11.9 | -13.1 | -17.7 |
| Running, nothing set | -9.6 | -21.5 | -35.4 | silent |
| Running, these settings | -7.5 | -9.5 | -11.6 | -17.7 |

A walking step at 15 m gains 18 dB and ends up as loud as a shout at the same distance. Your own shot comes out
8 dB down. For a cheaper headset, use less makeup (4 to 6 dB) and a rolloff nearer 1 (0.5 to 0.6).

The same two step lines without the compressor (`audio_set_category_rolloff` and `audio_set_steps_others` only) are
the "louder steps for everyone" setting: they need no headset.

## Test plan (in game)

Run these from the debug console with a Lua line, e.g. `run_string` / a debug script, or from your headset script.
Check the effect by ear, and with `audio_get_*` printed to the log.

1. **Feature check.** `printf("audio_api %s", audio_api())` prints 255.
2. **Ambient / weather.**
   1. Stand outside in rain.
   2. `audio_set_category_gain("weather", 0, 2000)`: the rain fades out over 2 s. Thunder is silent.
   3. `audio_set_category_gain("weather", 1, 2000)`: it fades back.
   4. Do the same with `"ambient"`, away from rain: birds, wind beds (unless the file says wind), the level's static
      sounds.
   5. Gunshots and voices must not change.
3. **World.** `audio_set_category_gain("world", 0.2, 0)`: weapons, voices and your own steps get quiet. Menus and
   music do not.
4. **Others' steps.**
   1. Near a walking NPC, `audio_set_steps_others(3, 1, 0)`: their steps are clearly louder. Your own steps are not.
   2. `audio_set_steps_others(3, 2, 0)`: steps of NPCs 50 to 100 m away become audible.
   3. `audio_set_steps_others(1, 1, 500)` returns to normal.
5. **Boost.** `audio_set_category_gain("world", 4, 0)` is clearly louder than 1. Without this change it was not. Put
   it back to 1.
6. **Limiter.**
   1. `audio_set_limiter(true, -12, 5, 400, 20)`.
   2. Fire your weapon while rain or an ambient bed plays: the bed and the shot itself dip, and recover about 0.4 to
      1 s after the shot's tail.
   3. Print `audio_get_limiter_reduction()` and `audio_get_loud_level()` from a timer: about -11 dB / 0 dB right
      after your own shot, back to 0 / -120 a bit later.
   4. `audio_set_limiter_detect(0x00200100, 0)`: your own shots no longer duck, an NPC's nearby shots still do.
   5. `audio_set_limiter(false, -12, 5, 400, 20)`: no more ducking.
7. **AI hearing unchanged.**
   1. With `audio_set_category_gain("world", 0, 0)` and `audio_set_steps_others(0, 4, 0)`, sneak up on an NPC, then
      fire a shot near one.
   2. They must react exactly as with all gains at 1. The same goes for the limiter.
   3. Compare against the same save with `audio_reset(0)`.
8. **Save / load.** Set some gains, save, load. Everything is back to 1 after a game restart, and nothing about it is
   in the save. Within one session the values persist across loads until a script changes them; that is intended, so
   the headset script sets them on load.
9. **Music / UI.** `audio_set_category_gain("music", 0, 1000)` fades level music. `"ui"` changes menu clicks. Neither
   is touched by the limiter.
10. **Step distance curve.**
    1. Stand 15 to 30 m from a walking NPC, in a quiet place. Their steps are faint or inaudible.
    2. `audio_set_category_rolloff("steps_others", 0.25)`: the steps are clearly louder at that distance. Walk up to
       the NPC: next to you the steps are as loud as before.
    3. `audio_set_steps_others(2, 1.5, 0)`: louder again, and steps of NPCs 40 to 60 m away become audible.
    4. Your own steps, voices and gunshots must not change.
    5. `audio_set_category_rolloff("steps_others", 1)` and `audio_set_steps_others(1, 1, 0)` put it back.
11. **Compressor, quiet scene.**
    1. In a quiet place with NPCs around, `audio_set_compressor(true, -24, 4, 10, 250, 10, 6)`.
    2. Over about half a second, distant voices, steps, reloads and your own handling sounds get clearly louder.
       Rain and ambient beds do not (they are not in the set).
    3. `printf("%s %s", audio_get_compressor_gain(), audio_get_compressor_level())` from a timer: about 10 and
       -120 (or a level under -27) while nothing loud plays.
12. **Compressor, loud sounds.**
    1. With the compressor on, fire a shot. The shot is quieter than without the compressor, the world dips with it
       and swells back within about a second after the shot's tail.
    2. Right after your shot the printout shows a gain of about -8 and a level of about 0.
    3. Stand next to a talking NPC: the gain sits between -5 and +6 depending on the distance.
    4. Walk and run: the gain must not move with your own steps.
    5. `audio_set_compressor_detect(0x07FFFF00, 0x00208100, 1)`: now it does dip with each of your steps. Put it back
       with `audio_set_compressor_detect(0x07FFFF00, 0x00200100, 1)`.
13. **Compressor, categories and off.**
    1. `audio_set_compressor(false, -24, 4, 10, 250, 10, 6)`: the boost fades out over about a second, no jump.
    2. `audio_set_compressor_category("ambient", true)`, then switch the compressor on: ambient beds are louder too
       and dip with your shots. `audio_set_compressor_category("music", true)` returns false.
    3. `audio_reset(500)`: everything is back to normal; `audio_get_compressor_category("ambient")` is false.
14. **The suggested headset settings.** Run the four lines of "Suggested headset settings". A walking NPC at 15 m
    should be about as loud as someone talking at that distance; with `audio_reset(0)` they are barely audible.
15. **AI hearing with the compressor.** Repeat test 7 with the suggested settings on, then with
    `audio_set_category_rolloff("world", 0)` and a makeup of 24. NPCs must react exactly as with `audio_reset(0)`.
