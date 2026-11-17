#ifndef SoundH
#define SoundH
#pragma once


#ifdef XRSOUND_EXPORTS
#define XRSOUND_API
//__declspec(dllexport)
#else
	#define XRSOUND_API
//__declspec(dllimport)
#endif

#ifdef __BORLANDC__
	#define XRSOUND_EDITOR_API XRSOUND_API

	// editor only refs
	class XRSOUND_EDITOR_API SoundEnvironment_LIB;
#else
#define XRSOUND_EDITOR_API
#endif

#define SNDENV_FILENAME				"sEnvironment.xr"
#define OGG_COMMENT_VERSION 		0x0003

// refs
class CObject;
class XRSOUND_API CSound_params;
class XRSOUND_API CSound_source;
class XRSOUND_API CSound_emitter;
class XRSOUND_API CSound_stream_interface;
class XRSOUND_API CSound_environment;

XRSOUND_API extern u32 psSoundModel;
XRSOUND_API extern float psSoundVEffects;
XRSOUND_API extern float psSoundVFactor;
XRSOUND_API extern float psSoundVMusic;
XRSOUND_API extern float psSoundVMusicFactor;
XRSOUND_API extern float psSoundRolloff;
XRSOUND_API extern float psSoundMaxDistanceMultiplier;
XRSOUND_API extern float psSoundMaxAIDistanceMultiplier;
XRSOUND_API extern float psSoundOcclusionScale;
XRSOUND_API extern Flags32 psSoundFlags;
XRSOUND_API extern int psSoundTargets;
XRSOUND_API extern float snd_efx_environment_change_time;
XRSOUND_API extern float psSpeedOfSound;
XRSOUND_API extern int psSoundCacheSizeMB;
XRSOUND_API extern xr_token* snd_devices_token;
XRSOUND_API extern xr_string snd_device_name;

// reverb overwrite
extern BOOL reverb_overwrite;

extern float psReverbDensity;
extern float psReverbDiffusion;
extern float psReverbGain;
extern float psReverbGainHF;
extern float psReverbGainLF;
extern float psReverbDecayTime;
extern float psReverbDecayHFRatio;
extern float psReverbDecayLFRatio;
extern float psReverbReflectionsGain;
extern float psReverbReflectionsDelay;
extern float psReverbReflectionsPan;
extern float psReverbLateReverbGain;
extern float psReverbLateReverbDelay;
extern float psReverbLateReflectionsPan;
extern float psReverbEchoTime;
extern float psReverbEchoDepth;
extern float psReverbModulationTime;
extern float psReverbModulationDepth;
extern float psReverbAirAbsorptionGainHF;
extern float psReverbHFReference;
extern float psReverbLFReference;
extern float psReverbRoomRolloffFactor;
extern BOOL   psReverbDecayHFLimit;

// Flags
enum
{
	ss_Hardware = (1ul << 1ul),
	//!< Use hardware mixing only
	ss_EFX = (1ul << 2ul),
	//!< Use eax
	ss_forcedword = u32(-1)
};

enum
{
	sq_DEFAULT,
	sq_NOVIRT,
	sq_LIGHT,
	sq_HIGH,
	sq_forcedword = u32(-1)
};

enum
{
	sg_Undefined = 0,
	sg_SourceType = u32(-1),
	sg_forcedword = u32(-1),
};

enum
{
	sm_Looped = (1ul << 0ul),
	//!< Looped
	sm_2D = (1ul << 1ul),
	//!< 2D mode
	sm_Intro = (1ul << 2ul), //!< Only for music and video
	sm_forcedword = u32(-1),
};

enum esound_type
{
	st_Effect = 0,
	st_Music = 1,
	st_forcedword = u32(-1),
};

