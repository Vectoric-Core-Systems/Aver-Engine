// The output policy: device loss, default-device change, no device, device back. See OutputDriver.hpp.
#include "aver/audio/OutputDriver.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace aver::audio {
namespace {

constexpr u32 kIdleChunk = 2048;        // frames mixed per call while there is no device
constexpr f64 kIdleSleep = 0.005;       // seconds between idle turns
constexpr f64 kIdleCatchUp = 0.5;       // a starved idle thread owes at most this much audio, not the whole gap

const char* layoutName(u32 channels) {
    switch (channels) {
        case 1: return "mono";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return "stereo";
    }
}

const char* orDefault(const std::string& s) { return s.empty() ? "default" : s.c_str(); }

} // namespace

const char* OutputDriver::errorText() const {
    const char* e = backend_.lastError();
    return e && *e ? e : "none available";
}

f64 SteadyClock::now() const {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SteadyClock::sleep(f64 seconds) {
    if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<f64>(seconds));
}

OutputDriver::OutputDriver(Mixer& mixer, OutputBackend& backend, OutputClock& clock, const OutputConfig& config)
    : mixer_(mixer), backend_(backend), clock_(clock), cfg_(config) {}

OutputInfo OutputDriver::info() const {
    std::lock_guard<std::mutex> g(infoMutex_);
    return info_;
}

f32 OutputDriver::latencyMs() const {
    std::lock_guard<std::mutex> g(infoMutex_);
    return info_.latencyMs;
}

// The mixer's block. Audio thread.
void OutputDriver::render(f32* out, u32 frames) { mixer_.mix(out, frames); }

// Opens the first device and brings the mixer up at its format.
bool OutputDriver::begin() {
    if (state() != OutputState::Stopped) return true;
    OutputInfo info;
    if (!backend_.open(info)) {
        AVER_WARN("[Audio] no output device: {}", errorText());
        return false;
    }
    appliedRequest_ = mixer_.speakerRequest();
    info.mixChannels = chooseMixChannels(info.deviceChannels, info.channelMask, appliedRequest_);
    if (info.latencyMs <= 0.0f && info.rate) info.latencyMs = 1000.0f * static_cast<f32>(info.bufferFrames) / static_cast<f32>(info.rate);

    if (!mixer_.init(info.rate, info.mixChannels, cfg_.maxVoices, cfg_.maxSounds)) {
        AVER_WARN("[Audio] no output device: the mixer refused the device format");
        backend_.close();
        return false;
    }
    idleBuf_.assign(static_cast<usize>(kIdleChunk) * kMaxSpeakers, 0.0f);

    if (!backend_.start(info, *this)) {
        AVER_WARN("[Audio] no output device: {}", errorText());
        backend_.close();
        mixer_.shutdown();
        return false;
    }
    {
        std::lock_guard<std::mutex> g(infoMutex_);
        info_ = info;
    }
    lastActivity_ = clock_.now();
    state_.store(OutputState::Running, std::memory_order_release);
    AVER_INFO("[Audio] output '{}': {} Hz, {} device channel(s), mixing {}, {} frame buffer ({:.1f} ms)",
              orDefault(info.name), info.rate, info.deviceChannels, layoutName(info.mixChannels),
              info.bufferFrames, info.latencyMs);
    return true;
}

void OutputDriver::end() {
    if (state() == OutputState::Stopped) return;
    backend_.close();
    state_.store(OutputState::Stopped, std::memory_order_release);
}

// One turn of the loop.
void OutputDriver::step() {
    switch (state()) {
        case OutputState::Stopped: return;
        case OutputState::NoDevice: idleStep(); return;
        case OutputState::Running: break;
    }
    const ServiceResult r = backend_.service(cfg_.serviceMs);
    const f64 now = clock_.now();
    switch (r) {
        case ServiceResult::Ok:
            lastActivity_ = now;
            break;
        case ServiceResult::Idle:
            // A pull device that has asked for nothing for seconds is wedged: count it and rebuild rather than wait forever.
            if (now - lastActivity_ >= cfg_.stallSeconds) { underrun(); rebuild("device stalled"); }
            break;
        case ServiceResult::Lost:
            rebuild("device lost");
            break;
        case ServiceResult::DefaultChanged:
            rebuild("default device changed");
            break;
    }

    // A new speaker request that picks another layout for this device: reopen it so the mixer changes channel count while no endpoint runs.
    if (state() == OutputState::Running) {
        const u32 req = mixer_.speakerRequest();
        if (req != appliedRequest_) {
            appliedRequest_ = req;
            const OutputInfo cur = info();
            if (chooseMixChannels(cur.deviceChannels, cur.channelMask, req) != mixer_.channels()) rebuild("speaker layout changed", true);
        }
    }
}

