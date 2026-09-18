# Issue 668: DX8-AVX diagnostic baseline

**This build does not contain a fix.** It retains the 2026.8.17 engine logic and adds opt-in logging to establish whether the reported failure occurs on this setup. Logging adds some timing overhead; compare with the normal executable if it changes the visible symptom.

- Upstream issue: https://github.com/themrdemonized/xray-monolith/issues/668
- Source baseline: tag `2026.8.17`, commit `62cb33722793166f3454a5ba08e03c67279f4ef8`.
- Configuration: **DX8-AVX**, stable branch, not MT-TEST.
- Intended installation: Anomaly 1.5.3 with the matching 2026.8.17 Monolith runtime files already installed.
- Test setup reported by the operator: MO2 profile `No Mods`, actually containing 106 enabled custom addons, not GAMMA.
- Diagnostic argument: `-corpse_debug`; all diagnostic lines contain `[DEBUG-668]`.
- The reporter, **vangorn1011-coder**, supplied the proposed retention guard. It has deliberately not been applied to this baseline.

## What to download

From the successful Actions run, download `issue-668-baseline-DX8-AVX-<run number>`. It contains:

- `bin/AnomalyDX8AVX.exe`
- `build-manifest.json` with source revision and SHA-256
- `License.txt`, this guide, and MSBuild version information

The separate `...-symbols-<run number>` artifact contains the matching PDB. It is optional unless we need crash debugging. Keep it paired with this exact executable. The archive is a diagnostic engine artifact, **not an addon** and not a complete game/engine update. Do not install it as an MO2 gamedata addon.

## Local Windows agent: preparation

1. Close Anomaly and verify no `Anomaly*` process is running.
2. Preserve the exact active executable path, SHA-256, MO2 executable entry and arguments, profile mod list, and relevant settings. Do not infer the executable from an unused DX11 shortcut.
3. Create a `668-test` MO2 profile with profile-specific saves and settings. Initially keep the same addon load order. Do not test by saving over the regular campaign.
4. **MO2 profiles do not isolate `bin`.** Prefer a correctly configured separate test installation if practical. Otherwise, back up the current Monolith `bin` directory outside the installation before replacing anything, record the backup location, and restore it after the test. The existing `bin_stock_1.5.3` backup is not a backup of the current Monolith installation.
5. Check `bin/AnomalyDX8AVX.exe` in the downloaded artifact against `exeSha256` in `build-manifest.json`. The Actions job summary also prints this hash. Stop on a mismatch.
6. Replace only the chosen test installation's `bin/AnomalyDX8AVX.exe`. Do not change other binaries, addons, shaders, gamedata, or renderer settings. If the installed Monolith version is not actually 2026.8.17, stop and report the mismatch first.
7. Duplicate the working DX8-AVX MO2 launch entry, point it at the chosen test installation, and preserve the existing working directory and arguments as appropriate for that installation. Append `-dbg -corpse_debug` without dropping existing arguments. Launch through MO2 so the virtual filesystem and selected profile remain active.
8. Confirm the running executable path. Anomaly logs normally live beneath the configured `$app_data_root$` in `logs`; resolve the actual path from this installation rather than assuming it.

No Visual Studio or source checkout is needed on the gaming PC. Do not share credentials or upload the game/addon folders.

## First test round

Use a disposable fresh game/scenario, not a progression save. Record map/location and dog/stalker sections used.

1. Enable the debug menu and hide the actor from AI, as in the issue report.
2. Spawn fresh blind dogs and an ordinary stalker in an accessible area. Avoid zombies and special human models for this first round.
3. Let the dogs kill the stalker. Stay near the scenario so the entities remain in the active simulation, but do not become an enemy.
4. Observe for roughly 60 seconds after combat. Record whether the dogs approach, eat, drag, or move away, and whether another enemy or interruption appears.
5. Exit cleanly and copy the log immediately so the next launch cannot overwrite it. If it crashes, preserve the full crash log locally as well.
6. If practical, repeat once with newly spawned participants rather than repeatedly using a possibly stranded, locked corpse.

A screenshot/video is optional. The useful deliverable is the diagnostic trace, short observation, source/build manifest, and exact MO2 mod list. The local agent should inspect logs for sensitive paths or unrelated personal data before sharing them.

## Collecting the trace

PowerShell 7, replacing the example path with the actual log:

```powershell
$log = 'D:\path\to\appdata\logs\xray_user.log'
Select-String -LiteralPath $log -SimpleMatch '[DEBUG-668]' |
    ForEach-Object { $_.Line } |
    Set-Content -LiteralPath '.\issue-668-trace.txt' -Encoding utf8
```

Also report the engine-version/startup header and any fatal error separately. If there are no diagnostic lines after spawning dogs, report that explicitly and verify the executable and `-corpse_debug` argument; do not interpret an empty trace as success.

## Reading the evidence

The suspected sequence, correlated by monster ID and corpse pointer within one run, is:

1. `memory_add`: the corpse entered that monster's memory.
2. `lock_set to=1` and `selection_lock`: the dog reserved it.
3. `memory_drop_locked selected=1`: the owner's memory removed its own selection because it was locked.
4. `selection_clear reason=not_in_memory`: the client-frame update cleared the selection.
5. `dog_state ... rest=1`: the dog returned to Rest, if that transition occurred.

Other events:

- `memory_drop_invalid`: age/window, food, alive, and destruction fields distinguish other invalidation conditions. This is not necessarily the lock-pruning bug.
- `eat_enter`: entered the eating substate; `eat_slice` shows actual food consumption.
- `drag_enter`: entered drag initialization; this alone does **not** prove that physics capture succeeded or the corpse moved.
- `lock_set to=0`: an explicit unlock was requested; the existing cooldown can still make the corpse temporarily unavailable.
- `dog_state` records requested top-level transitions, selected/available corpse pointers, and enemy pointer. It is logged before the transition is executed.

Do not dereference or reuse the logged pointer values; they are only correlation labels for this run. If the baseline never selects a corpse, investigate the scenario/configuration rather than claiming that the proposed fix works.

## Rollback

Close the game, restore the backed-up executable (and PDB if one was replaced), verify its original SHA-256, restore the launch arguments/entry, and return to the normal MO2 profile. Do not continue the regular campaign on this diagnostic executable. No new gamedata is installed by this package.

## Development checks and limitations

```powershell
pwsh ./tools/issue-668/verify-diagnostics.ps1
```

This structural check removes the ten opt-in diagnostic blocks and verifies that the remaining six engine files exactly match the pinned upstream source. It also rejects changes to other tracked files under `src`. It is **not** a C++ compiler, gameplay-equivalence proof, or regression reproduction.

The Windows CI build is the compilation check. The test above is the runtime check. A candidate fix will follow only after the baseline evidence is understood. Long interactions beyond the 20-second memory window, competing dogs, lock release, interruptions, and entity removal remain follow-up acceptance cases, not results already obtained.
