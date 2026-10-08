# EFT parts: native authored-hold crossfade (handon)

Local branch `svp-lite-mt-parts-handon` from `5b06284`. Default off.
No local full-engine build/game confirmation. Lead alone compiles/pushes.

| Added capability | Runtime Lua methods | Data / lifecycle |
|---|---|---|
| `eft_weapon_api()` bit256 | `CWeapon:SetHandOnProfile(section)`, `GetHandOnProfile()` | transient config profile; empty section clears, malformed replacement disables old profile; no save/net fields |

`CHudItem` reads `<profile>` hold_motion/subtree_root/anchor/bones/distance_range,
`<profile>_actions` enabled action names and `<profile>_motions` original marker
vector-track sections. Track sections contain contiguous numeric frame keys,
each a finite original marker difference vector in metres. Not fitted wrist
positions or pose keys. Range is sourced profile data, not a fade time.

Recovered owner-authorized read-only runtime evidence: `EFT.Player.method_20`
uses inverse-linear0.1m→0m distance of original `weapon_L_IK_marker` and
`weapon_L_hand_marker`; `TransformLinks.GatherIK` assigns them. `GripPose` caches
original part fingers, Common0 markers selected by `EFT.Player.UpdateFirstPersonGrip`;
`HandPoser.method_2`/`SimpleGrip` use Quaternion.Lerp. No fixed return-time curve
or HandOn/HandOff sound trigger. Interaction threshold.15/.01m and reset
coroutine2s/.5 are NOT used. No decompiled code is copied in this branch.

Our permitted adaptation mixes complete already-authored action/own-hold
subtrees, NOT Tarkov's limb solver; shoulder-path equivalence is not claimed.
Body animator outer curves and interactions are not invented for X-Ray HUD.

Implementation:
- `attachable_hud_item`: remember final winning native action after random/
  suffix resolution. Opt-in enabled actions choose original action keys; draw
  and unprofiled variants retain existing selection. Resolve part's existing
  own hold and plain/current-suffix original context IDs per numeric weaponID,
  profile generation, suffix and explicit HUD-reload fence, including pool reuse.
- Use existing channel0 cycle clocks/amounts, including live incoming/outgoing
  falloff context. Interpolate/wrap ORIGINAL marker difference vectors (same
  weapon parent), normalize amounts, measure distance only AFTER mixing.
  Unknown context/mismatched counts/missing own hold disables the layer.
  Idle is context only; no permanent idle override after profiled falloff dies.
- `CKinematicsAnimated`: sample/cache key0 of complete existing own hold when
  no own-hold cycle is live. At native incoming/outgoing boundaries, sample the
  EXISTING own-idle clocks/amounts using ordinary MixInterlerp, not scalar-clock
  averages. No new cycle/clock or retained blend pointers. Idle diagnostic found
  existing217-key own holds breathe (gun-relative root2.735mm/.198deg and4
  dynamic arm children); freezing them would create an avoidable boundary pop.
  Validate exactly the subtree and anchor exclusion (no name-prefix guessing).
  Rebase root using CURRENT normally blended gun and parent transforms. Apply
  shortest normalized-linear quaternion / linear translation only after normal
  mixing, only inside the selected subtree. Never change existing channel slerp.
- Apply only to the model rendering the anatomical left arm (`model_2`);
  resolve its actual bone IDs even when outfit visual_2 differs from visual.
- Clear each update AND at new native-cycle/left-or-both-script/detach/profile
  boundaries; foreign left/both script ownership, offhand item and
  override arms take precedence. Physical/permanent launcher attachment
  suppresses both target application and original-action selection (do not
  depend solely on a script clearing the foregrip suffix). No script slot
  claim/seek/stop/resync, new
  PlayCycle, changed action clock, duration, sound, mark or completion callback.
  Original per-model UpdateTracks/CalculateBones ordering is preserved.
- Renderer target writes/clears use UCalc_Mutex; Copy/Spawn call IBlend_Startup
  and reset all target caches. Changing only the selected bone list also forces
  validation. No per-frame allocation in the layer; allocations occur on
  profile loading or ID/profile/suffix/HUD generation changes.
