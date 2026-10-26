// Trajectory prediction. See Trajectory.hpp.
//
// Math (derived here). With gamma = K / halfLife, a critically damped approach to vd from v0 with zero
// initial acceleration is v(t) = vd + A (1 + gamma t) e^{-gamma t}, A = v0 - vd, whose integral is
// p(t) = vd t + A (2 - (2 + gamma t) e^{-gamma t}) / gamma. Heading toward psi_d from psi_0 with measured
// rate omega_0 is psi(t) = psi_d + (A + B t) e^{-gamma t}, A = wrap(psi_0 - psi_d), B = omega_0 + gamma A.
#include "aver/anim/Trajectory.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {

namespace {

constexpr f32 kEstimateHalfLife = 0.05f;   // smoothing of the measured velocity and yaw rate, seconds

f32 wrapPi(f32 a) {
    while (a > kPi) a -= kTwoPi;
    while (a <= -kPi) a += kTwoPi;
    return a;
}

f32 gammaOf(f32 halfLife) { return kSpringHalfLifeK / std::max(halfLife, 1e-4f); }

void toHeading(f32 yaw, f32 dx, f32 dy, f32& hx, f32& hy) {
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    hx = c * dx + s * dy;
    hy = -s * dx + c * dy;
}

} // namespace

f32 springVelocity(f32 v0, f32 vd, f32 halfLife, f32 t) {
    const f32 g = gammaOf(halfLife);
    return vd + (v0 - vd) * (1.0f + g * t) * std::exp(-g * t);
}

f32 springDisplacement(f32 v0, f32 vd, f32 halfLife, f32 t) {
    const f32 g = gammaOf(halfLife);
    return vd * t + (v0 - vd) * (2.0f - (2.0f + g * t) * std::exp(-g * t)) / g;
}

f32 springAngle(f32 a0, f32 w0, f32 aTarget, f32 halfLife, f32 t) {
    const f32 g = gammaOf(halfLife);
    const f32 a = wrapPi(a0 - aTarget);
    const f32 b = w0 + g * a;
    return aTarget + (a + b * t) * std::exp(-g * t);
}

void TrajectoryPredictor::reset() {
    ring_.clear();
    time_ = 0.0f;
    vel_[0] = vel_[1] = 0.0f;
    omega_ = 0.0f;
    desired_[0] = desired_[1] = 0.0f;
    yawTarget_ = 0.0f;
    intentSet_ = false;
}

void TrajectoryPredictor::observe(const Vec3& pos, f32 yaw, f32 dt, f32 historySeconds) {
    yaw = wrapPi(yaw);
    if (ring_.empty()) {
        time_ = 0.0f;
        ring_.push_back({0.0f, pos, yaw});
        vel_[0] = vel_[1] = 0.0f;
        omega_ = 0.0f;
        return;
    }
    if (dt <= 1e-6f) {
        ring_.back().pos = pos;
        ring_.back().yaw = yaw;
        return;
    }
    const Sample last = ring_.back();
    time_ += dt;
    const f32 alpha = 1.0f - std::pow(0.5f, dt / kEstimateHalfLife);
    vel_[0] += ((pos.x - last.pos.x) / dt - vel_[0]) * alpha;
    vel_[1] += ((pos.y - last.pos.y) / dt - vel_[1]) * alpha;
    omega_ += (wrapPi(yaw - last.yaw) / dt - omega_) * alpha;
    ring_.push_back({time_, pos, yaw});
    const f32 keepFrom = time_ - std::max(historySeconds, dt);
    while (ring_.size() > 2 && ring_.front().t < keepFrom) ring_.pop_front();
}

void TrajectoryPredictor::setIntent(f32 vx, f32 vy, bool faceYaw, f32 yawTarget, f32 faceSpeedMin) {
    desired_[0] = vx;
    desired_[1] = vy;
    intentSet_ = true;
    if (faceYaw) yawTarget_ = wrapPi(yawTarget);
    else if (std::hypot(vx, vy) > faceSpeedMin) yawTarget_ = std::atan2(vy, vx);
    else yawTarget_ = yaw();
}

