// Implements the Aver.Audio C ABI over one process-wide device, mixer and sound table.
#include "aver/audio/audio_abi.h"
#include "StreamSources.hpp"
#include "aver/audio/AudioDevice.hpp"
#include "aver/audio/Music.hpp"
#include "aver/formats/OcAudio.hpp"
#include "aver/formats/OcBusLayout.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

aver::audio::AudioDevice g_device;
aver::audio::MusicCrossfader g_music;
bool g_started = false;

// OFFLINE MODE (aver_audio_offline_init): the same mixer with no device behind it, rendered by the caller. For tests and tools; a real
// session never sets it, so mixerRef() is the device's mixer exactly as before.
aver::audio::Mixer g_offline;
bool g_isOffline = false;
aver::audio::Mixer& mixerRef() { return g_isOffline ? g_offline : g_device.mixer(); }

// Path <-> handle, so the same file loaded from several call sites is decoded and held once.
std::unordered_map<std::string, aver::audio::SoundHandle> g_byPath;
std::unordered_map<aver::audio::SoundHandle, std::string> g_paths;

AverAudioPlayObserver g_playObserver = nullptr;
void* g_playObserverUser = nullptr;

// Tells the observer (captions) a voice started. Called only for a voice != 0.
void notifyPlay(const char* path, int32_t voice, int32_t bus, bool positional, float x, float y, float z, bool looping) {
    if (g_playObserver && voice != 0)
        g_playObserver(g_playObserverUser, path, voice, bus, positional ? 1 : 0, x, y, z, looping ? 1 : 0);
}

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
    return static_cast<int32_t>(mixerRef().playStream(std::move(o.source), desc, sp));
}

// The mixer's bus back to the ABI constant, for the observer.
int32_t busValue(aver::audio::Bus b) { return static_cast<int32_t>(b); }

// Copies a string into the caller's buffer (always terminated, truncated to cap) and returns the full length.
int32_t copyOut(const std::string& s, uint8_t* out, int32_t cap) {
    if (out && cap > 0) {
        const size_t n = std::min(s.size(), static_cast<size_t>(cap - 1));
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return static_cast<int32_t>(s.size());
}

aver::audio::Effect* effectOf(int32_t id) {
    return g_started && id > 0 ? mixerRef().graph().effect(static_cast<aver::u32>(id)) : nullptr;
}

// The PlayDesc the *_ex calls share. spatial18 non-null makes a Godot-spatial positional voice at (x, y, z).
aver::audio::PlayDesc exDesc(float volume, float pitch, int32_t looping, int32_t bus, int32_t graphBus, float startSeconds, int32_t paused,
                             float x, float y, float z, const float* spatial18) {
    aver::audio::PlayDesc d;
    d.volume = volume;
    d.pitch = pitch;
    d.looping = looping != 0;
    d.bus = busOf(bus);
    d.graphBus = graphBus;
    d.startSeconds = startSeconds;
    d.paused = paused != 0;
    d.position[0] = x; d.position[1] = y; d.position[2] = z;
    if (spatial18) {
        d.positional = true;
        d.godotSpatial = true;
        d.spatial = aver::audio::Spatial3D::unpack(spatial18);
    }
    return d;
}

} // namespace

extern "C" {

// Opens the default output and starts the mixer. 0 when there is no device, which is not an error.
int32_t aver_audio_init(void) {
    if (g_started) return 1;
    g_started = g_device.start();
    if (g_started) g_music.bind(&mixerRef());
    if (!g_started) AVER_INFO("[Audio] running silent: no output device");
    return g_started ? 1 : 0;
}

// Stops everything, releases the device and forgets every loaded sound.
void aver_audio_shutdown(void) {
    if (!g_started) return;
    mixerRef().stopAll();
    g_music.bind(nullptr);
    if (g_isOffline) { g_offline.shutdown(); g_isOffline = false; }
    else g_device.stop();
    g_byPath.clear();
    g_paths.clear();
    g_started = false;
}

int32_t aver_audio_ready(void)       { return g_started && (g_isOffline || g_device.running()) ? 1 : 0; }
int32_t aver_audio_sample_rate(void) { return g_started ? static_cast<int32_t>(g_isOffline ? g_offline.sampleRate() : g_device.sampleRate()) : 0; }
int32_t aver_audio_channels(void)    { return g_started ? static_cast<int32_t>(g_isOffline ? g_offline.channels() : g_device.channels()) : 0; }
int32_t aver_audio_underruns(void)   { return g_started && !g_isOffline ? static_cast<int32_t>(g_device.underruns()) : 0; }

// Reclaims unloaded sounds whose last voice has ended.
void aver_audio_collect(void) { if (g_started) mixerRef().collect(); }

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
    const aver::audio::SoundHandle h = mixerRef().addSound(std::move(data));
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

    const aver::audio::SoundHandle h = mixerRef().addSound(std::move(data));
    return h ? static_cast<int32_t>(h) : 0;
}

