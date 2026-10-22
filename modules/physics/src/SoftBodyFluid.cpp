// SoftBodyFluid.cpp -- buoyancy for builder soft bodies (physics_softbody_ext_abi.h, aver_phys_sbx_set_buoyancy).
//
// WHY THIS IS OURS. Jolt has no buoyancy for soft bodies: Body::ApplyBuoyancyImpulse asserts IsRigidBody() and
// SoftBodyShape::GetSubmergedVolume is a stub (SoftBodyShape.cpp), and Buoyancy.cpp skips every non-rigid body for
// the same reason. So the pass runs here, in the fixed step before the solver (softbody::onStepBegin), as velocity
// changes on the vertices: frame-rate independent because dt is the fixed step.
//
// THE FORMULAS (textbook hydrostatics and quadratic drag; implemented from this text). Per face, in Jolt space with
// g = |gravity|, `up` the water surface normal and `depth` the distance of the face centroid below the surface:
//     n = normalize((p1 - p0) x (p2 - p0))   outward for a closed mesh wound as Jolt's pressure wants
//     h = sqrt(area),  s = clamp(depth / h + 0.5, 0, 1),  d = max(depth, 0)
//     closed meshes:  F_p = -n * (rho g d) * area * s        summed over a fully submerged shell this is rho g V up
//     every mesh:     F_d = 0.5 rho drag area s |vn| vn n,   vn = dot(fluidVelocity - mean(face velocity), n)
//     dv_i = (F / 3) * invMass_i * dt on each of the face's three vertices, kinematic vertices skipped.
// An open mesh gets drag only: the pressures on the two sides of a thin sheet cancel, and applying one side would
// push a cloth sideways.
#include "Buoyancy.hpp"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "SoftBodyInternal.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

struct Water {
    JPH::Vec3 point = JPH::Vec3::sZero();                 // a point on the surface
    JPH::Vec3 normal = JPH::Vec3::sAxisY();               // unit, pointing out of the water
    JPH::Vec3 current = JPH::Vec3::sZero();               // m/s
};

// The water at a Jolt world position: the body's own plane, else the host sampler (answers per position, and may say
// "no water here" or "no opinion"), else the global plane. The sampler comes before the plane exactly as it does for
// rigid bodies in Buoyancy.cpp.
bool waterAt(const softbody::SoftExt& e, JPH::Vec3Arg pos, Water& out) {
    if ((e.buoyFlags & AVER_PHYS_SBX_BUOY_OWN_PLANE) != 0 && e.hasOwnPlane) {
        out.point = toJolt(e.planePointCm);
        out.normal = toJoltUnit(e.planeNormal).NormalizedOr(JPH::Vec3::sAxisY());
        out.current = toJoltDir(e.planeCurrentCmS);
        return true;
    }
    if ((e.buoyFlags & AVER_PHYS_SBX_BUOY_WORLD_WATER) == 0) return false;
    if (const aver_phys_water_sampler_fn fn = phys::water::waterSampler().fn.load()) {
        const aver::Vec3 c = fromJolt(pos);
        AverPhysWaterSample s{};
        const int32_t r = fn(c.x, c.y, c.z, &s, phys::water::waterSampler().user.load());
        if (r == 0) return false;
        if (r == 1) {
            if (s.buoyancy <= 0.0f) return false;
            out.point = toJolt(aver::Vec3(c.x, c.y, s.surfaceZCm));
            out.normal = toJoltUnit(aver::Vec3(s.normal[0], s.normal[1], s.normal[2])).NormalizedOr(JPH::Vec3::sAxisY());
            out.current = toJoltDir(aver::Vec3(s.currentCmS[0], s.currentCmS[1], s.currentCmS[2]));
            return true;
        }
        // -1: no opinion, the plane below
    }
    const phys::water::WaterVolumeTable& volumes = phys::water::waterVolumes();
    if (volumes.hasPlane() && volumes.plane().buoyancy > 0.0f) {
        const phys::water::WaterVolume& p = volumes.plane();
        out.point = JPH::Vec3(p.surfacePosition);
        out.normal = p.surfaceNormal;
        out.current = p.fluidVelocity;
        return true;
    }
    return false;
}

// A sleeping body costs nothing until the water reaches its bounds: one sample at the box centre, then the lowest
// corner against the surface.
bool boundsReachWater(const softbody::SoftExt& e, const JPH::Body& body) {
    const JPH::AABox box = body.GetWorldSpaceBounds();
    Water w;
    if (!waterAt(e, box.GetCenter(), w)) return false;
    JPH::Vec3 low;
    for (int k = 0; k < 3; ++k) low.SetComponent(k, w.normal[k] > 0.0f ? box.mMin[k] : box.mMax[k]);
    return (low - w.point).Dot(w.normal) < 0.0f;
}

} // namespace

