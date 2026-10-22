// PhysicsShapes.cpp -- the rigid-body creators physics_shapes_abi.h adds: capsule, cylinder, tapered capsule, and
// a compound of boxes.
//
// ITS OWN TRANSLATION UNIT for the reason PhysicsBody.cpp already is one: PhysicsWorld.cpp is the world's
// lifetime and the step loop, and this is a flat sheet of creators that share nothing with it but addBody() and
// the Convert.hpp helpers. SINCE WAVE 11 every creator here builds its shape through the make* functions of
// PhysicsShapeTable.hpp (one implementation of each shape, with no SP block: Jolt's defaults) and only adds the
// body, so a capsule made here and one made by aver_phys_shape_capsule are the same shape.
#include "aver/physics/physics_shapes_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "Convert.hpp"
#include "PhysicsInternal.hpp"
#include "PhysicsShapeTable.hpp"

#include "aver/core/Log.hpp"

#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// Builds one StaticCompoundShape from `count` local boxes, or returns null having already logged why. Shared by
// the static and dynamic entry points below, which differ only in what addBody() does with the result.
//
// Each box is built as a SHAPE and the compound is handed the shape: the settings overload of AddShape keeps a
// pointer to what it is given, and a loop-local settings object died with the compound still pointing at it
// (all `count` sub-shapes then aliased one stack slot). Every box shares the identity rotation, by design.
JPH::Ref<JPH::Shape> buildBoxCompound(const float* offsetsCm, const float* halfExtentsCm,
                                      int32_t count, const char* label) {
    if (!offsetsCm || !halfExtentsCm || count < 1) {
        AVER_WARN("[Physics] {} compound: needs at least one box", label);
        return nullptr;
    }
    std::vector<CompoundChild> kids;
    kids.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        CompoundChild k;
        // A local OFFSET, not a world position -- toJolt is still the right converter, because the axis
        // permutation and the cm->m scale are both linear and do not care where the origin is.
        k.position = toJolt(Vec3(offsetsCm[i*3+0], offsetsCm[i*3+1], offsetsCm[i*3+2]));
        k.shape = makeBox(halfExtentsCm[i*3+0], halfExtentsCm[i*3+1], halfExtentsCm[i*3+2], 0, ShapeParams{});
        if (!k.shape) {
            AVER_WARN("[Physics] {} compound: box {} could not be built", label, i);
            return nullptr;
        }
        kids.push_back(std::move(k));
    }
    return makeCompound(AVER_PHYS_COMPOUND_STATIC, kids, label);
}

} // namespace

extern "C" {

// ---- Capsule -----------------------------------------------------------------------------------

int32_t aver_phys_add_static_capsule(float cx, float cy, float cz, float radius, float height) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeCapsule(radius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_capsule(float cx, float cy, float cz,
                                      float radius, float height, float massKg) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeCapsule(radius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), true, massKg);
}

// ---- Cylinder ------------------------------------------------------------------------------------

int32_t aver_phys_add_static_cylinder(float cx, float cy, float cz, float radius, float height) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeCylinder(radius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_cylinder(float cx, float cy, float cz,
                                       float radius, float height, float massKg) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeCylinder(radius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), true, massKg);
}

// ---- Tapered capsule -----------------------------------------------------------------------------

int32_t aver_phys_add_static_tapered_capsule(float cx, float cy, float cz,
                                             float topRadius, float bottomRadius, float height) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeTaperedCapsule(topRadius, bottomRadius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_tapered_capsule(float cx, float cy, float cz,
                                              float topRadius, float bottomRadius,
                                              float height, float massKg) {
    if (!g_world) return 0;
    const JPH::Ref<JPH::Shape> shape = makeTaperedCapsule(topRadius, bottomRadius, height, 0, ShapeParams{});
    if (!shape) return 0;
    return addBody(shape.GetPtr(), Vec3(cx, cy, cz), true, massKg);
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
