// Implements the Aver.Audio C ABI over one process-wide device, mixer and sound table.
#include "aver/audio/audio_abi.h"
#include "StreamSources.hpp"
#include "aver/audio/AudioDevice.hpp"
#include "aver/audio/Music.hpp"
#include "aver/formats/OcAudio.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include <unordered_map>

namespace {

aver::audio::AudioDevice g_device;
aver::audio::MusicCrossfader g_music;
bool g_started = false;

// Path <-> handle, so the same file loaded from several call sites is decoded and held once.
std::unordered_map<std::string, aver::audio::SoundHandle> g_byPath;
std::unordered_map<aver::audio::SoundHandle, std::string> g_paths;

// THE MACROS AND THE ENUM, PINNED. audio::Bus (Sound.hpp) and the AVER_AUDIO_BUS_* constants
// (audio_abi.h) are the same four values written twice, and busOf below folds anything it does not
// recognise into Sfx -- deliberately, because an ABI must not trust its caller, but it means a DRIFT
// between these two lists cannot announce itself. Reorder the enum without editing the macros and
// every managed SetBusVolume(1, ...) meant for Music keeps compiling and starts moving whatever
// landed in that slot, with no error anywhere.
//
// This file is the one translation unit that includes both, so the check costs nothing and lives
// where a change to either would be compiled.
static_assert(AVER_AUDIO_BUS_SFX   == static_cast<int32_t>(aver::audio::Bus::Sfx),   "audio_abi.h mirrors audio::Bus");
static_assert(AVER_AUDIO_BUS_MUSIC == static_cast<int32_t>(aver::audio::Bus::Music), "audio_abi.h mirrors audio::Bus");
static_assert(AVER_AUDIO_BUS_VOICE == static_cast<int32_t>(aver::audio::Bus::Voice), "audio_abi.h mirrors audio::Bus");
static_assert(AVER_AUDIO_BUS_UI    == static_cast<int32_t>(aver::audio::Bus::Ui), "audio_abi.h mirrors audio::Bus");
// The count is pinned too, so ADDING a bus to the enum without adding its macro fails here rather
// than silently routing the new bus to Sfx.
static_assert(static_cast<int32_t>(aver::audio::Bus::Count) == 4, "a new bus needs an AVER_AUDIO_BUS_* macro and a busOf case");

static_assert(AVER_AUDIO_FADE_LINEAR      == static_cast<int32_t>(aver::audio::FadeCurve::Linear),     "audio_abi.h mirrors audio::FadeCurve");
static_assert(AVER_AUDIO_FADE_EQUAL_POWER == static_cast<int32_t>(aver::audio::FadeCurve::EqualPower), "audio_abi.h mirrors audio::FadeCurve");

// Maps an AVER_AUDIO_BUS_* constant to a Bus. Anything unknown is Sfx.
aver::audio::Bus busOf(int32_t b) {
    switch (b) {
        case AVER_AUDIO_BUS_MUSIC: return aver::audio::Bus::Music;
        case AVER_AUDIO_BUS_VOICE: return aver::audio::Bus::Voice;
        case AVER_AUDIO_BUS_UI:    return aver::audio::Bus::Ui;
        default:                   return aver::audio::Bus::Sfx;
    }
}

aver::audio::FadeCurve curveOf(int32_t c) {
    return c == AVER_AUDIO_FADE_LINEAR ? aver::audio::FadeCurve::Linear : aver::audio::FadeCurve::EqualPower;
}

// Opens `path` as a stream and starts it as a voice. Returns the voice handle, or 0.
int32_t startStream(const char* utf8Path, const aver::audio::PlayDesc& desc) {
    if (!g_started || !utf8Path || !*utf8Path) return 0;
    std::string why;
    aver::audio::OpenedStream o = aver::audio::openStreamSource(utf8Path, &why);
    if (!o.source) { AVER_ERROR("[Audio] could not stream '{}': {}", utf8Path, why); return 0; }
    if (o.decoder == "resident")
        AVER_WARN("[Audio] '{}' has no incremental decoder here, so it is held in memory while it plays", utf8Path);
    aver::audio::StreamParams sp;
    sp.loopBegin = o.loopBegin;
    sp.loopEnd = o.loopEnd;
    return static_cast<int32_t>(g_device.mixer().playStream(std::move(o.source), desc, sp));
}

} // namespace

