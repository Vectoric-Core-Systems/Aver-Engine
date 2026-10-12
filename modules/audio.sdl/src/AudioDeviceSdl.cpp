// aver::audio::AudioDevice over SDL 3, everywhere but Windows (modules/audio.wasapi is the Windows one).
//
// SDL ALREADY HAS THE RENDER THREAD. WASAPI hands the engine a buffer event and the engine runs its
// own thread around it; SDL runs a thread per device and calls back whenever the stream wants more.
// So start() opens a stream on the default playback device in the device's own rate, the callback
// asks the mixer for exactly what SDL asked for, and SDL converts the channel layout and sample
// format to whatever the hardware takes -- PipeWire, PulseAudio or ALSA on Linux, Core Audio on macOS.
//
// The mixer side is unchanged: mixChannels is 2 for any device with two or more channels and 1 for a
// mono one, the rule AudioDeviceWasapi.cpp applies.
#include "aver/audio/AudioDevice.hpp"
#include "aver/core/Log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

namespace aver::audio {
namespace {

bool audioUp() {
    static const bool up = [] {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            AVER_WARN("[Audio] SDL audio unavailable ({})", SDL_GetError());
            return false;
        }
        return true;
    }();
    return up;
}

}  // namespace

AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::start(u32 maxVoices, u32 maxSounds) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (!audioUp()) return false;
    maxVoices_ = maxVoices;
    maxSounds_ = maxSounds;

    // The device's own format, so SDL resamples nothing: the mixer runs at the hardware rate, exactly
    // as it runs at WASAPI's mix rate.
    SDL_AudioSpec device{};
    int deviceFrames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, &deviceFrames)) {
        AVER_WARN("[Audio] no output device: {}", SDL_GetError());
        return false;
    }
    const u32 mixChannels = device.channels >= 2 ? 2u : 1u;
    if (!mixer_.init(static_cast<u32>(device.freq), mixChannels, maxVoices_, maxSounds_)) {
        AVER_WARN("[Audio] no output device: the mixer refused the device format");
        return false;
    }

    const SDL_AudioSpec spec{SDL_AUDIO_F32, static_cast<int>(mixChannels), device.freq};
    auto feed = [](void* user, SDL_AudioStream* stream, int additional, int) {
        auto* self = static_cast<AudioDevice*>(user);
        const u32 channels = self->mixer_.channels();
        const u32 frames = static_cast<u32>(additional) / (channels * static_cast<u32>(sizeof(f32)));
        if (frames == 0) return;
        if (self->scratch_.size() < static_cast<usize>(frames) * channels)
            self->scratch_.assign(static_cast<usize>(frames) * channels, 0.0f);
        self->mixer_.mix(self->scratch_.data(), frames);
        SDL_PutAudioStreamData(stream, self->scratch_.data(), static_cast<int>(frames * channels * sizeof(f32)));
    };
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
    if (!stream) {
        AVER_WARN("[Audio] no output device: {}", SDL_GetError());
        mixer_.shutdown();
        return false;
    }
    stream_ = stream;
    bufferFrames_ = static_cast<u32>(std::max(deviceFrames, 0));
    running_.store(true, std::memory_order_release);
    SDL_ResumeAudioStreamDevice(stream);   // streams open paused
    AVER_INFO("[Audio] SDL output ({}): {} Hz, {} device channel(s), mixing {}, {} frame buffer ({:.1f} ms)",
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", device.freq, device.channels,
              mixChannels == 2 ? "stereo" : "mono", bufferFrames_,
              device.freq ? 1000.0 * double(bufferFrames_) / double(device.freq) : 0.0);
    return true;
}

void AudioDevice::stop() {
    // Destroying the stream stops SDL's callback before it returns, so the mixer is idle after this.
    if (stream_) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));
        stream_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    mixer_.shutdown();
}

} // namespace aver::audio
