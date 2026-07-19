#include "aver/core/Time.hpp"

#include <chrono>

namespace aver {

static u64 nowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Clock::Clock() : startNs_(nowNs()) {}

f64 Clock::seconds() const {
    return static_cast<f64>(nowNs() - startNs_) * 1e-9;
}

f64 Clock::restart() {
    const u64 t = nowNs();
    const f64 elapsed = static_cast<f64>(t - startNs_) * 1e-9;
    startNs_ = t;
    return elapsed;
}

} // namespace aver
