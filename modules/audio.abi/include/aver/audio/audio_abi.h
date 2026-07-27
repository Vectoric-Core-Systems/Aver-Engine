#pragma once
// Aver.Audio — the plain-C ABI, following the engine's idiom: int32/float/const char* cross, UTF-8
// strings, setters return 1/0, and 0 is ALWAYS an invalid handle.
//
// The EIGHTH seam (see docs/ABI.md). It exists so a game triggers its own sounds: what a footstep
// sounds like, when a weapon is loud, which music plays where, are all content, and content belongs
// on the game's side of the line.
//
// UNITS are the engine's: centimetres, +X forward, +Y right, +Z up, LEFT-handed. Named here because
// a handedness error puts every sound on the wrong side of the player's head, which reads as a bug
// in the panning rather than as a sign in the caller.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_AUDIO_ABI_BUILD)
#    define AVER_AUDIO_API __declspec(dllexport)
#  else
#    define AVER_AUDIO_API __declspec(dllimport)
#  endif
#else
#  define AVER_AUDIO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Buses, matching aver::audio::Bus. Named constants rather than handles: there are four, they are
// the same four in every game, and a mixer with an authored bus graph is a mixer whose graph
// something has to author.
#define AVER_AUDIO_BUS_SFX   0
#define AVER_AUDIO_BUS_MUSIC 1
#define AVER_AUDIO_BUS_VOICE 2
#define AVER_AUDIO_BUS_UI    3

// ---- lifetime (the HOST calls these, not a game) --------------------------------------------------

// Opens the default output device and starts the mixer. Returns 0 when there is no output, which is
// a legitimate configuration -- a build server is one -- and NOT an error: everything below then
// succeeds and does nothing, so a game needs no special case for a silent machine.
AVER_AUDIO_API int32_t aver_audio_init(void);
AVER_AUDIO_API void    aver_audio_shutdown(void);
AVER_AUDIO_API int32_t aver_audio_ready(void);

// The device's own format, once init has succeeded. Zero before that.
AVER_AUDIO_API int32_t aver_audio_sample_rate(void);
AVER_AUDIO_API int32_t aver_audio_channels(void);
// Buffers the device asked for and did not get in time. THE number to watch: every one is an audible
// gap. Nonzero under load is a fact; nonzero on an idle machine is a bug.
AVER_AUDIO_API int32_t aver_audio_underruns(void);

// Reclaims sounds released with aver_audio_unload whose last voice has ended. Call once a frame from
// the game thread; never calling it is a leak, not a crash.
AVER_AUDIO_API void aver_audio_collect(void);

// ---- sounds ---------------------------------------------------------------------------------------

// Loads a .wav and returns a sound handle, or 0. The path is UTF-8 and absolute, or relative to the
// working directory; resolving a project-relative path is the caller's business, because this module
// knows nothing about projects.
//
// The same path loaded twice yields the SAME handle and does not decode again -- a game that plays a
// footstep from four call sites should not hold four copies of it.
AVER_AUDIO_API int32_t aver_audio_load(const char* utf8Path);
AVER_AUDIO_API void    aver_audio_unload(int32_t sound);

// ---- playback -------------------------------------------------------------------------------------

// 2D: the same in both ears, which is what music and UI want. Returns a VOICE handle, or 0.
AVER_AUDIO_API int32_t aver_audio_play(int32_t sound, float volume, float pitch, int32_t looping, int32_t bus);

// 3D: panned and attenuated against the listener. Radii in CENTIMETRES -- full volume at or inside
// `innerCm`, exactly silent at or beyond `outerCm`.
AVER_AUDIO_API int32_t aver_audio_play_at(int32_t sound, float x, float y, float z,
                                          float volume, float pitch, int32_t looping, int32_t bus,
                                          float innerCm, float outerCm);

AVER_AUDIO_API void    aver_audio_stop(int32_t voice);
AVER_AUDIO_API void    aver_audio_stop_all(void);
// False for a finished voice AND for a handle whose slot has been reused. A voice handle is
// generational precisely so a game cannot drive somebody else's sound by holding a stale one.
AVER_AUDIO_API int32_t aver_audio_playing(int32_t voice);

AVER_AUDIO_API void aver_audio_set_voice_volume(int32_t voice, float volume);
AVER_AUDIO_API void aver_audio_set_voice_pitch(int32_t voice, float pitch);
AVER_AUDIO_API void aver_audio_set_voice_position(int32_t voice, float x, float y, float z);

// ---- listener and buses -----------------------------------------------------------------------------

// Where the ears are and which way they face. Pushed once a frame by whatever owns the camera, the
// same way the renderer's camera is.
AVER_AUDIO_API void aver_audio_set_listener(float px, float py, float pz,
                                            float fx, float fy, float fz,
                                            float rx, float ry, float rz);

AVER_AUDIO_API void  aver_audio_set_bus_volume(int32_t bus, float volume);
AVER_AUDIO_API float aver_audio_bus_volume(int32_t bus);
AVER_AUDIO_API void  aver_audio_set_master_volume(float volume);
AVER_AUDIO_API float aver_audio_master_volume(void);

// ---- diagnostics ------------------------------------------------------------------------------------
AVER_AUDIO_API int32_t aver_audio_active_voices(void);
// Voices cut short because the pool was full. A tuning fact, not an error.
AVER_AUDIO_API int32_t aver_audio_stolen_voices(void);

#ifdef __cplusplus
} // extern "C"
#endif
