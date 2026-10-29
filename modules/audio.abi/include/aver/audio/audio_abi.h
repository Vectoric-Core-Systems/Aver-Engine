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

// Fade curves, matching aver::audio::FadeCurve.
#define AVER_AUDIO_FADE_LINEAR      0
#define AVER_AUDIO_FADE_EQUAL_POWER 1

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
// Floating-origin shift (LARGE_WORLD 2.2.5): moves every live voice and the listener by -(dx, dy, dz) cm so sounds keep their distance and pan.
// Game thread, once per committed shift.
AVER_AUDIO_API void aver_audio_origin_shift(float dx, float dy, float dz);

// Where the ears are and which way they face. Pushed once a frame by whatever owns the camera.
AVER_AUDIO_API void aver_audio_set_listener(float px, float py, float pz,
                                            float fx, float fy, float fz,
                                            float rx, float ry, float rz);

// Reads back the listener last set: position, forward and right (three floats each; any may be null).
AVER_AUDIO_API void aver_audio_get_listener(float* outPosition, float* outForward, float* outRight);

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

/* ---- streaming, fades, music, occlusion, reverb ---------------------------------------------
 * Streamed voices are ordinary voices: aver_audio_stop, _playing, _set_voice_* all work on them.
 * A stream owns a decoder thread; the decoder is freed by aver_audio_collect once the voice ends,
 * so keep calling that once a frame. Mono and stereo sources only. */

// Streams a file from disk (WAV and Media Foundation formats decode incrementally). The voice is
// silent until a quarter second is buffered. Flat in both ears. Returns a VOICE handle, or 0.
AVER_AUDIO_API int32_t aver_audio_stream_play(const char* utf8Path, float volume, float pitch,
                                              int32_t looping, int32_t bus, float fadeInSeconds);
// The positioned counterpart; radii in CENTIMETRES as for aver_audio_play_at.
AVER_AUDIO_API int32_t aver_audio_stream_play_at(const char* utf8Path, float x, float y, float z,
                                                 float volume, float pitch, int32_t looping, int32_t bus,
                                                 float innerCm, float outerCm, float fadeInSeconds);

// Ramps a voice's fade gain (a multiplier on top of its volume) to `targetGain` over `seconds`.
// With stopWhenDone nonzero the voice ends when the ramp does.
AVER_AUDIO_API void aver_audio_fade_voice(int32_t voice, float targetGain, float seconds,
                                          int32_t curve, int32_t stopWhenDone);

// The music slot: starting a track crossfades from the current one over `fadeSeconds`
// (equal-power by default). Returns the new track's VOICE handle, or 0, in which case the current
// track keeps playing. Plays on the Music bus.
AVER_AUDIO_API int32_t aver_audio_music_play(const char* utf8Path, float volume, float fadeSeconds,
                                             int32_t looping, int32_t curve);
// Fades the current music track out and ends it.
AVER_AUDIO_API void    aver_audio_music_stop(float fadeSeconds);
// The current music voice, or 0.
AVER_AUDIO_API int32_t aver_audio_music_voice(void);

// 0 clear .. 1 fully blocked. The mixer low-passes and ducks the voice and smooths the change.
AVER_AUDIO_API void aver_audio_set_voice_occlusion(int32_t voice, float occlusion);
// What fraction of a voice feeds the reverb return, 0..1. Sfx and Voice buses default to 1.
AVER_AUDIO_API void aver_audio_set_voice_reverb_send(int32_t voice, float send);
// The low-pass cutoff (Hz) and linear volume a fully occluded voice reaches.
AVER_AUDIO_API void aver_audio_set_occlusion_curve(float minCutoffHz, float minVolume);
// The listener-side reverb. wet 0 turns it off; decay is the RT60 in seconds; damping 0..1.
AVER_AUDIO_API void aver_audio_set_reverb(float wet, float decaySeconds, float damping);

/* ---- play observer (accessibility captions, docs/ACCESSIBILITY.md) ----------------------------------------
 * Called on the calling (game) thread right after every SUCCESSFUL start (voice != 0) from aver_audio_play,
 * aver_audio_play_at, aver_audio_stream_play, aver_audio_stream_play_at and aver_audio_music_play. utf8Path is
 * the file the sound was loaded or streamed from, NULL for generated PCM (aver_audio_load_pcm has no path).
 * positional is 1 for the _at calls (x, y, z in centimetres). With no output device no voice starts, so the
 * observer is never called. NULL clears. The observer must not call back into this ABI. */
typedef void (*AverAudioPlayObserver)(void* user, const char* utf8Path, int32_t voice, int32_t bus,
                                      int32_t positional, float x, float y, float z, int32_t looping);
AVER_AUDIO_API void aver_audio_set_play_observer(AverAudioPlayObserver fn, void* user);

