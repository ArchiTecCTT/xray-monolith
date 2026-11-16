# Audio API (headphones): listener-side mix controls

Engine controls for a headphones feature. A Tarkov-style headset boosts other people's footsteps, lowers background
noise and squashes loud sounds like gunshots. Scripts and items are written separately. This page lists the engine
functions those scripts call.

**What this changes:** only what the player hears, through the gain of each OpenAL source.

**What this never changes:**
- AI hearing: sound power, `p_source.volume`, `max_ai_distance`, the sound events, or the `feel_sound` path.
- Anything saved or sent over the network.

Every value goes back to its default when the game starts. Call `audio_reset(0)` to reset by hand.

Code:
- `src/xrSound/SoundRender_Mix.h`
- the "Listener-side mix" block at the end of `src/xrSound/SoundRender_Core.cpp`
- `CSoundRender_TargetA::fill_parameters`
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

### `audio_reset(fade_ms)`

Puts everything back to the defaults:
- every category fades to 1 over `fade_ms`;
- steps range goes to 1;
- the limiter is switched off (its reduction releases);
- the detect mask goes back to the default and `own_k` to 1.

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

The limiter and category gains above are therefore done in software, on the source gains.

## Defaults and what they cost

With no script calls:
- Every factor is 1, the limiter is off, and the step range is 1.
- The only new per-frame work is one pass over the playing emitters to find loud sounds, which only runs while the
  limiter is enabled, plus a few multiplies per source.

## Test plan (in game)

Run these from the debug console with a Lua line, e.g. `run_string` / a debug script, or from your headset script.
Check the effect by ear, and with `audio_get_*` printed to the log.

1. **Feature check.** `printf("audio_api %s", audio_api())` prints 63.
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
