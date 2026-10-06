#include "aver/synapse/Steering.hpp"

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>

namespace aver::synapse {

V2 steerSeek(V2 pos, V2 target, f32 maxSpeed) {
    return norm2(target - pos) * maxSpeed;
}

V2 steerArrive(V2 pos, V2 target, f32 maxSpeed, f32 slowRadius, f32 stopRadius) {
    const V2 to = target - pos;
    const f32 d = len2(to);
    if (d <= stopRadius || d < 1e-6f) return {};
    f32 speed = maxSpeed;
    if (slowRadius > stopRadius && d < slowRadius)
        speed = maxSpeed * (d - stopRadius) / (slowRadius - stopRadius);
    return to * (speed / d);
}

V2 steerFlee(V2 pos, V2 threat, f32 maxSpeed, f32 panicRadius) {
    const V2 away = pos - threat;
    const f32 d = len2(away);
    f32 speed = maxSpeed;
    if (panicRadius > 0.0f) {
        if (d >= panicRadius) return {};
        speed = maxSpeed * (1.0f - d / panicRadius);
    }
    // Exactly on the threat: flee along +X so the result is defined and repeatable.
    return norm2(away, V2{1.0f, 0.0f}) * speed;
}

u32 steerRandNext(u32& s) {
    if (s == 0) s = 0x2545F491u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

V2 steerWander(V2 heading, WanderState& st, f32 maxSpeed, f32 circleDist, f32 circleRadius,
               f32 jitterRadPerSec, f32 dt) {
    const f32 r = hashToUnit(steerRandNext(st.rng)) * 2.0f - 1.0f;   // [-1, 1)
    st.angleRad += r * jitterRadPerSec * dt;
    st.angleRad = std::fmod(st.angleRad, kTwoPi);

    const V2 h = norm2(heading, V2{1.0f, 0.0f});
    // The circle's offset angle is measured from the heading, so the walk turns smoothly instead
    // of snapping to a world axis.
    const f32 a = std::atan2(h.y, h.x) + st.angleRad;
    const V2 point = h * circleDist + V2{std::cos(a), std::sin(a)} * circleRadius;
    return norm2(point, h) * maxSpeed;
}

V2 steerSeparation(V2 pos, const V2* others, u32 count, f32 radius, f32 maxSpeed) {
    if (radius <= 0.0f) return {};
    V2 push{};
    for (u32 i = 0; i < count; ++i) {
        const V2 away = pos - others[i];
        const f32 d = len2(away);
        if (d >= radius) continue;
        // Coincident neighbours get a repeatable direction from their index.
        const V2 dir = d > 1e-6f ? away * (1.0f / d)
                                 : V2{std::cos(static_cast<f32>(i)), std::sin(static_cast<f32>(i))};
        push += dir * (1.0f - d / radius);
    }
    return clampLen2(push * maxSpeed, maxSpeed);
}

V2 steerPathFollow(V2 pos, const V2* points, u32 count, u32& index, f32 reachRadius, f32 maxSpeed,
                   f32 slowRadius) {
    if (count == 0 || index >= count) return {};
    while (index + 1 < count && dist2(pos, points[index]) <= reachRadius) ++index;
    const V2 target = points[index];
    if (index + 1 == count) {
        if (dist2(pos, target) <= reachRadius) { index = count; return {}; }
        // Stop radius under the reach radius, so the approach crosses it with speed left.
        return steerArrive(pos, target, maxSpeed, slowRadius, reachRadius * 0.5f);
    }
    return steerSeek(pos, target, maxSpeed);
}

V2 blendSteering(const SteerTerm* terms, u32 count, f32 maxSpeed) {
    V2 sum{};
    for (u32 i = 0; i < count; ++i) sum += terms[i].desired * terms[i].weight;
    return clampLen2(sum, maxSpeed);
}

V2 accelerateToward(V2 vel, V2 desired, f32 maxAccel, f32 dt) {
    const V2 dv = desired - vel;
    return vel + clampLen2(dv, std::max(0.0f, maxAccel * dt));
}

} // namespace aver::synapse
