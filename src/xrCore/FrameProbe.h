#pragma once

// Where a frame's time goes, written to the log. For finding out what a stutter is made of before changing anything:
// whether the frame was held up by the game update, by drawing, by the driver, by the worker tasks, by the Lua
// collector or by something read from disk. Off unless one of the two console values below is above 0, and then it
// costs a few timer reads per frame.
//
//   mt_frame_spike_ms N     one "[frame]" line for every frame that took N ms or longer (at most four a second)
//   mt_frame_summary_sec N  one "[frames]" line every N seconds with the averages of the same numbers
//
// docs/frame_probe.md explains how to read the two lines.
namespace FrameProbe
{
	// The main thread's part of a frame, in the order it runs
	enum EPhase
	{
		phUpdate = 0, // the game update: scripts, objects, physics (CRenderDevice::FrameMove) and the particle bookkeeping
		phPrep, // matrices, starting the worker tasks, the frame limiter's wait
		phRender, // building and drawing the scene, without the present below
		phPresent, // handing the finished frame to the driver. Long here means waiting for the GPU
		phWait, // waiting for the worker tasks after the frame is drawn. Long here means the workers are the limit
		phCount
	};

	// The worker tasks of a frame (xrEngine/EngineThreading.cpp). They run while the main thread draws
	enum ETask
	{
		tkParticles = 0, // rain and particle systems
		tkOcclGrass, // occlusion (HOM) and grass visibility
		tkBones, // skeleton poses
		tkGame, // the game task as a whole: HUD, sound events, the scheduler below, path/vision/bullet jobs, sound
		tkScheduler, // the deferred scheduler inside the game task (NPC and object updates, their scripts)
		tkVision, // NPC line-of-sight ray tests, one task per NPC
		tkLuaGC, // the Lua collector steps run on a worker (lua_parallel_gc)
		tkCount
	};

	// Things read from disk
	enum ELoad
	{
		ldModel = 0,
		ldTexture,
		ldSound,
		ldCount
	};

	XRCORE_API extern int spike_ms; // console mt_frame_spike_ms
	XRCORE_API extern int summary_sec; // console mt_frame_summary_sec

	// true for the whole of a frame that is being measured. Set in frame_begin, so a frame is all in or all out
	XRCORE_API extern bool measuring;

	XRCORE_API u64 now(); // timer ticks

	// Main thread
	XRCORE_API void frame_begin();
	XRCORE_API void mark(EPhase done); // the phase that ends here; the time since the last mark is its time
	XRCORE_API void add_inside(EPhase phase, u64 ticks); // time that belongs to "phase" though it ran inside another one
	XRCORE_API void frame_end(bool report); // after the worker tasks are joined. report = this frame is real gameplay

	// Any thread
	XRCORE_API void task_add(ETask task, u64 ticks, u32 calls);

	// The game tells how long the Lua collector ran. Called once per frame from frame_end with on = measuring.
	// out[0..3] are totals since start: ticks in GC steps on the main thread, on other threads, in atomic phases, and
	// the count of atomic phases. out[4] is the longest single step since the last call. Left alone when on is false.
	typedef void gc_hook_t(bool on, u64 out[5]);
	XRCORE_API extern gc_hook_t* gc_hook;

	struct TaskScope
	{
		ETask task;
		u64 t0;

		IC TaskScope(ETask t) : task(t), t0(measuring ? now() : 0)
		{
		}

		IC ~TaskScope()
		{
			if (t0)
				task_add(task, now() - t0, 1);
		}
	};

	// A load nested in another one on the same thread (a model that pulls in its textures) is counted as part of the
	// outer one. load_begin returns 0 for the nested one and when nothing is being measured.
	XRCORE_API u64 load_begin();
	XRCORE_API void load_end(ELoad kind, u64 t0, LPCSTR name);

	// "name" has to stay valid until the scope ends
	struct LoadScope
	{
		ELoad kind;
		LPCSTR name;
		u64 t0;

		IC LoadScope(ELoad k, LPCSTR n) : kind(k), name(n), t0(load_begin())
		{
		}

		IC ~LoadScope()
		{
			if (t0)
				load_end(kind, t0, name);
		}
	};
}
