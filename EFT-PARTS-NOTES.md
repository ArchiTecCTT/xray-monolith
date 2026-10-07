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
- `CKinematicsAnimated`: sample/cache key0 of complete existing own hold.
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
