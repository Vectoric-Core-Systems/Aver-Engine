// PhysicsShapes.cpp -- the rigid-body shapes physics_shapes_abi.h adds: capsule, cylinder, tapered
// capsule, and a compound of boxes.
//
// ITS OWN TRANSLATION UNIT for the reason PhysicsBody.cpp already is one: PhysicsWorld.cpp is the
// world's lifetime and the step loop, and this is a flat sheet of shape constructors that share
// nothing with it but addBody() and the Convert.hpp helpers. Every function here follows
// aver_phys_add_convex_hull's own idiom in PhysicsWorld.cpp: build a ShapeSettings, SetEmbedded() it
// so it can live on the stack, Create() it, check HasError(), then hand the result to addBody().
#include "aver/physics/physics_shapes_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>

#include <cmath>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// Builds one StaticCompoundShape from `count` local boxes, or returns null having already logged
// why. Shared by the static and dynamic entry points below, which differ only in what addBody()
// does with the result.
JPH::Ref<JPH::Shape> buildBoxCompound(const float* offsetsCm, const float* halfExtentsCm,
                                      int32_t count, const char* label) {
    if (!offsetsCm || !halfExtentsCm || count < 1) {
        AVER_WARN("[Physics] {} compound: needs at least one box", label);
        return nullptr;
    }

    JPH::StaticCompoundShapeSettings compound;
    for (int32_t i = 0; i < count; ++i) {
        // A local OFFSET, not a world position -- toJolt is still the right converter, because the
        // axis permutation and the cm->m scale are both linear and do not care where the origin is.
        const JPH::Vec3 offset = toJolt(Vec3(offsetsCm[i*3+0], offsetsCm[i*3+1], offsetsCm[i*3+2]));
        const float hx = halfExtentsCm[i*3+0], hy = halfExtentsCm[i*3+1], hz = halfExtentsCm[i*3+2];
        // Half-extents are a SIZE: the axis permutation applies but the sign flip does not, exactly
        // as aver_phys_add_dynamic_box already does in PhysicsWorld.cpp.
        JPH::BoxShapeSettings box(JPH::Vec3(std::abs(cmToM(hy)), std::abs(cmToM(hz)), std::abs(cmToM(hx))));
        box.SetEmbedded();
        // Every sub-box shares the compound's rotation: IDENTITY on both sides. toJolt(Quat) of the
        // identity has an all-zero vector part, so the axis permutation and sign flip Convert.hpp
        // warns about have nothing to act on -- there is no separate conversion to get wrong for a
        // per-box rotation this ABI does not yet expose.
        compound.AddShape(offset, JPH::Quat::sIdentity(), &box);
    }

    auto res = compound.Create();
    if (res.HasError()) {
        AVER_WARN("[Physics] {} compound: {}", label, res.GetError().c_str());
        return nullptr;
    }
    return res.Get();
}

} // namespace

extern "C" {

// ---- Capsule -----------------------------------------------------------------------------------

int32_t aver_phys_add_static_capsule(float cx, float cy, float cz, float radius, float height) {
    if (!g_world) return 0;
    // Jolt's capsule is described by the HALF height of its cylinder; `height` here is TOTAL, so
    // the caps come out first -- see the file comment and aver_phys_character_create, which already
    // made this same choice for the one capsule this ABI could build before now.
    const float rM = cmToM(radius);
    const float halfCyl = cmToM(height) * 0.5f - rM;
    if (radius <= 0.0f || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] capsule {}cm tall is too short for radius {}cm", height, radius);
        return 0;
    }
    JPH::CapsuleShapeSettings shape(halfCyl, rM);
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] static capsule: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_capsule(float cx, float cy, float cz,
                                      float radius, float height, float massKg) {
    if (!g_world) return 0;
    const float rM = cmToM(radius);
    const float halfCyl = cmToM(height) * 0.5f - rM;
    if (radius <= 0.0f || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] capsule {}cm tall is too short for radius {}cm", height, radius);
        return 0;
    }
    JPH::CapsuleShapeSettings shape(halfCyl, rM);
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] dynamic capsule: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