// Releases a sound and forgets its path.
void aver_audio_unload(int32_t sound) {
    if (!g_started || sound <= 0) return;
    const auto h = static_cast<aver::audio::SoundHandle>(sound);
    mixerRef().removeSound(h);
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
    const int32_t voice = static_cast<int32_t>(mixerRef().play(d));
    if (voice != 0) {
        const auto it = g_paths.find(d.sound);
        notifyPlay(it != g_paths.end() ? it->second.c_str() : nullptr, voice, busValue(d.bus), false, 0, 0, 0, d.looping);
    }
    return voice;
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
    const int32_t voice = static_cast<int32_t>(mixerRef().play(d));
    if (voice != 0) {
        const auto it = g_paths.find(d.sound);
        notifyPlay(it != g_paths.end() ? it->second.c_str() : nullptr, voice, busValue(d.bus), true, x, y, z, d.looping);
    }
    return voice;
}

void    aver_audio_stop(int32_t voice)     { if (g_started) mixerRef().stop(static_cast<aver::audio::VoiceHandle>(voice)); }
void    aver_audio_stop_all(void)          { if (g_started) mixerRef().stopAll(); }
int32_t aver_audio_playing(int32_t voice)  { return g_started && mixerRef().playing(static_cast<aver::audio::VoiceHandle>(voice)) ? 1 : 0; }

void aver_audio_set_voice_volume(int32_t v, float volume) { if (g_started) mixerRef().setVoiceVolume(static_cast<aver::audio::VoiceHandle>(v), volume); }
void aver_audio_set_voice_pitch(int32_t v, float pitch)   { if (g_started) mixerRef().setVoicePitch(static_cast<aver::audio::VoiceHandle>(v), pitch); }
void aver_audio_set_voice_position(int32_t v, float x, float y, float z) {
    if (g_started) mixerRef().setVoicePosition(static_cast<aver::audio::VoiceHandle>(v), x, y, z);
}

// Floating-origin shift (LARGE_WORLD 2.2.5): every voice position and the listener move by -(dx, dy, dz) cm.
void aver_audio_origin_shift(float dx, float dy, float dz) {
    if (g_started) mixerRef().rebase(dx, dy, dz);
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
    mixerRef().setListener(l);
}

// Reads the listener back, so native systems (reverb zones, occlusion) need not be told it twice.
void aver_audio_get_listener(float* outPosition, float* outForward, float* outRight) {
    aver::audio::Listener l;
    if (g_started) l = mixerRef().listener();
    for (int i = 0; i < 3; ++i) {
        if (outPosition) outPosition[i] = l.position[i];
        if (outForward)  outForward[i]  = l.forward[i];
        if (outRight)    outRight[i]    = l.right[i];
    }
}

void  aver_audio_set_bus_volume(int32_t bus, float v) { if (g_started) mixerRef().setBusVolume(busOf(bus), v); }
float aver_audio_bus_volume(int32_t bus)              { return g_started ? mixerRef().busVolume(busOf(bus)) : 0.0f; }
void  aver_audio_set_master_volume(float v)           { if (g_started) mixerRef().setMasterVolume(v); }
float aver_audio_master_volume(void)                  { return g_started ? mixerRef().masterVolume() : 0.0f; }

int32_t aver_audio_active_voices(void) { return g_started ? static_cast<int32_t>(mixerRef().activeVoices()) : 0; }
int32_t aver_audio_stolen_voices(void) { return g_started ? static_cast<int32_t>(mixerRef().stolenVoices()) : 0; }

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
    const int32_t voice = startStream(utf8Path, d);
    notifyPlay(utf8Path, voice, busValue(d.bus), false, 0, 0, 0, d.looping);
    return voice;
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
    const int32_t voice = startStream(utf8Path, d);
    notifyPlay(utf8Path, voice, busValue(d.bus), true, x, y, z, d.looping);
    return voice;
}