// Listener-side mix categories (headphones). They change only the gain the player hears (the OpenAL source gain);
// AI hearing (p_source.volume, max_ai_distance, the sound events) never reads them. Not saved, not networked.
enum esound_mix_category
{
	smc_world = 0, // everything not listed below: weapons, voices, explosions, physics, the actor's own sounds
	smc_ambient, // level ambient beds and effect sounds, static level sounds, random level sounds
	smc_weather, // rain, thunder, environment ambients whose file is wind/rain/thunder/storm
	smc_steps_others, // footsteps of NPCs and monsters (not the actor's own)
	smc_ui, // 2D sounds without a game object (menus, UI clicks, PDA)
	smc_music, // st_Music sounds (level music)
	smc_own, // the player's own sounds (weapon handling, steps, PDA, voice); only filled while the own split is on
	smc_count,
	smc_auto = 0xFF, // no tag: the category follows from the sound (music, ui, own or world)
};

// Listener-side flags of one play (headphones), set by the caller around play_no_feedback like the category.
enum esound_mix_flag
{
	// the sound exists only because a headset lets the player hear further (others' steps beyond the step gate): it
	// makes no AI sound event, so the AI hears exactly what it heard without the headset
	smf_listener_only = 1,
	// the sound is played for the AI only (others' steps a headset cut short of the step gate): OpenAL gain 0
	smf_mute = 2,
};

class CSound_UserDataVisitor;

class CSound_UserData : public xr_resource
{
public:
	virtual ~CSound_UserData()
	{
	}

	virtual void accept(CSound_UserDataVisitor*) =0;
	virtual void invalidate() =0;
};

typedef resptr_core<CSound_UserData, resptr_base<CSound_UserData>> CSound_UserDataPtr;

class ref_sound_data : public xr_resource
{
public:
	//	shared_str						nm;
	CSound_source* handle; //!< Pointer to wave-source interface
	CSound_emitter* feedback; //!< Pointer to emitter, automaticaly clears on emitter-stop
	esound_type s_type;
	int g_type; //!< Sound type, usually for AI
	CObject* g_object; //!< Game object that emitts ref_sound
	CSound_UserDataPtr g_userdata;
	shared_str fn_attached [2];
	u8 mix_cat; //!< esound_mix_category (listener-side mix only), smc_auto = derived from the sound
	u8 mix_flags; //!< esound_mix_flag bits of this play (0 for everything but headset-ranged footsteps)

	u32 dwBytesTotal;
	float fTimeTotal;
public:
	ref_sound_data();
	ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type);
	virtual ~ref_sound_data();
	float get_length_sec() const { return fTimeTotal; };
};

typedef resptr_core<ref_sound_data, resptr_base<ref_sound_data>> ref_sound_data_ptr;

/*! \class ref_sound
\brief Sound source + control

The main class respresenting source/emitter interface
This class infact just hides internals and redirect calls to 
specific sub-systems
*/
struct ref_sound
{
	ref_sound_data_ptr _p;
public:
	ref_sound()
	{
	}

	~ref_sound()
	{
	}

	IC CSound_source* _handle() const { return _p ? _p->handle : NULL; }
	IC CSound_emitter* _feedback() { return _p ? _p->feedback : 0; }
	IC CObject* _g_object()
	{
		VERIFY(_p);
		return _p->g_object;
	}

	IC int _g_type()
	{
		VERIFY(_p);
		return _p->g_type;
	}

	IC esound_type _sound_type()
	{
		VERIFY(_p);
		return _p->s_type;
	}

	//! Listener-side mix category (esound_mix_category) of the sounds this ref plays from now on; only what the
	//! player hears. play_no_feedback copies it. No-op on an empty ref.
	IC void set_mix_category(u8 cat) { if (_p) _p->mix_cat = cat; }
	IC u8 get_mix_category() const { return _p ? _p->mix_cat : u8(smc_auto); }
	//! esound_mix_flag bits of what this ref plays from now on; play_no_feedback copies them. No-op on an empty ref.
	IC void set_mix_flags(u8 flags) { if (_p) _p->mix_flags = flags; }
	IC u8 get_mix_flags() const { return _p ? _p->mix_flags : u8(0); }

	IC CSound_UserDataPtr _g_userdata()
	{
		VERIFY(_p);
		return _p->g_userdata;
	}

