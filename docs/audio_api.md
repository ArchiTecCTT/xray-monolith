# Audio API (headphones): listener-side mix controls

Engine controls for a headphones feature. A Tarkov-style headset boosts other people's footsteps, lowers background
noise, lifts quiet sounds and squashes loud ones like gunshots. Scripts and items are written separately. This page
lists the engine functions those scripts call.

**What this changes:** only what the player hears, through the gain and the distance rolloff of each OpenAL source.

**What this never changes:**
- AI hearing: sound power, `p_source.volume`, `max_ai_distance`, the sound events, or the `feel_sound` path.
  NPCs and mutants hear exactly the same sounds, at the same strength and range, whatever a script sets here.
- Anything saved or sent over the network.

Every value goes back to its default when the game starts. Call `audio_reset(0)` to reset by hand.

Code:
- `src/xrSound/SoundRender_Mix.h`
- the "Listener-side mix" block at the end of `src/xrSound/SoundRender_Core.cpp`
- `CSoundRender_TargetA::fill_parameters`
- `CSoundRender_Emitter::update_culling` (the level each sound has at the listener, for the limiter, the compressor
  and auto-gain)
- `CSoundRender_Core::mix_gain` (the gain of one source, with its auto-gain)
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

| 256 | per-source auto-gain and the bang guard: `audio_set_autogain`, `audio_set_autogain_category`, `audio_get_autogain_category`, `audio_get_autogain_enabled`, `audio_get_autogain_min`, `audio_get_autogain_max`, `audio_set_bang_guard`, `audio_set_bang_guard_detect`, `audio_get_bang_guard_enabled`, `audio_get_bang_guard_reduction` |
| 512 | the `own` category: `audio_set_own_split`, `audio_get_own_split`, and `"own"` as a category name everywhere |
| 1024 | the steps range of `audio_set_steps_others` no longer changes what the AI hears |

| exe (fork branch) | `audio_api()` |
|---|---|
| `headphones-audio` | 63 |
| `headphones-compressor` | 255 |
| `headphones-autogain` | 2047 |

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
   - `own`: your own sounds, **only after a script called `audio_set_own_split(true)`**. That is every untagged
     sound that the object you are (the actor) plays, and every 2D sound that has a game object. It covers your
     first-person weapon sounds (shots, reload, inspect), your footsteps, your character's voice, and PDA and item
     sounds a script plays on the actor. Without the split these are in `world`, as before.
   - `world`: everything else. That is weapons, bullet impacts, explosions, voices, monsters (except their steps),
     physics, anomalies and items; and, without the own split, your own sounds too.

Notes:
- **Wind.** Anomaly has no separate wind sound. Wind is part of the environment ambient channels, which is why
  files with `wind` in the path go to `weather`. A wind bed whose file name has none of the four words lands in
  `ambient`.
- **Your own weapon.** Your first-person weapon sounds are in `world`, or in `own` once a script switched the own
  split on. This includes HUD sounds, which play in 2D with you as the object.
- **The limiter** works on every category except `ui` and `music`.
- **Auto-gain** works on `world`, `steps_others` and `own` unless a script changes that. It can never work on `ui`
  or `music`, and it never lifts `own`.
- **2D sounds with no game object are `ui`**, also when they are not menu sounds (some mods play PDA beeps and
  voice lines that way). Auto-gain, the compressor and the limiter never touch them. If such a sound is too loud,
  use `audio_set_category_gain("ui", ...)`, or tag the `sound_object` with `set_audio_category("own")`.
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

  The `eOnFootStep` script callback still fires only within `step_sound_distance`.

  **NPCs and mutants do hear these footsteps.** The step sound makes AI sound events like any other sound with a
  game object and a type (`CSoundRender_Emitter::Event_Propagade`). An earlier version of this page said they do
  not; that was wrong.
  - With feature bit 1024 the range is safe for the AI. A step that plays only because the range is above 1 (beyond
    `step_sound_distance`) is played for you alone and makes no AI event. A step that a range below 1 cuts (inside
    `step_sound_distance`) is still played, silently, so its AI event stays.
  - On an exe without bit 1024 (`audio_api()` 63 or 255) a range other than 1 adds or removes those AI events.
    Leave the range at 1 there.
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

### `audio_set_autogain(enabled, target_db, ratio, max_boost_db, max_cut_db, attack_ms, release_ms)`

