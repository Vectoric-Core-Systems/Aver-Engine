#pragma once
// The device: a thread that asks the mixer for buffers and hands them to the sound card.
//
// This is the ONLY thing in the audio stack that knows a sound card exists, which is what makes the
// mixer replaceable-backend and testable-headless. It is the counterpart to Aver.RHI.D3D12 sitting
// under Aver.RHI, and to Aver.Render.UI sitting under Aver.UI.
//
// WASAPI shared mode. Chosen over XAudio2 -- the obvious Windows answer -- because XAudio2 would own
// the mixing, the 3D and the DSP, which would make Aver.Audio a wrapper around a thing it cannot
// test and cannot port. The point of owning the mixer is that the mixer is the part with the bugs
// in it. See docs/AUDIO.md §2.
#include "aver/audio/Mixer.hpp"

#include <atomic>
#include <thread>

namespace aver::audio {

class AudioDevice {
public:
    AudioDevice() = default;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the default output and starts the render thread. The MIXER is initialised here, at the
    // device's own rate and channel count, because those are the device's to decide -- asking the
    // caller for a sample rate would be asking it to guess something WASAPI is about to state.
    //
    // Returns false when there is no output device, which is a legitimate configuration (a headless
    // machine, a build server) and not an error: the caller runs silent.
    bool start(u32 maxVoices = 64, u32 maxSounds = 1024);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    Mixer& mixer() { return mixer_; }
    const Mixer& mixer() const { return mixer_; }

    u32 sampleRate() const { return mixer_.sampleRate(); }
    u32 channels() const { return mixer_.channels(); }
    // The device period actually granted, in frames. Latency is this over the sample rate.
    u32 bufferFrames() const { return bufferFrames_; }

    // Buffers the device asked for and did not get in time. THE number to watch: it is the only
    // direct evidence that the render thread is missing its deadline, and every one of them is an
    // audible gap. Nonzero on a machine under load is a fact; nonzero on an idle one is a bug.
    u32 underruns() const { return underruns_.load(std::memory_order_relaxed); }

private:
    void threadMain();

    Mixer mixer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<u32>  underruns_{0};
    u32 bufferFrames_ = 0;

    // Handed to the render thread so start() can report a REAL failure rather than "it might work":
    // the device is opened on that thread (the format is the device's to state), so this is what
    // start() waits on before deciding whether it succeeded. Null once the handshake is over.
    void* readyEvent_ = nullptr;
    u32   maxVoices_ = 64;
    u32   maxSounds_ = 1024;
};

} // namespace aver::audio