	IC void create(LPCSTR name, esound_type sound_type, int game_type);
	IC void attach_tail(LPCSTR name);

	IC void clone(const ref_sound& from, esound_type sound_type, int game_type);

	IC void destroy();

	IC void play(CObject* O, u32 flags = 0, float delay = 0.f);
	IC void play_at_pos(CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f);
	IC void play_no_feedback(CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0, float* vol = 0,
	                         float* freq = 0, Fvector2* range = 0);

	IC void stop();
	IC void stop_deffered();
	IC void set_position(const Fvector& pos);
	IC void set_frequency(float freq);
	IC void set_range(float min, float max);
	IC void set_volume(float vol);
	IC void set_priority(float vol);

	IC const CSound_params* get_params();
	IC void set_params(CSound_params* p);
	IC float get_length_sec() const { return _p ? _p->get_length_sec() : 0.0f; };
};

/// definition (Sound Source)
class XRSOUND_API CSound_source
{
public:
	virtual float length_sec() const = 0;
	virtual u32 game_type() const = 0;
	virtual LPCSTR file_name() const = 0;
	virtual u16 channels_num() const = 0;
	virtual u32 bytes_total() const = 0;
};

/// definition (Sound Source)
class XRSOUND_API CSound_environment
{
public:
};

namespace soundSmoothingParams {
	extern float pitchVariationPower;
	extern float distanceBasedDelayPower;
	extern float distanceBasedDelayMinDistance;
	extern float power;
	extern int steps;
	extern float alpha;
	extern float getAlpha();
	extern float getTimeDeltaSmoothing();
	extern float getSmoothedValue(float, float, float);
};

/// definition (Sound Params)
class XRSOUND_API CSound_params
{
public:
	CSound_params() :
		set(false)
		{
			position.set(0.0f, 0.0f, 0.0f);
			velocity.set(0.0f, 0.0f, 0.0f);
			accVelocity.set(0.f, 0.f, 0.f);

			// demonized: add pitch variation
			pitch_variation = 0.02 * Random.randF(-1.f, 1.f) * soundSmoothingParams::pitchVariationPower;
		}

private:
	bool set;

public:
	Fvector position;
	Fvector velocity;  // Cribbledirge.  Added for doppler effect.
	Fvector curVelocity;  // Current velocity.
	Fvector prevVelocity;  // Previous velocity.
	Fvector accVelocity;  // Velocity accumulator (for moving average).
	float base_volume;
	float volume;
	float freq;
	float min_distance;
	float max_distance;
	float max_ai_distance;

	float pitch_variation;

	// Functions added by Cribbledirge for doppler effect.
	IC virtual void update_position(const Fvector& newPosition)
	{
		// If the position has been set already, start getting a moving average of the velocity.
		if (set)
		{
			prevVelocity.set(accVelocity);
			curVelocity.sub(newPosition, position);

			//accVelocity.set(curVelocity.mul(alpha).add(prevVelocity.mul(1.f - alpha)));
		}
		else
		{
			set = true;
		}
		position.set(newPosition);
	}

	IC virtual void update_velocity(const float dt)
	{
		float a = soundSmoothingParams::getTimeDeltaSmoothing();
		int p = soundSmoothingParams::power;
		accVelocity.x = soundSmoothingParams::getSmoothedValue(curVelocity.x * p / dt, accVelocity.x, a);
		accVelocity.y = soundSmoothingParams::getSmoothedValue(curVelocity.y * p / dt, accVelocity.y, a);
		accVelocity.z = soundSmoothingParams::getSmoothedValue(curVelocity.z * p / dt, accVelocity.z, a);
		velocity.set(accVelocity);

		//Msg("VELOC: %f", velocity.magnitude());
	}
};

