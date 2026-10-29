#pragma once
// The output policy: what happens when the sound card goes away, changes, or was never there. Backend-independent, so the whole
// policy runs in tests/audio against a fake device (AudioDeviceTest); WASAPI and SDL only implement OutputBackend.
//
// The driver owns the loop and the decisions; a backend owns one open endpoint:
//   open()    probe/open the system's CURRENT default output (not started), report its format
//   start()   begin delivering; from here the backend asks OutputSource for audio
//   service() pull backends wait for the device and fill it; push backends (SDL) render from their own callback and only
//             watch the device here. Reports a lost or replaced device.
//   close()   stop and release; no OutputSource call after it returns
//
// What the driver does about it:
//   * a lost device, a changed default or a stalled one: close, reopen on the new default, move the mixer to its rate and channel
//     count (Mixer::setOutputFormat keeps every voice, stream and bus), log one line, count the change;
//   * no device at all: keep mixing into nothing at real-time pace, so playback positions, fades and finishing voices carry on, and
//     retry the open once a second;
//   * a device flapping faster than minRebuildGap is not reopened in a loop: it waits for the next retry;
//   * a changed speaker request (Mixer::speakerRequest: 2, 6 or 8) that picks another layout for the open device reopens it the same way
//     (the channel count of the mixer changes only between close and start), without the flapping wait.
#include "aver/audio/Mixer.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace aver::audio {

// What an open output looks like.
struct OutputInfo {
    std::string name;            // the endpoint's friendly name
    u32 rate = 0;                // Hz, the device's own mix rate
    u32 deviceChannels = 0;      // hardware channels
    u32 channelMask = 0;         // the device's speaker mask (WAVEFORMATEXTENSIBLE bits, Speakers.hpp chmask); 0 = the backend does not know
    u32 mixChannels = 0;         // what the mixer renders (set by the driver): 1, 2, 6 or 8, from chooseMixChannels (Speakers.hpp). Stereo unless
                                 // the project asked for more (Mixer::setSpeakerRequest) and the device can play it.
    u32 bufferFrames = 0;        // the period actually granted
    f32 latencyMs = 0.0f;        // 0: derived from bufferFrames / rate
};

// The audio the backend asks for. Implemented by the driver.
class OutputSource {
public:
    virtual ~OutputSource() = default;
    // Audio thread. Fills `out` with `frames` frames of info.mixChannels interleaved channels. Never allocates, locks, blocks or logs.
    virtual void render(f32* out, u32 frames) = 0;
    // The device wanted a buffer and did not get one in time (an audible gap). Any thread, lock-free.
    virtual void underrun() = 0;
};

enum class ServiceResult : u8 {
    Ok,               // served the device, or nothing was due
    Idle,             // a pull backend waited the whole timeout and the device asked for nothing
    Lost,             // the device was invalidated, unplugged or disabled
    DefaultChanged,   // the system default output is now another endpoint
};

class OutputBackend {
public:
    virtual ~OutputBackend() = default;
    // Called on the render thread before anything else and after everything else (COM, thread priority).
    virtual void threadBegin() {}
    virtual void threadEnd() {}
    // Opens the current default output without starting it. False: no device right now (lastError() says why).
    virtual bool open(OutputInfo& info) = 0;
    // Starts delivery at info.rate with info.mixChannels. False: the endpoint died between open and start.
    virtual bool start(const OutputInfo& info, OutputSource& source) = 0;
    // Stops and releases the endpoint. Safe when not open; after it returns the source is never called.
    virtual void close() = 0;
    // Waits up to timeoutMs for work, serving the device on the way.
    virtual ServiceResult service(u32 timeoutMs) = 0;
    virtual const char* lastError() const { return ""; }
};

// Time, so a test can run the policy without waiting for it.
class OutputClock {
public:
    virtual ~OutputClock() = default;
    virtual f64 now() const = 0;                    // seconds, monotonic
    virtual void sleep(f64 seconds) = 0;
};

class SteadyClock final : public OutputClock {
public:
    f64 now() const override;
    void sleep(f64 seconds) override;
};

enum class OutputState : u8 {
    Stopped = 0,   // never begun, or ended
    Running = 1,   // a device is open and buffers flow
    NoDevice = 2,  // none open: the mixer runs at real-time pace into nothing and the open is retried
};

struct OutputConfig {
    u32 maxVoices = 64;
    u32 maxSounds = 1024;
    u32 serviceMs = 100;          // how long one step() may wait on a pull device (the latency of stop())
    f64 stallSeconds = 2.0;       // a pull device silent this long is treated as lost
    f64 retrySeconds = 1.0;       // between opens while there is no device
    f64 minRebuildGap = 0.25;     // a second loss sooner than this after a rebuild waits for the retry instead
};

class OutputDriver final : private OutputSource {
public:
    OutputDriver(Mixer& mixer, OutputBackend& backend, OutputClock& clock, const OutputConfig& config = {});

    // ---- render thread only ----
    // Opens the first device and initialises the mixer at its format. False when there is none (nothing is started then).
    bool begin();
    // One turn of the loop: serve the device, or idle at real-time pace and retry. Call until told to quit.
    void step();
    // Closes the device. The mixer is left for the caller to shut down.
    void end();

    // ---- any thread ----
    OutputState state() const { return state_.load(std::memory_order_acquire); }
    // Buffers the device asked for and did not get in time (plus one per stalled device).
    u32 underruns() const { return underruns_.load(std::memory_order_relaxed); }
    // Devices opened after the first: every default change, loss and return.
    u32 deviceChanges() const { return changes_.load(std::memory_order_relaxed); }
    // The last device that was open (kept while there is none, so a HUD can say what was lost).
    OutputInfo info() const;
    f32 latencyMs() const;

private:
    void render(f32* out, u32 frames) override;
    void underrun() override { underruns_.fetch_add(1, std::memory_order_relaxed); }

    // Reopens after a loss. `why` is for the log. `force` skips the flapping guard (a layout change is not a failure).
    void rebuild(const char* why, bool force = false);
    // Takes a freshly opened device: formats the mixer, starts it, publishes it. False: it could not be started.
    bool adopt(OutputInfo& next, const char* why);
    void enterNoDevice(const char* why);
    void idleStep();
    const char* errorText() const;

    Mixer& mixer_;
    OutputBackend& backend_;
    OutputClock& clock_;
    OutputConfig cfg_;

    std::atomic<OutputState> state_{OutputState::Stopped};
    std::atomic<u32> underruns_{0};
    std::atomic<u32> changes_{0};

    mutable std::mutex infoMutex_;      // info_ only; never taken by render()
    OutputInfo info_;

    // Render thread only.
    f64 lastActivity_ = 0.0;            // last time a pull device did anything
    f64 lastRebuild_ = -1.0e9;
    f64 idleLast_ = 0.0, nextRetry_ = 0.0, owedFrames_ = 0.0;
    u32 appliedRequest_ = 2;            // the speaker request the open device was chosen for
    std::vector<f32> idleBuf_;          // the silent sink while there is no device; sized once in begin() for the widest layout
};

} // namespace aver::audio