extern "C" {

// Opens the default output and starts the mixer. 0 when there is no device, which is not an error.
int32_t aver_audio_init(void) {
    if (g_started) return 1;
    g_started = g_device.start();
    if (g_started) g_music.bind(&g_device.mixer());
    if (!g_started) AVER_INFO("[Audio] running silent: no output device");
    return g_started ? 1 : 0;
}

// Stops everything, releases the device and forgets every loaded sound.
void aver_audio_shutdown(void) {
    if (!g_started) return;
    g_device.mixer().stopAll();
    g_music.bind(nullptr);
    g_device.stop();
    g_byPath.clear();
    g_paths.clear();
    g_started = false;
}

int32_t aver_audio_ready(void)       { return g_started && g_device.running() ? 1 : 0; }
int32_t aver_audio_sample_rate(void) { return g_started ? static_cast<int32_t>(g_device.sampleRate()) : 0; }
int32_t aver_audio_channels(void)    { return g_started ? static_cast<int32_t>(g_device.channels()) : 0; }
int32_t aver_audio_underruns(void)   { return g_started ? static_cast<int32_t>(g_device.underruns()) : 0; }

// Reclaims unloaded sounds whose last voice has ended.
void aver_audio_collect(void) { if (g_started) g_device.mixer().collect(); }

// Loads a sound and returns its handle, or 0. The same path yields the same handle.
int32_t aver_audio_load(const char* utf8Path) {
    if (!g_started || !utf8Path || !*utf8Path) return 0;
    const std::string path = utf8Path;
    if (auto it = g_byPath.find(path); it != g_byPath.end()) return static_cast<int32_t>(it->second);

    aver::audio::SoundData data;
    // .ocaudio is the cooked fast path; anything else is imported on the spot.
    const bool cooked = path.size() > 8 &&
                        path.compare(path.size() - 8, 8, ".ocaudio") == 0;
    if (cooked) {
        std::string why;
        if (!aver::fmt::loadOcAudio(path, data, &why)) {
            AVER_ERROR("[Audio] could not load '{}': {}", path, why);
            return 0;
        }
    } else {
        const aver::fmt::AudioImportResult r = aver::fmt::audioImportFile(path, data);
        if (!r.ok) {
            AVER_ERROR("[Audio] could not import '{}': {}", path, r.error);
            return 0;
        }
        AVER_WARN("[Audio] '{}' was decoded at load time via {}; import it to .ocaudio for a shipped build",
                  path, r.decoder);
    }
    const aver::audio::SoundHandle h = g_device.mixer().addSound(std::move(data));
    if (!h) return 0;
    g_byPath[path] = h;
    g_paths[h] = path;
    return static_cast<int32_t>(h);
}

// Registers GENERATED samples as a sound. The seam that was missing: every other way into the
// mixer's sound table starts at a FILE, so nothing synthesised could ever be played.
//
// NOT PATH-CACHED, deliberately, and it is the one behavioural difference from aver_audio_load
// above. A generated buffer has no path to key on, and two renders of the same graph with different
// seeds are different sounds that must not collide -- so every call adds a new entry and the caller
// owns its lifetime through aver_audio_unload. A caller that renders per play should unload per
// play, or the table grows for the life of the process.
int32_t aver_audio_load_pcm(const float* samples, int32_t frames, int32_t channels,
                            int32_t sampleRate) {
    if (!g_started || !samples) return 0;
    if (frames <= 0 || sampleRate <= 0) return 0;
    // The mixer renders mono or stereo only (see Mixer.cpp's own channel check); refusing here is
    // clearer than letting it silently mis-stride a 6-channel buffer.
    if (channels != 1 && channels != 2) return 0;

    aver::audio::SoundData data;
    data.channels   = static_cast<aver::u32>(channels);
    data.sampleRate = static_cast<aver::u32>(sampleRate);
    data.samples.assign(samples, samples + static_cast<aver::usize>(frames) * channels);
    // No loop points: a generated one-shot. A caller wanting a loop sets them by authoring silence
    // at the ends, which is what a .ocaudio would carry anyway.
    data.loopBegin = data.loopEnd = 0;

    const aver::audio::SoundHandle h = g_device.mixer().addSound(std::move(data));
    return h ? static_cast<int32_t>(h) : 0;
}

// Releases a sound and forgets its path.
void aver_audio_unload(int32_t sound) {
    if (!g_started || sound <= 0) return;
    const auto h = static_cast<aver::audio::SoundHandle>(sound);
    g_device.mixer().removeSound(h);
    if (auto it = g_paths.find(h); it != g_paths.end()) {
        g_byPath.erase(it->second);
        g_paths.erase(it);
    }
}

// Starts a 2D voice. Returns the voice handle, or 0.
int32_t aver_audio_play(int32_t sound, float volume, float pitch, int32_t looping, int32_t bus) {
    if (!g_started || sound <= 0) return 0;
    aver::audio::PlayDesc d;
    d.sound   = static_cast<aver::audio::SoundHandle>(sound);
    d.volume  = volume;
    d.pitch   = pitch;
    d.looping = looping != 0;
    d.bus     = busOf(bus);
    return static_cast<int32_t>(g_device.mixer().play(d));
}

// Starts a positional voice at a world point. Returns the voice handle, or 0.
int32_t aver_audio_play_at(int32_t sound, float x, float y, float z,
                           float volume, float pitch, int32_t looping, int32_t bus,
                           float innerCm, float outerCm) {
    if (!g_started || sound <= 0) return 0;
    aver::audio::PlayDesc d;
    d.sound      = static_cast<aver::audio::SoundHandle>(sound);
    d.volume     = volume;
    d.pitch      = pitch;
    d.looping    = looping != 0;
    d.bus        = busOf(bus);
    d.positional = true;
    d.position[0] = x; d.position[1] = y; d.position[2] = z;
    if (innerCm > 0.0f) d.attenuation.innerRadius = innerCm;
    if (outerCm > innerCm) d.attenuation.outerRadius = outerCm;
    return static_cast<int32_t>(g_device.mixer().play(d));
}

void    aver_audio_stop(int32_t voice)     { if (g_started) g_device.mixer().stop(static_cast<aver::audio::VoiceHandle>(voice)); }
void    aver_audio_stop_all(void)          { if (g_started) g_device.mixer().stopAll(); }
int32_t aver_audio_playing(int32_t voice)  { return g_started && g_device.mixer().playing(static_cast<aver::audio::VoiceHandle>(voice)) ? 1 : 0; }

void aver_audio_set_voice_volume(int32_t v, float volume) { if (g_started) g_device.mixer().setVoiceVolume(static_cast<aver::audio::VoiceHandle>(v), volume); }
void aver_audio_set_voice_pitch(int32_t v, float pitch)   { if (g_started) g_device.mixer().setVoicePitch(static_cast<aver::audio::VoiceHandle>(v), pitch); }
void aver_audio_set_voice_position(int32_t v, float x, float y, float z) {
    if (g_started) g_device.mixer().setVoicePosition(static_cast<aver::audio::VoiceHandle>(v), x, y, z);
}

// Places the listener: position, forward and right, in engine units.
void aver_audio_set_listener(float px, float py, float pz,
                             float fx, float fy, float fz,
                             float rx, float ry, float rz) {
    if (!g_started) return;
    aver::audio::Listener l;
    l.position[0] = px; l.position[1] = py; l.position[2] = pz;
    l.forward[0]  = fx; l.forward[1]  = fy; l.forward[2]  = fz;
    l.right[0]    = rx; l.right[1]    = ry; l.right[2]    = rz;
    g_device.mixer().setListener(l);
}

// Reads the listener back, so native systems (reverb zones, occlusion) need not be told it twice.
void aver_audio_get_listener(float* outPosition, float* outForward, float* outRight) {
    aver::audio::Listener l;
    if (g_started) l = g_device.mixer().listener();
    for (int i = 0; i < 3; ++i) {
        if (outPosition) outPosition[i] = l.position[i];
        if (outForward)  outForward[i]  = l.forward[i];
        if (outRight)    outRight[i]    = l.right[i];
    }
}

void  aver_audio_set_bus_volume(int32_t bus, float v) { if (g_started) g_device.mixer().setBusVolume(busOf(bus), v); }
float aver_audio_bus_volume(int32_t bus)              { return g_started ? g_device.mixer().busVolume(busOf(bus)) : 0.0f; }
void  aver_audio_set_master_volume(float v)           { if (g_started) g_device.mixer().setMasterVolume(v); }
float aver_audio_master_volume(void)                  { return g_started ? g_device.mixer().masterVolume() : 0.0f; }

int32_t aver_audio_active_voices(void) { return g_started ? static_cast<int32_t>(g_device.mixer().activeVoices()) : 0; }
int32_t aver_audio_stolen_voices(void) { return g_started ? static_cast<int32_t>(g_device.mixer().stolenVoices()) : 0; }

// ---- streaming, fades, music, occlusion, reverb ----

// Streams a file as a flat voice.
int32_t aver_audio_stream_play(const char* utf8Path, float volume, float pitch, int32_t looping,
                               int32_t bus, float fadeInSeconds) {
    aver::audio::PlayDesc d;
    d.volume = volume;
    d.pitch = pitch;
    d.looping = looping != 0;
    d.bus = busOf(bus);
    d.fadeInSeconds = fadeInSeconds;
    return startStream(utf8Path, d);
}

// Streams a file as a positioned voice.
int32_t aver_audio_stream_play_at(const char* utf8Path, float x, float y, float z, float volume,
                                  float pitch, int32_t looping, int32_t bus, float innerCm,
                                  float outerCm, float fadeInSeconds) {
    aver::audio::PlayDesc d;
    d.volume = volume;
    d.pitch = pitch;
    d.looping = looping != 0;
    d.bus = busOf(bus);
    d.positional = true;
    d.position[0] = x; d.position[1] = y; d.position[2] = z;
    if (innerCm > 0.0f) d.attenuation.innerRadius = innerCm;
    if (outerCm > innerCm) d.attenuation.outerRadius = outerCm;
    d.fadeInSeconds = fadeInSeconds;
    return startStream(utf8Path, d);
}

void aver_audio_fade_voice(int32_t voice, float targetGain, float seconds, int32_t curve, int32_t stopWhenDone) {
    if (!g_started) return;
    g_device.mixer().fadeVoice(static_cast<aver::audio::VoiceHandle>(voice), targetGain, seconds,
                               curveOf(curve), stopWhenDone != 0);
}

// Starts a music track, crossfading from the current one.
int32_t aver_audio_music_play(const char* utf8Path, float volume, float fadeSeconds, int32_t looping, int32_t curve) {
    if (!g_started || !utf8Path || !*utf8Path) return 0;
    std::string why;
    aver::audio::OpenedStream o = aver::audio::openStreamSource(utf8Path, &why);
    if (!o.source) { AVER_ERROR("[Audio] could not stream music '{}': {}", utf8Path, why); return 0; }
    aver::audio::MusicCrossfader::Options opt;
    opt.volume = volume;
    opt.looping = looping != 0;
    opt.fadeSeconds = fadeSeconds;
    opt.curve = curveOf(curve);
    opt.stream.loopBegin = o.loopBegin;
    opt.stream.loopEnd = o.loopEnd;
    return static_cast<int32_t>(g_music.play(std::move(o.source), opt));
}

void    aver_audio_music_stop(float fadeSeconds) { if (g_started) g_music.stop(fadeSeconds); }
int32_t aver_audio_music_voice(void)             { return g_started ? static_cast<int32_t>(g_music.current()) : 0; }

void aver_audio_set_voice_occlusion(int32_t v, float occlusion) {
    if (g_started) g_device.mixer().setVoiceOcclusion(static_cast<aver::audio::VoiceHandle>(v), occlusion);
}
void aver_audio_set_voice_reverb_send(int32_t v, float send) {
    if (g_started) g_device.mixer().setVoiceReverbSend(static_cast<aver::audio::VoiceHandle>(v), send);
}
void aver_audio_set_occlusion_curve(float minCutoffHz, float minVolume) {
    if (!g_started) return;
    aver::audio::OcclusionCurve c;
    c.minCutoffHz = minCutoffHz;
    c.minVolume = minVolume;
    g_device.mixer().setOcclusionCurve(c);
}
void aver_audio_set_reverb(float wet, float decaySeconds, float damping) {
    if (!g_started) return;
    aver::audio::ReverbParams p;
    p.wet = wet;
    p.decaySec = decaySeconds;
    p.damping = damping;
    g_device.mixer().setReverb(p);
}

int32_t aver_audio_stream_underruns(void) { return g_started ? static_cast<int32_t>(g_device.mixer().streamUnderruns()) : 0; }
int32_t aver_audio_active_streams(void)   { return g_started ? static_cast<int32_t>(g_device.mixer().activeStreams()) : 0; }

} // extern "C"
