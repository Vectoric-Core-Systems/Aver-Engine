// Pure camera math: exact exponential follow, lag with sub-steps, angle follow, the yaw/pitch/roll basis, pose blends,
// look-ahead, the spring-arm resolve, fade hysteresis and FOV composition. Header-only, Aver.Core only, no clock and no
// allocation: each function is a pure function of its arguments (replay equality depends on it).
#pragma once

#include "aver/camera/CameraTypes.hpp"

#include <algorithm>
#include <cmath>

namespace aver::camera {

inline constexpr f32 kMinFovRad = 10.0f * (kPi / 180.0f), kMaxFovRad = 140.0f * (kPi / 180.0f);
inline constexpr f32 kLn2 = 0.69314718f;

inline f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Wraps an angle to (-pi, pi].
inline f32 wrapPi(f32 a) {
    a = std::fmod(a, kTwoPi);
    if (a > kPi) a -= kTwoPi;
    else if (a <= -kPi) a += kTwoPi;
    return a;
}

// Exact exponential follow: cur + (target - cur) * (1 - exp(-speed * dt)). speed <= 0 returns target.
inline f32 expFollow(f32 cur, f32 target, f32 speed, f32 dt) {
    if (speed <= 0.0f) return target;
    if (dt <= 0.0f) return cur;
    return cur + (target - cur) * (1.0f - std::exp(-speed * dt));
}

// Follow of a target that moves LINEARLY from t0 to t1 over h seconds, solved exactly (c' = s (T(t) - c)), so a pivot
// at constant velocity v settles to a lag of exactly v / s at any step size.
inline Vec3 rampFollow(Vec3 cur, Vec3 t0, Vec3 t1, f32 speed, f32 h) {
    const f32 x = speed * h;
    const f32 e = std::exp(-x);
    const f32 g = x < 1e-4f ? 1.0f - 0.5f * x : -std::expm1(-x) / x;     // (1 - e) / x without the cancellation
    return t1 + (cur - t0) * e - (t1 - t0) * g;
}

// UE-style lag with sub-stepping: the TARGET is interpolated from prevTarget to target across ceil(dt / maxStepS)
// (<= 8) steps, each step integrated exactly for a target in linear motion; then |target - result| is clamped to maxDist
// (0 = unlimited). speed <= 0 returns the target.
inline Vec3 lagFollow(Vec3 cur, Vec3 prevTarget, Vec3 target, f32 speed, f32 maxStepS, f32 maxDist, f32 dt) {
    Vec3 out = target;
    if (speed > 0.0f) {
        out = cur;
        if (dt > 0.0f) {
            const f32 step = maxStepS > 1e-4f ? maxStepS : kDefaultLagStepS;
            int n = static_cast<int>(std::ceil(dt / step));
            n = n < 1 ? 1 : (n > 8 ? 8 : n);
            const f32 h = dt / static_cast<f32>(n);
            for (int i = 0; i < n; ++i) {
                const Vec3 a = lerp(prevTarget, target, static_cast<f32>(i) / static_cast<f32>(n));
                const Vec3 b = (i + 1 == n) ? target : lerp(prevTarget, target, static_cast<f32>(i + 1) / static_cast<f32>(n));
                out = rampFollow(out, a, b, speed, h);
            }
        }
    }
    if (maxDist > 0.0f) {
        const Vec3 gap = target - out;
        const f32 len = gap.size();
        if (len > maxDist) out = target - gap * (maxDist / len);
    }
    return out;
}

// Shortest-path angle follow in radians (wraps at pi), optional max angular distance (0 = unlimited). The result is
// wrapped to (-pi, pi]. speed <= 0 returns the target.
inline f32 angleFollow(f32 cur, f32 target, f32 speed, f32 dt, f32 maxDist) {
    if (speed <= 0.0f) return target;
    const f32 diff = wrapPi(target - cur);
    f32 step = dt > 0.0f ? diff * (1.0f - std::exp(-speed * dt)) : 0.0f;
    f32 left = diff - step;
    if (maxDist > 0.0f && std::fabs(left) > maxDist) {
        left = left < 0.0f ? -maxDist : maxDist;
        step = diff - left;
    }
    return wrapPi(cur + step);
}

// ---- the basis -------------------------------------------------------------------------------------------------

struct Basis { Vec3 fwd, right, up; };

// fwd is game::cameraForward bit for bit; right = normalize(cross(worldUp, fwd)) = (-sin yaw, cos yaw, 0); roll turns
// right/up about fwd, positive tipping the camera's top toward its right (a clockwise head tilt).
inline Basis basisFromYawPitchRoll(f32 yaw, f32 pitch, f32 roll) {
    Basis b;
    b.fwd = Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const Vec3 r0{-std::sin(yaw), std::cos(yaw), 0.0f};
    const Vec3 u0 = cross(b.fwd, r0);
    if (roll == 0.0f) {
        b.right = r0;
        b.up = u0;
    } else {
        const f32 c = std::cos(roll), s = std::sin(roll);
        b.right = r0 * c - u0 * s;
        b.up = u0 * c + r0 * s;
    }
    return b;
}

inline void yawPitchRollFromBasis(const Basis& b, f32& yaw, f32& pitch, f32& roll) {
    yaw = std::atan2(b.fwd.y, b.fwd.x);
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, b.fwd.z)));
    const Vec3 r0{-std::sin(yaw), std::cos(yaw), 0.0f};
    const Vec3 u0 = cross(b.fwd, r0);
    roll = std::atan2(dot(b.up, r0), dot(b.up, u0));
}