// Closes the dead endpoint and opens whatever is the default now.
void OutputDriver::rebuild(const char* why, bool force) {
    const f64 now = clock_.now();
    const bool flapping = !force && now - lastRebuild_ < cfg_.minRebuildGap;
    backend_.close();
    if (flapping) { enterNoDevice(why); return; }
    OutputInfo next;
    if (backend_.open(next) && adopt(next, why)) return;
    enterNoDevice(why);
}

// Starts a freshly opened endpoint and moves the mixer to its format. The endpoint is closed again when this fails.
bool OutputDriver::adopt(OutputInfo& next, const char* why) {
    appliedRequest_ = mixer_.speakerRequest();
    next.mixChannels = chooseMixChannels(next.deviceChannels, next.channelMask, appliedRequest_);
    if (next.latencyMs <= 0.0f && next.rate) next.latencyMs = 1000.0f * static_cast<f32>(next.bufferFrames) / static_cast<f32>(next.rate);

    // Nothing is rendering now: the old endpoint is closed and the new one has not been started.
    if (!mixer_.setOutputFormat(next.rate, next.mixChannels)) {
        backend_.close();
        return false;
    }
    if (!backend_.start(next, *this)) {
        backend_.close();
        return false;
    }

    OutputInfo prev;
    {
        std::lock_guard<std::mutex> g(infoMutex_);
        prev = info_;
        info_ = next;
    }
    lastActivity_ = lastRebuild_ = clock_.now();
    changes_.fetch_add(1, std::memory_order_relaxed);
    state_.store(OutputState::Running, std::memory_order_release);
    AVER_INFO("[Audio] output device changed ({}): '{}' {} Hz -> '{}' {} Hz, {} device channel(s), mixing {}, {} frame buffer ({:.1f} ms)",
              why, orDefault(prev.name), prev.rate, orDefault(next.name), next.rate, next.deviceChannels,
              layoutName(next.mixChannels), next.bufferFrames, next.latencyMs);
    return true;
}

void OutputDriver::enterNoDevice(const char* why) {
    const bool already = state() == OutputState::NoDevice;
    const f64 now = clock_.now();
    idleLast_ = now;
    nextRetry_ = now + cfg_.retrySeconds;
    owedFrames_ = 0.0;
    state_.store(OutputState::NoDevice, std::memory_order_release);
    if (!already)
        AVER_WARN("[Audio] no output device ({}): {}. Mixing silently at real time and retrying every {} s",
                  why, errorText(), cfg_.retrySeconds);
}

// No device: render what real time would have consumed into nothing, so game-side state (positions, fades, finishing voices) keeps
// moving, and try the open again when it is due.
void OutputDriver::idleStep() {
    const f64 now = clock_.now();
    const f64 rate = static_cast<f64>(mixer_.sampleRate());
    owedFrames_ = std::min(owedFrames_ + (now - idleLast_) * rate, rate * kIdleCatchUp);
    idleLast_ = now;
    const u32 ch = mixer_.channels() ? mixer_.channels() : 1;
    const u32 chunk = static_cast<u32>(idleBuf_.size() / ch);
    while (owedFrames_ >= 1.0) {
        const u32 n = std::min(static_cast<u32>(owedFrames_), chunk);
        mixer_.mix(idleBuf_.data(), n);
        owedFrames_ -= static_cast<f64>(n);
    }

    if (now >= nextRetry_) {
        nextRetry_ = now + cfg_.retrySeconds;
        OutputInfo next;
        if (backend_.open(next) && adopt(next, "device back")) return;
    }
    clock_.sleep(kIdleSleep);
}

} // namespace aver::audio
