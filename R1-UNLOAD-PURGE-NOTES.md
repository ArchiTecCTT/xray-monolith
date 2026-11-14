# Free unused models and textures on save load (branch `r1-unload-purge`)

## The problem

Every save load in one session kept everything loaded before it. In a session with 6 reloads on the
same level, D3D textures grew 1.52 -> 2.01 -> 2.45 -> 2.64 -> 2.86 GB and committed memory 4.77 -> 7.25 GB until the
game ran out of memory.

Two causes:

1. **Base models were never freed.** A save load runs `CLevel::net_Stop` -> `Render->models_Clear(FALSE)` ->
   `CModelPool::ClearPool(FALSE)`. That deletes the model *clones* (one per object) and lowers the base model's
   `refs`. With `FALSE` it never frees the *base* model, even at `refs == 0` (only names containing `#` were freed).
   The base keeps its geometry, its shaders and, through them, its textures. Base models were only freed at
   app exit. R2/R4 call `ClearPool(true)` in `level_Unload`, but by then the pool is already empty, so it did
   nothing either.
2. **R1 ignored `r__clear_models_on_unload` / `r__clear_resources_on_unload`.** Only the R2/R3/R4 loaders read
   them.

## What changed

* `CModelPool::PurgeUnused()` (`src/Layers/xrRender/ModelPool.cpp`) frees base models that nothing uses. A base
  is freed only when all of these hold:
  1. `refs == 0` (no clone counted),
  2. no entry in `Registry` has its name (no clone in use, parked in the pool, or queued for deferred deletion),
  3. it is not a child/LOD of another base model, and has none.

  Clones share geometry, bones and shaders with their base, so a base with any clone left is always kept: the
  actor's hands and HUD, weapons, NPCs, anything a script spawned. A model asked for again later is loaded again
  from disk by `Create()`. Unused animations (`motions_container::clean(false)`) are freed after the models.
* Two `refs` fixes, so the count can be trusted: `Instance_Duplicate` now increments it under the exclusive lock
  (it was a write under a shared lock), and `Instance_Register` no longer adds an extra ref when two loads of the
  same model race (that ref was never dropped, so the model could never be freed).
* R1 `CRender::level_Unload` (`src/Layers/xrRenderPC_R1/FStaticRender_Loader.cpp`), after `Shaders.clear_and_free()`:
  * `r__clear_models_on_unload on` or `r__clear_resources_on_unload on`: discard parked clones, then
    `PurgeUnused()`. Textures used only by the freed models are freed with them.
  * `r__clear_resources_on_unload on` also does what R2 does: `UnloadAllTexturesOnLevelUnload()` (release the
    surface of every texture except UI, `$` render targets and `$user$` textures; they reload when next used),
    drop the "necessary textures" list, `Evict()`.
* Background texture loads are waited for first (`CResourceManager::WaitTextureLoads()`): at the start of R1
  `level_Unload`, before the purge on R2/R3/R4, and inside `UnloadAllTexturesOnLevelUnload()`. A queued load holds a
  raw `CTexture*`; freeing that texture before the task ran could corrupt the heap (the 0xc0000374 crash).
* R2/R3/R4 call `PurgeUnused()` right after their `ClearPool(true)`, under the same settings.
* `r__tex_evict_*` (DX11 only) is untouched.

## Settings

At least one of these must be on, otherwise nothing changes:

```
r__clear_models_on_unload on      // free unused models (and their textures)
r__clear_resources_on_unload on   // the above, plus release all non-UI texture surfaces
```

`r__clear_resources_on_unload on` alone is enough (it implies the model purge). Loads can get a little slower
because models and textures that were still cached are read from disk again (noticeable on a USB HDD).

## Log lines

On every level unload (save load, level change, quit to menu) R1 writes:

```
* [R1 unload purge]: models 1830 -> 412 (freed 1418), textures 5120 -> 2210, D3D textures 1490000 K -> 610000 K (surfaces unloaded, reload on use)
```

(numbers made up). R2/R3/R4 write `* [unload purge]: freed N unused base models, M left`. The existing
`* [ D3D ]: textures[... K]` line right after it is the total after the unload.

## How to check in game

1. Load a save. Open the console and run `stat_memory`, then `dump_resources`.
2. Load the same save 3 more times (no level change). After load 4 run `stat_memory` and `dump_resources` again.
3. In the log compare, load 1 against load 4:
   * the `* [x-ray]: Full Memory Stats` block: `* [ D3D ]: textures[... K]` and `process heap`;
   * `dump_resources`: `--- models: N` and `* RM_Dump: textures : N`.

Before this change those numbers climb with every load. With the change they should stay about the same
(+-10%) from load 2 on. Also check after a reload: the hands and the weapon in them, inventory icons, NPCs, weapons
on the ground and particles (fire, anomalies) look right.
