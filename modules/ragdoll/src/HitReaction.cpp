// HitReaction: see HitReaction.hpp.
#include "aver/ragdoll/HitReaction.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ragdoll {

void HitReaction::configure(const ResolvedPhysAnim& r) {
    r_ = r;
    env_.assign(r.boneCount, Env{});
    value_.assign(r.boneCount, 0.0f);
}

void HitReaction::reset() {
    std::fill(env_.begin(), env_.end(), Env{});
    std::fill(value_.begin(), value_.end(), 0.0f);
}

f32 HitReaction::valueOf(i32 bone, const Env& e) const {
    if (e.peak <= 0.0f) return 0.0f;
    const PhysAnimDrive& d = r_.drive[static_cast<usize>(bone)];
    const f32 t = e.age;
    if (t < d.attack) return e.peak * (t / d.attack);
    if (t < d.attack + d.hold) return e.peak;
    if (d.decay <= 0.0f) return 0.0f;
    const f32 k = (t - d.attack - d.hold) / d.decay;
    return k >= 1.0f ? 0.0f : e.peak * (1.0f - k);
}

void HitReaction::add(i32 bone, f32 amount) {
    if (amount <= 0.0f) return;
    Env& e = env_[static_cast<usize>(bone)];
    const f32 cur = value_[static_cast<usize>(bone)];
    const f32 attack = r_.drive[static_cast<usize>(bone)].attack;
    // Still rising: stack on the peak it is heading for, so hits in the same frame add up too.
    const f32 base = e.age < attack ? std::max(cur, e.peak) : cur;
    const f32 np = std::min(1.0f, base + amount);
    // Continue from where the envelope is now: the new attack starts at `cur`, not at zero.
    e.age = (attack > 0.0f) ? attack * (cur / np) : 0.0f;
    e.peak = np;
    value_[static_cast<usize>(bone)] = valueOf(bone, e);
}

Vec3 HitReaction::trigger(i32 bone, const Vec3& impulse, f32 size) {
    if (bone < 0 || bone >= static_cast<i32>(env_.size())) return Vec3{0, 0, 0};
    const f32 mag = size >= 0.0f ? std::min(size, 1.0f)
                                 : std::min(1.0f, impulse.size() / std::max(r_.fullImpulse, 1e-6f));
    r_.withinHops(bone, r_.maxHops, scratch_);
    for (const auto& [b, hops] : scratch_)
        add(b, mag * std::pow(r_.falloffPerHop, static_cast<f32>(hops)));
    return impulse * r_.drive[static_cast<usize>(bone)].impulse;
}

void HitReaction::triggerRadial(i32 bone, f32 size) {
    if (bone < 0 || bone >= static_cast<i32>(env_.size())) return;
    add(bone, std::min(std::max(size, 0.0f), 1.0f));
}

void HitReaction::step(f32 dt) {
    for (usize i = 0; i < env_.size(); ++i) {
        Env& e = env_[i];
        if (e.peak <= 0.0f) continue;
        e.age += dt;
        const PhysAnimDrive& d = r_.drive[i];
        f32 v = valueOf(static_cast<i32>(i), e);
        // Over: past the end of the decay, or past the hold and too small for anything to show it.
        if (e.age >= d.attack + d.hold + d.decay || (e.age >= d.attack + d.hold && v < r_.epsilon * 0.5f)) {
            e = Env{};
            v = 0.0f;
        }
        value_[i] = v;
    }
}

f32 HitReaction::envelope(i32 bone) const {
    return bone < 0 || bone >= static_cast<i32>(value_.size()) ? 0.0f : value_[static_cast<usize>(bone)];
}

f32 HitReaction::blend(i32 bone) const {
    return bone < 0 || bone >= static_cast<i32>(value_.size())
               ? 0.0f
               : value_[static_cast<usize>(bone)] * r_.drive[static_cast<usize>(bone)].blend;
}

f32 HitReaction::strength(i32 bone) const {
    if (bone < 0 || bone >= static_cast<i32>(value_.size())) return 0.0f;
    const PhysAnimDrive& d = r_.drive[static_cast<usize>(bone)];
    return std::min(1.0f, d.strength + d.boost * value_[static_cast<usize>(bone)]);
}

bool HitReaction::reacting(i32 bone) const {
    return bone >= 0 && bone < static_cast<i32>(env_.size()) && env_[static_cast<usize>(bone)].peak > 0.0f;
}

bool HitReaction::active() const { return activeBones() > 0; }

u32 HitReaction::activeBones() const {
    u32 n = 0;
    for (usize i = 0; i < value_.size(); ++i)
        if (env_[i].peak > 0.0f) ++n;
    return n;
}

} // namespace aver::ragdoll
