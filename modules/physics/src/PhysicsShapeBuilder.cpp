// PhysicsShapeBuilder.cpp -- every shape builder (physics_shape_builder_abi.h) and the one implementation of
// each shape: the old aver_phys_add_* creators call the make* functions below, so a capsule is built the
// same way whichever entry point asked for it.
//
// A builder sets the Jolt defaults itself and overrides a field only when the caller's SP block asked, which
// is what keeps an old creator (no SP block) bit-identical to what it built before this file existed.
#include "aver/physics/physics_shape_builder_abi.h"
#include "PhysicsShapeTable.hpp"
#include "PhysicsMeshBuild.hpp"
#include "PhysicsSurface.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/UnorderedSet.h>   // Shape::VisitedShapes
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>
#include <Jolt/Physics/Collision/Shape/TriangleShape.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace aver;
using namespace aver::physics;

namespace aver::physics::detail {

namespace {

using aver::AbiError;

JPH::Ref<JPH::Shape> finish(const JPH::Shape::ShapeResult& res, const char* what) {
    if (res.HasError()) {
        AVER_WARN("[Physics] {}: {}", what, res.GetError().c_str());
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    return res.Get();
}

void applyConvex(JPH::ConvexShapeSettings& s, int32_t surface, const ShapeParams& sp) {
    if (surface != 0) s.mMaterial = surfaceMaterial(surface);
    if (sp.densityKgM3 > 0.0f) s.SetDensity(sp.densityKgM3);
}

bool positive(float v) { return std::isfinite(v) && v > 0.0f; }

} // namespace

JPH::Ref<JPH::Shape> makeBox(float hx, float hy, float hz, int32_t surface, const ShapeParams& sp) {
    // Half-extents are a SIZE: the axis permutation applies but the sign flip does not.
    JPH::BoxShapeSettings s(JPH::Vec3(std::abs(cmToM(hy)), std::abs(cmToM(hz)), std::abs(cmToM(hx))));
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    if (sp.convexRadiusCm >= 0.0f) s.mConvexRadius = cmToM(sp.convexRadiusCm);
    return finish(s.Create(), "box");
}

JPH::Ref<JPH::Shape> makeSphere(float radius, int32_t surface, const ShapeParams& sp) {
    if (!positive(radius)) { AVER_WARN("[Physics] sphere needs a positive radius ({}cm)", radius); aver::setAbiError(AbiError::InvalidArgument); return nullptr; }
    JPH::SphereShapeSettings s(cmToM(radius));
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    return finish(s.Create(), "sphere");
}

JPH::Ref<JPH::Shape> makeCapsule(float radius, float height, int32_t surface, const ShapeParams& sp) {
    // Jolt's capsule is described by the HALF height of its cylinder; `height` is TOTAL, so the caps come out first.
    const float rM = cmToM(radius);
    const float halfCyl = cmToM(height) * 0.5f - rM;
    if (!positive(radius) || !(halfCyl > 0.0f)) {
        AVER_WARN("[Physics] capsule {}cm tall is too short for radius {}cm", height, radius);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    JPH::CapsuleShapeSettings s(halfCyl, rM);
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    return finish(s.Create(), "capsule");
}

JPH::Ref<JPH::Shape> makeCylinder(float radius, float height, int32_t surface, const ShapeParams& sp) {
    const float halfHeight = cmToM(height) * 0.5f;
    if (!positive(radius) || !(halfHeight > 0.0f)) {
        AVER_WARN("[Physics] cylinder needs a positive radius and height ({}cm radius, {}cm height)", radius, height);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    JPH::CylinderShapeSettings s(halfHeight, cmToM(radius));
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    if (sp.convexRadiusCm >= 0.0f) s.mConvexRadius = cmToM(sp.convexRadiusCm);
    return finish(s.Create(), "cylinder");
}

JPH::Ref<JPH::Shape> makeTaperedCapsule(float topRadius, float bottomRadius, float height, int32_t surface, const ShapeParams& sp) {
    const float topM = cmToM(topRadius);
    const float bottomM = cmToM(bottomRadius);
    // TOTAL height, both caps included: the two ends subtract DIFFERENT radii.
    const float halfCyl = (cmToM(height) - topM - bottomM) * 0.5f;
    if (!positive(topRadius) || !positive(bottomRadius) || !(halfCyl > 0.0f)) {
        AVER_WARN("[Physics] tapered capsule {}cm tall is too short for radii {}cm/{}cm", height, topRadius, bottomRadius);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    JPH::TaperedCapsuleShapeSettings s(halfCyl, topM, bottomM);
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    return finish(s.Create(), "tapered capsule");
}

JPH::Ref<JPH::Shape> makeTaperedCylinder(float topRadius, float bottomRadius, float height, int32_t surface, const ShapeParams& sp) {
    const float halfHeight = cmToM(height) * 0.5f;
    if (!std::isfinite(topRadius) || !std::isfinite(bottomRadius) || topRadius < 0.0f || bottomRadius < 0.0f ||
        (topRadius <= 0.0f && bottomRadius <= 0.0f) || !(halfHeight > 0.0f)) {
        AVER_WARN("[Physics] tapered cylinder needs non-negative radii (not both zero) and a positive height "
                  "({}cm/{}cm radii, {}cm height)", topRadius, bottomRadius, height);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    JPH::TaperedCylinderShapeSettings s(halfHeight, cmToM(topRadius), cmToM(bottomRadius));
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    if (sp.convexRadiusCm >= 0.0f) s.mConvexRadius = cmToM(sp.convexRadiusCm);
    return finish(s.Create(), "tapered cylinder");
}

JPH::Ref<JPH::Shape> makeTriangle(const float* abc, int32_t surface, const ShapeParams& sp) {
    if (!abc) { aver::setAbiError(AbiError::NullPointer); return nullptr; }
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(abc[i])) { AVER_WARN("[Physics] triangle: a non-finite vertex"); aver::setAbiError(AbiError::InvalidArgument); return nullptr; }
    const JPH::Vec3 a = toJolt(Vec3(abc[0], abc[1], abc[2]));
    const JPH::Vec3 b = toJolt(Vec3(abc[3], abc[4], abc[5]));
    const JPH::Vec3 c = toJolt(Vec3(abc[6], abc[7], abc[8]));
    if ((b - a).Cross(c - a).LengthSq() < 1.0e-12f) {
        AVER_WARN("[Physics] triangle: the three vertices are collinear");
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    // Wound like the mesh builder (the axis map mirrors, so B and C swap): Jolt's triangle is one-sided against bodies, and
    // the face a caller sees is the one (B - A) x (C - A) points to, as for a mesh.
    JPH::TriangleShapeSettings s(a, c, b, 0.0f);
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    if (sp.convexRadiusCm >= 0.0f) s.mConvexRadius = cmToM(sp.convexRadiusCm);
    return finish(s.Create(), "triangle");
}

JPH::Ref<JPH::Shape> makePlane(const float* normal, float offsetCm, float halfExtentCm, int32_t surface) {
    if (!normal) { aver::setAbiError(AbiError::NullPointer); return nullptr; }
    const float len = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
    if (!std::isfinite(len) || len < 1.0e-6f || !std::isfinite(offsetCm) ||
        !std::isfinite(halfExtentCm) || halfExtentCm < 1.0f || halfExtentCm > 1.0e6f) {
        AVER_WARN("[Physics] plane needs a non-zero normal and a half extent of 1 to 1e6 cm");
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    // The points with n . x = offset are the plane; Jolt's constant is the other sign (n . x + c = 0).
    const JPH::Vec3 n = toJoltUnit(Vec3(normal[0] / len, normal[1] / len, normal[2] / len)).Normalized();
    JPH::PlaneShapeSettings s(JPH::Plane(n, -cmToM(offsetCm)), surfaceMaterial(surface).GetPtr(), cmToM(halfExtentCm));
    s.SetEmbedded();
    return finish(s.Create(), "plane");
}

JPH::Ref<JPH::Shape> makeEmpty(const float* com) {
    const Vec3 c = com ? Vec3(com[0], com[1], com[2]) : Vec3(0, 0, 0);
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) { aver::setAbiError(AbiError::InvalidArgument); return nullptr; }
    JPH::EmptyShapeSettings s(toJolt(c));
    s.SetEmbedded();
    return finish(s.Create(), "empty");
}

// A thin capsule whose axis runs from the shape origin along `dir`; the far cap's tip is at lengthCm. See the header for what this approximates.
JPH::Ref<JPH::Shape> makeSeparationRay(float lengthCm, const float* dir, float radiusCm, int32_t surface) {
    if (!dir) { aver::setAbiError(AbiError::NullPointer); return nullptr; }
    const float len = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    const float r = radiusCm > 0.0f ? radiusCm : 0.1f;
    if (!std::isfinite(len) || len < 1.0e-6f || !positive(lengthCm) || !positive(r) || !(lengthCm > 2.0f * r)) {
        AVER_WARN("[Physics] separation ray needs a direction and a length over twice its radius ({}cm long, {}cm radius)", lengthCm, r);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    const float rM = cmToM(r);
    JPH::CapsuleShapeSettings s((cmToM(lengthCm) - 2.0f * rM) * 0.5f, rM);
    s.SetEmbedded();
    if (surface != 0) s.mMaterial = surfaceMaterial(surface);
    s.SetDensity(0.001f);   // no weight worth the name: a ray has none
    const JPH::Ref<JPH::Shape> capsule = finish(s.Create(), "separation ray");
    if (!capsule) return nullptr;
    const JPH::Vec3 d = toJoltUnit(Vec3(dir[0] / len, dir[1] / len, dir[2] / len)).Normalized();
    // The capsule stands along Jolt's Y; turn it onto d.
    const JPH::Quat rot = d.GetY() < -0.9999f ? JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI) : JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), d);
    return new JPH::RotatedTranslatedShape(d * (cmToM(lengthCm) * 0.5f), rot, capsule.GetPtr());
}

JPH::Ref<JPH::Shape> makeHull(const float* pts, int32_t count, int32_t surface, const ShapeParams& sp) {
    if (!pts || count < 4) { aver::setAbiError(AbiError::InvalidArgument); return nullptr; }   // fewer than four points has no volume
    JPH::Array<JPH::Vec3> hull;
    hull.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
        hull.push_back(toJolt(Vec3(pts[i*3+0], pts[i*3+1], pts[i*3+2])));
    JPH::ConvexHullShapeSettings s(hull);
    s.SetEmbedded();
    applyConvex(s, surface, sp);
    if (sp.convexRadiusCm >= 0.0f) s.mMaxConvexRadius = cmToM(sp.convexRadiusCm);
    if (sp.hullToleranceCm > 0.0f) s.mHullTolerance = cmToM(sp.hullToleranceCm);
    JPH::Ref<JPH::Shape> out = finish(s.Create(), "convex hull");
    if (!out) return nullptr;
    // Jolt turns coplanar points into a flat two-faced hull; a shape with no volume is not what this builder promises.
    const JPH::Vec3 ext = out->GetLocalBounds().GetSize();
    const float big = std::max({ext.GetX(), ext.GetY(), ext.GetZ()});
    if (!(out->GetVolume() > 1.0e-6f * big * big * big)) {
        AVER_WARN("[Physics] convex hull: the points are coplanar (no volume)");
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    return out;
}

JPH::Ref<JPH::Shape> makeCompound(int32_t kind, const std::vector<CompoundChild>& kids, const char* label) {
    if (kids.empty()) { aver::setAbiError(AbiError::InvalidArgument); return nullptr; }
    // The sub-shape is a SHAPE, not settings: the settings overload keeps a pointer to what it is given.
    if (kind == AVER_PHYS_COMPOUND_MUTABLE) {
        JPH::MutableCompoundShapeSettings cs;
        for (const CompoundChild& k : kids) cs.AddShape(k.position, k.rotation, k.shape.GetPtr(), k.userData);
        return finish(cs.Create(), label);
    }
    JPH::StaticCompoundShapeSettings cs;
    for (const CompoundChild& k : kids) cs.AddShape(k.position, k.rotation, k.shape.GetPtr(), k.userData);
    return finish(cs.Create(), label);
}

} // namespace aver::physics::detail

// ---- ABI ------------------------------------------------------------------------------------------------

using namespace aver::physics::detail;

namespace {

bool needWorld() {
    if (g_world) return true;
    aver::setAbiError(aver::AbiError::NotInitialised);
    return false;
}

int32_t bad(aver::AbiError e) {
    aver::setAbiError(e);
    return 0;
}

// A shape that built becomes a handle; one that did not leaves the error the builder recorded.
int32_t publish(const JPH::Ref<JPH::Shape>& s) {
    if (!s) return 0;
    aver::setAbiError(aver::AbiError::Ok);
    return shapeTableAdd(s);
}

int32_t shapeTypeOf(const JPH::Shape& s) {
    switch (s.GetSubType()) {
        case JPH::EShapeSubType::Box:                return AVER_PHYS_SHAPE_TYPE_BOX;
        case JPH::EShapeSubType::Sphere:             return AVER_PHYS_SHAPE_TYPE_SPHERE;
        case JPH::EShapeSubType::Capsule:            return AVER_PHYS_SHAPE_TYPE_CAPSULE;
        case JPH::EShapeSubType::Cylinder:           return AVER_PHYS_SHAPE_TYPE_CYLINDER;
        case JPH::EShapeSubType::TaperedCapsule:     return AVER_PHYS_SHAPE_TYPE_TAPERED_CAPSULE;
        case JPH::EShapeSubType::TaperedCylinder:    return AVER_PHYS_SHAPE_TYPE_TAPERED_CYLINDER;
        case JPH::EShapeSubType::ConvexHull:         return AVER_PHYS_SHAPE_TYPE_CONVEX_HULL;
        case JPH::EShapeSubType::Triangle:           return AVER_PHYS_SHAPE_TYPE_TRIANGLE;
        case JPH::EShapeSubType::Plane:              return AVER_PHYS_SHAPE_TYPE_PLANE;
        case JPH::EShapeSubType::Empty:              return AVER_PHYS_SHAPE_TYPE_EMPTY;
        case JPH::EShapeSubType::Mesh:               return AVER_PHYS_SHAPE_TYPE_MESH;
        case JPH::EShapeSubType::HeightField:        return AVER_PHYS_SHAPE_TYPE_HEIGHTFIELD;
        case JPH::EShapeSubType::StaticCompound:     return AVER_PHYS_SHAPE_TYPE_STATIC_COMPOUND;
        case JPH::EShapeSubType::MutableCompound:    return AVER_PHYS_SHAPE_TYPE_MUTABLE_COMPOUND;
        case JPH::EShapeSubType::RotatedTranslated:  return AVER_PHYS_SHAPE_TYPE_ROTATED_TRANSLATED;
        case JPH::EShapeSubType::Scaled:             return AVER_PHYS_SHAPE_TYPE_SCALED;
        case JPH::EShapeSubType::OffsetCenterOfMass: return AVER_PHYS_SHAPE_TYPE_OFFSET_COM;
        case JPH::EShapeSubType::SoftBody:           return AVER_PHYS_SHAPE_TYPE_SOFT_BODY;
        default:                                     return AVER_PHYS_SHAPE_TYPE_UNKNOWN;
    }
}

int32_t countLeaves(const JPH::Shape* s) {
    if (s->GetType() == JPH::EShapeType::Compound) {
        int32_t n = 0;
        for (const JPH::CompoundShape::SubShape& sub : static_cast<const JPH::CompoundShape*>(s)->GetSubShapes())
            n += countLeaves(sub.mShape.GetPtr());
        return n;
    }
    if (s->GetType() == JPH::EShapeType::Decorated)
        return countLeaves(static_cast<const JPH::DecoratedShape*>(s)->GetInnerShape());
    return 1;
}

// The SI and SII blocks of aver_phys_shape_info for one shape.
void fillShapeInfo(const JPH::Shape& s, float* f, int32_t* iv) {
    const JPH::MassProperties mp = s.GetMassProperties();
    f[AVER_PHYS_SI_VOLUME_CM3] = s.GetVolume() * 1.0e6f;
    f[AVER_PHYS_SI_MASS_KG] = mp.mMass;

    const JPH::Vec3 comJ = s.GetCenterOfMass();
    const Vec3 com = fromJolt(comJ);
    f[AVER_PHYS_SI_COM_X] = com.x; f[AVER_PHYS_SI_COM_Y] = com.y; f[AVER_PHYS_SI_COM_Z] = com.z;

    // Local bounds are centred on the centre of mass; the ABI reports them in the shape's own frame.
    JPH::AABox lb = s.GetLocalBounds();
    lb.Translate(comJ);
    const Vec3 a = fromJolt(lb.mMin), b = fromJolt(lb.mMax);
    f[AVER_PHYS_SI_BOUNDS_MIN_X] = std::fmin(a.x, b.x); f[AVER_PHYS_SI_BOUNDS_MAX_X] = std::fmax(a.x, b.x);
    f[AVER_PHYS_SI_BOUNDS_MIN_Y] = std::fmin(a.y, b.y); f[AVER_PHYS_SI_BOUNDS_MAX_Y] = std::fmax(a.y, b.y);
    f[AVER_PHYS_SI_BOUNDS_MIN_Z] = std::fmin(a.z, b.z); f[AVER_PHYS_SI_BOUNDS_MAX_Z] = std::fmax(a.z, b.z);
    f[AVER_PHYS_SI_INNER_RADIUS] = s.GetInnerRadius() * kCmPerMetre;

    // PRINCIPAL INERTIA, kg cm^2, in the principal frame. The inertia tensor transforms as B I B^T under the
    // axis map B (a polar tensor, so the reflection in B does not flip it); its eigenvectors are the engine
    // axes B^T p_k. Those three form a LEFT-handed set, so the third is negated (an eigenvector's sign is free)
    // before the frame is read as a rotation. In Jolt axes the equivalent rotation is R' * B^T, with R' the
    // Jolt eigenvector matrix with its third column negated, and fromJolt turns that into an engine quaternion.
    // ShapeBuilderTest rebuilds a (2,3,4) box's engine-axis tensor from these numbers, which pins the conversion.
    f[AVER_PHYS_SI_INERTIA_PRINCIPAL_X] = f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Y] = f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Z] = 0.0f;
    f[AVER_PHYS_SI_INERTIA_FRAME_X] = f[AVER_PHYS_SI_INERTIA_FRAME_Y] = f[AVER_PHYS_SI_INERTIA_FRAME_Z] = 0.0f;
    f[AVER_PHYS_SI_INERTIA_FRAME_W] = 1.0f;
    if (mp.mMass > 0.0f) {
        JPH::Mat44 rot;
        JPH::Vec3 diag;
        if (mp.DecomposePrincipalMomentsOfInertia(rot, diag)) {
            f[AVER_PHYS_SI_INERTIA_PRINCIPAL_X] = diag.GetX() * 1.0e4f;
            f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Y] = diag.GetY() * 1.0e4f;
            f[AVER_PHYS_SI_INERTIA_PRINCIPAL_Z] = diag.GetZ() * 1.0e4f;
            rot.SetAxisZ(-rot.GetAxisZ());
            const JPH::Mat44 bT(JPH::Vec4(0, 1, 0, 0), JPH::Vec4(0, 0, 1, 0), JPH::Vec4(-1, 0, 0, 0), JPH::Vec4(0, 0, 0, 1));
            const Quat q = fromJolt((rot * bT).GetQuaternion().Normalized());
            f[AVER_PHYS_SI_INERTIA_FRAME_X] = q.x; f[AVER_PHYS_SI_INERTIA_FRAME_Y] = q.y;
            f[AVER_PHYS_SI_INERTIA_FRAME_Z] = q.z; f[AVER_PHYS_SI_INERTIA_FRAME_W] = q.w;
        }
    }

    JPH::Shape::VisitedShapes visited;
    const JPH::Shape::Stats st = s.GetStatsRecursive(visited);
    iv[AVER_PHYS_SII_TYPE] = shapeTypeOf(s);
    iv[AVER_PHYS_SII_CHILDREN] = s.GetType() == JPH::EShapeType::Compound
        ? static_cast<int32_t>(static_cast<const JPH::CompoundShape&>(s).GetNumSubShapes()) : 0;
    iv[AVER_PHYS_SII_LEAVES] = countLeaves(&s);
    iv[AVER_PHYS_SII_TRIANGLES] = static_cast<int32_t>(st.mNumTriangles);
    iv[AVER_PHYS_SII_BYTES] = static_cast<int32_t>(std::min<size_t>(st.mSizeBytes, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    iv[AVER_PHYS_SII_MUST_BE_STATIC] = s.MustBeStatic() ? 1 : 0;
    iv[AVER_PHYS_SII_HAS_MASS] = mp.mMass > 0.0f ? 1 : 0;
}

} // namespace

extern "C" {

int32_t aver_phys_shape_abi_version(void) { return AVER_PHYS_SHAPE_ABI_VERSION; }

// ---- leaves -----------------------------------------------------------------------------------------------

int32_t aver_phys_shape_box(float hx, float hy, float hz, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    if (!positive(std::abs(hx)) || !positive(std::abs(hy)) || !positive(std::abs(hz))) {
        AVER_WARN("[Physics] box needs non-zero half extents ({}, {}, {})", hx, hy, hz);
        return bad(AbiError::InvalidArgument);
    }
    return publish(makeBox(hx, hy, hz, surface, sp));
}

int32_t aver_phys_shape_sphere(float radius, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeSphere(radius, surface, sp));
}

int32_t aver_phys_shape_capsule(float radius, float height, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeCapsule(radius, height, surface, sp));
}

int32_t aver_phys_shape_cylinder(float radius, float height, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeCylinder(radius, height, surface, sp));
}

int32_t aver_phys_shape_tapered_capsule(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeTaperedCapsule(topRadius, bottomRadius, height, surface, sp));
}

