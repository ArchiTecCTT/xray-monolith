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
exe. `level.scripted_throw_version()` returns `1`; the number goes up if these calls ever change. Do not call
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

Refused when (checked in this order):

| reason | when |
|---|---|
| `disabled` | `ai_scripted_throw 0` |
| `dead` | he is dead |
| `busy` | a scripted request is already active on him |
| `wounded` | he is wounded (`npc:wounded()`) or critically hit |
| `script_control` | a script action runs him (`npc:script(true, ...)`) |
| `smart_cover` | he is in a smart cover or entering one |
| `animation` | an animation moves him (animpoint with a position, cutscene) or he is on a mounted weapon |
| `no_grenade` | no grenade in his grenade slot |
| `busy` | the grenade is already in his hand: the combat planner is throwing it |
| `bad_target` | `pos` is not a valid vector |
| `too_close` | the target is under 5 m away |
| `too_far` | the target is past the engine's throw range less 1 m. The range depends on the **game difficulty**: 30 m novice, 40 m stalker, 50 m veteran, 60 m master |
| `cooldown` | under 2 s since his last completed throw (scripted or the planner's) |
| `reloading` | his gun is in the middle of a reload |

A script animation that only plays on him (the state manager's `hide`, say) is no reason to refuse: it waits while
he throws and starts again afterwards.

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
  `arc_blocked`. Nothing is thrown at a wall. (The planner checks from his feet while he still holds the gun.)

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
| 3 s passed and the throw is not ordered yet (he could not turn, or the gun would not go away) | `-1` | `timeout` |
| 8 s passed in all (a safety net; a normal throw takes 2.5 to 4 s) | `-1` | `timeout` |
| the grenade left his slot before the release (dropped, traded, taken by a script) | `-1` | `no_grenade` |
| before the throw was ordered he was wounded, taken by a script action, sent into a smart cover or an animation | `-1` | `wounded`, `script_control`, `smart_cover`, `animation` |
| he died before the release | `-1` | `dead` |
| `npc:cancel_throw_grenade()` before the throw was ordered (or the engine's weapon handling failed) | `-1` | `cancelled` |

The throw is **ordered** the moment the arc is found clear. From then on it goes through, wounded or not, and
cannot be cancelled: the engine has no way to put a grenade away once its throw has started (the game's own planner
finishes such a throw too). If he dies after the release, the request counts as thrown. If the 8 s cap ends a
request after the throw was ordered, the engine puts the grenade at rest in his hand so that the gun can come back.

The 3 s timeout covers turning, swapping and aiming. The wind-up with the pin out is the engine's own
(1 to 2.5 s with the default vars) and is not cut short.

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
`no_grenade`, `bad_target`, `too_close`, `too_far`, `cooldown`, `reloading`, `arc_blocked`, `timeout`, `cancelled`.

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

```
[THROW] id=1234 sec=sim_default_duty_2 dist=22.4 result=start reason=none
[THROW] id=1234 sec=sim_default_duty_2 dist=22.4 result=thrown reason=none
[THROW] id=1234 sec=sim_default_duty_2 dist=41.0 result=refused reason=too_far
[THROW] id=1234 sec=sim_default_duty_2 dist=18.3 result=failed reason=arc_blocked
```

`dist` is from the stalker to the point the script gave, at the time of the line.

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
| the grenade reports its release | `Grenade.cpp`, `CGrenade::Throw` |
| the combat planner steps aside | `stalker_property_evaluators.cpp` (should throw grenade), `stalker_combat_actions.cpp` (throw grenade action) |
| script animations wait | `stalker_animation_manager_update.cpp`, `play_script` |
| the calls, the feature test, the vars | `script_game_object_inventory_owner.cpp`, `script_game_object_script2.cpp`, `level_script.cpp`, `console_commands.cpp` |
