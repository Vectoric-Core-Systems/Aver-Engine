#pragma once
// Stutter hunting: a HitchMarks logs, when its scope took longer than the threshold, how long each
// marked stretch of it took. The threshold is AVER_HITCH_MS (0 turns it off), 250 ms when unset, so a
// long hitch always explains itself in the log.
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

namespace aver {

inline f64 hitchThresholdMs() {
    static const f64 v = [] { const char* e = std::getenv("AVER_HITCH_MS"); return e ? std::atof(e) : 250.0; }();
    return v;
}
// AVER_HITCH_MS was set: also the periodic reports (present cadence) that would be noise by default.
inline bool hitchVerbose() {
    static const bool v = std::getenv("AVER_HITCH_MS") != nullptr;
    return v;
}

class HitchMarks {
public:
    using Clock = std::chrono::steady_clock;
    explicit HitchMarks(const char* where, f64 thresholdScale = 1.0)
        : where_(where), on_(hitchThresholdMs() > 0.0), limit_(hitchThresholdMs() * thresholdScale) {
        if (on_) start_ = last_ = Clock::now();
    }
    // Closes the stretch since the previous mark (or the start) under `name`.
    void mark(const char* name) {
        if (!on_ || n_ >= kMax) return;
        const Clock::time_point now = Clock::now();
        names_[n_] = name;
        ms_[n_++] = std::chrono::duration<f64, std::milli>(now - last_).count();
        last_ = now;
    }
    ~HitchMarks() {
        if (!on_) return;
        mark("rest");
        const f64 total = std::chrono::duration<f64, std::milli>(last_ - start_).count();
        if (total <= limit_) return;
        std::string parts;
        for (int i = 0; i < n_; ++i) {
            if (ms_[i] < 1.0) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %.1f", parts.empty() ? "" : " | ", names_[i], ms_[i]);
            parts += buf;
        }
        AVER_INFO("[HitchMarks] {} {:.1f} ms: {}", where_, total, parts);
    }

private:
    static constexpr int kMax = 48;
    const char* where_;
    bool on_;
    f64 limit_;
    Clock::time_point start_{}, last_{};
    const char* names_[kMax] = {};
    f64 ms_[kMax] = {};
    int n_ = 0;
};

} // namespace aver