int32_t aver_phys_shape_tapered_cylinder(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeTaperedCylinder(topRadius, bottomRadius, height, surface, sp));
}

int32_t aver_phys_shape_triangle(const float* abcXyz, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(makeTriangle(abcXyz, surface, sp));
}

int32_t aver_phys_shape_plane(const float* normalXyz, float offsetCm, float halfExtentCm, int32_t surface) {
    if (!needWorld()) return 0;
    return publish(makePlane(normalXyz, offsetCm, halfExtentCm, surface));
}

int32_t aver_phys_shape_empty(const float* comXyz) {
    if (!needWorld()) return 0;
    return publish(makeEmpty(comXyz));
}

int32_t aver_phys_shape_separation_ray(float lengthCm, const float* dirXyz, float radiusCm, int32_t surface) {
    if (!needWorld()) return 0;
    return publish(makeSeparationRay(lengthCm, dirXyz, radiusCm, surface));
}

int32_t aver_phys_shape_convex_hull(const float* pointsXyz, int32_t count, int32_t surface, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    if (!pointsXyz || count < 4) {
        AVER_WARN("[Physics] convex hull needs at least 4 points (got {})", count);
        return bad(AbiError::InvalidArgument);
    }
    return publish(makeHull(pointsXyz, count, surface, sp));
}

