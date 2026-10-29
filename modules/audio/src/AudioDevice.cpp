// AudioDevice's platform-independent half: the render thread around an OutputDriver. start() lives in the backend module.
#include "aver/audio/AudioDevice.hpp"

#include <chrono>
#include <future>

namespace aver::audio {

// Stops the render thread and tears the mixer down.
AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::startWith(std::unique_ptr<OutputBackend> backend, std::unique_ptr<OutputClock> clock, const OutputConfig& config) {
    if (running_.load(std::memory_order_acquire)) return true;
    if (!backend) return false;
    stop();     // a previous run's thread, if start() failed after spawning it

    backend_ = std::move(backend);
    clock_ = clock ? std::move(clock) : std::make_unique<SteadyClock>();
    driver_ = std::make_unique<OutputDriver>(mixer_, *backend_, *clock_, config);
    quit_.store(false, std::memory_order_release);

    // The device is opened, used and closed on this one thread (COM apartments, SDL's thread rules), so the thread reports whether it
    // started. The promise is shared so the thread may finish with it after this frame is gone.
    auto started = std::make_shared<std::promise<bool>>();
    std::future<bool> result = started->get_future();
    thread_ = std::thread([this, started] {
        backend_->threadBegin();
        const bool ok = driver_->begin();
        if (ok) running_.store(true, std::memory_order_release);
        started->set_value(ok);
        if (ok) {
            while (!quit_.load(std::memory_order_acquire)) driver_->step();
            running_.store(false, std::memory_order_release);
            driver_->end();
        }
        backend_->threadEnd();
    });

    // THE THREAD IS JOINED BEFORE ANYTHING IT USES IS RELEASED: on the timeout path the old code closed the start handle while the thread
    // could still signal it. Here the future is the handshake and a slow start is simply waited for after quit_ is raised.
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready || !result.get()) {
        quit_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

// Joins the render thread, then shuts the mixer down. Safe when never started.
void AudioDevice::stop() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    mixer_.shutdown();
}

} // namespace aver::audio
