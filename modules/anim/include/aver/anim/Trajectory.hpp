// Trajectory prediction for motion matching: what the character is about to do (a critically damped
// spring toward the desired velocity and facing) and what it just did (a short history ring of the
// measured motion), expressed in the heading frame the pose database was baked in.
//
// Closed forms, derived for this engine: nothing here integrates, so the prediction is exact at any
// horizon and the same for any frame rate. Pure arithmetic over Core types.
#pragma once

#include "aver/anim/MotionAsset.hpp"
#include "aver/core/Math.hpp"

#include <deque>
#include <vector>

namespace aver::anim {

// Everything the matcher needs from the world, in the heading frame at "now".
struct MotionQuery {
    bool valid = false;
    f32 futurePos[kMaxTrajectorySamples][2] = {};
    f32 futureDir[kMaxTrajectorySamples][2] = {};
    f32 pastPos[kMaxTrajectorySamples][2] = {};
    f32 rootVel[3] = {};                   // vx, vy in cm/s (heading frame), yaw rate in rad/s
    u32 require = 0, exclude = 0;          // tag masks
};

struct SpringParams { f32 velocityHalfLife = 0.2f, turnHalfLife = 0.15f; };

// Root of (1 + z) e^{-z} = 0.5: with gamma = K / halfLife the spring's step response is half done at
// exactly halfLife seconds.
inline constexpr f32 kSpringHalfLifeK = 1.67835f;

// Velocity at t when starting at v0 (initial acceleration 0) toward vd.
f32 springVelocity(f32 v0, f32 vd, f32 halfLife, f32 t);
// The integral of springVelocity from 0 to t.
f32 springDisplacement(f32 v0, f32 vd, f32 halfLife, f32 t);
// Heading at t toward aTarget by the shortest arc, starting at a0 with angular velocity w0.
f32 springAngle(f32 a0, f32 w0, f32 aTarget, f32 halfLife, f32 t);

class TrajectoryPredictor {
public:
    void reset();
    // Once per tick: the entity's world position and heading. Keeps a ring of the last historySeconds and a
    // smoothed velocity / yaw-rate estimate.
    void observe(const Vec3& worldPos, f32 yaw, f32 dt, f32 historySeconds);
    // The desired world-space planar velocity (cm/s) and facing. With faceYaw false the character faces its
    // velocity when that is above faceSpeedMin and otherwise keeps its heading.
    void setIntent(f32 vx, f32 vy, bool faceYaw, f32 yawTarget, f32 faceSpeedMin);
    // Fills futurePos / futureDir at `futureTimes`, pastPos at `pastTimes` and rootVel (the measured motion
    // in the heading frame). Leaves out.valid false until something was observed.
    void fill(const SpringParams&, const std::vector<f32>& futureTimes, const std::vector<f32>& pastTimes,
              MotionQuery& out) const;
    // World-space polyline of the prediction, for the debug view.
    void predictedWorld(const SpringParams&, const std::vector<f32>& times, std::vector<Vec3>& pts,
                        std::vector<f32>& yaws) const;
    void historyWorld(std::vector<Vec3>& pts) const;
    // Floating-origin shift (LARGE_WORLD 2.2.5): every stored sample position -= delta. Velocity, yaw rate and the intent are
    // invariant. Without it the first observe() after a shift reads a 16 km jump as a velocity.
    void rebase(const Vec3& delta);

    bool observed() const { return !ring_.empty(); }
    Vec3 velocity() const { return {vel_[0], vel_[1], 0.0f}; }
    f32 yawRate() const { return omega_; }
    Vec3 position() const { return ring_.empty() ? Vec3{} : ring_.back().pos; }
    f32 yaw() const { return ring_.empty() ? 0.0f : ring_.back().yaw; }
    Vec3 desiredVelocity() const { return {desired_[0], desired_[1], 0.0f}; }

private:
    struct Sample { f32 t; Vec3 pos; f32 yaw; };
    std::deque<Sample> ring_;
    f32 time_ = 0.0f;
    f32 vel_[2] = {0, 0};
    f32 omega_ = 0.0f;
    f32 desired_[2] = {0, 0};
    f32 yawTarget_ = 0.0f;
    bool intentSet_ = false;
};

} // namespace aver::anim