int32_t aver_phys_shape_mesh(const float* verticesXyz, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                             const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                             const int32_t* triUserData, const float* p, int32_t n) {
    if (!needWorld()) return 0;
    ShapeParams sp;
    if (!parseShapeParams(p, n, sp)) return 0;
    return publish(buildMeshShape(verticesXyz, vertexCount, indices, indexCount, triPaletteIndex, palette, paletteCount,
                                  triUserData, sp, "mesh shape"));
}

// ---- decorators and compounds ------------------------------------------------------------------------------

int32_t aver_phys_shape_rotated_translated(int32_t inner, const float* posCm, const float* quatXyzw) {
    if (!needWorld()) return 0;
    const JPH::RefConst<JPH::Shape> in = shapeTableGet(inner);
    if (!in) return bad(AbiError::BadHandle);
    if (posCm && !allFiniteN(posCm, 3)) return bad(AbiError::InvalidArgument);
    JPH::Quat q;
    if (!readJoltQuat(quatXyzw, q)) return bad(AbiError::InvalidArgument);
    const JPH::Vec3 pos = posCm ? toJolt(Vec3(posCm[0], posCm[1], posCm[2])) : JPH::Vec3::sZero();
    const JPH::Ref<JPH::Shape> out = new JPH::RotatedTranslatedShape(pos, q, in.GetPtr());
    return publish(out);
}

