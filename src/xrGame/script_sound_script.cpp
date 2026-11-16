////////////////////////////////////////////////////////////////////////////
//	Module 		: script_sound_script.cpp
//	Created 	: 06.02.2004
//  Modified 	: 06.02.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script sound class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_sound.h"
#include "script_game_object.h"

using namespace luabind;

//////////////////////////////////////////////////////////////////////////
// headphones: listener-side audio controls (docs/audio_api.md). Only what the player hears: the OpenAL gain of the
// sources. Sound power, max AI distance and the feel_sound path are never touched. Not saved, not networked.

// feature mask, like eft_weapon_api():
//   1 = audio_set_category_gain / audio_get_category_gain / audio_get_category_target
//   2 = audio_set_steps_others / audio_get_steps_range (others' footsteps gain and audible range)
//   4 = audio_set_limiter / audio_get_limiter_enabled / audio_get_limiter_reduction / audio_get_loud_level /
//       audio_set_limiter_detect
//   8 = gains above 1 really get louder (up to 4)
//  16 = sound_object:set_audio_category / get_audio_category
//  32 = audio_reset
static int audio_api() { return 1 | 2 | 4 | 8 | 16 | 32; }

static const char* const audio_category_names[smc_count] = {"world", "ambient", "weather", "steps_others", "ui", "music"};

static int audio_category_id(LPCSTR name)
{
	if (!name)
		return -1;
	for (int i = 0; i < smc_count; ++i)
		if (!xr_strcmp(name, audio_category_names[i]))
			return i;
	return -1;
}

static bool audio_set_category_gain(LPCSTR category, float gain, float fade_ms)
{
	const int c = audio_category_id(category);
	if (c < 0)
		return false;
	::Sound->mix_set_category_gain(u32(c), gain, fade_ms);
	return true;
}

static float audio_get_category_gain(LPCSTR category)
{
	const int c = audio_category_id(category);
	return c < 0 ? 1.f : ::Sound->mix_get_category_gain(u32(c), false);
}

static float audio_get_category_target(LPCSTR category)
{
	const int c = audio_category_id(category);
	return c < 0 ? 1.f : ::Sound->mix_get_category_gain(u32(c), true);
}

static void audio_set_steps_others(float gain, float range_k, float fade_ms)
{
	::Sound->mix_set_category_gain(smc_steps_others, gain, fade_ms);
	::Sound->mix_set_steps_range(range_k);
}

static float audio_get_steps_range() { return ::Sound->mix_get_steps_range(); }

static void audio_set_limiter(bool enabled, float threshold_db, float attack_ms, float release_ms, float ratio)
{
	::Sound->mix_set_limiter(enabled, threshold_db, attack_ms, release_ms, ratio);
}

static void audio_set_limiter_detect(double game_type_mask, float own_k)
{
	const u32 mask = (game_type_mask >= 0. && game_type_mask <= 4294967295.) ? u32(game_type_mask) : 0x00200100u;
	::Sound->mix_set_limiter_detect(mask, own_k);
}

static bool audio_get_limiter_enabled() { return ::Sound->mix_get_limiter_enabled(); }
static float audio_get_limiter_reduction() { return ::Sound->mix_get_limiter_reduction_db(); }
static float audio_get_loud_level() { return ::Sound->mix_get_loud_level_db(); }
static void audio_reset(float fade_ms) { ::Sound->mix_reset(fade_ms); }

bool CScriptSound::SetAudioCategory(LPCSTR name)
{
	if (name && !xr_strcmp(name, "auto"))
	{
		m_sound.set_mix_category(smc_auto);
		return true;
	}
	const int c = audio_category_id(name);
	if (c < 0)
		return false;
	m_sound.set_mix_category(u8(c));
	return true;
}

LPCSTR CScriptSound::GetAudioCategory() const
{
	const u8 c = m_sound.get_mix_category();
	return c < smc_count ? audio_category_names[c] : "auto";
}

#pragma optimize("s",on)
void CScriptSound::script_register(lua_State* L)
{
	module(L)
	[
		class_<CSound_params>("sound_params")
		.def_readwrite("position", &CSound_params::position)
		.def_readwrite("volume", &CSound_params::volume)
		.def_readwrite("frequency", &CSound_params::freq)
		.def_readwrite("min_distance", &CSound_params::min_distance)
		.def_readwrite("max_distance", &CSound_params::max_distance),

		class_<CScriptSound>("sound_object")
		.enum_("sound_play_type")
		[
			value("looped", sm_Looped),
			value("s2d", sm_2D),
			value("s3d", 0)
		]
		.property("frequency", &CScriptSound::GetFrequency, &CScriptSound::SetFrequency)
		.property("min_distance", &CScriptSound::GetMinDistance, &CScriptSound::SetMinDistance)
		.property("max_distance", &CScriptSound::GetMaxDistance, &CScriptSound::SetMaxDistance)
		.property("volume", &CScriptSound::GetVolume, &CScriptSound::SetVolume)
		.property("position", &CScriptSound::GetPosition, &CScriptSound::SetPosition)
		.def(constructor<LPCSTR>())
		.def(constructor<LPCSTR, ESoundTypes>())
		.def("get_position", &CScriptSound::GetPosition)
		.def("set_position", &CScriptSound::SetPosition)
		.def("play", (void (CScriptSound::*)(CScriptGameObject*))(&CScriptSound::Play))
		.def("play", (void (CScriptSound::*)(CScriptGameObject*, float))(&CScriptSound::Play))
		.def("play", (void (CScriptSound::*)(CScriptGameObject*, float, int))(&CScriptSound::Play))
		.def("play_at_pos", (void (CScriptSound::*)(CScriptGameObject*, const Fvector&))(&CScriptSound::PlayAtPos))
		.def("play_at_pos",
		     (void (CScriptSound::*)(CScriptGameObject*, const Fvector&, float))(&CScriptSound::PlayAtPos))
		.def("play_at_pos",
		     (void (CScriptSound::*)(CScriptGameObject*, const Fvector&, float, int))(&CScriptSound::PlayAtPos))
		.def("play_no_feedback", &CScriptSound::PlayNoFeedback)
		.def("stop", &CScriptSound::Stop)
		.def("stop_deffered", &CScriptSound::StopDeffered)
		.def("playing", &CScriptSound::IsPlaying)
		.def("length", &CScriptSound::Length)
		.def("attach_tail", &CScriptSound::AttachTail)
		.def("set_audio_category", &CScriptSound::SetAudioCategory)
		.def("get_audio_category", &CScriptSound::GetAudioCategory)
	];

	module(L)
	[
		def("audio_api", &audio_api),
		def("audio_set_category_gain", &audio_set_category_gain),
		def("audio_get_category_gain", &audio_get_category_gain),
		def("audio_get_category_target", &audio_get_category_target),
		def("audio_set_steps_others", &audio_set_steps_others),
		def("audio_get_steps_range", &audio_get_steps_range),
		def("audio_set_limiter", &audio_set_limiter),
		def("audio_set_limiter_detect", &audio_set_limiter_detect),
		def("audio_get_limiter_enabled", &audio_get_limiter_enabled),
		def("audio_get_limiter_reduction", &audio_get_limiter_reduction),
		def("audio_get_loud_level", &audio_get_loud_level),
		def("audio_reset", &audio_reset)
	];
}
