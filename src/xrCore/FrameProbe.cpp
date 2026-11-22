#include "stdafx.h"
#pragma hdrstop

#include "FrameProbe.h"

// See FrameProbe.h and docs/frame_probe.md

namespace FrameProbe
{
	XRCORE_API int spike_ms = 0;
	XRCORE_API int summary_sec = 0;
	XRCORE_API bool measuring = false;
	XRCORE_API gc_hook_t* gc_hook = nullptr;

	// One frame, or the sum of many, in milliseconds and counts
	struct Row
	{
		double total;
		double phase[phCount];
		double task[tkCount];
		double task_n[tkCount];
		double gc_main, gc_other, gc_atomic, gc_atomics;
		double load[ldCount];
		double load_n[ldCount];
		double load_workers;

		void clear() { ZeroMemory(this, sizeof(*this)); }

		void add(const Row& r)
		{
			total += r.total;
			for (int i = 0; i < phCount; ++i) phase[i] += r.phase[i];
			for (int i = 0; i < tkCount; ++i)
			{
				task[i] += r.task[i];
				task_n[i] += r.task_n[i];
			}
			gc_main += r.gc_main;
			gc_other += r.gc_other;
			gc_atomic += r.gc_atomic;
			gc_atomics += r.gc_atomics;
			for (int i = 0; i < ldCount; ++i)
			{
				load[i] += r.load[i];
				load_n[i] += r.load_n[i];
			}
			load_workers += r.load_workers;
		}

		void scale(double k)
		{
			total *= k;
			for (int i = 0; i < phCount; ++i) phase[i] *= k;
			for (int i = 0; i < tkCount; ++i)
			{
				task[i] *= k;
				task_n[i] *= k;
			}
			gc_main *= k;
			gc_other *= k;
			gc_atomic *= k;
			gc_atomics *= k;
			for (int i = 0; i < ldCount; ++i)
			{
				load[i] *= k;
				load_n[i] *= k;
			}
			load_workers *= k;
		}
	};

	// This frame. The main thread owns everything that is not atomic
	static DWORD s_main_tid = 0;
	static u64 s_frame_start = 0;
	static u64 s_last_mark = 0;
	static u64 s_inside = 0; // counted by add_inside since the last mark
	static u64 s_phase[phCount] = {};
	static std::atomic<u64> s_task[tkCount];
	static std::atomic<u32> s_task_n[tkCount];
	static u64 s_load[ldCount] = {};
	static u32 s_load_n[ldCount] = {};
	static u64 s_load_max = 0;
	static string128 s_load_name = "";
	static std::atomic<u64> s_load_workers;
	static thread_local bool tl_loading = false;

	// The collector's totals at the end of the previous measured frame
	static bool s_gc_on = false;
	static bool s_gc_valid = false;
	static u64 s_gc_prev[4] = {};

	// The [frame] lines
	static u64 s_spike_last = 0;
	static u32 s_spike_hidden = 0;

	// The [frames] line
	static Row s_sum;
	static u32 s_sum_frames = 0;
	static u32 s_sum_long = 0;
	static double s_sum_max = 0.;
	static u64 s_sum_start = 0;

