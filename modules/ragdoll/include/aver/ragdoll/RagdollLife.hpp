// RagdollLife: death and recovery of a physically animated character, as pure logic.
//
//   Alive --die--> Dying (motor strength ramps to zero) --> Dead (limp) --getUp--> GettingUp --> Alive
//
// and the rule that picks which get-up montage to play from how the body lies. No world, no physics.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/ragdoll/PhysAnimProfile.hpp"

namespace aver::ragdoll {

enum class Life : i32 { Alive = 0, Dying = 1, Dead = 2, GettingUp = 3 };

// How the body lies, from where the character's own up and forward axes point NOW (world space, unit):
// the rest pose's axes carried by the rotation the root part has undergone since the ragdoll was built.
enum class Facing : i32 { Upright = 0, Supine = 1, Prone = 2, Side = 3 };

// Upright: up is within ~45 degrees of world up. Otherwise lying: face up (forward points up) is Supine,
// face down is Prone, and a body on its side (forward within ~20 degrees of horizontal) is Side.
Facing classifyFacing(const Vec3& up, const Vec3& forward, const Vec3& worldUp = Vec3{0, 0, 1});

// The montage id the profile names for the way the body lies; Upright gives 0 (nothing to get up from),
// and so does a lying facing the profile names no montage for, except that Side falls back to Prone.
u64 chooseGetUp(const ResolvedPhysAnim& profile, Facing f);

class LifeMachine {
public:
    Life state() const { return state_; }
    // Starts dying over `seconds` (0 = at once). Only from Alive or GettingUp; false otherwise.
    bool die(f32 seconds);
    // Dead -> GettingUp. False in any other state.
    bool getUp();
    // The get-up finished: -> Alive.
    bool finishGetUp();
    // Back to Alive from anywhere (a respawn).
    void revive() { state_ = Life::Alive; t_ = 0.0f; }

    void step(f32 dt);
    // 1 when Alive, ramping 1 -> 0 while Dying, 0 when Dead or GettingUp.
    f32 strengthScale() const;

private:
    Life state_ = Life::Alive;
    f32 t_ = 0.0f, seconds_ = 0.0f;
};

} // namespace aver::ragdoll
