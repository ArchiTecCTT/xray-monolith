# Frame probe and the two Lua collector options

Two things for hunting stutters on a CPU with few cores:

* a log line that says where a long frame's time went, and
* two options that move Lua collector work away from the scripts.

Everything is switched by console vars, saved in `user.ltx`, and all four are `0` = off by default. With
all four at `0` the exe behaves as before.

| var | default | range | what it does |
|---|---|---|---|
| `mt_frame_spike_ms` | 0 | 0 .. 1000 | One `[frame]` line for every frame that took this many ms or longer. At most four lines a second. |
| `mt_frame_summary_sec` | 0 | 0 .. 600 | One `[frames]` line every this many seconds, with the averages of the same numbers. |
| `lua_parallel_gc_pause` | 0 | 0 .. 400 % | The parallel collector starts its next collection only when the Lua heap has grown to this percentage of its size at the end of the last one. `0`..`100` = start at once, as before. |
| `lua_parallel_gc_slack` | 0 | 0 .. 262144 KB | While a collection runs, scripts do no collector work of their own until they have allocated this much since the parallel collector's last step. `0` = as before. |

A first session: `mt_frame_spike_ms 33` and `mt_frame_summary_sec 10`, play a big firefight, then read the log.

## How a frame runs

The main thread does the game update first: scripts, objects, physics. Nothing else runs then. After it,
the main thread draws the frame, and while it draws, worker tasks do the rest: particles, occlusion and
grass, skeleton poses, the deferred scheduler with its NPC updates, NPC sight rays, Lua collector steps.
When the frame is drawn the main thread waits for those tasks, and the frame is over.

So a frame lasts as long as the update, plus the longer of "draw it" and "the worker tasks". On two
cores the second half is crowded and the first half leaves a core idle.

## The lines

```
[frame] 58.3 ms | main: update 31.2, prep 0.4, render 9.8, present 12.1, wait 4.8 | workers: particles 3.1, occl+grass 1.2, bones 6.5, game 14.0 (scheduler 9.1), vision 2.2 x14, lua gc 5.0 x25 | lua gc: main thread 22.4, workers 3.1, atomic 19.8 x1 | loads on main: model 0.0 x0, texture 0.0 x0, sound 18.2 x1; on workers 0.0 | longest gc step 19.8 | slowest load 18.2 weapons\ak74\ak74_shoot
```

All numbers are milliseconds; `xN` is a count.

**main** is the main thread, in order, and adds up to the frame:

| field | what |
|---|---|
| `update` | the game update: scripts, objects, physics, the particle bookkeeping |
| `prep` | camera matrices, starting the worker tasks, and the frame limiter's wait when a limit is set |
| `render` | building and drawing the scene, without the present |
| `present` | handing the finished frame to the driver |
| `wait` | waiting for the worker tasks after the frame is drawn. The main thread runs tasks itself here if no worker has taken them |

**workers** is how long each worker task ran. They run at the same time as `render`, `present` and
`wait`, and as each other, so they do not add up to anything.

| field | what |
|---|---|
| `particles` | rain and particle systems |
| `occl+grass` | occlusion and grass visibility |
| `bones` | skeleton poses. With `mt_calc_bones 0`, or when the R1 thermal sync pulls them forward, the main thread does them and the time is inside `render` as well |
| `game` | the game task as a whole: HUD, sound events, the scheduler, path / bullet / level-sound jobs, the sound thread's frame |
| `scheduler` | the deferred scheduler inside `game`: NPC and object updates and the scripts they call |
| `vision` | NPC line-of-sight ray tests, summed over the NPCs that tested this frame |
| `lua gc` | the parallel collector's task, and how many steps it took |

**lua gc** is the time inside the collector, by the thread that ran it. `main thread` is collector work
done in the middle of scripts on the main thread; it is part of `update`. `workers` is the rest: the
parallel collector's task and collector work inside scripts the scheduler ran. `atomic` is the part of
both spent in atomic phases, the one piece of a collection that cannot be split up. `longest gc step` is
the longest single step in the frame. The first measured frame has no collector numbers.

