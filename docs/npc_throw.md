# Grenade throws asked for by scripts

A script can make a stalker throw the grenade in his grenade slot at a point, on purpose. The script decides
**when and where** (cover only, cooldowns, no friends near the target, crouch or not). The engine only does the
throw, reliably, whatever is running the stalker at that moment: a combat planner action, a script combat type
(`xr_combat` `script_combat_type`, Useful Idiots' modes), the state manager.

Nothing changes for a stalker that is not asked. The game's own throws (the combat planner, the
`npc_on_should_throw` gate, the body state hook) work as before.

## Feature test

```lua
local HAS_THROW = level.scripted_throw_version ~= nil   -- nil on an exe without it
```

`level` is a plain table, so reading a field that is not there gives `nil`: no Lua error and no log line on an older
exe. `level.scripted_throw_version()` returns `2`; the number goes up when these calls change (`1` was the first
build: no `prone` reason, one 3 s limit before the throw, the short debug line). Do not call
`npc:throw_grenade` without this test: calling a method the exe does not have is a Lua error.

## The calls (`game_object`)

```lua
local ok = npc:throw_grenade(pos)            -- bool
local ok = npc:throw_grenade(pos, crouch)    -- bool
local st = npc:throw_grenade_state()         -- int
local why = npc:throw_grenade_reason()       -- string
local stopped = npc:cancel_throw_grenade()   -- bool
```

Stalkers only. On any other object: `throw_grenade` returns `false`, the state is `0`, the reason is
`not_stalker`, and nothing is written to the log.

### `npc:throw_grenade(pos [, crouch])`

`pos` is a world position (`vector`). `true` = accepted, the throw is under way. `false` = refused right away;
`throw_grenade_reason()` says why. A refused request changes nothing on the stalker.

`crouch`:

| | |
|---|---|
| `true` | he throws from crouch |
| `false` | he throws standing |
| left out | the `npc_on_combat_set_body_state` hook is asked, with the operator "throw grenade" (`stalker_ids.action_throw_grenade`), as for a throw of the combat planner. Standing if no script answers. |

Pass a real boolean or nothing. `nil` as the second argument is not "left out" for the script binding and may be
turned down as a wrong call: write `npc:throw_grenade(pos, want_crouch and true or false)`.

Refused when (checked in this order):

