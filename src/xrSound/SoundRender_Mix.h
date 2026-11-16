#pragma once

// Listener-side mix for the headphones feature (docs/audio_api.md).
//
// Everything here only scales the gain handed to OpenAL for a playing source (CSoundRender_TargetA::fill_parameters).
// It never touches p_source.volume, max_ai_distance, the sound events or anything else the AI hearing path
// (CSoundRender_Emitter::Event_Propagade -> IGame_Level::SoundEvent_Register -> feel_sound_new) reads.
// Not saved, not networked; mix_reset() or a game restart puts everything back to 1.

// highest gain a source may get after the mix (OpenAL AL_MAX_GAIN of every source is set to this)
#define SOUND_MIX_MAX_GAIN 4.f
// SOUND_TYPE_SHOOTING (0x00200000) | SOUND_TYPE_OBJECT_EXPLODING (0x00000100), see ai_sounds.h
#define SOUND_MIX_DEFAULT_LOUD_MASK 0x00200100u

// One frame of the loud-sound limiter: a hard-knee gain computer on the detector level and a one-pole smoother on the
// gain reduction in dB (attack when it deepens, release when it recovers). level_lin: the loudest "loud" sound at the
// listener this frame (linear, 1 = full). Returns the detector level in dB (-120 = nothing). Self-contained so
// tests/audio_mix_test.cpp runs this same code.
inline float sound_mix_limiter_step(float& gr_db, float level_lin, bool on, float threshold_db, float ratio,
                                    float attack_sec, float release_sec, float dt)
{
	const float level_db = level_lin > 1e-6f ? 20.f * log10f(level_lin) : -120.f;
	float target = 0.f;
	if (on && level_db > threshold_db && ratio > 1.f)
		target = -(level_db - threshold_db) * (1.f - 1.f / ratio);
	const float tau = target < gr_db ? attack_sec : release_sec;
	if (tau <= 0.f || dt <= 0.f)
		gr_db = tau <= 0.f ? target : gr_db;
	else
		gr_db += (target - gr_db) * (1.f - expf(-dt / tau));
	if (target == 0.f && gr_db > -1e-4f)
		gr_db = 0.f;
	return level_db;
}

struct CSoundRender_Mix
{
	// a gain that fades linearly to its target over the time given when it was set (real time, not game time)
	struct ramp
	{
		float cur = 1.f;
		float target = 1.f;
		float rate = 0.f; // gain units per second, 0 = jump

		void set(float g, float fade_sec)
		{
			target = g;
			rate = fade_sec > 0.f ? _abs(target - cur) / fade_sec : 0.f;
			if (rate <= 0.f)
				cur = target;
		}

		void step(float dt)
		{
			if (cur == target)
				return;
			const float d = target - cur;
			const float m = rate * dt;
			if (rate <= 0.f || m >= _abs(d))
				cur = target;
			else
				cur += d > 0.f ? m : -m;
		}
	};

	// parameters (written by the setters under 'lock', read by the update under 'lock')
	ramp cat[smc_count];
	float steps_range_k = 1.f;

	bool lim_on = false;
	float lim_threshold_db = -12.f;
	float lim_attack_ms = 5.f;
	float lim_release_ms = 400.f;
	float lim_ratio = 20.f;
	// g_type bits that make a sound "loud" (any bit set): SOUND_TYPE_SHOOTING | SOUND_TYPE_OBJECT_EXPLODING of
	// xrServerEntities/ai_sounds.h (shots, and explosions, which CExplosive types as weapon shooting)
	u32 lim_mask = SOUND_MIX_DEFAULT_LOUD_MASK;
	float lim_own_k = 1.f; // weight of 2D loud sounds (the actor's own first-person shots)

	// state, written by the sound update only
	float lim_gr_db = 0.f; // current gain reduction (<= 0)
	float lim_gain = 1.f; // 10^(lim_gr_db / 20)
	float lim_level_db = -120.f; // last detector level

	xrCriticalSection lock;
};