int32_t aver_phys_shape_offset_com(int32_t inner, const float* offsetCm) {
    if (!needWorld()) return 0;
    const JPH::RefConst<JPH::Shape> in = shapeTableGet(inner);
    if (!in) return bad(AbiError::BadHandle);
    if (!offsetCm || !allFiniteN(offsetCm, 3)) return bad(offsetCm ? AbiError::InvalidArgument : AbiError::NullPointer);
    const JPH::Ref<JPH::Shape> out = new JPH::OffsetCenterOfMassShape(in.GetPtr(), toJolt(Vec3(offsetCm[0], offsetCm[1], offsetCm[2])));
    return publish(out);
}

int32_t aver_phys_shape_scaled(int32_t inner, const float* scaleXyz) {
    if (!needWorld()) return 0;
    const JPH::RefConst<JPH::Shape> in = shapeTableGet(inner);
    if (!in) return bad(AbiError::BadHandle);
    if (!scaleXyz) return bad(AbiError::NullPointer);
    if (!allFiniteN(scaleXyz, 3) || std::abs(scaleXyz[0]) < 1.0e-4f || std::abs(scaleXyz[1]) < 1.0e-4f || std::abs(scaleXyz[2]) < 1.0e-4f) {
        AVER_WARN("[Physics] scaled shape: a zero or non-finite scale is refused");
        return bad(AbiError::InvalidArgument);
    }
    // The axis permutation without sign changes: a scale is the diagonal of a linear map, and a signed
    // permutation conjugates a diagonal into a diagonal (see aver_phys_add_mesh_shape_body).
    const JPH::Shape::ShapeResult res = in->ScaleShape(JPH::Vec3(scaleXyz[1], scaleXyz[2], scaleXyz[0]));
    return publish(finish(res, "scaled shape"));
}