// (fwd, right, up) is a proper rotation (fwd x right = up), so the standard matrix-to-quaternion applies. For blends only.
inline Quat quatFromBasis(const Basis& b) {
    // Matrix columns: fwd, right, up (the images of +X, +Y, +Z).
    const f32 m00 = b.fwd.x, m10 = b.fwd.y, m20 = b.fwd.z;
    const f32 m01 = b.right.x, m11 = b.right.y, m21 = b.right.z;
    const f32 m02 = b.up.x, m12 = b.up.y, m22 = b.up.z;
    const f32 tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0.0f) {
        const f32 s = std::sqrt(tr + 1.0f) * 2.0f;
        q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = Quat{0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = Quat{(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return q.normalized();
}

inline Basis basisFromQuat(const Quat& q) {
    return Basis{q.rotate(Vec3{1, 0, 0}), q.rotate(Vec3{0, 1, 0}), q.rotate(Vec3{0, 0, 1})};
}

// Blend curve on t in [0, 1]. Endpoints are exact and every curve is monotone. EaseInOut is smoothstep.
inline f32 blendAlpha(f32 t01, BlendCurve curve) {
    if (!(t01 > 0.0f)) return 0.0f;
    if (t01 >= 1.0f) return 1.0f;
    switch (curve) {
        case BlendCurve::Linear: return t01;
        case BlendCurve::EaseIn: return t01 * t01;
        case BlendCurve::EaseOut: return 1.0f - (1.0f - t01) * (1.0f - t01);
        default: return t01 * t01 * (3.0f - 2.0f * t01);
    }
}

// Lerps position, FOV and clip; slerps the rotation by the short path. alpha is clamped; 0 and 1 return the endpoints exactly.
inline CameraPov blendPov(const CameraPov& a, const CameraPov& b, f32 alpha) {
    if (!(alpha > 0.0f)) return a;
    if (alpha >= 1.0f) return b;
    CameraPov o;
    o.pos = lerp(a.pos, b.pos, alpha);
    o.fovYRad = a.fovYRad + (b.fovYRad - a.fovYRad) * alpha;
    o.nearCm = a.nearCm + (b.nearCm - a.nearCm) * alpha;
    o.farCm = a.farCm + (b.farCm - a.farCm) * alpha;
    const Quat qa = quatFromBasis(basisFromYawPitchRoll(a.yaw, a.pitch, a.roll));
    const Quat qb = quatFromBasis(basisFromYawPitchRoll(b.yaw, b.pitch, b.roll));
    yawPitchRollFromBasis(basisFromQuat(Quat::slerp(qa, qb, alpha).normalized()), o.yaw, o.pitch, o.roll);
    return o;
}

// ---- look-ahead ------------------------------------------------------------------------------------------------

struct LeadState { Vec3 lastPivot; Vec3 vel; Vec3 lead; bool valid = false; };

// Planar velocity estimate (half-life 0.1 s) and the clamped, smoothed lead. A displacement above teleportCm in one
// call resets the estimate and returns zero lead (and sets `teleported`). Vertical velocity is ignored.
inline Vec3 lookAhead(LeadState& s, Vec3 pivot, f32 aheadS, f32 maxCm, f32 dt, f32 teleportCm, bool& teleported) {
    teleported = false;
    if (!s.valid) {
        s = LeadState{};
        s.valid = true;
        s.lastPivot = pivot;
        return Vec3{};
    }
    const Vec3 disp = pivot - s.lastPivot;
    s.lastPivot = pivot;
    if (teleportCm > 0.0f && disp.size() > teleportCm) {
        teleported = true;
        s.vel = Vec3{};
        s.lead = Vec3{};
        return Vec3{};
    }
    if (dt <= 0.0f) return s.lead;
    const Vec3 inst{disp.x / dt, disp.y / dt, 0.0f};
    const f32 k = 1.0f - std::exp(-(kLn2 / 0.1f) * dt);
    s.vel = s.vel + (inst - s.vel) * k;
    Vec3 want = s.vel * aheadS;
    const f32 len = want.size();
    if (maxCm > 0.0f && len > maxCm) want = want * (maxCm / len);
    s.lead = s.lead + (want - s.lead) * (1.0f - std::exp(-(kLn2 / 0.12f) * dt));
    return s.lead;
}

// ---- arm, fade, fov --------------------------------------------------------------------------------------------

// Spring-arm resolve over one scalar length: hitDist is how far the probe travelled before touching (< 0 for a clear
// sweep), armWanted the unobstructed length. Inward is instant, outward recovers with the half-life; the result is
// never below minArm (when the arm can be that long) and never above armWanted.
inline f32 resolveArm(f32 armWanted, f32 hitDist, f32 prevArm, f32 minArm, f32 recoverHalfLifeS, f32 dt) {
    f32 allowed = armWanted;
    if (hitDist >= 0.0f && hitDist < allowed) allowed = hitDist;
    const f32 floorArm = minArm < armWanted ? minArm : armWanted;
    if (allowed < floorArm) allowed = floorArm;
    if (prevArm < 0.0f || allowed <= prevArm) return allowed;
    const f32 k = recoverHalfLifeS > 0.0f ? 1.0f - std::exp(-(kLn2 / recoverHalfLifeS) * (dt > 0.0f ? dt : 0.0f)) : 1.0f;
    const f32 next = prevArm + (allowed - prevArm) * k;
    return next > allowed ? allowed : next;
}

// Collision fade with hysteresis: hides below endCm, returns above endCm * 1.15. (startCm is carried for a dithered fade.)
inline bool fadeHide(bool wasHidden, f32 camToPivotCm, f32 startCm, f32 endCm) {
    (void)startCm;
    return wasHidden ? camToPivotCm < endCm * 1.15f : camToPivotCm < endCm;
}

// FOV: (base + speed kick + modifier add) * modifier scale, clamped to 10..140 degrees.
inline f32 composeFov(f32 baseRad, f32 kickRad, f32 modAddRad, f32 modScale) {
    const f32 v = (baseRad + kickRad + modAddRad) * modScale;
    return v < kMinFovRad ? kMinFovRad : (v > kMaxFovRad ? kMaxFovRad : v);
}

inline bool isFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool isFinite(const CameraPov& p) {
    return isFinite(p.pos) && std::isfinite(p.yaw) && std::isfinite(p.pitch) && std::isfinite(p.roll) &&
           std::isfinite(p.fovYRad) && std::isfinite(p.nearCm) && std::isfinite(p.farCm);
}

}  // namespace aver::camera