namespace aver::physics::softbody {

void sbxFluidStep(SoftRecord& rec, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, float dt) {
    SoftExt& e = rec.ext;
    e.submergedFraction = 0.0f;
    e.submergedVolumeCm3 = 0.0f;
    if (!(dt > 0.0f) || !(e.fluidDensity > 0.0f)) return;
    sbxEnsureAnalysed(rec);
    if (!body.IsActive() && !boundsReachWater(e, body)) return;

    const float rho = e.fluidDensity, drag = e.fluidDrag;
    const float g = g_world->system.GetGravity().Length();
    const JPH::RMat44 com = body.GetCenterOfMassTransform();
    auto& verts = mp.GetVertices();
    const size_t n = verts.size();

    // World positions and velocities once, and the world-space velocity change per vertex.
    static std::vector<JPH::Vec3> wp, wv, dv;
    wp.resize(n); wv.resize(n); dv.assign(n, JPH::Vec3::sZero());
    for (size_t i = 0; i < n; ++i) {
        wp[i] = JPH::Vec3(com * verts[i].mPosition);
        wv[i] = com.Multiply3x3(verts[i].mVelocity);
    }

    const bool closed = e.closed && e.restVolumeM3 > 0.0f;
    double totalArea = 0.0, subArea = 0.0, volume = 0.0;
    for (const JPH::SoftBodySharedSettings::Face& f : mp.GetFaces()) {
        const uint32_t i0 = f.mVertex[0], i1 = f.mVertex[1], i2 = f.mVertex[2];
        const JPH::Vec3 cross = (wp[i1] - wp[i0]).Cross(wp[i2] - wp[i0]);
        const float area2 = cross.Length();
        if (area2 < 1.0e-9f) continue;
        const JPH::Vec3 nrm = cross / area2;
        const float area = 0.5f * area2;
        totalArea += static_cast<double>(area);

        const JPH::Vec3 centroid = (wp[i0] + wp[i1] + wp[i2]) * (1.0f / 3.0f);
        Water w;
        if (!waterAt(e, centroid, w)) continue;
        const float depth = (w.point - centroid).Dot(w.normal);
        const float h = std::sqrt(area);
        const float s = std::clamp(depth / h + 0.5f, 0.0f, 1.0f);
        if (s <= 0.0f) continue;
        subArea += static_cast<double>(area * s);

        JPH::Vec3 force = JPH::Vec3::sZero();
        if (closed) {
            const float d = std::max(depth, 0.0f);
            force -= nrm * (rho * g * d * area * s);
            volume -= static_cast<double>(w.normal.Dot(nrm) * area * d * s);
        }
        const JPH::Vec3 vrel = w.current - (wv[i0] + wv[i1] + wv[i2]) * (1.0f / 3.0f);
        const float vn = vrel.Dot(nrm);
        float fd = 0.5f * rho * drag * area * s * std::fabs(vn) * vn;
        // Quadratic drag is explicit: on a light face it would overshoot the relative speed and feed on itself. Cap it
        // so one step closes at most 0.3 of the gap (the face shares its vertices with about as many neighbours).
        const float wsum = (verts[i0].mInvMass + verts[i1].mInvMass + verts[i2].mInvMass);
        if (wsum > 0.0f) {
            const float cap = 0.3f * 9.0f * std::fabs(vn) / (dt * wsum);
            fd = std::clamp(fd, -cap, cap);
        }
        force += nrm * fd;

        const JPH::Vec3 share = force * (dt / 3.0f);
        if (verts[i0].mInvMass > 0.0f) dv[i0] += share * verts[i0].mInvMass;
        if (verts[i1].mInvMass > 0.0f) dv[i1] += share * verts[i1].mInvMass;
        if (verts[i2].mInvMass > 0.0f) dv[i2] += share * verts[i2].mInvMass;
    }

    if (subArea > 0.0) {
        const JPH::Mat44 rotT = com.GetRotation();
        for (size_t i = 0; i < n; ++i)
            if (verts[i].mInvMass > 0.0f) verts[i].mVelocity += rotT.Multiply3x3Transposed(dv[i]);
        // Keep it awake while any face is in the water (the aero pattern): sbxKeepAwake reads the fraction.
        if (!body.IsActive()) g_world->system.GetBodyInterfaceNoLock().ActivateBody(body.GetID());
    }
    e.submergedFraction = totalArea > 0.0 ? static_cast<float>(subArea / totalArea) : 0.0f;
    e.submergedVolumeCm3 = closed ? static_cast<float>(std::max(0.0, volume)) * kCmPerMetre * kCmPerMetre * kCmPerMetre : 0.0f;
}

} // namespace aver::physics::softbody
