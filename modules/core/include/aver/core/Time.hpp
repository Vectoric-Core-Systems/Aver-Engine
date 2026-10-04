#pragma once
// Timing: a monotonic clock and the per-frame timestep.
#include "Types.hpp"

namespace aver {

// Monotonic wall-clock timer built on std::chrono::steady_clock.
class Clock {
public:
    Clock();
    f64 restart();         // returns elapsed, then resets to now
private:
    u64 startNs_;
};

// Per-frame timing handed to Application::onUpdate.
struct Timestep {
    f32 dt = 0.0f;     // seconds since previous frame
    f32 total = 0.0f;  // seconds since engine start
    u64 frame = 0;     // frame counter (1-based once ticking)
};

} // namespace aver
