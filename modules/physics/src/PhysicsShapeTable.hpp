// PhysicsShapeTable.hpp -- the one shape-handle table, and the internal builders every shape creator shares.
//
// SRC-LOCAL like PhysicsInternal.hpp (it names JPH:: types). The table is process-wide and mutex-guarded, has
// its own counter (a shape handle and a body handle can be the same integer; only the function tells them
// apart) and is NOT cleared by aver_phys_shutdown: a JPH::Shape is bare geometry, so a handle a caller forgot
// to release simply outlives the world, exactly as a mesh shape handle always did.
#pragma once
#include "PhysicsInternal.hpp"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstdint>
#include <vector>

namespace aver::physics::detail {

// ---- the table -----------------------------------------------------------------------------------------

// Adds a shape and returns its handle (never 0). Safe from any thread.
int32_t shapeTableAdd(JPH::RefConst<JPH::Shape> shape);
// The shape behind a handle, or null.
JPH::RefConst<JPH::Shape> shapeTableGet(int32_t handle);
// Drops the handle (the shape lives on while a body or another shape uses it). False for an unknown handle.
bool shapeTableRelease(int32_t handle);
// Live handles.
int32_t shapeTableCount();

// A MUTABLE compound (possibly under decorators) is single-owner. Keyed by the shape object, so a second
// handle on the same shape (aver_phys_shape_of_body) cannot bypass the rule.
// True when `shape` holds no mutable compound or the one it holds has no live owner.
bool shapeTableOwnerFree(const JPH::Shape* shape);
// Records `bodyHandle` as the owner of the mutable compound `shape` holds (a no-op for any other shape).
void shapeTableBindOwner(const JPH::Shape* shape, int32_t bodyHandle);
// Forgets every owner binding (body handles restart at 1 with the next world).
void shapeTableOnShutdown();

// ---- parameters (the SP block of physics_shape_builder_abi.h) -------------------------------------------

struct ShapeParams {
    float convexRadiusCm = -1.0f;    // < 0: the shape's own default
    float densityKgM3 = -1.0f;       // < 0: 1000
    float hullToleranceCm = -1.0f;   // < 0: Jolt's 1 mm
    bool  edgeSet = false;           // false: Jolt's 5 degrees
    float edgeAngleDeg = 5.0f;       // when edgeSet; negative = every edge active
    bool  favorBuildSpeed = false;
    int   maxTrisPerLeaf = 8;
};

// Reads `n` entries of `p` into `out`; false (error recorded, reason logged) for a block that is too long,
// non-finite or out of range.
bool parseShapeParams(const float* p, int32_t n, ShapeParams& out);

// True when all `n` floats are finite.
bool allFiniteN(const float* v, int n);
// An optional engine quaternion (x, y, z, w) as a unit Jolt quaternion: null = identity; false for a
// non-finite or zero-length one.
bool readJoltQuat(const float* xyzw, JPH::Quat& out);

// ---- the builders (null on failure, with the reason logged and recorded) -------------------------------
// Sizes are engine centimetres. `surface` 0 = no material.

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

// The HP block of physics_terrain_abi.h. Defaults are Jolt's (block 2, 8 bits, 5 degrees) and an unset field is not
// passed to Jolt at all, which is what keeps aver_phys_add_heightfield (an empty block) bit-identical to what it built.
struct HeightfieldParams {
    int   blockSize = 2;
    int   bits = 8;
    bool  edgeSet = false;
    float edgeAngleDeg = 5.0f;
    float headroomCm = 0.0f;
};
// Reads `n` entries of `p`; false (reason logged and recorded) for a block that is too long, non-finite or out of range.
bool parseHeightfieldParams(const float* p, int32_t n, HeightfieldParams& out);
// A heightfield from cols * rows samples in the v1 physics order (PhysicsTerrain.cpp). NaN, infinite and FLT_MAX samples are
// holes; the shape is built square (the larger side) with the extra samples holes. cellPalette is (cols-1)*(rows-1) bytes
// indexing `palette` (null = none, or palette[0] everywhere when a palette is given). Null on failure.
JPH::Ref<JPH::Shape> makeHeightfield(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                     const uint8_t* cellPalette, const int32_t* palette, int32_t paletteCount,
                                     const HeightfieldParams& hp);

// A compound child: position and rotation in the compound's frame (Jolt axes, metres), user data.
struct CompoundChild {
    JPH::RefConst<JPH::Shape> shape;
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
    uint32_t userData = 0;
};
// kind is AVER_PHYS_COMPOUND_*.
JPH::Ref<JPH::Shape> makeCompound(int32_t kind, const std::vector<CompoundChild>& children, const char* label);

// ---- helpers other files use ---------------------------------------------------------------------------

// The MutableCompoundShape a shape IS (possibly under decorators), or null.
const JPH::Shape* mutableCompoundOf(const JPH::Shape* shape);
// A mesh, possibly under decorators.
bool isMeshLike(const JPH::Shape* shape);
// A shape's mass in kg (0 for a mesh, a heightfield, a plane).
float shapeMassKg(const JPH::Shape* shape);

} // namespace aver::physics::detail
