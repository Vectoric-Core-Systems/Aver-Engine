#pragma once
// The audio device: a render thread that asks the mixer for buffers and hands them to the sound card.
// The only part of the audio stack that knows a sound card exists.
//
// DECLARED HERE, PART IMPLEMENTED ONE MODULE UP. This header lives in Aver.Audio, which stays device-free. The policy (OutputDriver.hpp:
// device loss, default change, no device, retry) and the thread are here; start() is implemented by Aver.Audio.Wasapi on Windows and by
// Aver.Audio.Sdl everywhere else, and each builds the OutputBackend for its platform. Exactly one of the two is built on any platform.
#include "aver/audio/OutputDriver.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace aver::audio {

// The default output device, owning the render thread and the mixer: WASAPI shared mode on Windows, an SDL 3 audio stream elsewhere.
// It follows the system default: unplug, replace or change it and the mixer moves to the new one with every voice and bus intact; with
// no device it keeps mixing into nothing and retries.
class AudioDevice {
public:
    AudioDevice() = default;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the default output and starts the render thread, initialising the mixer at the device's own rate and channel count. False when
    // there is no output device at start, which is not an error. (Platform module.)
    bool start(u32 maxVoices = 64, u32 maxSounds = 1024);
    // The same over any backend: how the platform modules start and how tests run the thread against a fake device. A null clock is the
    // real one.
    bool startWith(std::unique_ptr<OutputBackend> backend, std::unique_ptr<OutputClock> clock = nullptr,
                   const OutputConfig& config = {});
    // Stops the render thread and shuts the mixer down.
    void stop();
    // The render thread is alive and the mixer is up. Stays true while there is no device (see state()).
    bool running() const { return running_.load(std::memory_order_acquire); }

    Mixer& mixer() { return mixer_; }
    const Mixer& mixer() const { return mixer_; }

    // ---- reporting (any thread; they keep the last device's values while there is none) ----
    OutputState state() const { return driver_ ? driver_->state() : OutputState::Stopped; }
    // A device is open and buffers are flowing.
    bool deviceOpen() const { return state() == OutputState::Running; }
    u32 sampleRate() const { return mixer_.sampleRate(); }
    u32 channels() const { return mixer_.channels(); }
    // The device's hardware channel count (channels() is what the mixer renders).
    u32 deviceChannels() const { return driver_ ? driver_->info().deviceChannels : 0; }
    std::string deviceName() const { return driver_ ? driver_->info().name : std::string(); }
    // The device period actually granted, in frames. Latency is this over the sample rate.
    u32 bufferFrames() const { return driver_ ? driver_->info().bufferFrames : 0; }
    f32 latencyMs() const { return driver_ ? driver_->latencyMs() : 0.0f; }
    // Buffers the device asked for and did not get in time. Every one is an audible gap.
    u32 underruns() const { return driver_ ? driver_->underruns() : 0; }
    // Devices opened after the first one: default changes, unplugs and returns.
    u32 deviceChanges() const { return driver_ ? driver_->deviceChanges() : 0; }

private:
    Mixer mixer_;
    std::unique_ptr<OutputBackend> backend_;
    std::unique_ptr<OutputClock> clock_;
    std::unique_ptr<OutputDriver> driver_;   // kept after stop() so the counters stay readable
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
};

} // namespace aver::audio
