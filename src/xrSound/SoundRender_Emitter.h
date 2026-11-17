#ifndef SoundRender_EmitterH
#define SoundRender_EmitterH
#pragma once

#include "soundrender.h"
#include "SoundRender_Core.h"
#include "soundrender_environment.h"

class CSoundRender_Emitter : public CSound_emitter
{
	float starting_delay;
public:
	enum State
	{
		stStopped = 0,

		stStartingDelayed,
		stStartingLoopedDelayed,

		stStarting,
		stStartingLooped,

		stPlaying,
		stPlayingLooped,

		stSimulating,
		stSimulatingLooped,

		stFORCEDWORD = u32(-1)
	};

private:
	bool need_preplay_update;

public:
#ifdef DEBUG
	u32							dbg_ID;
#endif

	CSoundRender_Target* target;
	IC CSoundRender_Source* source() { return (CSoundRender_Source*)owner_data->handle; };
	ref_sound_data_ptr owner_data;

	u32 get_bytes_total() const;
	float get_length_sec() const;

	float priority_scale;
	float smooth_volume;
	float occluder_volume; // USER
	float fade_volume;
	float mix_level; // headphones limiter detector: base_volume x distance x occlusion x fade, without the volume sliders
	float mix_heard; // headphones compressor detector: mix_level (at most 1) x OpenAL's distance attenuation
	float mix_src_level; // headphones auto-gain: the same without the fade in / out (the source's own level)
	float mix_ag_db; // this source's auto-gain, dB, smoothed (CSoundRender_Core::mix_gain)
	bool mix_ag_set; // false until the source's first rendered update, which takes its auto-gain without smoothing
	Fvector occluder [3];

	State m_current_state;
	u32 m_stream_cursor;
	u32 m_cur_handle_cursor;
	CSound_params p_source;

	int iPaused;
	BOOL bMoved;
	BOOL b2D;
	bool bIntro;
	BOOL bStopping;
	BOOL bRewind;
	float fTimeStarted; // time of "Start"
	float fTimeToStop; // time to "Stop"
	float fTimeToPropagade;

	u32 marker;
	void i_stop();

	void set_cursor(u32 p);
	u32 get_cursor(bool b_absolute) const;
	void move_cursor(int offset);

public:
	void Event_Propagade();
	void Event_ReleaseOwner();
	BOOL isPlaying(void) { return m_current_state != stStopped; }
	// headphones bang guard: seconds since this one-shot sound began to play; -1 while it waits, loops or is stopped
	float mix_onset_age(float now) const
	{
		return (m_current_state == stPlaying || m_current_state == stSimulating) ? now - fTimeStarted : -1.f;
	}

	virtual BOOL is_2D() { return b2D; }
	virtual void switch_to_2D();
	virtual void switch_to_Intro() override;
	virtual void switch_to_3D();
	virtual void set_position(const Fvector& pos);

	virtual void set_frequency(float scale)
	{
		VERIFY(_valid(scale));
		p_source.freq = scale;

		// demonized: if the sound is short, apply pitch variation, so that stuff like music and most of speech won't be randomized
		if (get_length_sec() < 10)
			p_source.freq *= (1.f + p_source.pitch_variation);

		if (fTimeToStop != 0.f)
			fTimeToStop = SoundRender->fTimer_Value + ((get_length_sec() - (SoundRender->fTimer_Value - fTimeStarted)) / (scale * psSpeedOfSound));
	}

	virtual void set_range(float min, float max)
	{
		VERIFY(_valid(min)&&_valid(max));
		p_source.min_distance = min;
		p_source.max_distance = max;
	}

	virtual void set_volume(float vol)
	{
		if (!_valid(vol)) vol = 0.0f;
		p_source.volume = vol;
	}

	virtual void set_priority(float p) { priority_scale = p; }
	virtual const CSound_params* get_params() { return &p_source; }

	void fill_block(void* ptr, u32 size);
	void fill_data(u8* ptr, u32 offset, u32 size);

	float priority();
	void start(ref_sound* _owner, BOOL _loop, float delay);
	void cancel(); // manager forces out of rendering
	void update(float dt);
	BOOL update_culling(float dt);
	void update_environment(float dt);
	void rewind();
	virtual void stop(BOOL bDeffered);
	void pause(BOOL bVal, int id);

	virtual u32 play_time();

	CSoundRender_Emitter();
	~CSoundRender_Emitter();
};
#endif
