// RagdollLife: see RagdollLife.hpp.
#include "aver/ragdoll/RagdollLife.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ragdoll {

Facing classifyFacing(const Vec3& up, const Vec3& forward, const Vec3& worldUp) {
    constexpr f32 kUprightCos = 0.7071f;   // 45 degrees
    constexpr f32 kSideSin = 0.342f;       // 20 degrees
    if (dot(up, worldUp) >= kUprightCos) return Facing::Upright;
    const f32 f = dot(forward, worldUp);
    if (std::fabs(f) < kSideSin) return Facing::Side;
    return f > 0.0f ? Facing::Supine : Facing::Prone;
}

u64 chooseGetUp(const ResolvedPhysAnim& p, Facing f) {
    switch (f) {
        case Facing::Supine: return p.getUpSupine;
        case Facing::Prone:  return p.getUpProne;
        case Facing::Side:   return p.getUpSide ? p.getUpSide : p.getUpProne;
        default:             return 0;
    }
}

bool LifeMachine::die(f32 seconds) {
    if (state_ != Life::Alive && state_ != Life::GettingUp) return false;
    seconds_ = std::max(seconds, 0.0f);
    t_ = 0.0f;
    state_ = seconds_ > 0.0f ? Life::Dying : Life::Dead;
    return true;
}

bool LifeMachine::getUp() {
    if (state_ != Life::Dead) return false;
    state_ = Life::GettingUp;
    return true;
}

bool LifeMachine::finishGetUp() {
    if (state_ != Life::GettingUp) return false;
    state_ = Life::Alive;
    return true;
}

void LifeMachine::step(f32 dt) {
    if (state_ != Life::Dying) return;
    t_ += dt;
    if (t_ >= seconds_) state_ = Life::Dead;
}

f32 LifeMachine::strengthScale() const {
    switch (state_) {
        case Life::Alive: return 1.0f;
        case Life::Dying: return seconds_ > 0.0f ? std::max(0.0f, 1.0f - t_ / seconds_) : 0.0f;
        default:          return 0.0f;
    }
}

} // namespace aver::ragdoll