| reason | when |
|---|---|
| `disabled` | `ai_scripted_throw 0` |
| `dead` | he is dead |
| `busy` | a scripted request is already active on him |
| `wounded` | he is wounded (`npc:wounded()`) or critically hit |
| `script_control` | a script action runs him (`npc:script(true, ...)`) |
| `smart_cover` | he is in a smart cover, entering one, or sent to one |
| `animation` | an animation moves him (animpoint with a position, cutscene) or he is on a mounted weapon |
| `prone` | he lies in the state manager's prone pose (see below) |
| `no_grenade` | no grenade in his grenade slot |
| `busy` | the grenade is already in his hand: the combat planner is throwing it |
| `bad_target` | `pos` is not a valid vector |
| `too_close` | the target is under 5 m away |
| `too_far` | the target is past the engine's throw range less 1 m. The range depends on the **game difficulty**: 30 m novice, 40 m stalker, 50 m veteran, 60 m master |
| `cooldown` | under 2 s since his last completed throw (scripted or the planner's) |
| `reloading` | his gun is in the middle of a reload |

A script animation that only plays on him (the state manager's `hide`, say) is no reason to refuse: it waits while
he throws and starts again afterwards.

The prone pose is the exception. The game has throw animations for a standing and a crouching man only, so a lying
man would have to leave the pose for the whole throw (3 s and more), in a spot that was picked for lying down. He
is refused at once with `prone`, so the script can pick somebody else. "Lying" means: the script animation at the
head of his queue is `loophole_9_idle_0` or `loophole_9_in_front_0`, the two animations of the state manager's
`prone` and `prone_idle` (`state_mgr_animation_list.script`). A man who is getting up (any other animation) is
not refused.

### While a request is active

He stops, turns to `pos`, puts the gun away, takes the grenade, pulls the pin, holds it for the engine's wind-up
time (`ai_grenade_throw_delay_base`, plus `ai_grenade_throw_delay_step` past 15 / 30 / 45 m) and throws.

It is the combat planner's own throw: the same arc from the hand, the same miss offset (2 to 5 m around the target,
picked on the AI map), the same force with its 0.75 to 1.25 spread, the `npc_on_missile_throw_force` hook, the same
fuse. The grenade leaves his inventory the same way, the next grenade (if he has one) goes into the slot, and his
"last throw" time and his group's are set as after any throw.

Two things differ from a planner throw, both on purpose:

- `npc_on_should_throw` is **not** asked. The caller has decided.
- The arc is checked from his hand, once he is turned and holds the grenade, before the pin is pulled. If it is
  blocked, the engine tries three more miss offsets (one per frame) and then ends the request as failed with
  `arc_blocked`: nothing is thrown at a wall in front of him. (The planner checks from his feet while he still
  holds the gun.) As for the game's own throws, the check is of the arc at that moment: the force spread is applied
  after it and he may still turn a little during the wind-up, so a grenade can clip something near the far end.

**Away from the player it takes the same time as next to him.** An NPC's gun counts its "put away" and "draw"
animations by its own frame updates, and the game gives a gun that is farther than 30 m from the camera and not
drawn only a few of those a second: its half-second "put away" then lasts many seconds. (A grenade is updated every
frame anyway.) So while a request is active, and afterwards until the gun is back in his hands (4 s at most), the
engine keeps him and the items that are changing hands on the per-frame update, and lets his inventory make the
slot change every frame, as the player's does. Nobody else's updates change.

For that time, and only on this stalker, the engine overrides what anybody else tells him about his weapon, where
he looks, how he moves and his body state. The orders about his weapon, movement and body state are not lost: when
the request ends, the weapon goal that was set last is still there and the gun comes back by itself; movement and
body state are back on his next update. Where he looks is **not** put back by the engine: he keeps looking at the
target until whoever runs him sets his sight again (the combat planner's actions and the state manager do that on
their own). The combat planner does not start or continue a throw of its own on him during a request.

A request ends by itself:

| | state | reason |
|---|---|---|
| the grenade left his hand and the thrown grenade is gone from the slot (or 1.5 s after the release) | `2` | `none` |
| the arc was blocked on all four tries | `-1` | `arc_blocked` |
| the engine found the target out of range from his hand | `-1` | `too_far` |
| 4.5 s passed and the grenade is not in his hand and at rest yet (the gun would not go away) | `-1` | `timeout` |
| the grenade is in his hand, 2 s passed since then and the throw is not ordered yet (he could not turn to the target) | `-1` | `timeout` |
| 8 s passed in all (the hard cap; a normal throw takes 2.5 to 4 s) | `-1` | `timeout` |
| the grenade left his slot before the release (dropped, traded, taken by a script) | `-1` | `no_grenade` |
| before the throw was ordered he was wounded, taken by a script action, sent into a smart cover or an animation | `-1` | `wounded`, `script_control`, `smart_cover`, `animation` |
| he died before the release | `-1` | `dead` |
| `npc:cancel_throw_grenade()` before the throw was ordered (or the engine's weapon handling failed) | `-1` | `cancelled` |

The throw is **ordered** the moment the arc is found clear. From then on it goes through, wounded or not, and
cannot be cancelled: the engine has no way to put a grenade away once its throw has started (the game's own planner
finishes such a throw too). If he dies after the release, the request counts as thrown. If the 8 s cap ends a
request after the throw was ordered (a slow swap of 4 s and more followed by a long wind-up can get there), the
engine puts the grenade at rest in his hand so that the gun can come back; the request is reported as failed, and a
throw animation that was already playing at that instant still lets the grenade go.

The script sees one reason, `timeout`, for all three limits; the debug line says which one it was (`limit=`). The
swap is timed by the two items' own animations (the gun's "put away", the grenade's "draw"): about 1 to 2.5 s, a
machine gun being the slowest. Turning runs at the same time, so the 2 s after the grenade is out are rarely
needed. The wind-up with the pin out is the engine's own (1 to 2.5 s with the default vars) and is not cut short.

### `npc:throw_grenade_state()`

| | |
|---|---|
| `0` | nothing asked yet (also after a save is loaded or he came online again) |
| `1` | a request is active |
| `2` | the last request threw the grenade |
| `-1` | the last request was refused or failed; `throw_grenade_reason()` says why |

`1` lasts until the engine has given him back, a few tenths of a second after the grenade left his hand.
A refusal with `busy` while a request runs leaves the state at `1`.

### `npc:throw_grenade_reason()`

The reason of the last refusal or end, one of:
`none`, `not_stalker`, `disabled`, `dead`, `busy`, `wounded`, `script_control`, `smart_cover`, `animation`,
`no_grenade`, `bad_target`, `too_close`, `too_far`, `cooldown`, `reloading`, `arc_blocked`, `timeout`, `cancelled`,
`prone`.

### `npc:cancel_throw_grenade()`

`true` = an active request was stopped: it ends as failed with `cancelled` and he takes his gun again.
`false` = there was nothing to stop, or it is too late: the throw is already ordered and goes through (the state
stays `1` and ends as `2`).

## The callback

```lua
RegisterScriptCallback("npc_on_scripted_throw", function(npc, ok, reason)
    -- ok = true: the grenade left his hand. ok = false: no throw, reason says why
end)
```

Once for every **accepted** request, when it ends. Never for a request that `throw_grenade` refused (it returned
`false`). Not sent when his object is destroyed (going offline, level change). It needs the
`callbacks_gameobject.script` of this build; without that file the state getter still works.

## Console vars

Both are saved in `user.ltx` and are on the options page (Modded Exes > Gameplay > Stalkers).

| var | default | |
|---|---|---|
| `ai_scripted_throw` | 1 | 0 = every request is refused with `disabled` (for A/B tests) |
| `ai_scripted_throw_debug` | 0 | 1 = one log line when a request starts and one when it ends |

A refused request gets a short line. The start and the end of an accepted one carry the facts of that moment:

```
[THROW] id=1234 sec=sim_default_duty_2 dist=41.0 result=refused reason=too_far
[THROW] id=1234 sec=sim_default_duty_2 dist=22.4 result=start reason=none phase=hold limit=none ms=0 in_hand_ms=-1 aligned_ms=-1 active=wpn_ak74 astate=0 mstate=hidden yaw_err=64 body=crouch script_anim=1 arc_tries=0 crouch=1 op=10 strap=0 frames=0 cam=47
[THROW] id=1234 sec=sim_default_duty_2 dist=22.4 result=thrown reason=none phase=tail limit=none ms=3620 in_hand_ms=1410 aligned_ms=380 active=none astate=-1 mstate=none yaw_err=2 body=crouch script_anim=1 arc_tries=0 crouch=1 op=32 strap=0 frames=217 cam=47
[THROW] id=1234 sec=sim_default_duty_2 dist=18.3 result=failed reason=timeout phase=hold limit=swap ms=4512 in_hand_ms=-1 aligned_ms=350 active=wpn_pkm astate=2 mstate=hidden yaw_err=3 body=stand script_anim=0 arc_tries=0 crouch=0 op=2 strap=0 frames=271 cam=62
```

| field | |
|---|---|
| `dist` | from the stalker to the point the script gave, at the time of the line |
| `phase=` | where the request was: `hold` the grenade is not in his hand yet (the gun goes away, the grenade comes out); `aim` it is in his hand, he turns and the arc is tried; `throw` the throw is ordered (wind-up, swing); `tail` the grenade has left his hand |
| `limit=` | for a `timeout`, which limit it was: `swap` (4.5 s from the accept), `aim` (2 s from "in his hand"), `total` (8 s), `overdue` (10 s, his updates had stopped altogether). `none` for every other end |
| `ms=` | milliseconds since the request was accepted |
| `in_hand_ms=` | when the grenade was first in his hand and at rest; `-1` = never |
| `aligned_ms=` | when his head first pointed within 22.5 degrees of the target; `-1` = never |
| `active=` | the section of the item in his hands; `none` = empty hands |
| `astate=` | that item's state: 0 idle, 1 showing, 2 hiding, 3 hidden, 4 bored. A gun also: 5 firing, 7 reloading, 8 jammed, 9 / 10 switching. A grenade: 5 pin being pulled, 6 held ready, 7 swing, 8 swing done. `-1` = no item |
| `mstate=` | the request's grenade: `hidden`, `showing`, `idle`, `hiding`, `throw_start`, `ready`, `throw`, `throw_end`. `none` = it is not in his grenade slot any more (thrown, or lost) |
| `yaw_err=` | degrees between where his head points and the target, on the ground plane. Under 22.5 he counts as turned |
| `body=` | his body state now: `stand` or `crouch` |
| `script_anim=` | `0` no script animation is queued on him; `1` one is (the state manager's; it waits during a request); `prone` it is a prone pose |
| `arc_tries=` | how many arcs were found blocked (4 ends the request) |
| `crouch=` | what was asked: `1`, `0`, or `-1` = left out and the hook has not been asked yet |
| `op=` | the step his weapon handling was at: 0 draw, 2 put away, 4 to 7 strap / unstrap, 8 strapped, 9 idle, 10 to 13 and 25 / 26 aim, 14 to 17 reload, 18 to 20 fire, 23 / 24 wait between bursts, 29 pull the pin and hold, 31 swing, 32 after the swing. `-1` = none yet |
| `strap=` | `1` = his gun is strapped or on its way there or back: it is unstrapped first, which makes the swap slower |
| `frames=` | how many frame updates the request got. Divided by `ms` / 1000 it should be near the frame rate; far below means he was not updated every frame |
| `cam=` | metres from the camera |

How to read a failed line:

- `phase=hold limit=swap`: the gun did not go away in 4.5 s. `active=` is the gun and `astate=2` = its "put away"
  was still running (a very long animation, or `frames=` is low and it was starved of updates); `astate=7` = it
  went into a reload; `strap=1` or `op=4..7` = it was being unstrapped first; `astate=3` with `mstate=hidden` = the
  gun was away but the grenade was never brought out.
- `phase=aim limit=aim`: the grenade was out in time (`in_hand_ms`), but `yaw_err` stayed above 22.5: he did not
  turn. `aligned_ms=-1` says he never faced the target at all.
- `phase=aim reason=arc_blocked`: the four arcs from his hand were blocked; `yaw_err`, `body` and `dist` describe
  the throw that was refused.
- `phase=throw limit=total`: the throw was ordered but the grenade did not leave his hand before the 8 s cap.
  `mstate=throw_start` or `ready` = the wind-up never finished; `mstate=throw` = the swing was playing;
  `in_hand_ms` shows how late the swap was.

Shortcuts in `appdata\`: `cfg_load throw_on`, `cfg_load throw_off`, `cfg_load throw_debug` (on, with the log).

## Save, load, death

A request is never saved. After a load, a level change, or when the stalker goes offline and comes back, his state is
`0`. Death ends it (`dead`, or thrown if the grenade was already in the air). A grenade that has left his hand is an
ordinary thrown grenade from then on.

## Example

```lua
local HAS_THROW = level.scripted_throw_version ~= nil

local function try_throw(npc, pos, crouch)
    if not HAS_THROW then return false end
    if npc:throw_grenade(pos, crouch) then return true end
    local why = npc:throw_grenade_reason()      -- "too_far", "cooldown", ...
    return false, why
end

RegisterScriptCallback("npc_on_scripted_throw", function(npc, ok, reason)
    if not ok then printf("throw of %s failed: %s", npc:name(), reason) end
end)
```

## Where it is in the engine

| | |
|---|---|
| the rules: refusals, states, timeouts (no engine includes; `tests/scripted_throw_test.cpp` runs it) | `src/xrGame/scripted_throw.h` |
| what the stalker does | `src/xrGame/ai/stalker/ai_stalker_scripted_throw.cpp` |
| the three borrowed updates, death, reinit, net_destroy | `ai_stalker.cpp`: `update_object_handler`, `Think`, `UpdateCL`, `Die`, `reinit`, `net_Destroy` |
| he stays on the per-frame update during a request and until the gun is back | `ai_stalker_scripted_throw.cpp`: `scripted_throw_keep_updated`, `scripted_throw_restore`; `ai_stalker.cpp`: `AlwaysTheCrow` |
| the grenade reports its release | `Grenade.cpp`, `CGrenade::Throw` |
| the combat planner steps aside | `stalker_property_evaluators.cpp` (should throw grenade), `stalker_combat_actions.cpp` (throw grenade action) |
| script animations wait | `stalker_animation_manager_update.cpp`, `play_script` |
| the calls, the feature test, the vars | `script_game_object_inventory_owner.cpp`, `script_game_object_script2.cpp`, `level_script.cpp`, `console_commands.cpp` |