int32_t aver_phys_shape_compound(int32_t kind, const int32_t* children, const float* posCm, const float* quatXyzw,
                                 const int32_t* childUserData, int32_t count) {
    if (!needWorld()) return 0;
    if (kind != AVER_PHYS_COMPOUND_STATIC && kind != AVER_PHYS_COMPOUND_MUTABLE) return bad(AbiError::InvalidArgument);
    if (!children || count < 1 || count > 65536) {
        AVER_WARN("[Physics] compound needs 1 to 65536 children (got {})", count);
        return bad(AbiError::InvalidArgument);
    }
    std::vector<CompoundChild> kids;
    kids.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        CompoundChild k;
        k.shape = shapeTableGet(children[i]);
        if (!k.shape) { AVER_WARN("[Physics] compound child {}: unknown shape handle {}", i, children[i]); return bad(AbiError::BadHandle); }
        if (posCm) {
            if (!allFiniteN(posCm + i * 3, 3)) return bad(AbiError::InvalidArgument);
            k.position = toJolt(Vec3(posCm[i*3+0], posCm[i*3+1], posCm[i*3+2]));
        }
        if (!readJoltQuat(quatXyzw ? quatXyzw + i * 4 : nullptr, k.rotation)) {
            AVER_WARN("[Physics] compound child {}: a zero or non-finite rotation", i);
            return bad(AbiError::InvalidArgument);
        }
        k.userData = childUserData ? static_cast<uint32_t>(childUserData[i]) : 0u;
        kids.push_back(std::move(k));
    }
    return publish(makeCompound(kind, kids, "compound"));
}