// Blocks in which a stream ran out of decoded audio and went silent. Each is audible.
AVER_AUDIO_API int32_t aver_audio_stream_underruns(void);
// Streams currently held by a playing voice.
AVER_AUDIO_API int32_t aver_audio_active_streams(void);

/* ---- bus graph, effects, Godot-style players (G5 S1; docs/AUDIO.md "Bus graph") ---------------------------------
 * Opt-in: nothing below changes the four legacy buses. The graph is Godot's AudioServer model (named buses, bus 0 is "Master", sends,
 * mute / solo / bypass, effect chains, peaks); voices started with aver_audio_play_ex / aver_audio_stream_play_ex / aver_audio_live_play
 * and a graphBus >= 0 sum into it. All calls are game-thread calls. Strings are UTF-8; a name written to a caller's buffer is truncated
 * to `cap` bytes and always terminated, and the call returns the full length.
 *
 * OFFLINE MODE: aver_audio_offline_init makes the same mixer with NO device, for tests and tools; aver_audio_offline_mix renders a block
 * into the caller's buffer. It is used instead of aver_audio_init, never with it. */
AVER_AUDIO_API int32_t aver_audio_offline_init(int32_t sampleRate, int32_t channels);
AVER_AUDIO_API int32_t aver_audio_offline_mix(float* outInterleaved, int32_t frames);
AVER_AUDIO_API int32_t aver_audio_is_offline(void);

AVER_AUDIO_API void    aver_audio_graph_set_active(int32_t active);
AVER_AUDIO_API int32_t aver_audio_graph_active(void);

AVER_AUDIO_API int32_t aver_audio_bus_count(void);
AVER_AUDIO_API int32_t aver_audio_bus_add(int32_t at);
AVER_AUDIO_API int32_t aver_audio_bus_remove(int32_t index);
AVER_AUDIO_API int32_t aver_audio_bus_move(int32_t from, int32_t to);
AVER_AUDIO_API void    aver_audio_bus_set_count(int32_t count);
AVER_AUDIO_API int32_t aver_audio_bus_find(const char* utf8Name);
AVER_AUDIO_API int32_t aver_audio_bus_name(int32_t index, uint8_t* textOut, int32_t cap);
AVER_AUDIO_API void    aver_audio_bus_set_name(int32_t index, const char* utf8Name);
AVER_AUDIO_API void    aver_audio_bus_set_send(int32_t index, const char* utf8Name);
AVER_AUDIO_API int32_t aver_audio_bus_send_name(int32_t index, uint8_t* textOut, int32_t cap);
AVER_AUDIO_API void    aver_audio_bus_set_volume_db(int32_t index, float db);
AVER_AUDIO_API float   aver_audio_bus_get_volume_db(int32_t index);
AVER_AUDIO_API void    aver_audio_bus_set_flag(int32_t index, int32_t flag, int32_t value);
AVER_AUDIO_API int32_t aver_audio_bus_get_flag(int32_t index, int32_t flag);
#define AVER_AUDIO_BUSFLAG_MUTE   0
#define AVER_AUDIO_BUSFLAG_SOLO   1
#define AVER_AUDIO_BUSFLAG_BYPASS 2
AVER_AUDIO_API float   aver_audio_bus_peak_db(int32_t index, int32_t channel);
// Replaces the layout with a .ocbus file / its text. Returns the number of effects that could not be made, or -1 when the file is unreadable.
AVER_AUDIO_API int32_t aver_audio_bus_load_layout(const char* utf8Path);
AVER_AUDIO_API int32_t aver_audio_bus_apply_layout_text(const char* utf8Text);
// The current layout as .ocbus text (length returned; truncated to cap).
AVER_AUDIO_API int32_t aver_audio_bus_layout_text(uint8_t* textOut, int32_t cap);

