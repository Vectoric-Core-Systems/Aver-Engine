#pragma once
// Aver.Audio — the plain-C ABI. int32/float/const char* cross, UTF-8 strings, 0 is always an
// invalid handle. Units are the engine's: centimetres, +X forward, +Y right, +Z up, LEFT-handed.
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

// Buses, matching aver::audio::Bus.
#define AVER_AUDIO_BUS_SFX   0
#define AVER_AUDIO_BUS_MUSIC 1
#define AVER_AUDIO_BUS_VOICE 2
#define AVER_AUDIO_BUS_UI    3

// Opens the default output device and starts the mixer. Returns 0 when there is no output, which is
// a legitimate configuration: everything below then succeeds and does nothing.
AVER_AUDIO_API int32_t aver_audio_init(void);
// Stops the mixer, releases the device and forgets every loaded sound.
AVER_AUDIO_API void    aver_audio_shutdown(void);
// Whether the device is open and the render thread is running.
AVER_AUDIO_API int32_t aver_audio_ready(void);

// The device's sample rate. Zero before init has succeeded.
AVER_AUDIO_API int32_t aver_audio_sample_rate(void);
// The mixer's channel count. Zero before init has succeeded.
AVER_AUDIO_API int32_t aver_audio_channels(void);
// Buffers the device asked for and did not get in time. Every one is an audible gap.
AVER_AUDIO_API int32_t aver_audio_underruns(void);

// Reclaims sounds released with aver_audio_unload whose last voice has ended. Call once a frame.
AVER_AUDIO_API void aver_audio_collect(void);

// Loads a sound file and returns a sound handle, or 0. The path is UTF-8, absolute or relative to
// the working directory. The same path loaded twice yields the SAME handle and does not decode again.
AVER_AUDIO_API int32_t aver_audio_load(const char* utf8Path);
// Releases a sound. Its memory goes on the next aver_audio_collect.
/* Registers GENERATED samples as a sound, returning a handle, or 0. `samples` is interleaved f32,
 * `frames` counts FRAMES (not individual samples), and `channels` must be 1 or 2 -- the mixer
 * renders no others. The bytes are copied, so the caller may free its buffer immediately.
 *
 * THE SEAM FOR SYNTHESISED AUDIO. Every other route into the sound table starts at a file, so
 * nothing procedurally generated could be played at all; Aver.Sound renders a .ocsnd graph and
 * arrives here. NOT path-cached, unlike aver_audio_load: a generated buffer has no path, and two
 * renders of one graph with different seeds are different sounds. Every call therefore adds an
 * entry the caller owns and must aver_audio_unload. */
AVER_AUDIO_API int32_t aver_audio_load_pcm(const float* samples, int32_t frames, int32_t channels,
                                           int32_t sampleRate);
AVER_AUDIO_API void    aver_audio_unload(int32_t sound);

// Plays a sound the same in both ears. Returns a VOICE handle, or 0.
AVER_AUDIO_API int32_t aver_audio_play(int32_t sound, float volume, float pitch, int32_t looping, int32_t bus);

// Plays a sound panned and attenuated against the listener. Returns a VOICE handle, or 0.
// Radii in CENTIMETRES: full volume at or inside `innerCm`, exactly silent at or beyond `outerCm`.
AVER_AUDIO_API int32_t aver_audio_play_at(int32_t sound, float x, float y, float z,
                                          float volume, float pitch, int32_t looping, int32_t bus,
                                          float innerCm, float outerCm);

// Stops one voice.
AVER_AUDIO_API void    aver_audio_stop(int32_t voice);
// Stops every voice on every bus.
AVER_AUDIO_API void    aver_audio_stop_all(void);
// Whether a voice is still sounding. False for a finished voice and for a stale generational handle.
AVER_AUDIO_API int32_t aver_audio_playing(int32_t voice);

// Sets a live voice's volume.
AVER_AUDIO_API void aver_audio_set_voice_volume(int32_t voice, float volume);
// Sets a live voice's pitch.
AVER_AUDIO_API void aver_audio_set_voice_pitch(int32_t voice, float pitch);
// Moves a live positional voice.
AVER_AUDIO_API void aver_audio_set_voice_position(int32_t voice, float x, float y, float z);

// Where the ears are and which way they face. Pushed once a frame by whatever owns the camera.
AVER_AUDIO_API void aver_audio_set_listener(float px, float py, float pz,
                                            float fx, float fy, float fz,
                                            float rx, float ry, float rz);

// Sets one bus's volume.
AVER_AUDIO_API void  aver_audio_set_bus_volume(int32_t bus, float volume);
// One bus's volume.
AVER_AUDIO_API float aver_audio_bus_volume(int32_t bus);
// Sets the master volume, applied after every bus.
AVER_AUDIO_API void  aver_audio_set_master_volume(float volume);
// The master volume.
AVER_AUDIO_API float aver_audio_master_volume(void);

// Voices currently sounding.
AVER_AUDIO_API int32_t aver_audio_active_voices(void);
// Voices cut short because the pool was full. A tuning fact, not an error.
AVER_AUDIO_API int32_t aver_audio_stolen_voices(void);

#ifdef __cplusplus
} // extern "C"
#endif
