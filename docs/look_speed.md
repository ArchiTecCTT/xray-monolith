# Look speed factor

For mod authors. One number that slows how fast the player's view turns, for things like heavy gear. The
engine multiplies the actor's look rotation by it. The player's `mouse_sens` and every other setting stay
exactly as they are: nothing is written to the console or to `user.ltx`, so there is nothing to restore
after a pause, a death or a crash.

## Wiring a penalty (heavy armor, a helmet, an injury)

Two calls in the `level` namespace. Nothing needs the console.

```lua
local function look_speed_supported()
    return level.set_look_speed_factor ~= nil        -- nil on an exe without it: use your fallback
end

local function apply_turn_penalty(factor)            -- 1 = the player's own speed, 0.84 = 16 % slower
    if not look_speed_supported() then return false end
    local used = level.set_look_speed_factor(factor) -- returns the value now in use (after the limits)
    return used
end

local function clear_turn_penalty()
    if look_speed_supported() then level.set_look_speed_factor(1) end
end
```

| call | does | returns |
|---|---|---|
| `level.set_look_speed_factor(f)` | sets the factor. `f` is a number, limited to 0.2 .. 1 | the value now in use |
| `level.get_look_speed_factor()` | reads it | 1 when nothing set it |

| when | call |
|---|---|
| the gear changes (put on, taken off, switched off in your options) | `set_look_speed_factor(your factor)`, 1 for none |
| **the first update of every level** (`actor_on_first_update`) | set it again: the engine has put it back to 1 |
| the actor dies | nothing has to be done. Set 1 if a dead player should look around at full speed |

Rules:

- **Test for it before use**: `if level.set_look_speed_factor then ... end`. An older exe has no such call,
  and calling a missing function is a script error.
- **The engine resets it to 1** at every game start, new game, loaded save and level change, and when the
  game goes back to the main menu. It is not in the save file and not in `user.ltx`. So decide the state
  again on the first update of every level, never assume.
- It is one number for the whole game and the last call wins. Two mods that both slow the view have to
  agree on one of them making the call (multiply your shares, then set once).
- Set it when something changes. Setting it every frame is fine too, it is one assignment.
- The limits. Above 1 is refused (set to 1): it would be a second mouse sensitivity the player never chose
  and cannot see in the options. Under 0.2 is raised to 0.2: slower than that the view hardly answers the
  mouse and reads as a frozen game, and 0 or a negative number from bad arithmetic would lock or mirror the
  view. Not a number (`0/0`) counts as 1.

## What it scales

| scaled (once each) | not touched |
|---|---|
| mouse look on foot: from the hip, aiming and through a scope, third person, free look | menus, the inventory, the PDA and every UI cursor |
| the look keys (the `left` / `right` / `up` / `down` binds) | `mouse_sens`, `mouse_sens_aim`, `mouse_sens_vertical`, `mouse_invert` and every other setting |
| | mounted guns and vehicles (they turn the gun or the car's camera, not the actor) |
| | recoil, camera effects and script camera moves |
| | the weapon position tuner (`hud_adj_mode`), `demo_record`, the spectator camera |
| | the numbers scripts get from the mouse-move callbacks (`level_input.on_mouse_move`, the actor's `mouse_move`): still the raw mouse counts |

The factor sits on top of the player's settings: the turn per mouse count is
`mouse_sens x scope and aim scale x factor`. With factor 1 the result is the same number as before, bit for
bit.

There is no gamepad look in this engine (the stick code is commented out, and it fed the mouse path).

In the engine: `src/xrGame/look_speed.h` (the limits), `CActor::IR_OnMouseMove` and
`CActor::IR_OnKeyboardHold` in `ActorInput.cpp` (where it is applied), `CLevel::CLevel` (the reset),
`level_script.cpp` (the two calls). `tests/look_speed_test.cpp` checks the limits and the wiring.