/// definition (Sound Interface)
class XRSOUND_API CSound_emitter
{
public:
	virtual BOOL is_2D() = 0;
	virtual void switch_to_2D() = 0;
	virtual void switch_to_Intro() = 0;
	virtual void switch_to_3D() = 0;
	virtual void set_position(const Fvector& pos) = 0;
	virtual void set_frequency(float freq) = 0;
	virtual void set_range(float min, float max) = 0;
	virtual void set_volume(float vol) = 0;
	virtual void set_priority(float vol) = 0;
	virtual void stop(BOOL bDeffered) = 0;
	virtual const CSound_params* get_params() = 0;
	virtual u32 play_time() = 0;
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stream_interface
{
public:
};

/// definition (Sound Stream Interface)
class XRSOUND_API CSound_stats
{
public:
	u32 _rendered;
	u32 _simulated;
	u32 _cache_hits;
	u32 _cache_misses;
	u32 _events;
};

class XRSOUND_API CSound_stats_ext
{
public:
	struct SItem
	{
		shared_str name;
		CSound_params params;
		float volume;
		esound_type type;
		int game_type;
		CObject* game_object;

		struct
		{
			u32 _3D :1;
			u32 _rendered :1;
		};
	};

	DEFINE_VECTOR(SItem, item_vec, item_vec_it);
	item_vec items;
public:
	void clear() { items.clear(); }
	void append(const SItem& itm) { items.push_back(itm); }
};

/// definition (Sound Callback)
typedef void __stdcall sound_event(ref_sound_data_ptr S, float range);

/// definition (Sound Manager Interface)
class XRSOUND_API CSound_manager_interface
{
	virtual void _initialize(int stage) = 0;
	virtual void _clear() = 0;

protected:
	friend class ref_sound_data;
	virtual void _create_data(ref_sound_data& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
	virtual void _destroy_data(ref_sound_data& S) = 0;
public:
	virtual ~CSound_manager_interface()
	{
	}

	static void _create(int stage);
	static void _destroy();

	virtual void _restart() = 0;
	virtual BOOL i_locked() = 0;
	virtual BOOL is_ready() = 0;
	virtual void refresh_devices() = 0;
	virtual void default_device_changed() = 0;
	virtual void switch_device(LPCSTR device_name) = 0;

	virtual void create(ref_sound& S, LPCSTR fName, esound_type sound_type, int game_type) = 0;
	virtual void attach_tail(ref_sound& S, LPCSTR fName) = 0;
	virtual void clone(ref_sound& S, const ref_sound& from, esound_type sound_type, int game_type) = 0;
	virtual void destroy(ref_sound& S) = 0;
	virtual void stop_emitters() = 0;
	virtual int pause_emitters(bool val) = 0;

	virtual void play(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f) = 0;
	virtual void play_at_pos(ref_sound& S, CObject* O, const Fvector& pos, u32 flags = 0, float delay = 0.f) = 0;
	virtual void play_no_feedback(ref_sound& S, CObject* O, u32 flags = 0, float delay = 0.f, Fvector* pos = 0,
	                              float* vol = 0, float* freq = 0, Fvector2* range = 0) = 0;

	virtual void set_master_volume(float f = 1.f) = 0;
	virtual void set_geometry_env(IReader* I) = 0;
	virtual void set_geometry_som(IReader* I) = 0;
	virtual void set_geometry_occ(CDB::MODEL* M) = 0;
	virtual void set_handler(sound_event* E) = 0;

	virtual void update(const Fvector& P, const Fvector& D, const Fvector& N) = 0;
	virtual void statistic(CSound_stats* s0, CSound_stats_ext* s1) = 0;

	virtual float get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion = 0.2f) = 0;
	virtual float get_occlusion(Fvector& P, float R, Fvector* occ) = 0;

	virtual void object_relcase(CObject* obj) = 0;
	virtual const Fvector& listener_position() = 0;

