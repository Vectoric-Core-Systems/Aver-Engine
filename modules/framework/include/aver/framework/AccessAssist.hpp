#pragma once
// Input assist: per-action "toggle instead of hold" latches, the exempt set, and the global timing scales
// (shorter holds, longer tap and combo windows). Header-only; FrameworkAbi.cpp's actionEval calls filter() on the
// summed value and passes timing() to the trigger engine. Raw key reads (Input.IsKeyDown) are not assisted.
// docs/ACCESSIBILITY.md section 2.6.

#include "aver/core/Types.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace aver::fw::access {

struct AssistTiming {
    f32 hold = 1.0f;     // multiplies Hold and HoldRelease time (<= 1)
    f32 window = 1.0f;   // multiplies Tap max time and Combo window (>= 1)
};

class AccessAssist {
public:
    static constexpr f32 kActuation = 0.15f;   // the action layer's default actuation threshold

    void setTiming(f32 hold, f32 window) {
        timing_.hold = hold > 0 ? hold : 1.0f;
        timing_.window = window > 0 ? window : 1.0f;
    }
    const AssistTiming& timing() const { return timing_; }
    bool timingActive() const { return timing_.hold != 1.0f || timing_.window != 1.0f; }

    // The player's choice, kept even for an exempt action (so lifting the exemption restores it).
    void setToggle(const std::string& action, bool on) {
        if (on) toggled_.insert(action);
        else { toggled_.erase(action); latches_.erase(action); }
    }
    bool toggleChosen(const std::string& action) const { return toggled_.count(action) != 0; }
    void setAllowed(const std::string& action, bool allowed) {
        if (allowed) exempt_.erase(action);
        else { exempt_.insert(action); latches_.erase(action); }
    }
    bool allowed(const std::string& action) const { return exempt_.count(action) == 0; }
    // Whether the latch applies to this action right now.
    bool toggleOn(const std::string& action) const { return toggleChosen(action) && allowed(action); }

    // `v` is the action's summed, modified value. Digital actions with the toggle on come back as
    // (latched ? the last held value : 0); everything else is untouched. Call once per update per action.
    void filter(const std::string& action, bool digital, f32 v[3]) {
        if (!digital || toggled_.empty() || !toggleOn(action)) return;
        Latch& l = latches_[action];
        f32 mag = 0;
        for (int k = 0; k < 3; ++k) mag = std::fmax(mag, std::fabs(v[k]));
        const bool active = mag > kActuation;
        if (active && !l.wasActive) l.on = !l.on;
        if (active) for (int k = 0; k < 3; ++k) l.held[k] = v[k];
        l.wasActive = active;
        for (int k = 0; k < 3; ++k) v[k] = l.on ? l.held[k] : 0.0f;
    }

    // Clears every latch (play start, context change); the choices and exemptions stay.
    void resetLatches() { latches_.clear(); }
    // The latch state of one action, for tests and UI.
    bool latched(const std::string& action) const {
        const auto it = latches_.find(action);
        return it != latches_.end() && it->second.on;
    }
    void clearAll() { toggled_.clear(); exempt_.clear(); latches_.clear(); timing_ = AssistTiming{}; }

private:
    struct Latch { bool wasActive = false, on = false; f32 held[3] = {1.0f, 0.0f, 0.0f}; };
    AssistTiming timing_;
    std::unordered_set<std::string> toggled_, exempt_;
    std::unordered_map<std::string, Latch> latches_;
};

// The one process-wide instance (inside Aver.Framework).
inline AccessAssist& accessAssist() {
    static AccessAssist a;
    return a;
}

} // namespace aver::fw::access