Per-source auto-gain, the way electronic hearing protection works: every sound gets its own gain from how loud it
is at your ears. A quiet, distant sound is lifted. A loud, close one is turned down. No sound changes the gain of
another, so nothing "goes quiet for a bit" after a shot.

- `enabled`: true / false. Default false. When you disable it, each source's gain goes back to 0 dB through its own
  smoother; nothing jumps.
- `target_db`: -60 ... 0. Default -14. The level sounds are pulled toward.
- `ratio`: 1 ... 20. Default 3. A sound d dB away from the target comes out d / ratio dB away. 1 switches the
  effect off; a high ratio makes everything equally loud.
- `max_boost_db`: 0 ... 24. Default 12. The most a quiet sound is lifted (the makeup ceiling).
- `max_cut_db`: 0 ... 40. Default 12. The most a loud sound is turned down.
- `attack_ms`: 0 ... 1000. Default 5. How fast a source's gain falls when the source gets louder.
- `release_ms`: 0 ... 10000. Default 80. How fast it rises when the source gets quieter.

**How it works.** For every source that is being played, once per sound update:

1. **Its level:** `base volume x engine fade with distance x OpenAL distance x occlusion`, times its category gain.
   The fade in and out of the sound and the volume sliders are left out.
2. **Its gain:** `(target - level) x (1 - 1/ratio)` dB, limited to `+max_boost` and `-max_cut`, times the amount of
   its category (`audio_set_autogain_category`).
3. **Smoothing, per source.** A sound that starts takes its gain at once: a loud onset is never played at full
   volume first. After that the gain follows the sound's level with the attack and release times. The level of a
   sound changes with distance and occlusion, so this is slow, gentle movement.
4. The gain multiplies the source, on top of its category gain, the limiter, the compressor and the bang guard.

With the defaults (target -14, ratio 3, boost 12, cut 12), using the levels of the measurements above:

| Sound | Its level | Auto-gain | Comes out at |
|---|---|---|---|
| NPC step, walking, 30 m | -35.7 dB | +12.0 dB (the ceiling) | -23.7 dB |
| NPC step, walking, 15 m | -29.8 dB | +10.5 dB | -19.3 dB |
| NPC step, crouching, 5 m | -28.8 dB | +9.9 dB | -18.9 dB |
| Gunshot, 100 m | -23.7 dB | +6.5 dB | -17.2 dB |
| Idle voice, 15 m | -18.1 dB | +2.7 dB | -15.4 dB |
| Gunshot, 30 m | -13.5 dB | -0.3 dB | -13.8 dB |
| Shout, 15 m | -12.8 dB | -0.8 dB | -13.6 dB |
| NPC step, running, 5 m | -9.6 dB | -2.9 dB | -12.5 dB |
| Gunshot, 15 m | -8.0 dB | -4.0 dB | -12.0 dB |
| Shout, 5 m | -3.6 dB | -6.9 dB | -10.5 dB |
| Your own shot, reload, inspect, PDA or voice at full volume (`own`) | 0 dB | -9.3 dB | -9.3 dB |
| A quiet sound of your own (`own`) | -30 dB | 0 dB (never lifted) | -30 dB |

A louder sound always still comes out louder than a quieter one, so distance can still be heard; the differences
are a third of what they were. An anomaly is a sound like any other here: a far one is lifted, a near one is turned
down a little, and no shot makes it disappear.

**Limits.**
- The engine knows a sound's gain, not its waveform. A file that is quiet inside is treated like a loud one at the
  same gain.
- Only sources that are actually being played are processed. A sound that is too quiet for the engine to play at
  all (under its cull level before the mix) is not brought back by auto-gain. Use `audio_set_category_rolloff` and
  the steps range to make far footsteps exist; auto-gain then lifts them.