void TrajectoryPredictor::fill(const SpringParams& sp, const std::vector<f32>& futureTimes,
                               const std::vector<f32>& pastTimes, MotionQuery& out) const {
    out.valid = false;
    if (ring_.empty()) return;
    const f32 yaw0 = ring_.back().yaw;
    const Vec3 p0 = ring_.back().pos;

    toHeading(yaw0, vel_[0], vel_[1], out.rootVel[0], out.rootVel[1]);
    out.rootVel[2] = omega_;

    const f32 vdx = intentSet_ ? desired_[0] : 0.0f, vdy = intentSet_ ? desired_[1] : 0.0f;
    const f32 target = intentSet_ ? yawTarget_ : yaw0;
    for (usize i = 0; i < kMaxTrajectorySamples; ++i) {
        out.futurePos[i][0] = out.futurePos[i][1] = 0.0f;
        out.futureDir[i][0] = 1.0f;
        out.futureDir[i][1] = 0.0f;
        out.pastPos[i][0] = out.pastPos[i][1] = 0.0f;
    }
    for (usize i = 0; i < futureTimes.size() && i < kMaxTrajectorySamples; ++i) {
        const f32 tau = futureTimes[i];
        const f32 dx = springDisplacement(vel_[0], vdx, sp.velocityHalfLife, tau);
        const f32 dy = springDisplacement(vel_[1], vdy, sp.velocityHalfLife, tau);
        toHeading(yaw0, dx, dy, out.futurePos[i][0], out.futurePos[i][1]);
        const f32 psi = springAngle(yaw0, omega_, target, sp.turnHalfLife, tau);
        out.futureDir[i][0] = std::cos(psi - yaw0);
        out.futureDir[i][1] = std::sin(psi - yaw0);
    }
    for (usize i = 0; i < pastTimes.size() && i < kMaxTrajectorySamples; ++i) {
        const f32 at = time_ + pastTimes[i];
        Vec3 p = ring_.front().pos;
        if (at >= ring_.back().t) {
            p = ring_.back().pos;
        } else if (at > ring_.front().t) {
            for (usize k = ring_.size() - 1; k > 0; --k) {
                const Sample& b = ring_[k];
                const Sample& a = ring_[k - 1];
                if (at >= a.t) {
                    const f32 u = b.t > a.t ? (at - a.t) / (b.t - a.t) : 0.0f;
                    p = a.pos + (b.pos - a.pos) * u;
                    break;
                }
            }
        }
        toHeading(yaw0, p.x - p0.x, p.y - p0.y, out.pastPos[i][0], out.pastPos[i][1]);
    }
    out.valid = true;
}

void TrajectoryPredictor::predictedWorld(const SpringParams& sp, const std::vector<f32>& times, std::vector<Vec3>& pts,
                                         std::vector<f32>& yaws) const {
    pts.clear();
    yaws.clear();
    if (ring_.empty()) return;
    const Vec3 p0 = ring_.back().pos;
    const f32 yaw0 = ring_.back().yaw;
    const f32 vdx = intentSet_ ? desired_[0] : 0.0f, vdy = intentSet_ ? desired_[1] : 0.0f;
    const f32 target = intentSet_ ? yawTarget_ : yaw0;
    for (const f32 tau : times) {
        pts.push_back({p0.x + springDisplacement(vel_[0], vdx, sp.velocityHalfLife, tau),
                       p0.y + springDisplacement(vel_[1], vdy, sp.velocityHalfLife, tau), p0.z});
        yaws.push_back(springAngle(yaw0, omega_, target, sp.turnHalfLife, tau));
    }
}

void TrajectoryPredictor::rebase(const Vec3& delta) {
    for (Sample& s : ring_) s.pos -= delta;
}
void TrajectoryPredictor::historyWorld(std::vector<Vec3>& pts) const {
    pts.clear();
    for (const Sample& s : ring_) pts.push_back(s.pos);
}

} // namespace aver::anim
