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

// ---- compressor (docs/audio_api.md "Compressor") --------------------------------------------------------------------
// Default detector mask: every action bit of ESoundTypes except SOUND_TYPE_AMBIENT (0x80), so shots, explosions, hits,
// voices, steps, item and physics sounds count and ambient-typed or untyped loops never hold the compressor down.
#define SOUND_MIX_DEFAULT_COMP_MASK 0x07FFFF00u

// Static curve of the compressor: the gain reduction in dB (<= 0) for a detector level, a soft knee of knee_db around
// the threshold (0 = hard knee). Below the knee nothing is reduced; above it each dB over the threshold becomes
// 1/ratio dB; inside the knee the two join with a parabola (no corner).
inline float sound_mix_compressor_curve(float level_db, float threshold_db, float ratio, float knee_db)
{
	if (ratio <= 1.f)
		return 0.f;
	const float slope = 1.f / ratio - 1.f; // <= 0
	const float over = level_db - threshold_db;
	if (knee_db > 0.f)
	{
		if (2.f * over <= -knee_db)
			return 0.f;
		if (2.f * over < knee_db)
			return slope * (over + knee_db * 0.5f) * (over + knee_db * 0.5f) / (2.f * knee_db);
	}
	return over > 0.f ? slope * over : 0.f;
}

// One frame of the compressor: the gain in dB that every compressed source gets (makeup + gain reduction), smoothed
// with a one-pole filter: attack when the gain falls, release when it rises. Switched off, the gain goes back to 0 dB
// with the release time (no jump) and ends on exactly 0. level_lin: the loudest counted sound at the listener this
// frame (linear, 1 = full). Returns the detector level in dB (-120 = nothing).
inline float sound_mix_compressor_step(float& gain_db, float level_lin, bool on, float threshold_db, float ratio,
                                       float knee_db, float makeup_db, float attack_sec, float release_sec, float dt)
{
	const float level_db = level_lin > 1e-6f ? 20.f * log10f(level_lin) : -120.f;
	const float target = on ? makeup_db + sound_mix_compressor_curve(level_db, threshold_db, ratio, knee_db) : 0.f;
	const float tau = (on && target < gain_db) ? attack_sec : release_sec;
	if (tau <= 0.f)
		gain_db = target;
	else if (dt > 0.f)
		gain_db += (target - gain_db) * (1.f - expf(-dt / tau));
	if (_abs(target - gain_db) < 1e-4f)
		gain_db = target;
	return level_db;
}

// What OpenAL does with distance for a mono source (the default AL_INVERSE_DISTANCE_CLAMPED model; the engine never
// sets another): gain = ref / (ref + rolloff * (clamp(dist, ref, max) - ref)). The engine multiplies its own linear
// fade on top (CSoundRender_Emitter::update_culling). The compressor's detector uses this so that its level is what
// is heard. Mirrors alu.cpp (CalcAttnSourceParams) of the OpenAL Soft in this tree.
inline float sound_mix_al_distance_gain(float dist, float ref_dist, float max_dist, float rolloff)
{
	if (max_dist < ref_dist || ref_dist <= 0.f)
		return 1.f;
	const float d = dist < ref_dist ? ref_dist : (dist > max_dist ? max_dist : dist);
	const float den = ref_dist + rolloff * (d - ref_dist);
	return den > 0.f ? ref_dist / den : 1.f;
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

	// compressor: one gain for every source of the categories in comp_cats (bit = 1 << esound_mix_category)
	bool comp_on = false;
	float comp_threshold_db = -24.f;
	float comp_ratio = 4.f;
	float comp_attack_ms = 10.f;
	float comp_release_ms = 250.f;
	float comp_makeup_db = 0.f;
	float comp_knee_db = 6.f;
	u32 comp_cats = (1u << smc_world) | (1u << smc_steps_others);
	u32 comp_mask = SOUND_MIX_DEFAULT_COMP_MASK; // g_type bits a 3D sound needs (any) to drive the detector
	// the same for 2D sounds (the actor's own): shots and explosions only, so the actor's steps, breathing loops and
	// item sounds, which play at full gain whatever their sample holds, never pump the compressor or hold it down
	u32 comp_own_mask = SOUND_MIX_DEFAULT_LOUD_MASK;
	float comp_own_k = 1.f; // detector weight of those 2D sounds

	// distance curve per category: a factor on OpenAL's rolloff (1 = unchanged, 0 = no inverse-distance falloff, only
	// the engine's linear fade to max_distance). Read by fill_parameters and the detector; never by the AI path.
	float rolloff_k[smc_count] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};

	// state, written by the sound update only
	float lim_gr_db = 0.f; // current gain reduction (<= 0)
	float lim_gain = 1.f; // 10^(lim_gr_db / 20)
	float lim_level_db = -120.f; // last detector level
	float comp_gain_db = 0.f; // current compressor gain: makeup + gain reduction, smoothed
	float comp_gain = 1.f; // 10^(comp_gain_db / 20)
	float comp_level_db = -120.f; // last compressor detector level
	u32 comp_cats_cur = 0; // comp_cats as of the last update (read by mix_gain without the lock)

	xrCriticalSection lock;
};
