#pragma once
// Steering behaviours. Each returns a DESIRED VELOCITY (cm/s) for one agent; the caller blends
// them (blendSteering), limits acceleration (accelerateToward) and, for crowds, hands the result
// to the avoidance solver as the preferred velocity. Pure: no scene, no physics.
#include "aver/synapse/SteerMath.hpp"

namespace aver::synapse {

// Full speed at the target.
V2 steerSeek(V2 pos, V2 target, f32 maxSpeed);

// Seek that slows linearly inside slowRadius and stops inside stopRadius.
V2 steerArrive(V2 pos, V2 target, f32 maxSpeed, f32 slowRadius, f32 stopRadius);

// Away from threat at full speed inside panicRadius, tapering to zero at its edge. panicRadius <= 0
// means "always flee".
V2 steerFlee(V2 pos, V2 threat, f32 maxSpeed, f32 panicRadius);

struct WanderState {
    f32 angleRad = 0.0f;   // current offset on the wander circle
    u32 rng = 1;           // xorshift32 state; seed per agent for decorrelated walks
};

// Reynolds-style wander: a target point on a circle ahead of the agent, nudged by a bounded random
// turn each call. Deterministic for a given rng state.
V2 steerWander(V2 heading, WanderState& s, f32 maxSpeed, f32 circleDist, f32 circleRadius,
               f32 jitterRadPerSec, f32 dt);

// Push away from neighbours inside `radius`, stronger the closer they are.
V2 steerSeparation(V2 pos, const V2* others, u32 count, f32 radius, f32 maxSpeed);

// Walks a polyline. `index` is the current leg's end point and advances when within reachRadius.
// Slows into the LAST point (arrive), holds speed through the rest. Returns zero past the end.
V2 steerPathFollow(V2 pos, const V2* points, u32 count, u32& index, f32 reachRadius, f32 maxSpeed,
                   f32 slowRadius);

// One weighted contribution to a blend.
struct SteerTerm {
    V2  desired;
    f32 weight = 1.0f;
};

// Weighted sum clamped to maxSpeed. A zero-weight or all-zero blend returns zero.
V2 blendSteering(const SteerTerm* terms, u32 count, f32 maxSpeed);

// Moves `vel` toward `desired` by at most maxAccel*dt.
V2 accelerateToward(V2 vel, V2 desired, f32 maxAccel, f32 dt);

// Advances an xorshift32 state (a zero state is replaced with a fixed seed).
u32 steerRandNext(u32& state);

} // namespace aver::synapse