	// Listener-side mix (headphones). Only the gain the player hears; AI hearing is untouched. Not saved.
	// cat: esound_mix_category. gain 0 .. 4 (above 1 = boost), fade_ms 0 .. 60000 (linear ramp from the current gain).
	virtual void mix_set_category_gain(u32 cat, float gain, float fade_ms) = 0;
	virtual float mix_get_category_gain(u32 cat, bool target) = 0; // target: the value it is fading to
	// k 0.1 .. 4: max audible distance of others' footsteps (and xrGame's 50 m step gate) x k
	virtual void mix_set_steps_range(float k) = 0;
	virtual float mix_get_steps_range() = 0;
	// loud-sound limiter: threshold dB (-60 .. 0) on the detector level, attack / release ms, ratio 1 .. 100 (>= 20 is
	// a limiter); gain reduction applies to every category but ui and music
	virtual void mix_set_limiter(bool enabled, float threshold_db, float attack_ms, float release_ms, float ratio) = 0;
	virtual void mix_set_limiter_detect(u32 game_type_mask, float own_k) = 0; // which g_type bits count as loud
	virtual bool mix_get_limiter_enabled() = 0;
	virtual float mix_get_limiter_reduction_db() = 0; // current gain reduction, dB (<= 0)
	virtual float mix_get_loud_level_db() = 0; // current detector level, dB (-120 = nothing loud)
	virtual void mix_reset(float fade_ms) = 0; // all gains to 1, limiter and compressor off, steps range and rolloff 1
	// compressor on the categories of mix_set_compressor_category (default world + steps_others): detector = the
	// loudest counted sound at the listener, soft-knee gain computer, attack / release, makeup gain. threshold dB
	// (-60 .. 0), ratio 1 .. 20, attack / release ms, makeup dB (-24 .. 24), knee dB (0 .. 24)
	virtual void mix_set_compressor(bool enabled, float threshold_db, float ratio, float attack_ms, float release_ms,
	                                float makeup_db, float knee_db) = 0;
	// which g_type bits drive it: game_type_mask for 3D sounds, own_type_mask (x own_k) for 2D, the actor's own
	virtual void mix_set_compressor_detect(u32 game_type_mask, u32 own_type_mask, float own_k) = 0;
	virtual bool mix_set_compressor_category(u32 cat, bool enabled) = 0; // false: ui / music / unknown (never)
	virtual bool mix_get_compressor_category(u32 cat) = 0;
	virtual bool mix_get_compressor_enabled() = 0;
	virtual float mix_get_compressor_gain_db() = 0; // current gain (makeup + reduction), dB
	virtual float mix_get_compressor_level_db() = 0; // current detector level, dB (-120 = nothing counted)
	// distance curve of a category: k 0 .. 2 on OpenAL's rolloff factor (1 = unchanged, 0 = only the linear fade)
	virtual void mix_set_category_rolloff(u32 cat, float k) = 0;
	virtual float mix_get_category_rolloff(u32 cat) = 0;
	// per-source auto-gain: every source of a category with an amount above 0 gets its own gain from its own level at
	// the listener: (target - level) x (1 - 1/ratio) dB, at most +max_boost and -max_cut, x the category's amount,
	// smoothed per source (attack when the gain falls, release when it rises). smc_own is never lifted.
	virtual void mix_set_autogain(bool enabled, float target_db, float ratio, float max_boost_db, float max_cut_db,
	                              float attack_ms, float release_ms) = 0;
	virtual bool mix_set_autogain_category(u32 cat, float amount) = 0; // 0 .. 1; false: ui / music / unknown
	virtual float mix_get_autogain_category(u32 cat) = 0;
	virtual bool mix_get_autogain_enabled() = 0;
	virtual float mix_get_autogain_min_db() = 0; // lowest / highest auto-gain of the sources rendered last update
	virtual float mix_get_autogain_max_db() = 0;
	// bang guard: a short duck of every category but ui and music while a loud bang starts (the first hold_ms of a
	// sound whose g_type has a bit of the mask), instant attack, release in tens of ms
	virtual void mix_set_bang_guard(bool enabled, float threshold_db, float max_duck_db, float hold_ms,
	                                float release_ms) = 0;
	virtual void mix_set_bang_guard_detect(u32 game_type_mask) = 0;
	virtual bool mix_get_bang_guard_enabled() = 0;
	virtual float mix_get_bang_guard_reduction_db() = 0; // current duck, dB (<= 0)
	// own split: untagged sounds of the listener's object (and 2D sounds with a game object) are smc_own, not smc_world
	virtual void mix_set_own_split(bool enabled) = 0;
	virtual bool mix_get_own_split() = 0;
	// the object the listener is (the actor); compared only, never dereferenced. Set by the engine every frame.
	virtual void mix_set_listener_object(CObject* O) = 0;
#ifdef __BORLANDC__
	virtual SoundEnvironment_LIB*	get_env_library			()																						= 0;
	virtual void					refresh_env_library		()																						= 0;
	virtual void					set_user_env			(CSound_environment* E)																	= 0;
	virtual void					refresh_sources			()																						= 0;
    virtual void					set_environment			(u32 id, CSound_environment** dst_env)													= 0;
    virtual void					set_environment_size	(CSound_environment* src_env, CSound_environment** dst_env)								= 0;
#endif
};

extern XRSOUND_API CSound_manager_interface* Sound;

/// ********* Sound ********* (utils, accessors, helpers)
IC ref_sound_data::ref_sound_data()
{
	handle = 0;
	feedback = 0;
	g_type = 0;
	g_object = 0;
	s_type = st_Effect;
	mix_cat = smc_auto;
	mix_flags = 0;
}

IC ref_sound_data::ref_sound_data(LPCSTR fName, esound_type sound_type, int game_type)
{
	::Sound->_create_data(*this, fName, sound_type, game_type);
}

IC ref_sound_data::~ref_sound_data() { ::Sound->_destroy_data(*this); }

IC void ref_sound::create(LPCSTR name, esound_type sound_type, int game_type)
{
	VERIFY(!::Sound->i_locked());
	::Sound->create(*this, name, sound_type, game_type);
}

IC void ref_sound::attach_tail(LPCSTR name)
{
	VERIFY(!::Sound->i_locked());
	::Sound->attach_tail(*this, name);
}

IC void ref_sound::clone(const ref_sound& from, esound_type sound_type, int game_type)
{
	VERIFY(!::Sound->i_locked());
	::Sound->clone(*this, from, sound_type, game_type);
}

IC void ref_sound::destroy()
{
	VERIFY(!::Sound->i_locked());
	::Sound->destroy(*this);
}

IC void ref_sound::play(CObject* O, u32 flags, float d)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play(*this, O, flags, d);
}

