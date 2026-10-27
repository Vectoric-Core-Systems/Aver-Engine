// PhysicsShapeTable.hpp -- the shape-handle table and the builders every shape creator shares. SRC-LOCAL like PhysicsInternal.hpp.
// The table is process-wide and NOT cleared by aver_phys_shutdown: a shape handle outlives the world if never released.
// A shape handle and a body handle can be the same integer; only the function tells them apart.
#pragma once
#include "PhysicsInternal.hpp"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstdint>
#include <vector>

namespace aver::physics::detail {

// Adds a shape and returns its handle (never 0). Safe from any thread.
int32_t shapeTableAdd(JPH::RefConst<JPH::Shape> shape);
JPH::RefConst<JPH::Shape> shapeTableGet(int32_t handle);
// The shape lives on while a body or another shape uses it. False for an unknown handle.
bool shapeTableRelease(int32_t handle);
int32_t shapeTableCount();

// A MUTABLE compound (possibly under decorators) is single-owner, keyed by the shape object so a second handle cannot bypass it.
bool shapeTableOwnerFree(const JPH::Shape* shape);
void shapeTableBindOwner(const JPH::Shape* shape, int32_t bodyHandle);
// Body handles restart at 1 with the next world.
void shapeTableOnShutdown();

struct ShapeParams {
    float convexRadiusCm = -1.0f;    // < 0: the shape's own default
    float densityKgM3 = -1.0f;       // < 0: 1000
    float hullToleranceCm = -1.0f;   // < 0: Jolt's 1 mm
    bool  edgeSet = false;           // false: Jolt's 5 degrees
    float edgeAngleDeg = 5.0f;       // when edgeSet; negative = every edge active
    bool  favorBuildSpeed = false;
    int   maxTrisPerLeaf = 8;
};

// False (error recorded, reason logged) for a block too long, non-finite or out of range.
bool parseShapeParams(const float* p, int32_t n, ShapeParams& out);

bool allFiniteN(const float* v, int n);
// Null = identity; false for a non-finite or zero-length quaternion.
bool readJoltQuat(const float* xyzw, JPH::Quat& out);

// Null on failure, with the reason logged and recorded. Sizes are engine centimetres. `surface` 0 = no material.
JPH::Ref<JPH::Shape> makeBox(float hxCm, float hyCm, float hzCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeSphere(float radiusCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeCapsule(float radiusCm, float heightCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeCylinder(float radiusCm, float heightCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeTaperedCapsule(float topCm, float bottomCm, float heightCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeTaperedCylinder(float topCm, float bottomCm, float heightCm, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makeTriangle(const float* abcXyz, int32_t surface, const ShapeParams& sp);
JPH::Ref<JPH::Shape> makePlane(const float* normalXyz, float offsetCm, float halfExtentCm, int32_t surface);
JPH::Ref<JPH::Shape> makeEmpty(const float* comXyz);
JPH::Ref<JPH::Shape> makeHull(const float* pointsXyz, int32_t count, int32_t surface, const ShapeParams& sp);

// The HP block of physics_terrain_abi.h. Unset fields are not passed to Jolt, so an empty block stays bit-identical.
struct HeightfieldParams {
    int   blockSize = 2;
    int   bits = 8;
    bool  edgeSet = false;
    float edgeAngleDeg = 5.0f;
    float headroomCm = 0.0f;
};
// False (reason logged and recorded) for a block too long, non-finite or out of range.
bool parseHeightfieldParams(const float* p, int32_t n, HeightfieldParams& out);
// v1 physics sample order (PhysicsTerrain.cpp). NaN, infinite and FLT_MAX samples are holes; built square on the larger side.
// cellPalette: (cols-1)*(rows-1) bytes indexing palette (null = none, or palette[0] everywhere when given). Null on failure.
JPH::Ref<JPH::Shape> makeHeightfield(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                     const uint8_t* cellPalette, const int32_t* palette, int32_t paletteCount,
                                     const HeightfieldParams& hp);

// Position and rotation in the compound's frame (Jolt axes, metres).
struct CompoundChild {
    JPH::RefConst<JPH::Shape> shape;
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
    uint32_t userData = 0;
};
JPH::Ref<JPH::Shape> makeCompound(int32_t kind, const std::vector<CompoundChild>& children, const char* label);

// The helpers below look through decorators.
const JPH::Shape* mutableCompoundOf(const JPH::Shape* shape);
bool isMeshLike(const JPH::Shape* shape);
// 0 for a mesh, heightfield or plane.
float shapeMassKg(const JPH::Shape* shape);

} // namespace aver::physics::detail