void aver_audio_fade_voice(int32_t voice, float targetGain, float seconds, int32_t curve, int32_t stopWhenDone) {
    if (!g_started) return;
    mixerRef().fadeVoice(static_cast<aver::audio::VoiceHandle>(voice), targetGain, seconds,
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
    const int32_t voice = static_cast<int32_t>(g_music.play(std::move(o.source), opt));
    notifyPlay(utf8Path, voice, AVER_AUDIO_BUS_MUSIC, false, 0, 0, 0, opt.looping);
    return voice;
}

void aver_audio_set_play_observer(AverAudioPlayObserver fn, void* user) {
    g_playObserver = fn;
    g_playObserverUser = user;
}

void    aver_audio_music_stop(float fadeSeconds) { if (g_started) g_music.stop(fadeSeconds); }
int32_t aver_audio_music_voice(void)             { return g_started ? static_cast<int32_t>(g_music.current()) : 0; }

void aver_audio_set_voice_occlusion(int32_t v, float occlusion) {
    if (g_started) mixerRef().setVoiceOcclusion(static_cast<aver::audio::VoiceHandle>(v), occlusion);
}
void aver_audio_set_voice_reverb_send(int32_t v, float send) {
    if (g_started) mixerRef().setVoiceReverbSend(static_cast<aver::audio::VoiceHandle>(v), send);
}
void aver_audio_set_occlusion_curve(float minCutoffHz, float minVolume) {
    if (!g_started) return;
    aver::audio::OcclusionCurve c;
    c.minCutoffHz = minCutoffHz;
    c.minVolume = minVolume;
    mixerRef().setOcclusionCurve(c);
}
void aver_audio_set_reverb(float wet, float decaySeconds, float damping) {
    if (!g_started) return;
    aver::audio::ReverbParams p;
    p.wet = wet;
    p.decaySec = decaySeconds;
    p.damping = damping;
    mixerRef().setReverb(p);
}

int32_t aver_audio_stream_underruns(void) { return g_started ? static_cast<int32_t>(mixerRef().streamUnderruns()) : 0; }
int32_t aver_audio_active_streams(void)   { return g_started ? static_cast<int32_t>(mixerRef().activeStreams()) : 0; }

// ---- bus graph, effects, Godot-style players (G5 S1) ----

int32_t aver_audio_offline_init(int32_t sampleRate, int32_t channels) {
    if (g_started) return g_isOffline ? 1 : 0;
    if (sampleRate <= 0 || (channels != 1 && channels != 2)) return 0;
    if (!g_offline.init(static_cast<aver::u32>(sampleRate), static_cast<aver::u32>(channels))) return 0;
    g_isOffline = true;
    g_started = true;
    g_music.bind(&g_offline);
    return 1;
}

int32_t aver_audio_offline_mix(float* out, int32_t frames) {
    if (!g_started || !g_isOffline || !out || frames <= 0) return 0;
    g_offline.mix(out, static_cast<aver::u32>(frames));
    return frames;
}

int32_t aver_audio_is_offline(void) { return g_isOffline ? 1 : 0; }

void    aver_audio_graph_set_active(int32_t active) { if (g_started) mixerRef().graph().setActive(active != 0); }
int32_t aver_audio_graph_active(void) { return g_started && mixerRef().graph().active() ? 1 : 0; }

int32_t aver_audio_bus_count(void) { return g_started ? mixerRef().graph().busCount() : 0; }
int32_t aver_audio_bus_add(int32_t at) { return g_started ? mixerRef().graph().addBus(at) : -1; }
int32_t aver_audio_bus_remove(int32_t index) { return g_started && mixerRef().graph().removeBus(index) ? 1 : 0; }
int32_t aver_audio_bus_move(int32_t from, int32_t to) { return g_started && mixerRef().graph().moveBus(from, to) ? 1 : 0; }
void    aver_audio_bus_set_count(int32_t count) { if (g_started) mixerRef().graph().setBusCount(count); }
int32_t aver_audio_bus_find(const char* name) { return g_started && name ? mixerRef().graph().findBus(name) : -1; }
int32_t aver_audio_bus_name(int32_t index, uint8_t* out, int32_t cap) { return g_started ? copyOut(mixerRef().graph().busName(index), out, cap) : 0; }
void    aver_audio_bus_set_name(int32_t index, const char* name) { if (g_started && name) mixerRef().graph().setBusName(index, name); }
void    aver_audio_bus_set_send(int32_t index, const char* name) { if (g_started && name) mixerRef().graph().setSend(index, name); }
int32_t aver_audio_bus_send_name(int32_t index, uint8_t* out, int32_t cap) { return g_started ? copyOut(mixerRef().graph().sendName(index), out, cap) : 0; }
void    aver_audio_bus_set_volume_db(int32_t index, float db) { if (g_started) mixerRef().graph().setVolumeDb(index, db); }
float   aver_audio_bus_get_volume_db(int32_t index) { return g_started ? mixerRef().graph().volumeDb(index) : 0.0f; }

void aver_audio_bus_set_flag(int32_t index, int32_t flag, int32_t value) {
    if (!g_started) return;
    aver::audio::BusGraph& g = mixerRef().graph();
    switch (flag) {
        case AVER_AUDIO_BUSFLAG_MUTE: g.setMute(index, value != 0); break;
        case AVER_AUDIO_BUSFLAG_SOLO: g.setSolo(index, value != 0); break;
        case AVER_AUDIO_BUSFLAG_BYPASS: g.setBypassEffects(index, value != 0); break;
        default: break;
    }
}

int32_t aver_audio_bus_get_flag(int32_t index, int32_t flag) {
    if (!g_started) return 0;
    const aver::audio::BusGraph& g = mixerRef().graph();
    switch (flag) {
        case AVER_AUDIO_BUSFLAG_MUTE: return g.mute(index) ? 1 : 0;
        case AVER_AUDIO_BUSFLAG_SOLO: return g.solo(index) ? 1 : 0;
        case AVER_AUDIO_BUSFLAG_BYPASS: return g.bypassEffects(index) ? 1 : 0;
        default: return 0;
    }
}

float aver_audio_bus_peak_db(int32_t index, int32_t channel) { return g_started ? mixerRef().graph().peakDb(index, channel) : -200.0f; }

int32_t aver_audio_bus_load_layout(const char* path) {
    if (!g_started || !path) return -1;
    aver::audio::BusLayout layout;
    std::string why;
    if (!aver::fmt::loadOcBusLayout(path, layout, &why)) { AVER_ERROR("[Audio] bus layout '{}': {}", path, why); return -1; }
    return static_cast<int32_t>(mixerRef().graph().applyLayout(layout));
}

int32_t aver_audio_bus_apply_layout_text(const char* text) {
    if (!g_started || !text) return -1;
    aver::audio::BusLayout layout;
    std::string why;
    if (!aver::fmt::parseOcBusLayout(text, layout, &why)) { AVER_ERROR("[Audio] bus layout: {}", why); return -1; }
    return static_cast<int32_t>(mixerRef().graph().applyLayout(layout));
}

int32_t aver_audio_bus_layout_text(uint8_t* out, int32_t cap) {
    return g_started ? copyOut(aver::fmt::writeOcBusLayout(mixerRef().graph().exportLayout()), out, cap) : 0;
}

int32_t aver_audio_effect_add(int32_t bus, const char* godotClass, int32_t at) {
    if (!g_started || !godotClass) return 0;
    const aver::audio::EffectKind k = aver::audio::Effect::kindFromClassName(godotClass);
    if (k == aver::audio::EffectKind::Count) return 0;
    return static_cast<int32_t>(mixerRef().graph().addEffect(bus, k, at));
}
int32_t aver_audio_effect_remove(int32_t bus, int32_t index) { return g_started && mixerRef().graph().removeEffect(bus, index) ? 1 : 0; }
int32_t aver_audio_effect_move(int32_t bus, int32_t from, int32_t to) { return g_started && mixerRef().graph().moveEffect(bus, from, to) ? 1 : 0; }
int32_t aver_audio_effect_count(int32_t bus) { return g_started ? mixerRef().graph().effectCount(bus) : 0; }
int32_t aver_audio_effect_id(int32_t bus, int32_t index) { return g_started ? static_cast<int32_t>(mixerRef().graph().effectId(bus, index)) : 0; }
int32_t aver_audio_effect_find(int32_t id, int32_t* bus, int32_t* index) {
    int32_t b = -1, e = -1;
    if (!g_started || id <= 0 || !mixerRef().graph().findEffect(static_cast<aver::u32>(id), b, e)) return 0;
    if (bus) *bus = b;
    if (index) *index = e;
    return 1;
}
void    aver_audio_effect_set_enabled(int32_t bus, int32_t index, int32_t enabled) { if (g_started) mixerRef().graph().setEffectEnabled(bus, index, enabled != 0); }
int32_t aver_audio_effect_get_enabled(int32_t bus, int32_t index) { return g_started && mixerRef().graph().effectEnabled(bus, index) ? 1 : 0; }
void    aver_audio_effect_set_sidechain(int32_t bus, int32_t index, const char* name) { if (g_started) mixerRef().graph().setSidechain(bus, index, name ? name : ""); }

int32_t aver_audio_effect_sidechain(int32_t bus, int32_t index, uint8_t* out, int32_t cap) {
    return g_started ? copyOut(mixerRef().graph().sidechain(bus, index), out, cap) : 0;
}

int32_t aver_audio_effect_set_param(int32_t id, const char* name, float value) {
    aver::audio::Effect* fx = effectOf(id);
    const int32_t i = fx ? fx->findParam(name) : -1;
    if (i < 0) return 0;
    fx->setParam(static_cast<aver::u32>(i), value);
    return 1;
}
float aver_audio_effect_get_param(int32_t id, const char* name) {
    aver::audio::Effect* fx = effectOf(id);
    const int32_t i = fx ? fx->findParam(name) : -1;
    return i < 0 ? 0.0f : fx->param(static_cast<aver::u32>(i));
}
int32_t aver_audio_effect_class(int32_t id, uint8_t* out, int32_t cap) {
    aver::audio::Effect* fx = effectOf(id);
    return fx ? copyOut(aver::audio::Effect::godotClassName(fx->kind()), out, cap) : 0;
}
int32_t aver_audio_effect_param_count(int32_t id) { aver::audio::Effect* fx = effectOf(id); return fx ? static_cast<int32_t>(fx->paramCount()) : 0; }
int32_t aver_audio_effect_param_info(int32_t id, int32_t index, uint8_t* nameOut, int32_t cap, float* def, float* lo, float* hi) {
    aver::audio::Effect* fx = effectOf(id);
    if (!fx || index < 0 || static_cast<aver::u32>(index) >= fx->paramCount()) return 0;
    const aver::audio::ParamInfo& p = fx->paramInfo(static_cast<aver::u32>(index));
    if (def) *def = p.def;
    if (lo) *lo = p.lo;
    if (hi) *hi = p.hi;
    return copyOut(p.name, nameOut, cap);
}

int32_t aver_audio_capture_available(int32_t id) { aver::audio::Effect* fx = effectOf(id); return fx ? static_cast<int32_t>(fx->captureAvailable()) : 0; }
int32_t aver_audio_capture_read(int32_t id, float* dst, int32_t maxFrames) {
    aver::audio::Effect* fx = effectOf(id);
    return fx && dst && maxFrames > 0 ? static_cast<int32_t>(fx->captureRead(dst, static_cast<aver::u32>(maxFrames))) : 0;
}
int32_t aver_audio_capture_capacity(int32_t id) { aver::audio::Effect* fx = effectOf(id); return fx ? static_cast<int32_t>(fx->captureCapacity()) : 0; }
int32_t aver_audio_capture_discarded(int32_t id) { aver::audio::Effect* fx = effectOf(id); return fx ? static_cast<int32_t>(fx->captureDiscarded()) : 0; }
void    aver_audio_capture_clear(int32_t id) { if (aver::audio::Effect* fx = effectOf(id)) fx->captureClear(); }
int32_t aver_audio_spectrum(int32_t id, float fromHz, float toHz, int32_t mode, float* outLeft, float* outRight) {
    aver::audio::Effect* fx = effectOf(id);
    float l = 0.0f, r = 0.0f;
    const bool ok = fx && fx->spectrum(fromHz, toHz, mode, l, r);
    if (outLeft) *outLeft = l;
    if (outRight) *outRight = r;
    return ok ? 1 : 0;
}

int32_t aver_audio_play_ex(int32_t sound, float volume, float pitch, int32_t looping, int32_t bus, int32_t graphBus, float startSeconds,
                           int32_t paused, float x, float y, float z, const float* spatial18) {
    if (!g_started || sound <= 0) return 0;
    aver::audio::PlayDesc d = exDesc(volume, pitch, looping, bus, graphBus, startSeconds, paused, x, y, z, spatial18);
    d.sound = static_cast<aver::audio::SoundHandle>(sound);
    const int32_t voice = static_cast<int32_t>(mixerRef().play(d));
    if (voice != 0) {
        const auto it = g_paths.find(d.sound);
        notifyPlay(it != g_paths.end() ? it->second.c_str() : nullptr, voice, busValue(d.bus), d.positional, x, y, z, d.looping);
    }
    return voice;
}

int32_t aver_audio_stream_play_ex(const char* path, float volume, float pitch, int32_t looping, int32_t bus, int32_t graphBus, float startSeconds,
                                  int32_t paused, float x, float y, float z, const float* spatial18) {
    const aver::audio::PlayDesc d = exDesc(volume, pitch, looping, bus, graphBus, startSeconds, paused, x, y, z, spatial18);
    const int32_t voice = startStream(path, d);
    notifyPlay(path, voice, busValue(d.bus), d.positional, x, y, z, d.looping);
    return voice;
}

void aver_audio_set_voice_bus(int32_t voice, int32_t graphBus) { if (g_started) mixerRef().setVoiceGraphBus(static_cast<aver::audio::VoiceHandle>(voice), graphBus); }
void aver_audio_set_voice_paused(int32_t voice, int32_t paused) { if (g_started) mixerRef().setVoicePaused(static_cast<aver::audio::VoiceHandle>(voice), paused != 0); }
void aver_audio_set_voice_spatial(int32_t voice, const float* spatial18) {
    if (g_started && spatial18) mixerRef().setVoiceSpatial(static_cast<aver::audio::VoiceHandle>(voice), aver::audio::Spatial3D::unpack(spatial18));
}
double  aver_audio_voice_position(int32_t voice) { return g_started ? mixerRef().voicePositionSeconds(static_cast<aver::audio::VoiceHandle>(voice)) : -1.0; }
double  aver_audio_voice_length(int32_t voice) { return g_started ? mixerRef().voiceLengthSeconds(static_cast<aver::audio::VoiceHandle>(voice)) : 0.0; }
int32_t aver_audio_voice_underruns(int32_t voice) { return g_started ? static_cast<int32_t>(mixerRef().voiceUnderruns(static_cast<aver::audio::VoiceHandle>(voice))) : 0; }
void    aver_audio_set_listener_velocity(float vx, float vy, float vz) { if (g_started) mixerRef().setListenerVelocity(vx, vy, vz); }
void    aver_audio_set_speed_scale(float scale) { if (g_started) mixerRef().setPlaybackSpeedScale(scale); }
float   aver_audio_speed_scale(void) { return g_started ? mixerRef().playbackSpeedScale() : 1.0f; }
double  aver_audio_sound_length(int32_t sound) { return g_started && sound > 0 ? mixerRef().soundLengthSeconds(static_cast<aver::audio::SoundHandle>(sound)) : 0.0; }
void    aver_audio_sound_set_loop(int32_t sound, int32_t beginFrame, int32_t endFrame) {
    if (g_started && sound > 0) mixerRef().setSoundLoop(static_cast<aver::audio::SoundHandle>(sound), beginFrame > 0 ? static_cast<aver::u32>(beginFrame) : 0u, endFrame > 0 ? static_cast<aver::u32>(endFrame) : 0u);
}

int32_t aver_audio_live_play(float volume, float pitch, int32_t bus, int32_t graphBus, float x, float y, float z, const float* spatial18,
                             int32_t rate, int32_t channels, int32_t ringFrames) {
    if (!g_started || rate <= 0) return 0;
    const aver::audio::PlayDesc d = exDesc(volume, pitch, 0, bus, graphBus, 0.0f, 0, x, y, z, spatial18);
    return static_cast<int32_t>(mixerRef().playLive(d, static_cast<aver::u32>(rate), static_cast<aver::u32>(channels), ringFrames > 0 ? static_cast<aver::u32>(ringFrames) : 0u));
}
int32_t aver_audio_live_push(int32_t voice, const float* frames, int32_t count) {
    return g_started && frames && count > 0 ? static_cast<int32_t>(mixerRef().pushLive(static_cast<aver::audio::VoiceHandle>(voice), frames, static_cast<aver::u32>(count))) : 0;
}
int32_t aver_audio_live_free(int32_t voice) { return g_started ? static_cast<int32_t>(mixerRef().liveFree(static_cast<aver::audio::VoiceHandle>(voice))) : 0; }
void    aver_audio_live_flush(int32_t voice) { if (g_started) mixerRef().liveFlush(static_cast<aver::audio::VoiceHandle>(voice)); }

} // extern "C"
