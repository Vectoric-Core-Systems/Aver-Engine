// SoftBodyAero.cpp -- the wind field and the per-triangle drag and lift. See SoftBodyAero.hpp.
//
// DETERMINISM: the wind is a pure function of (world position, fixed step index, dt). Turbulence is an
// integer-hash noise of the 100 cm cell, blended between 8-step blocks; there is no wall clock and no
// rand, so the same run twice is bit identical (tests/physics SoftBodyBuilderTest pins it).
#include "SoftBodyAero.hpp"
#include "Convert.hpp"

#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <cmath>

using namespace aver;
using namespace aver::physics;

namespace {

uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint32_t hash4(int32_t a, int32_t b, int32_t c, uint32_t d) {
    uint32_t h = mix(static_cast<uint32_t>(a) ^ 0x9e3779b9U);
    h = mix(h ^ (static_cast<uint32_t>(b) * 0x85ebca6bU));
    h = mix(h ^ (static_cast<uint32_t>(c) * 0xc2b2ae35U));
    h = mix(h ^ (d * 0x27d4eb2fU));
    return h;
}

// [0, 1)
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

} // namespace

namespace aver::physics::softbody {

Vec3 windAt(const WindParams& w, const Vec3& p, uint64_t step, float dt) {
    const int32_t cx = static_cast<int32_t>(std::floor(p.x / 100.0f));
    const int32_t cy = static_cast<int32_t>(std::floor(p.y / 100.0f));
    const int32_t cz = static_cast<int32_t>(std::floor(p.z / 100.0f));
    const Vec3 dir = w.base.sizeSquared() > 0.0f ? w.base.getSafeNormal() : Vec3{1, 0, 0};

    Vec3 out = w.base;
    if (w.gustSpeed != 0.0f) {
        const float phase = unit(hash4(cx, cy, cz, 0x51edU)) * kTwoPi;
        const float t = static_cast<float>(step) * dt;
        out += dir * (w.gustSpeed * std::sin(kTwoPi * w.gustHz * t + phase));
    }
    if (w.turbulence != 0.0f) {
        const uint32_t block = static_cast<uint32_t>(step >> 3);
        const float frac = static_cast<float>(step & 7u) * 0.125f;
        float n[3];
        for (uint32_t k = 0; k < 3; ++k) {
            const float a = unit(hash4(cx, cy, cz, block * 3U + k)) * 2.0f - 1.0f;
            const float b = unit(hash4(cx, cy, cz, (block + 1U) * 3U + k)) * 2.0f - 1.0f;
            n[k] = a + (b - a) * frac;
        }
        out += Vec3{n[0], n[1], n[2]} * w.turbulence;
    }
    return out;
}

void applyAero(JPH::SoftBodyMotionProperties& mp, const JPH::RMat44& com, float drag, float lift,
               const WindParams& wind, uint64_t step, float dt) {
    auto& verts = mp.GetVertices();
    for (const JPH::SoftBodySharedSettings::Face& f : mp.GetFaces()) {
        auto& v0 = verts[f.mVertex[0]];
        auto& v1 = verts[f.mVertex[1]];
        auto& v2 = verts[f.mVertex[2]];
        const JPH::Vec3 cross = (v1.mPosition - v0.mPosition).Cross(v2.mPosition - v0.mPosition);
        const float area2 = cross.Length();
        if (area2 < 1.0e-9f) continue;
        const JPH::Vec3 n = cross / area2;
        const float area = 0.5f * area2;

        const JPH::Vec3 centroid = (v0.mPosition + v1.mPosition + v2.mPosition) * (1.0f / 3.0f);
        const JPH::Vec3 vm = (v0.mVelocity + v1.mVelocity + v2.mVelocity) * (1.0f / 3.0f);
        const JPH::Vec3 vWind = toJoltDir(windAt(wind, fromJolt(JPH::Vec3(com * centroid)), step, dt));
        const JPH::Vec3 vrel = vWind - vm;
        const float vn = vrel.Dot(n);
        const float speed = vrel.Length();
        const JPH::Vec3 force = (0.5f * 1.2f * area) * (n * (drag * std::fabs(vn) * vn) + (vrel - n * vn) * (lift * speed));
        const JPH::Vec3 dv = force * (dt / 3.0f);
        if (v0.mInvMass > 0.0f) v0.mVelocity += dv * v0.mInvMass;
        if (v1.mInvMass > 0.0f) v1.mVelocity += dv * v1.mInvMass;
        if (v2.mInvMass > 0.0f) v2.mVelocity += dv * v2.mInvMass;
    }
}

} // namespace aver::physics::softbody
