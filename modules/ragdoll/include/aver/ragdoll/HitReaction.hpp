// HitReaction: the envelope of a physical reaction to a hit. Pure arithmetic on a ResolvedPhysAnim -- no world,
// no physics -- so the envelope math, the spread along the skeleton, stacking and the return to rest are decided
// by a unit test and a hit reacts the same way on every machine (fixed float order, no random).
//
// A hit on a bone gives that bone a reaction of size |impulse| / fullImpulse (capped at 1) and its neighbours
// along the skeleton a share that falls by `falloffPerHop` per link, up to `maxHops`. Each bone's reaction
// follows its region's attack / hold / decay. A second hit on a bone that is still reacting continues from
// where its envelope is now and caps at 1 (hits stack, they do not restart from zero or exceed full).
// The caller reads, per bone, the blend weight and the motor strength to apply, and when nothing is left.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/ragdoll/PhysAnimProfile.hpp"

#include <vector>

namespace aver::ragdoll {

class HitReaction {
public:
    void configure(const ResolvedPhysAnim& r);
    // Drops every envelope.
    void reset();

    // A hit on `bone`. Returns the impulse to add to that bone's part (the hit's, scaled by its region), or
    // zero when the bone is unknown. `size` overrides the reaction size (0..1) when >= 0.
    Vec3 trigger(i32 bone, const Vec3& impulse, f32 size = -1.0f);
    // A radial event: every bone gets `size` reduced by its distance, which the caller measured (cm).
    void triggerRadial(i32 bone, f32 size);

    void step(f32 dt);

    // 0..1 now.
    f32 envelope(i32 bone) const;
    // The simulated pose's weight for the bone: envelope * its region's blend.
    f32 blend(i32 bone) const;
    // Motor strength for the bone: its region's strength plus the boost at the current envelope, capped at 1.
    f32 strength(i32 bone) const;
    // True from a hit until its envelope has run out (so also during the attack, when the value is still 0).
    bool reacting(i32 bone) const;
    // True while any bone is reacting.
    bool active() const;
    u32  activeBones() const;
    u32  boneCount() const { return static_cast<u32>(env_.size()); }

private:
    struct Env {
        f32 peak = 0.0f;
        f32 age = 0.0f;       // seconds since this envelope started its attack
    };
    f32 valueOf(i32 bone, const Env& e) const;
    void add(i32 bone, f32 amount);

    ResolvedPhysAnim r_;
    std::vector<Env> env_;
    std::vector<f32> value_;
    std::vector<std::pair<i32, i32>> scratch_;
};

} // namespace aver::ragdoll