// ---- Cylinder ------------------------------------------------------------------------------------

int32_t aver_phys_add_static_cylinder(float cx, float cy, float cz, float radius, float height) {
    if (!g_world) return 0;
    const float halfHeight = cmToM(height) * 0.5f;
    if (radius <= 0.0f || halfHeight <= 0.0f) {
        AVER_WARN("[Physics] cylinder needs a positive radius and height ({}cm radius, {}cm height)",
                  radius, height);
        return 0;
    }
    JPH::CylinderShapeSettings shape(halfHeight, cmToM(radius));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] static cylinder: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_cylinder(float cx, float cy, float cz,
                                       float radius, float height, float massKg) {
    if (!g_world) return 0;
    const float halfHeight = cmToM(height) * 0.5f;
    if (radius <= 0.0f || halfHeight <= 0.0f) {
        AVER_WARN("[Physics] cylinder needs a positive radius and height ({}cm radius, {}cm height)",
                  radius, height);
        return 0;
    }
    JPH::CylinderShapeSettings shape(halfHeight, cmToM(radius));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] dynamic cylinder: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

// ---- Tapered capsule -----------------------------------------------------------------------------

int32_t aver_phys_add_static_tapered_capsule(float cx, float cy, float cz,
                                             float topRadius, float bottomRadius, float height) {
    if (!g_world) return 0;
    const float topM = cmToM(topRadius);
    const float bottomM = cmToM(bottomRadius);
    // TOTAL height, both caps included -- see the file comment. Unlike the equal-radius capsule
    // above, the two ends subtract DIFFERENT radii, so this cannot reuse that shape's formula.
    const float halfCyl = (cmToM(height) - topM - bottomM) * 0.5f;
    if (topRadius <= 0.0f || bottomRadius <= 0.0f || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] tapered capsule {}cm tall is too short for radii {}cm/{}cm",
                  height, topRadius, bottomRadius);
        return 0;
    }
    JPH::TaperedCapsuleShapeSettings shape(halfCyl, topM, bottomM);
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) {
        AVER_WARN("[Physics] static tapered capsule: {}", res.GetError().c_str());
        return 0;
    }
    return addBody(res.Get(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_tapered_capsule(float cx, float cy, float cz,
                                              float topRadius, float bottomRadius,
                                              float height, float massKg) {
    if (!g_world) return 0;
    const float topM = cmToM(topRadius);
    const float bottomM = cmToM(bottomRadius);
    const float halfCyl = (cmToM(height) - topM - bottomM) * 0.5f;
    if (topRadius <= 0.0f || bottomRadius <= 0.0f || halfCyl <= 0.0f) {
        AVER_WARN("[Physics] tapered capsule {}cm tall is too short for radii {}cm/{}cm",
                  height, topRadius, bottomRadius);
        return 0;
    }
    JPH::TaperedCapsuleShapeSettings shape(halfCyl, topM, bottomM);
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) {
        AVER_WARN("[Physics] dynamic tapered capsule: {}", res.GetError().c_str());
        return 0;
    }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

// ---- Compound of boxes ---------------------------------------------------------------------------

int32_t aver_phys_add_static_compound_boxes(float cx, float cy, float cz,
                                            const float* offsetsCm, const float* halfExtentsCm,
                                            int32_t count) {
    if (!g_world) return 0;
    JPH::Ref<JPH::Shape> shape = buildBoxCompound(offsetsCm, halfExtentsCm, count, "static");
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_compound_boxes(float cx, float cy, float cz,
                                             const float* offsetsCm, const float* halfExtentsCm,
                                             int32_t count, float massKg) {
    if (!g_world) return 0;
    JPH::Ref<JPH::Shape> shape = buildBoxCompound(offsetsCm, halfExtentsCm, count, "dynamic");
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), true, massKg);
}

} // extern "C"