// ---- handles and introspection -----------------------------------------------------------------------------

int32_t aver_phys_shape_release(int32_t shape) {
    if (shapeTableRelease(shape)) { aver::setAbiError(aver::AbiError::Ok); return 1; }
    return bad(AbiError::BadHandle);
}

int32_t aver_phys_shape_count(void) { return shapeTableCount(); }

int32_t aver_phys_shape_info(int32_t shape, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    const JPH::RefConst<JPH::Shape> s = shapeTableGet(shape);
    if (!s) return bad(AbiError::BadHandle);
    if (nF < 0 || nI < 0) return bad(AbiError::InvalidArgument);
    if ((nF > 0 && !outF) || (nI > 0 && !outI)) return bad(AbiError::NullPointer);
    float f[AVER_PHYS_SI_COUNT];
    int32_t iv[AVER_PHYS_SII_COUNT];
    fillShapeInfo(*s, f, iv);
    for (int32_t k = 0; k < std::min<int32_t>(nF, AVER_PHYS_SI_COUNT); ++k) outF[k] = f[k];
    for (int32_t k = 0; k < std::min<int32_t>(nI, AVER_PHYS_SII_COUNT); ++k) outI[k] = iv[k];
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

int32_t aver_phys_shape_of_body(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;   // a character is not in the body table: BadHandle, as the header says
    JPH::RefConst<JPH::Shape> shape;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return bad(AbiError::BadHandle);
        if (lock.GetBody().IsSoftBody()) return bad(AbiError::Unsupported);
        shape = lock.GetBody().GetShape();
    }
    if (!shape) return bad(AbiError::BadHandle);
    aver::setAbiError(aver::AbiError::Ok);
    return shapeTableAdd(shape);
}

} // extern "C"