- Auto-gain reads levels after the category gains. A category gain of +6 dB therefore ends up as about +2 dB at
  ratio 3. To make the headset louder or quieter as a whole, move `target_db` and `max_boost_db` (see "Suggested
  headset settings").
- Use it instead of the compressor. With both on, the compressor adds its makeup and its ducking on top.

### `audio_set_autogain_category(category, amount) -> bool` / `audio_get_autogain_category(category) -> number`

How much of the auto-gain a category gets.
- `amount`: 0 ... 1. 0 = none, 1 = all of it, 0.5 = half the dB.
- Defaults: `world` 1, `steps_others` 1, `own` 1, `ambient` 0, `weather` 0.
- `"ui"`, `"music"` and unknown names return `false`: they never get auto-gain.
- `own` is never lifted, whatever the amount: its boost ceiling is 0. With amount 1 your own loud sounds are turned
  down like any other loud sound; with 0 they are left alone.

The amount also sets how much of a level difference survives. Two sounds d dB apart come out
`d x (1 - amount x (1 - 1/ratio))` dB apart. A crouching and a walking NPC are 8 dB apart: at ratio 3 that is
2.7 dB with amount 1, 5.3 dB with amount 0.5 and the full 8 dB with amount 0.

### `audio_get_autogain_enabled() -> bool`

Whether auto-gain is on.

### `audio_get_autogain_min() -> number` / `audio_get_autogain_max() -> number`

The lowest and the highest auto-gain, in dB, among the sources played in the last sound update (0 and 0 when no
source has one). For a debug HUD: with a shot nearby and steps far away you see about -9 and +12.

### `audio_set_bang_guard(enabled, threshold_db, max_duck_db, hold_ms, release_ms)`

A very short duck of everything when a loud bang starts. It stands in for the clamp of real electronic ear
protection, and it is the only part of the auto-gain set where one sound changes the others.

- `enabled`: true / false. Default false. When you disable it during a duck, the duck releases.
- `threshold_db`: -60 ... 0. Default -9. Bangs heard above this level trigger it.
- `max_duck_db`: 0 ... 40. Default 6. The duck is the overshoot over the threshold, at most this much.
- `hold_ms`: 0 ... 500. Default 30. How long the start of a bang counts.
- `release_ms`: 0 ... 1000. Default 25. How fast the duck goes away after the hold.

**How it works.** A bang is a one-shot sound whose type has a bit of the detect mask (shots and explosions by
default). Only its first `hold_ms` count, not the rest of its sample. While it counts, every category except `ui`
and `music` is ducked at once. After the hold the duck fades with the release time. With the defaults a
full-volume shot ducks 6 dB for 30 ms, less than 1 dB is left 50 ms later, about 0.1 dB after 100 ms and nothing
after 0.25 s. The limiter and the compressor, in contrast, hold for as long as the shot's sample plays, which is
what made the world go quiet.

The shot itself is turned down by its own auto-gain for its whole length; the guard is only the click of protection
on everything else. In automatic fire the guard re-triggers with every shot, so continuous sounds flutter slightly
at the rate of fire. Lower `max_duck_db` if that is unpleasant.

### `audio_set_bang_guard_detect(game_type_mask)`

Which sounds are bangs: bits of `ESoundTypes`. Default `0x00200100` (shots and explosions), for 3D sounds and for
your own. Add `0x00080000` for bullet impacts.

### `audio_get_bang_guard_enabled() -> bool` / `audio_get_bang_guard_reduction() -> number`

Whether the guard is on, and its current duck in dB (≤ 0).

### `audio_set_own_split(enabled)` / `audio_get_own_split() -> bool`

Switches the `own` category on (see [Categories](#categories)). Default false.

- With the split on, your own sounds leave `world`. A boost on `world`, the compressor's makeup and the auto-gain's
  lift then no longer reach your reload, inspect, PDA and voice sounds.
- `audio_set_category_gain("own", gain, fade_ms)` sets their volume directly.
- The compressor's set does not contain `own` by default, so with the split on your own shots neither get
  compressed nor drive the compressor. Add it with `audio_set_compressor_category("own", true)` if you still use the
  compressor. The limiter covers `own` like every category but `ui` and `music`.
- Switching it while sounds play moves them between `world` and `own` at once. Set it before the gains, when the
  headset goes on.

A script can also put any `sound_object` into the category by hand: `snd:set_audio_category("own")`. That works
without the split.

### `audio_reset(fade_ms)`

Puts everything back to the defaults:
- every category fades to 1 over `fade_ms`;
- steps range goes to 1 and every category's rolloff to 1;
- the limiter is switched off (its reduction releases);
- the limiter's detect mask goes back to the default and `own_k` to 1;
- the compressor is switched off (its gain goes back to 0 dB over its release time), its category set goes back to
  `world` and `steps_others`, and its detect masks and `own_k` go back to the defaults;
- auto-gain is switched off (each source's gain goes back to 0 dB) and its category amounts go back to the defaults;
- the bang guard is switched off and its detect mask goes back to the default;
- the own split is switched off.

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
- Per frame: one pass over the playing emitters for the limiter, one for the compressor and one for the bang guard,
  each only while it is enabled, plus a few multiplies per source and one distance formula per playing sound.

**Cost of auto-gain.** It runs once per source that is being played, per sound update: one logarithm, the curve, the
smoother and one exponential. `tests/audio_mix_test.cpp` times exactly the functions the engine calls, with every
source changing its level on every update (the worst case):

| Sources being played | Time per update | Share of a 60 fps frame (16.7 ms) |
|---|---|---|
| 64 | 0.8 µs | 0.005 % |
| 256 | 3.4 µs | 0.02 % |
| 1024 (the engine's maximum, `snd_targets`) | 13.5 µs | 0.08 % |

That is 13 ns per source, measured offline on ornyx-1 (AMD EPYC 9V74, g++ -O2), not in a running game. A laptop CPU
several times slower still stays under 0.1 ms at the maximum. Switched off, a source costs one compare. The bang
guard's pass does a type test per emitter and a few operations for the rare bang.

## Suggested headset settings

A starting point for a good headset on the `headphones-autogain` exe (`audio_api()` 2047), from the measurements.
Not played yet.

```lua
audio_set_own_split(true)                              -- your own sounds become the "own" category
audio_set_limiter(false, -12, 5, 400, 20)              -- replaced by auto-gain and the bang guard
audio_set_compressor(false, -24, 4, 10, 250, 0, 6)     -- replaced by auto-gain
audio_set_category_rolloff("steps_others", 0.3)        -- steps lose less with distance
audio_set_steps_others(1.0, 1.5, 750)                  -- steps exist to about 60 m (safe for the AI with bit 1024)
audio_set_autogain_category("steps_others", 0.5)       -- keeps 5 of the 8 dB between crouching and walking
audio_set_autogain(true, -14, 3, 12, 12, 5, 80)        -- every sound levelled by its own loudness
audio_set_bang_guard(true, -9, 6, 30, 25)              -- a 6 dB click of protection at the start of a bang
```

What an NPC's footsteps then come out at (dB; vanilla step files, the louder-footsteps mod not counted):

| | 5 m | 15 m | 30 m | 50 m |
|---|---|---|---|---|
| Crouching, nothing set | -28.8 | -37.8 | -43.7 | silent |
| Crouching, these settings | -20.4 | -24.4 | -28.4 | -32.4 |
| Walking, nothing set | -20.8 | -29.8 | -35.7 | silent |
| Walking, these settings | -15.1 | -19.1 | -22.3 | -27.7 |
| Running, nothing set | -9.6 | -21.5 | -35.4 | silent |
| Running, these settings | -7.5 | -12.6 | -18.2 | -27.7 |

With `audio_set_autogain_category("steps_others", 1)` the walking step is at -14.6 / -16.6 / -18.2 / -21.7 dB, but a
crouching NPC is then only 2.6 dB quieter than a walking one.

**Volume steps (PgUp / PgDn).** Move `target_db` and `max_boost_db` together, 3 dB per step. Each step makes quiet
and far sounds 3 dB louder and everything else 2 dB louder:

| Step | `target_db` | `max_boost_db` | Walking step 30 m | Walking step 15 m | Idle voice 15 m | Gunshot 15 m | Your own shot |
|---|---|---|---|---|---|---|---|
| 1 | -20 | 6 | -29.7 | -23.8 | -19.4 | -16.0 | -12.0 |
| 2 | -17 | 9 | -26.7 | -21.3 | -17.4 | -14.0 | -11.3 |
| 3 | -14 | 12 | -23.7 | -19.3 | -15.4 | -12.0 | -9.3 |
| 4 | -11 | 15 | -20.7 | -17.3 | -13.4 | -10.0 | -7.3 |
| 5 | -8 | 18 | -17.7 | -15.3 | -11.4 | -8.0 | -5.3 |

(The step columns are at auto-gain amount 1 and without the rolloff, to show the curve alone.)

**Auto-gain switched off by the player (a fixed boost).** `audio_set_autogain(false, ...)`, then a plain category
gain: `audio_set_category_gain("world", g, fade)` and `audio_set_steps_others(g, range, fade)`. With the own split
on, that boost does not reach your own reload, inspect, PDA and voice sounds. Keep the bang guard on.

**A cheaper headset:** a lower `max_boost_db` (4 to 6) and a rolloff nearer 1 (0.5 to 0.6).

**Louder steps for everyone, no headset:** `audio_set_category_rolloff("steps_others", k)` and
`audio_set_steps_others(gain, range, fade)` alone.

The compressor of the `headphones-compressor` exe is still there and unchanged. Its settings were
`audio_set_compressor(true, -24, 4, 10, 250, 10, 6)` with a step rolloff of 0.3 and
`audio_set_steps_others(2.0, 1.5, 750)`.

## Test plan (in game)

Run these from the debug console with a Lua line, e.g. `run_string` / a debug script, or from your headset script.
Check the effect by ear, and with `audio_get_*` printed to the log.

1. **Feature check.** `printf("audio_api %s", audio_api())` prints 2047.
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
14. **The compressor settings.** `audio_set_limiter(false, -12, 5, 400, 20)`,
    `audio_set_category_rolloff("steps_others", 0.3)`, `audio_set_steps_others(2.0, 1.5, 750)`,
    `audio_set_compressor(true, -24, 4, 10, 250, 10, 6)`. A walking NPC at 15 m should be about as loud as someone
    talking at that distance; with `audio_reset(0)` they are barely audible.
15. **AI hearing with the compressor.** Repeat test 7 with the suggested settings on, then with
    `audio_set_category_rolloff("world", 0)` and a makeup of 24. NPCs must react exactly as with `audio_reset(0)`.
16. **Auto-gain, far and near.** `audio_reset(0)`, then `audio_set_autogain(true, -14, 3, 12, 12, 5, 80)`.
    1. Distant voices, steps and far gunfire are clearly louder. An NPC talking next to you is a little quieter.
    2. Walk toward a talking NPC from 30 m: they get louder as you approach, but much less than without auto-gain.
    3. Print `audio_get_autogain_min()` and `audio_get_autogain_max()` from a timer: about +12 with only far sounds
       around, about -7 to -9 next to a loud one.
17. **Nothing goes quiet after a shot.** With auto-gain on (and limiter and compressor off), stand near an anomaly
    field or a campfire and fire. The anomaly and every other sound keep their loudness during and after the shot.
    The shot itself is quieter than with auto-gain off. Do the same with a reload: nothing else changes.
18. **Bang guard.** `audio_set_bang_guard(true, -9, 6, 30, 25)`. Fire single shots near a steady sound: at most a
    very short dip right at the shot, gone before the shot's tail. `audio_get_bang_guard_reduction()` printed every
    frame shows -6 for one or two frames, then 0 within a few frames. Try `max_duck_db` 20 to hear the dip clearly.
19. **Your own sounds.** `audio_set_own_split(true)`.
    1. Reload, inspect the weapon, open the PDA, trigger a voice line of your character: with auto-gain on they are
       quieter than without, and never louder.
    2. `audio_set_autogain(false, -14, 3, 12, 12, 5, 80)` and `audio_set_category_gain("world", 3, 0)`: the world is
       louder, your reload and inspect sounds are not. `audio_set_category_gain("own", 0.3, 0)` turns only them down.
    3. If a PDA or voice sound does not react, it is played without a game object and is in `ui`; see "Categories".
    4. `audio_set_own_split(false)`: your sounds follow `world` again.
20. **Crouch gap.** With auto-gain on, listen to an NPC crouch-walking and then walking at about 10 m, once with
    `audio_set_autogain_category("steps_others", 1)` and once with 0.5. With 0.5 the crouched steps are clearly the
    quieter ones.
21. **Steps range and the AI.** `audio_set_steps_others(1, 4, 0)`: you hear NPC steps far beyond 50 m. NPCs and
    mutants must not react to those far steps any differently than with `audio_reset(0)`. Then
    `audio_set_steps_others(1, 0.2, 0)`: you hear steps only within 10 m, and NPCs still react to each other's steps
    as before.
22. **Auto-gain off and reset.** `audio_set_autogain(false, -14, 3, 12, 12, 5, 80)`: within a fraction of a second
    everything is at its normal loudness, no jump. `audio_reset(0)`: `audio_get_own_split()` is false,
    `audio_get_autogain_category("world")` is 1, `audio_get_bang_guard_enabled()` is false.
23. **AI hearing with auto-gain.** Repeat test 7 with the suggested headset settings on. NPCs must react exactly as
    with `audio_reset(0)`.
