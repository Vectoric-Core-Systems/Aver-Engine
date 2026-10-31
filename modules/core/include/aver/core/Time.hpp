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

// The arithmetic of a frame rate cap, apart from the waiting so it can be checked without a clock.
// `elapsed` is the time the frame has taken so far; the answer is how much longer to wait so that it
// lasts 1 / fps seconds, or 0 for no cap (fps <= 0, NaN) and for a frame that is already that long.
inline f64 frameCapWaitSeconds(f64 elapsed, f64 fps) {
    if (!(fps > 0.0)) return 0.0;
    const f64 target = 1.0 / fps;
    return elapsed < target ? target - elapsed : 0.0;
}

// Per-frame timing handed to Application::onUpdate.
struct Timestep {
    f32 dt = 0.0f;     // seconds since previous frame
    f32 total = 0.0f;  // seconds since engine start
    u64 frame = 0;     // frame counter (1-based once ticking)
};

} // namespace aver