**loads on main** is what the main thread read from disk in the frame: models, textures, sounds. A load
inside another one (a model that pulls in its textures) counts as the outer one. `slowest load` names
the one that took longest. `on workers` is the same for other threads, where it holds nobody up directly.

`[frames]` has the same fields as averages per frame, after a header with the frame count, the average
and longest frame, and how many frames were as long as `mt_frame_spike_ms` (33 ms when that is 0).
Only frames of running gameplay are counted: not the menu, a pause, or the first frames after a load.

## Reading it

| what is large | what it means |
|---|---|
| `present` | the GPU is the limit (or vsync, if it is on). No CPU-side change will help |
| `wait`, with `render` small | the worker tasks are the limit. The `workers` fields say which |
| `update`, and `lua gc: main thread` is most of it | a script paid for collector work. Try `lua_parallel_gc_slack` |
| `update`, and `loads on main` is most of it | something was read from disk for the first time. The name says what to prefetch |
| `update`, and neither of those | the scripts or the engine's own update, or the OS paging memory in. The probe cannot tell these apart |
| `lua gc` under workers, in every `[frames]` line | the parallel collector keeps a core busy. Try `lua_parallel_gc_pause` |

## The two collector options

With `lua_parallel_gc 1` (the default) a worker task runs collector steps while the frame is drawn: up
to `lua_parallel_gc_call_amount` steps of `lua_parallel_gcstep` KB each.

**`lua_parallel_gc_pause`.** Each of those steps asks for work whether a collection is running or not,
so the next collection starts the moment the last one ends and the collector never rests. On many cores
that is free. On two it competes with the other worker tasks for every frame of a firefight. With `120`
here the worker waits until the heap is 20 % larger than it was at the end of the last collection.
The cost is memory: the heap is allowed to grow by that much before anything is freed.

**`lua_parallel_gc_slack`.** While a collection is running, Lua makes the thread that allocates do a
slice of collector work about every 1 KB it allocates. That is how a long step, an atomic phase most of
all, lands inside whatever script happened to be running. With `8192` here (8 MB) the scripts do none
until they have allocated 8 MB since the worker's last step, and the worker does all of it. If the
worker stops stepping, as in a loading screen, the scripts take the work up again by themselves once
the slack is used up. The cost is that a collection takes more frames to finish, so the heap runs
somewhat larger.

Both raise the Lua heap. This exe's LuaJIT (2.0.4) has a hard ceiling below 2 GB for that heap and a
heavy mod list can already sit high. `jit.util.gcstat().total` is the heap in bytes; watch it for a
session before keeping a value. Suggested first values: `lua_parallel_gc_pause 120`,
`lua_parallel_gc_slack 8192`, one at a time, each compared against a `[frames]` line taken without it.

## For scripts

`jit.util.gcstat()` has four more fields: `gc_ms_main`, `gc_ms_other` and `gc_ms_atomic` are the
milliseconds the collector has run since start (main thread, other threads, atomic phases) and `atomics`
is the count of atomic phases. A script can take the difference across its own update to see what the
collector cost it. They only advance while `mt_frame_spike_ms` or `mt_frame_summary_sec` is above 0.

## Cost

Off: one test per frame and per hooked call. On: a few timer reads per frame, and two per collector
step, which is the larger part. Expect a fraction of a millisecond per frame on a script-heavy game.

## Files

| file | what |
|---|---|
| `src/xrCore/FrameProbe.h`, `.cpp` | the counters and the two log lines |
| `src/xrEngine/device.cpp`, `EngineThreading.cpp` | the marks in the frame and the worker tasks |
| `src/xrGame/Level.cpp` | `CLevel::LuaGC` with the two options, and the collector's times for the probe |
| `src/xrGame/vision_client.cpp`, `CustomMonster.cpp` | the sight-ray tasks |
| `src/Layers/xrRender/ModelPool.cpp`, `SH_Texture.cpp`, `src/xrSound/SoundRender_Source_loader.cpp` | the loads |
| `src/3rd party/luajit-2/src/lj_gc.c`, `lj_gc.h`, `lib_jit.c` | the collector's clock, the slack, the `gcstat` fields |
