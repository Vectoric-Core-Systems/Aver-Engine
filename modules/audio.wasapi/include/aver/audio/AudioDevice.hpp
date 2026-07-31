#pragma once
// The audio device: a render thread that asks the mixer for buffers and hands them to the sound card.
// The only part of the audio stack that knows a sound card exists.
#include "aver/audio/Mixer.hpp"

#include <atomic>
#include <thread>

namespace aver::audio {

// WASAPI shared-mode output device, owning the render thread and the mixer.
class AudioDevice {
public:
    AudioDevice() = default;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the default output and starts the render thread, initialising the mixer at the device's
    // own rate and channel count. False when there is no output device, which is not an error.
    bool start(u32 maxVoices = 64, u32 maxSounds = 1024);
    // Stops the render thread and shuts the mixer down.
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    Mixer& mixer() { return mixer_; }
    const Mixer& mixer() const { return mixer_; }

    u32 sampleRate() const { return mixer_.sampleRate(); }
    u32 channels() const { return mixer_.channels(); }
    // The device period actually granted, in frames. Latency is this over the sample rate.
    u32 bufferFrames() const { return bufferFrames_; }

    // Buffers the device asked for and did not get in time. Every one is an audible gap.
    u32 underruns() const { return underruns_.load(std::memory_order_relaxed); }

private:
    // The render thread body: opens the endpoint, then fills buffers until asked to quit.
    void threadMain();

    Mixer mixer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<u32>  underruns_{0};
    u32 bufferFrames_ = 0;

    // Signalled by the render thread once it has decided whether it started. Null after the handshake.
    void* readyEvent_ = nullptr;
    u32   maxVoices_ = 64;
    u32   maxSounds_ = 1024;
};

} // namespace aver::audio
