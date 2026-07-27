#include "aver/audio/audio_abi.h"
#include "aver/audio/AudioDevice.hpp"
#include "aver/formats/OcAudio.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include <unordered_map>

// The ABI's state. One device, one mixer, one sound table -- there is one pair of speakers, and a
// second mixer would be a second thing fighting for them.
namespace {

aver::audio::AudioDevice g_device;
bool g_started = false;

// Path -> handle, so the same file loaded from four call sites is decoded once and held once. The
// alternative is four copies of a footstep in memory and four sound-table slots, and nothing at the
// call site would ever say so.
std::unordered_map<std::string, aver::audio::SoundHandle> g_byPath;
std::unordered_map<aver::audio::SoundHandle, std::string> g_paths;

aver::audio::Bus busOf(int32_t b) {
    switch (b) {
        case AVER_AUDIO_BUS_MUSIC: return aver::audio::Bus::Music;
        case AVER_AUDIO_BUS_VOICE: return aver::audio::Bus::Voice;
        case AVER_AUDIO_BUS_UI:    return aver::audio::Bus::Ui;
        default:                   return aver::audio::Bus::Sfx;
    }
}

} // namespace

extern "C" {

int32_t aver_audio_init(void) {
    if (g_started) return 1;
    // A machine with no output device is a legitimate configuration, so this returns 0 and every
    // call below then succeeds and does nothing. A game needs no special case for a silent machine,
    // which is the only way that path ever gets exercised.
    g_started = g_device.start();
    if (!g_started) AVER_INFO("[Audio] running silent: no output device");
    return g_started ? 1 : 0;
}

void aver_audio_shutdown(void) {
    if (!g_started) return;
    g_device.mixer().stopAll();
    g_device.stop();
    g_byPath.clear();
    g_paths.clear();
    g_started = false;
}

int32_t aver_audio_ready(void)       { return g_started && g_device.running() ? 1 : 0; }
int32_t aver_audio_sample_rate(void) { return g_started ? static_cast<int32_t>(g_device.sampleRate()) : 0; }
int32_t aver_audio_channels(void)    { return g_started ? static_cast<int32_t>(g_device.channels()) : 0; }
int32_t aver_audio_underruns(void)   { return g_started ? static_cast<int32_t>(g_device.underruns()) : 0; }

void aver_audio_collect(void) { if (g_started) g_device.mixer().collect(); }

int32_t aver_audio_load(const char* utf8Path) {
    if (!g_started || !utf8Path || !*utf8Path) return 0;
    const std::string path = utf8Path;
    if (auto it = g_byPath.find(path); it != g_byPath.end()) return static_cast<int32_t>(it->second);

    aver::audio::SoundData data;
    // .ocaudio is the ENGINE's format and the fast path: decoded interleaved float, ready to play,
    // no decoder involved. Anything else is imported on the spot, which is a convenience for
    // development -- pointing at a .wav or an .mp3 while iterating -- and is not what a shipped game
    // should be doing when it opens a door.
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

void aver_audio_unload(int32_t sound) {
    if (!g_started || sound <= 0) return;
    const auto h = static_cast<aver::audio::SoundHandle>(sound);
    g_device.mixer().removeSound(h);
    if (auto it = g_paths.find(h); it != g_paths.end()) {
        g_byPath.erase(it->second);
        g_paths.erase(it);
    }
}

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

void  aver_audio_set_bus_volume(int32_t bus, float v) { if (g_started) g_device.mixer().setBusVolume(busOf(bus), v); }
float aver_audio_bus_volume(int32_t bus)              { return g_started ? g_device.mixer().busVolume(busOf(bus)) : 0.0f; }
void  aver_audio_set_master_volume(float v)           { if (g_started) g_device.mixer().setMasterVolume(v); }
float aver_audio_master_volume(void)                  { return g_started ? g_device.mixer().masterVolume() : 0.0f; }

int32_t aver_audio_active_voices(void) { return g_started ? static_cast<int32_t>(g_device.mixer().activeVoices()) : 0; }
int32_t aver_audio_stolen_voices(void) { return g_started ? static_cast<int32_t>(g_device.mixer().stolenVoices()) : 0; }

} // extern "C"