IC void ref_sound::play_at_pos(CObject* O, const Fvector& pos, u32 flags, float d)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play_at_pos(*this, O, pos, flags, d);
}

IC void ref_sound::play_no_feedback(CObject* O, u32 flags, float d, Fvector* pos, float* vol, float* freq,
                                    Fvector2* range)
{
	VERIFY(!::Sound->i_locked());
	::Sound->play_no_feedback(*this, O, flags, d, pos, vol, freq, range);
}

IC void ref_sound::set_position(const Fvector& pos)
{
	VERIFY(!::Sound->i_locked());
	VERIFY(_feedback());
	_feedback()->set_position(pos);
}

IC void ref_sound::set_frequency(float freq)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_frequency(freq);
}

IC void ref_sound::set_range(float min, float max)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_range(min, max);
}

IC void ref_sound::set_volume(float vol)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_volume(vol);
}

IC void ref_sound::set_priority(float p)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->set_priority(p);
}

IC void ref_sound::stop()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->stop(FALSE);
}

IC void ref_sound::stop_deffered()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) _feedback()->stop(TRUE);
}

IC const CSound_params* ref_sound::get_params()
{
	VERIFY(!::Sound->i_locked());
	if (_feedback()) return _feedback()->get_params();
	else return NULL;
}

IC void ref_sound::set_params(CSound_params* p)
{
	VERIFY(!::Sound->i_locked());
	if (_feedback())
	{
		_feedback()->set_position(p->position);
		_feedback()->set_frequency(p->freq);
		_feedback()->set_range(p->min_distance, p->max_distance);
		_feedback()->set_volume(p->volume);
	}
}
#endif