	static u64 ticks_per_second()
	{
		static u64 freq = 0;
		if (!freq)
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			freq = u64(f.QuadPart);
		}
		return freq;
	}

	static double to_ms(u64 ticks) { return double(ticks) * 1000. / double(ticks_per_second()); }

	u64 now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return u64(t.QuadPart);
	}

	void frame_begin()
	{
		measuring = (spike_ms > 0) || (summary_sec > 0);
		if (!measuring)
			return;

		// Whatever ran between two frames (a loading screen) is not this frame's
		s_main_tid = GetCurrentThreadId();
		for (int i = 0; i < phCount; ++i) s_phase[i] = 0;
		for (int i = 0; i < tkCount; ++i)
		{
			s_task[i].store(0);
			s_task_n[i].store(0);
		}
		for (int i = 0; i < ldCount; ++i)
		{
			s_load[i] = 0;
			s_load_n[i] = 0;
		}
		s_load_max = 0;
		s_load_name[0] = 0;
		s_load_workers.store(0);
		s_inside = 0;
		s_frame_start = s_last_mark = now();
	}

	void mark(EPhase done)
	{
		if (!measuring)
			return;
		const u64 t = now();
		const u64 elapsed = t - s_last_mark;
		s_phase[done] += (elapsed > s_inside) ? (elapsed - s_inside) : 0;
		s_inside = 0;
		s_last_mark = t;
	}

	void add_inside(EPhase phase, u64 ticks)
	{
		if (!measuring)
			return;
		s_phase[phase] += ticks;
		s_inside += ticks;
	}

	void task_add(ETask task, u64 ticks, u32 calls)
	{
		s_task[task].fetch_add(ticks);
		s_task_n[task].fetch_add(calls);
	}

	u64 load_begin()
	{
		if (!measuring || tl_loading)
			return 0;
		tl_loading = true;
		return now();
	}

	void load_end(ELoad kind, u64 t0, LPCSTR name)
	{
		tl_loading = false;
		const u64 ticks = now() - t0;
		if (GetCurrentThreadId() != s_main_tid)
		{
			s_load_workers.fetch_add(ticks);
			return;
		}
		s_load[kind] += ticks;
		s_load_n[kind]++;
		if (ticks > s_load_max)
		{
			s_load_max = ticks;
			strncpy_s(s_load_name, sizeof(s_load_name), name ? name : "?", _TRUNCATE);
		}
	}

	// Cuts the text off if it does not fit. xr_sprintf stops the game instead (vsprintf_s), which a log line must not
	static void put_text(char* dest, size_t size, LPCSTR text, ...)
	{
		va_list args;
		va_start(args, text);
		_vsnprintf_s(dest, size, _TRUNCATE, text, args);
		va_end(args);
	}

	// "workers: ... | lua gc: ... | loads ..." for one frame (counts are whole) or an average (counts have a decimal)
	static void describe(string1024& out, const Row& r, bool average)
	{
		LPCSTR n = average ? "%.1f" : "%.0f";
		string32 vision_n, gc_n, atomics, model_n, texture_n, sound_n;
		put_text(vision_n, sizeof(vision_n), n, r.task_n[tkVision]);
		put_text(gc_n, sizeof(gc_n), n, r.task_n[tkLuaGC]);
		put_text(atomics, sizeof(atomics), average ? "%.2f" : "%.0f", r.gc_atomics);
		put_text(model_n, sizeof(model_n), n, r.load_n[ldModel]);
		put_text(texture_n, sizeof(texture_n), n, r.load_n[ldTexture]);
		put_text(sound_n, sizeof(sound_n), n, r.load_n[ldSound]);

		put_text(out, sizeof(out),
		           "main: update %.1f, prep %.1f, render %.1f, present %.1f, wait %.1f"
		           " | workers: particles %.1f, occl+grass %.1f, bones %.1f, game %.1f (scheduler %.1f), vision %.1f x%s, lua gc %.1f x%s"
		           " | lua gc: main thread %.1f, workers %.1f, atomic %.1f x%s"
		           " | loads on main: model %.1f x%s, texture %.1f x%s, sound %.1f x%s; on workers %.1f",
		           r.phase[phUpdate], r.phase[phPrep], r.phase[phRender], r.phase[phPresent], r.phase[phWait],
		           r.task[tkParticles], r.task[tkOcclGrass], r.task[tkBones], r.task[tkGame], r.task[tkScheduler],
		           r.task[tkVision], vision_n, r.task[tkLuaGC], gc_n,
		           r.gc_main, r.gc_other, r.gc_atomic, atomics,
		           r.load[ldModel], model_n, r.load[ldTexture], texture_n, r.load[ldSound], sound_n, r.load_workers);
	}

	void frame_end(bool report)
	{
		// The collector's clock runs only while somebody is measuring
		u64 gc[5] = {};
		if (gc_hook && (measuring || s_gc_on))
		{
			gc_hook(measuring, gc);
			s_gc_on = measuring;
		}
		if (!measuring)
		{
			s_gc_valid = false;
			return;
		}

		const u64 t_end = now();

		Row r;
		r.clear();
		r.total = to_ms(t_end - s_frame_start);
		for (int i = 0; i < phCount; ++i) r.phase[i] = to_ms(s_phase[i]);
		for (int i = 0; i < tkCount; ++i)
		{
			r.task[i] = to_ms(s_task[i].load());
			r.task_n[i] = double(s_task_n[i].load());
		}
		for (int i = 0; i < ldCount; ++i)
		{
			r.load[i] = to_ms(s_load[i]);
			r.load_n[i] = double(s_load_n[i]);
		}
		r.load_workers = to_ms(s_load_workers.load());

		double gc_step_max = 0.;
		if (s_gc_valid)
		{
			r.gc_main = to_ms(gc[0] - s_gc_prev[0]);
			r.gc_other = to_ms(gc[1] - s_gc_prev[1]);
			r.gc_atomic = to_ms(gc[2] - s_gc_prev[2]);
			r.gc_atomics = double(gc[3] - s_gc_prev[3]);
			gc_step_max = to_ms(gc[4]);
		}
		if (gc_hook)
		{
			for (int i = 0; i < 4; ++i) s_gc_prev[i] = gc[i];
			s_gc_valid = true;
		}

		if (!report)
			return;

		// A frame is "long" for the summary at the spike limit, or at 33 ms (30 fps) when no limit is set
		const double long_ms = (spike_ms > 0) ? double(spike_ms) : 33.;

		if (spike_ms > 0 && r.total >= double(spike_ms))
		{
			// at most four lines a second, so that a bad stretch does not flood the log and make itself worse
			if (s_spike_last && (t_end - s_spike_last) < ticks_per_second() / 4)
				s_spike_hidden++;
			else
			{
				string1024 text;
				describe(text, r, false);
				string256 slowest = "";
				if (s_load_max)
					put_text(slowest, sizeof(slowest), " | slowest load %.1f %s", to_ms(s_load_max), s_load_name);
				string64 hidden = "";
				if (s_spike_hidden)
					put_text(hidden, sizeof(hidden), " | +%u long frames not shown", s_spike_hidden);
				Msg("[frame] %.1f ms | %s | longest gc step %.1f%s%s", r.total, text, gc_step_max, slowest, hidden);
				s_spike_last = t_end;
				s_spike_hidden = 0;
			}
		}

		if (summary_sec > 0)
		{
			if (!s_sum_frames)
			{
				s_sum.clear();
				s_sum_start = s_frame_start;
				s_sum_long = 0;
				s_sum_max = 0.;
			}
			s_sum.add(r);
			s_sum_frames++;
			if (r.total >= long_ms)
				s_sum_long++;
			if (r.total > s_sum_max)
				s_sum_max = r.total;

			if ((t_end - s_sum_start) >= u64(summary_sec) * ticks_per_second())
			{
				s_sum.scale(1. / double(s_sum_frames));
				string1024 text;
				describe(text, s_sum, true);
				Msg("[frames] %u frames in %.1f s, average %.1f ms, longest %.1f, %u at %.0f ms or more | averages: %s",
				    s_sum_frames, to_ms(t_end - s_sum_start) / 1000., s_sum.total, s_sum_max, s_sum_long, long_ms, text);
				s_sum_frames = 0;
			}
		}
		else
			s_sum_frames = 0;
	}
}
