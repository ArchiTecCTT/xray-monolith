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

// ---- per-source auto-gain (docs/audio_api.md "Auto-gain") ------------------------------------------------------------
// The gain in dB one source gets for its own level at the listener: levels are pulled toward target_db by the ratio
// (a level d dB away from the target comes out d / ratio dB away), quiet sounds by at most +max_boost_db, loud ones by
// at most -max_cut_db. Never turns the order of two levels around: a louder input never comes out quieter.
inline float sound_mix_autogain_curve(float level_db, float target_db, float ratio, float max_boost_db,
                                      float max_cut_db)
{
	if (ratio <= 1.f)
		return 0.f;
	float g = (target_db - level_db) * (1.f - 1.f / ratio);
	if (g > max_boost_db)
		g = max_boost_db;
	if (g < -max_cut_db)
		g = -max_cut_db;
	return g;
}

// 1 - e^(-dt / tau): the share of the way to its target a one-pole smoother goes in one frame (1 = jump)
inline float sound_mix_smooth_k(float dt, float tau_sec)
{
	if (tau_sec <= 0.f)
		return 1.f;
	return dt > 0.f ? 1.f - expf(-dt / tau_sec) : 0.f;
}

// One frame of one source's auto-gain smoother: attack (k_attack) when the gain has to fall, that is when the source
// got louder, release (k_release) when it may rise. Ends on exactly the target.
inline void sound_mix_autogain_smooth(float& gain_db, float target_db, float k_attack, float k_release)
{
	gain_db += (target_db - gain_db) * (target_db < gain_db ? k_attack : k_release);
	if (_abs(target_db - gain_db) < 1e-3f)
		gain_db = target_db;
}

inline float sound_mix_db_to_gain(float db) { return db == 0.f ? 1.f : expf(db * 0.11512925465f); } // 10^(db / 20)

// One source, one sound update: its auto-gain in dB. level_lin: the source's own level at the listener (linear,
// 1 = full). amount: its category's share of the curve (0 = none). The first call of a source (set == false) takes the
// gain at once, so a loud onset is never played at full gain first and a quiet one does not fade in; after that the
// gain follows through the smoother. Off (or amount 0) the target is 0 dB, so switching off fades each source back.
inline float sound_mix_autogain_source(float& gain_db, bool& set, float level_lin, bool on, float amount,
                                       float target_db, float ratio, float max_boost_db, float max_cut_db,
                                       float k_attack, float k_release)
{
	float target = 0.f;
	if (on && amount > 0.f)
	{
		const float level_db = level_lin > 1e-6f ? 20.f * log10f(level_lin) : -120.f;
		target = amount * sound_mix_autogain_curve(level_db, target_db, ratio, max_boost_db, max_cut_db);
	}
	if (!set)
	{
		set = true;
		gain_db = target;
	}
	else if (gain_db != target)
		sound_mix_autogain_smooth(gain_db, target, k_attack, k_release);
	return gain_db;
}

// ---- bang guard -----------------------------------------------------------------------------------------------------
// One frame of the bang guard: a duck in dB (<= 0) for everything but menus and music. level_lin: the loudest bang
// that started within the hold time (linear, 0 = none). Over the threshold the duck is the overshoot, at most
// max_duck_db, at once (no attack time); after the hold it recovers with the release time and ends on exactly 0.
inline float sound_mix_bang_step(float& gr_db, float level_lin, bool on, float threshold_db, float max_duck_db,
                                 float release_sec, float dt)
{
	const float level_db = level_lin > 1e-6f ? 20.f * log10f(level_lin) : -120.f;
	float target = 0.f;
	if (on && level_db > threshold_db)
	{
		const float over = level_db - threshold_db;
		target = -(over < max_duck_db ? over : max_duck_db);
	}
	if (target <= gr_db)
		gr_db = target;
	else
		gr_db += (target - gr_db) * sound_mix_smooth_k(dt, release_sec);
	if (target == 0.f && gr_db > -1e-3f)
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
	float rolloff_k[smc_count] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};

	// per-source auto-gain; ag_amount: how much of the curve a category gets (0 = none, 1 = all), ui and music stay 0
	bool ag_on = false;
	float ag_target_db = -14.f;
	float ag_ratio = 3.f;
	float ag_max_boost_db = 12.f; // smc_own is never boosted, whatever this says
	float ag_max_cut_db = 12.f;
	float ag_attack_ms = 5.f;
	float ag_release_ms = 80.f;
	float ag_amount[smc_count] = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f}; // world, steps_others, own

	// bang guard
	bool bang_on = false;
	float bang_threshold_db = -9.f;
	float bang_max_duck_db = 6.f;
	float bang_hold_ms = 30.f;
	float bang_release_ms = 25.f;
	u32 bang_mask = SOUND_MIX_DEFAULT_LOUD_MASK;

	// own split: untagged sounds of the listener's object, and 2D sounds with a game object, are smc_own
	bool own_split = false;
	const void* listener_obj = nullptr; // compared with ref_sound_data::g_object only, never dereferenced

	// state, written by the sound update only
	float lim_gr_db = 0.f; // current gain reduction (<= 0)
	float lim_gain = 1.f; // 10^(lim_gr_db / 20)
	float lim_level_db = -120.f; // last detector level
	float comp_gain_db = 0.f; // current compressor gain: makeup + gain reduction, smoothed
	float comp_gain = 1.f; // 10^(comp_gain_db / 20)
	float comp_level_db = -120.f; // last compressor detector level
	u32 comp_cats_cur = 0; // comp_cats as of the last update (read by mix_gain without the lock)
	// auto-gain parameters as of the last update (read per source by mix_gain without the lock)
	bool ag_on_cur = false;
	float ag_target_cur = -14.f, ag_ratio_cur = 3.f, ag_boost_cur = 12.f, ag_cut_cur = 12.f;
	float ag_k_attack = 1.f, ag_k_release = 1.f; // this update's smoother shares
	float ag_amount_cur[smc_count] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	float ag_min_db = 0.f, ag_max_db = 0.f; // range of the auto-gains applied in the last update
	float ag_min_acc = 0.f, ag_max_acc = 0.f; // the same, being collected in this update
	float bang_gr_db = 0.f; // current bang duck (<= 0)
	float bang_gain = 1.f; // 10^(bang_gr_db / 20)

	xrCriticalSection lock;
};
