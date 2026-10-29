// The OutputBackend over SDL 3, everywhere but Windows (modules/audio.wasapi is the Windows one).
//
// SDL ALREADY HAS THE RENDER THREAD. WASAPI hands the engine a buffer event and the engine waits on it; SDL runs a thread per device and
// calls back whenever the stream wants more. So this is a PUSH backend: start() opens a stream on the default playback device in the
// device's own rate, the callback asks the policy's OutputSource for exactly what SDL asked for, and SDL converts the channel layout and
// sample format to whatever the hardware takes (PipeWire, PulseAudio or ALSA on Linux, Core Audio on macOS). service() only watches the
// device: it reports a removed device or a changed default so the policy can re-open and move the mixer to the new rate.
//
// Detection is two things together: an event watcher (SDL_EVENT_AUDIO_DEVICE_ADDED / REMOVED / FORMAT_CHANGED, which only sets a flag, as
// SDL may call a watcher from any thread) and a poll of the default device's name and format every kPollMs, which is what is trusted.
#include "aver/audio/AudioDevice.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace aver::audio {
namespace {

constexpr u32 kSliceMs = 10;        // service() sleeps in slices this long, so a quit is noticed quickly
constexpr u32 kPollMs = 250;        // how often the default device is compared with the open one
constexpr u32 kScratchFrames = 4096;

bool audioUp() {
    static const bool up = [] {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
        return true;
    }();
    return up;
}

class SdlBackend final : public OutputBackend {
public:
    ~SdlBackend() override { close(); }

    bool open(OutputInfo& info) override {
        close();
        if (!audioUp()) { lastError_ = std::string("SDL audio unavailable (") + SDL_GetError() + ")"; return false; }
        SDL_AudioSpec spec{};
        int frames = 0;
        // The device's own format, so SDL resamples nothing: the mixer runs at the hardware rate, exactly as it runs at WASAPI's mix rate.
        if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) {
            lastError_ = SDL_GetError();
            return false;
        }
        info = OutputInfo{};
        info.rate = static_cast<u32>(spec.freq);
        info.deviceChannels = static_cast<u32>(spec.channels);
        info.channelMask = 0;   // SDL gives no mask; its 6 and 8 channel layouts are the standard order the mixer uses, and SDL maps them to the hardware
        info.bufferFrames = static_cast<u32>(std::max(frames, 0));
        info.name = defaultName();
        openName_ = info.name;
        openRate_ = spec.freq;
        openChannels_ = spec.channels;
        lastError_.clear();
        return true;
    }

    bool start(const OutputInfo& info, OutputSource& source) override {
        mixChannels_ = info.mixChannels;
        scratch_.assign(static_cast<size_t>(kScratchFrames) * mixChannels_, 0.0f);   // the callback must not allocate
        source_.store(&source, std::memory_order_release);

        const SDL_AudioSpec spec{SDL_AUDIO_F32, static_cast<int>(mixChannels_), static_cast<int>(info.rate)};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlBackend::feed, this);
        if (!stream) {
            lastError_ = SDL_GetError();
            source_.store(nullptr, std::memory_order_release);
            return false;
        }
        stream_ = stream;
        changed_.store(false, std::memory_order_relaxed);
        SDL_AddEventWatch(&SdlBackend::watch, this);
        watching_ = true;
        lastPollMs_ = SDL_GetTicks();
        SDL_ResumeAudioStreamDevice(stream);   // streams open paused
        return true;
    }

    void close() override {
        if (watching_) { SDL_RemoveEventWatch(&SdlBackend::watch, this); watching_ = false; }
        // Destroying the stream stops SDL's callback before it returns, so nothing renders after this.
        if (stream_) { SDL_DestroyAudioStream(stream_); stream_ = nullptr; }
        source_.store(nullptr, std::memory_order_release);
    }

    ServiceResult service(u32 timeoutMs) override {
        if (!stream_) return ServiceResult::Lost;
        u32 waited = 0;
        for (;;) {
            const Uint64 now = SDL_GetTicks();
            if (changed_.exchange(false, std::memory_order_acq_rel) || now - lastPollMs_ >= kPollMs) {
                lastPollMs_ = now;
                SDL_AudioSpec spec{};
                int frames = 0;
                if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) return ServiceResult::Lost;
                if (spec.freq != openRate_ || spec.channels != openChannels_ || defaultName() != openName_)
                    return ServiceResult::DefaultChanged;
            }
            if (waited >= timeoutMs) return ServiceResult::Ok;
            SDL_Delay(kSliceMs);
            waited += kSliceMs;
        }
    }

    const char* lastError() const override { return lastError_.c_str(); }

private:
    // SDL's thread. Pulls `additional` bytes' worth of frames from the source in scratch-sized pieces; no allocation.
    static void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int) {
        auto* self = static_cast<SdlBackend*>(user);
        OutputSource* src = self->source_.load(std::memory_order_acquire);
        const u32 ch = self->mixChannels_;
        if (!src || ch == 0 || additional <= 0) return;
        u32 frames = static_cast<u32>(additional) / (ch * static_cast<u32>(sizeof(float)));
        while (frames > 0) {
            const u32 n = std::min(frames, kScratchFrames);
            src->render(self->scratch_.data(), n);
            SDL_PutAudioStreamData(stream, self->scratch_.data(), static_cast<int>(n * ch * sizeof(float)));
            frames -= n;
        }
    }

    // Any thread: only raises a flag for service() to look at.
    static bool SDLCALL watch(void* user, SDL_Event* event) {
        if (event->type == SDL_EVENT_AUDIO_DEVICE_ADDED || event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
            event->type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)
            static_cast<SdlBackend*>(user)->changed_.store(true, std::memory_order_release);
        return true;
    }

    static std::string defaultName() {
        const char* n = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        return n ? std::string(n) : std::string();
    }

    SDL_AudioStream* stream_ = nullptr;
    std::atomic<OutputSource*> source_{nullptr};
    std::atomic<bool> changed_{false};
    std::vector<float> scratch_;
    std::string openName_, lastError_;
    int openRate_ = 0, openChannels_ = 0;
    u32 mixChannels_ = 0;
    Uint64 lastPollMs_ = 0;
    bool watching_ = false;
};

} // namespace

// Opens the default output and starts the render thread (SDL's own callback thread renders; the engine thread watches the device).
bool AudioDevice::start(u32 maxVoices, u32 maxSounds) {
    OutputConfig config;
    config.maxVoices = maxVoices;
    config.maxSounds = maxSounds;
    return startWith(std::make_unique<SdlBackend>(), nullptr, config);
}

} // namespace aver::audio