// Effects. `godotClass` is Godot's class name ("AudioEffectReverb"); the id is stable until the effect is removed.
AVER_AUDIO_API int32_t aver_audio_effect_add(int32_t bus, const char* godotClass, int32_t at);
AVER_AUDIO_API int32_t aver_audio_effect_remove(int32_t bus, int32_t index);
AVER_AUDIO_API int32_t aver_audio_effect_move(int32_t bus, int32_t from, int32_t to);
AVER_AUDIO_API int32_t aver_audio_effect_count(int32_t bus);
AVER_AUDIO_API int32_t aver_audio_effect_id(int32_t bus, int32_t index);
// The bus and index an id is at; 1 when found.
AVER_AUDIO_API int32_t aver_audio_effect_find(int32_t id, int32_t* bus, int32_t* index);
AVER_AUDIO_API void    aver_audio_effect_set_enabled(int32_t bus, int32_t index, int32_t enabled);
AVER_AUDIO_API int32_t aver_audio_effect_get_enabled(int32_t bus, int32_t index);
AVER_AUDIO_API void    aver_audio_effect_set_sidechain(int32_t bus, int32_t index, const char* utf8BusName);
AVER_AUDIO_API int32_t aver_audio_effect_sidechain(int32_t bus, int32_t index, uint8_t* textOut, int32_t cap);
// Parameters by Godot property name; set returns 1 when the effect has it.
AVER_AUDIO_API int32_t aver_audio_effect_set_param(int32_t id, const char* name, float value);
AVER_AUDIO_API float   aver_audio_effect_get_param(int32_t id, const char* name);
// Godot's class name of an effect ("AudioEffectReverb"); its length is returned, 0 for an unknown id.
AVER_AUDIO_API int32_t aver_audio_effect_class(int32_t id, uint8_t* textOut, int32_t cap);
AVER_AUDIO_API int32_t aver_audio_effect_param_count(int32_t id);
AVER_AUDIO_API int32_t aver_audio_effect_param_info(int32_t id, int32_t index, uint8_t* nameOut, int32_t cap, float* def, float* lo, float* hi);
// The readable effects. Capture: stereo frames, interleaved.
AVER_AUDIO_API int32_t aver_audio_capture_available(int32_t id);
AVER_AUDIO_API int32_t aver_audio_capture_read(int32_t id, float* dst, int32_t maxFrames);
AVER_AUDIO_API int32_t aver_audio_capture_capacity(int32_t id);
AVER_AUDIO_API int32_t aver_audio_capture_discarded(int32_t id);
AVER_AUDIO_API void    aver_audio_capture_clear(int32_t id);
// SpectrumAnalyzer: linear magnitude over a frequency range, mode 0 average / 1 maximum; 1 when it has enough audio.
AVER_AUDIO_API int32_t aver_audio_spectrum(int32_t id, float fromHz, float toHz, int32_t mode, float* outLeft, float* outRight);

/* Players. `spatial18` is 18 floats (aver::audio::Spatial3D::pack: model, unit_size, max_db, max_distance, panning_strength, global panning,
 * cone enabled, cone angle, cone filter dB, filter cutoff, filter dB, emit dir xyz, doppler, velocity xyz in cm/s) or NULL for a flat voice.
 * Positions are engine centimetres. graphBus < 0 plays on the legacy `bus`. */
AVER_AUDIO_API int32_t aver_audio_play_ex(int32_t sound, float volume, float pitch, int32_t looping, int32_t bus, int32_t graphBus,
                                          float startSeconds, int32_t paused, float x, float y, float z, const float* spatial18);
AVER_AUDIO_API int32_t aver_audio_stream_play_ex(const char* utf8Path, float volume, float pitch, int32_t looping, int32_t bus, int32_t graphBus,
                                                 float startSeconds, int32_t paused, float x, float y, float z, const float* spatial18);
AVER_AUDIO_API void    aver_audio_set_voice_bus(int32_t voice, int32_t graphBus);
AVER_AUDIO_API void    aver_audio_set_voice_paused(int32_t voice, int32_t paused);
AVER_AUDIO_API void    aver_audio_set_voice_spatial(int32_t voice, const float* spatial18);
// Seconds played (a looping source wraps) and the source's length (0 unknown); -1 for a stale handle.
AVER_AUDIO_API double  aver_audio_voice_position(int32_t voice);
AVER_AUDIO_API double  aver_audio_voice_length(int32_t voice);
AVER_AUDIO_API int32_t aver_audio_voice_underruns(int32_t voice);
AVER_AUDIO_API void    aver_audio_set_listener_velocity(float vx, float vy, float vz);
AVER_AUDIO_API void    aver_audio_set_speed_scale(float scale);
AVER_AUDIO_API float   aver_audio_speed_scale(void);
// A loaded sound's length in seconds and loop points (frames; end 0 = to the end).
AVER_AUDIO_API double  aver_audio_sound_length(int32_t sound);
AVER_AUDIO_API void    aver_audio_sound_set_loop(int32_t sound, int32_t beginFrame, int32_t endFrame);
// A voice fed by the caller (AudioStreamGenerator): stereo or mono frames at `rate`, in a ring of ringFrames frames.
AVER_AUDIO_API int32_t aver_audio_live_play(float volume, float pitch, int32_t bus, int32_t graphBus, float x, float y, float z,
                                            const float* spatial18, int32_t rate, int32_t channels, int32_t ringFrames);
AVER_AUDIO_API int32_t aver_audio_live_push(int32_t voice, const float* interleaved, int32_t frames);
AVER_AUDIO_API int32_t aver_audio_live_free(int32_t voice);
AVER_AUDIO_API void    aver_audio_live_flush(int32_t voice);

#ifdef __cplusplus
} // extern "C"
#endif