- Kinematics implementation is shared by renderers, including r1/DX8 and MT;
  those real builds must still be tested, not inferred from this fact.

`tests/eft_handon_probe.cpp` compiles standalone with system g++ and exercises
actual production arithmetic: range/invalid input, clock interpolation/wrap,
vector-before-distance mixtures and normalized amounts. This is NOT a full
engine/HUD compile. Pipeline has data-only Lua mocks, source/mask/ownership
invariants, original-source recomputation and13 killed focused mutations.

Before enabling/shipping: lead compile/API lookup; DX8/MT lock/pool behavior;
real incoming/outgoing mixes, interrupted action/repeated attempt, holster,
swap/load/outfit/HUD generation, parent variants, WPO/MRAA split marks/sounds/
speeds, FDDA/detector/offhand ownership, part placements, skin penetration and
startup/map/FPS/heap timing. Offline reconstruction does not test these.

## Hand-on weight rate limit (`blend_time`)

Branch `svp-lite-mt-parts-handon-smooth` from `79e3ff4`. Same opt-in: no profile, no change.

Why: Tarkov's weight (1 - distance/0.1 m) goes 1->0, or 0->1, in 4-5 frames
(0.13-0.17 s) when the hand leaves or reaches the grip at the start/end of a
reload or check. The gap between the grip's authored hold and the plain clip
hand (default handguard spot) then closes, or opens, inside those frames on top
of the clip's own motion: it reads as a flick through the default pose, most on
slanted grips (their hold is furthest from the default). Suspected from the
code and the shipped per-frame weights; only the game can confirm it.

What: `EftHandOnEase` (`EftHandOn.h`), used at the end of
`attachable_hud_item::update_handon`, rate-limits the weight AFTER Tarkov's rule.
The shown weight may not move faster than `1 / blend_time` per second (`Device.fTimeDelta`),
up or down. Same two authored poses, same per-bone blend; only the crossing time
changes. Nothing is invented: no new pose, no solver.
- Profile key `blend_time` in the `<profile>` section, seconds for a full 0<->1
  swing. Absent = `EFT_HANDON_BLEND_TIME_DEFAULT` (0.25). `0` = the raw Tarkov
  rule, frame for frame (the old behaviour). Valid range 0..2; anything else
  (negative, NaN, over 2) makes `SetHandOnProfile` fail and the profile stays off.
- Raw weight slower than the limit passes through unchanged, so the steady-state
  rule is still Tarkov's.
- The limiter has no history when the layer was not live last frame (every early
  return in `update_handon`, a profile/suffix/HUD generation change, or a gap of more than 2 frames without a call forgets it): the first live frame starts AT the raw
  weight, so activation is never worse than before. One step per `Device.dwFrame`.
- Cost: one float compare/add per frame in the HUD update; nothing allocated;
  the renderer side and `UCalc_Mutex` use are untouched (`SetAuthoredHold` is called as before).
- Trade-off: the grip's pull outlasts the raw ramp by up to `blend_time` while the
  hand travels away (a soft rubber-band at the start of a reload), and on return
  the last part of the slide onto the grip finishes slightly after the clip hand
  arrives. Lower `blend_time` if the departure feels sticky.
- Rejected: easing the raw weight with smoothstep alone (same 5 frames, same gap
  closing speed, and it reshapes Tarkov's steady-state rule); slerping the subtree
  root in world space once (changes the wrist path, not the closing speed, and
  touches the renderer blend); sampling the clip ahead of time to start the rise
  early (multi-blend speeds/wrap make it fragile).

`tests/eft_handon_ease_test.cpp` (standalone g++, production header): slope bound,
monotonic fall and rise, exact pass-through of slow weights, no-history start at raw,
`blend_time` 0 equals raw, one step per frame, NaN/inf/negative/zero `dt`, profile
value validation. Not a full engine/HUD build; the Windows exe is built by CI.
