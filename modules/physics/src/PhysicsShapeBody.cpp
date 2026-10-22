// PhysicsShapeBody.cpp -- bodies from shape handles, runtime shape and scale change, mutable-compound edits, the
// collision-triangle read-out and the chassis-from-shape call (physics_shape_builder_abi.h).
//
// Jolt's BodyInterface::SetShape(updateMass = true) REPLACES a body's mass by the new shape's volume times its
// density, so every change here passes false and sets the mass properties itself: it is the only way a 10 kg
// crate that grows stays a crate whose mass the caller chose.
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_ragdoll_abi.h"
#include "PhysicsShapeTable.hpp"
#include "PhysicsSurface.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Constraints/Constraint.h>

#include <algorithm>
#include <cmath>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace aver::physics::detail { void warnBodyLimit(); }   // PhysicsWorld.cpp

namespace {

using aver::AbiError;

int32_t fail(AbiError e) {
    aver::setAbiError(e);
    return 0;
}

struct BodyParams {
    u32  layer = 0;
    bool sensor = false, inertiaFromBounds = false, enhancedEdges = false, activate = true;
};

bool parseBodyParams(const float* p, int32_t n, BodyParams& out) {
    out = BodyParams{};
    if (n < 0 || n > AVER_PHYS_BP_COUNT || (n > 0 && !p) || (n > 0 && !allFiniteN(p, n))) {
        AVER_WARN("[Physics] body parameters: a block of {} entries is not valid (the block has {})", n, AVER_PHYS_BP_COUNT);
        aver::setAbiError(AbiError::InvalidArgument);
        return false;
    }
    if (n > AVER_PHYS_BP_LAYER) {
        const long v = std::lround(p[AVER_PHYS_BP_LAYER]);
        if (v < 0 || v >= static_cast<long>(Layers::kUserLayerCount)) {
            AVER_WARN("[Physics] body parameters: collision layer {} is out of range", v);
            aver::setAbiError(AbiError::InvalidArgument);
            return false;
        }
        out.layer = static_cast<u32>(v);
    }
    if (n > AVER_PHYS_BP_SENSOR)               out.sensor = p[AVER_PHYS_BP_SENSOR] != 0.0f;
    if (n > AVER_PHYS_BP_INERTIA_FROM_BOUNDS)  out.inertiaFromBounds = p[AVER_PHYS_BP_INERTIA_FROM_BOUNDS] != 0.0f;
    if (n > AVER_PHYS_BP_ENHANCED_EDGES)       out.enhancedEdges = p[AVER_PHYS_BP_ENHANCED_EDGES] != 0.0f;
    if (n > AVER_PHYS_BP_ACTIVATE)             out.activate = p[AVER_PHYS_BP_ACTIVATE] != 0.0f;
    return true;
}

// Mass and inertia of a solid box that fills the shape's local bounds, at `massKg`. For a shape with no volume
// of its own (a mesh), which is why a concave mesh's inertia is a box's: documented as rough.
JPH::MassProperties boxMassProperties(const JPH::Shape& shape, float massKg) {
    const JPH::Vec3 size = JPH::Vec3::sMax(shape.GetLocalBounds().GetSize(), JPH::Vec3::sReplicate(0.01f));
    JPH::MassProperties mp;
    mp.SetMassAndInertiaOfSolidBox(size, massKg / (size.GetX() * size.GetY() * size.GetZ()));
    return mp;
}

// The mass properties a body takes for `shape`. target > 0 scales the shape's own to that mass; a shape with no
// mass (a mesh) gets a box's inertia from its bounds when there is a target. False when there is nothing to set.
bool massPropertiesFor(const JPH::Shape& shape, float targetKg, JPH::MassProperties& out) {
    out = shape.GetMassProperties();
    if (out.mMass > 0.0f) {
        if (targetKg > 0.0f) out.ScaleToMass(targetKg);
        return true;
    }
    if (targetKg > 0.0f) {
        out = boxMassProperties(shape, targetKg);
        return true;
    }
    return false;
}

float bodyMassKg(const JPH::BodyID& id) {
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0.0f;
    const JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    const float inv = mp ? mp->GetInverseMassUnchecked() : 0.0f;
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

// A mesh given a mass moves: its centre of mass goes to the bounds centre, where a box's inertia is right.
JPH::RefConst<JPH::Shape> wrapMovableMesh(const JPH::RefConst<JPH::Shape>& mesh) {
    const JPH::Vec3 centre = mesh->GetLocalBounds().GetCenter();
    return JPH::RefConst<JPH::Shape>(new JPH::OffsetCenterOfMassShape(mesh.GetPtr(), centre));
}

// A body whose shape its owner rebuilds (a vehicle chassis, a ragdoll part) or that has no rigid shape (a soft
// body) is refused. A character is not in the body table, so findBody has already refused it.
bool reshapable(int32_t handle, const JPH::BodyID& id) {
    if (g_world->vehicleOfBody.count(handle)) {
        AVER_WARN("[Physics] body {} is a vehicle chassis: change it with aver_phys_vehicle_set_chassis_*", handle);
        return fail(AbiError::Unsupported), false;
    }
    int32_t bone = -1;
    if (aver_phys_ragdoll_of_body(handle, &bone) != 0) {
        AVER_WARN("[Physics] body {} is a ragdoll part: its ragdoll owns its shape", handle);
        return fail(AbiError::Unsupported), false;
    }
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return fail(AbiError::BadHandle), false;
    if (lock.GetBody().IsSoftBody()) return fail(AbiError::Unsupported), false;
    return true;
}

// Tells every constraint that a body's centre of mass moved by `delta` (Jolt: the bodies do not know their joints).
void notifyConstraints(const JPH::BodyID& id, JPH::Vec3 delta) {
    if (delta.IsNearZero(1.0e-12f)) return;
    const JPH::Constraints all = g_world->system.GetConstraints();
    for (const JPH::Ref<JPH::Constraint>& c : all) c->NotifyShapeChanged(id, delta);
}

// Sets the motion properties of a body from `shape` (see massPropertiesFor). A static body has none.
void applyMass(const JPH::BodyID& id, const JPH::Shape& shape, float targetKg) {
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked();
    JPH::MassProperties props;
    if (mp && massPropertiesFor(shape, targetKg, props)) mp->SetMassProperties(mp->GetAllowedDOFs(), props);
}

// Puts `next` on the body (Jolt keeps the body ORIGIN where it was and moves the centre of mass), then fixes
// mass and constraints. targetKg > 0 is the mass the body ends with, 0 the new shape's own.
void commitShape(const JPH::BodyID& id, const JPH::RefConst<JPH::Shape>& next, float targetKg) {
    const JPH::RefConst<JPH::Shape> prev = bi().GetShape(id);
    if (prev == next) return;
    bi().SetShape(id, next.GetPtr(), false, JPH::EActivation::Activate);
    applyMass(id, *next, targetKg);
    if (prev) notifyConstraints(id, next->GetCenterOfMass() - prev->GetCenterOfMass());
}

// A mutable compound edited in place: the shape is the body's own and has already been changed.
void commitEdit(const JPH::BodyID& id, const JPH::Shape& shape, JPH::Vec3 prevCom, float targetKg) {
    bi().NotifyShapeChanged(id, prevCom, false, JPH::EActivation::Activate);
    applyMass(id, shape, targetKg);
    notifyConstraints(id, shape.GetCenterOfMass() - prevCom);
}

struct Editable {
    JPH::BodyID id;
    JPH::RefConst<JPH::Shape> shape;
    JPH::MutableCompoundShape* mc = nullptr;
};

// The body's shape as a mutable compound it owns. Edits go through the body's own shape, which is why it must
// BE the compound and not a decorator around one (a decorator caches the centre of mass and the bounds).
bool editable(int32_t body, Editable& e) {
    const JPH::BodyID* idp = findBody(body);
    if (!idp) return false;
    e.id = *idp;
    e.shape = bi().GetShape(e.id);
    if (!e.shape || e.shape->GetSubType() != JPH::EShapeSubType::MutableCompound) {
        AVER_WARN("[Physics] body {} is not a mutable compound", body);
        aver::setAbiError(AbiError::Unsupported);
        return false;
    }
    // The compound is single-owner and this body is its owner; Jolt's edit calls are not const.
    e.mc = const_cast<JPH::MutableCompoundShape*>(static_cast<const JPH::MutableCompoundShape*>(e.shape.GetPtr()));
    return true;
}

} // namespace

extern "C" {

int32_t aver_phys_body_from_shape(int32_t shape, const float* originCm, const float* quatXyzw,
                                  int32_t motionType, float massKg, const float* p, int32_t n) {
    if (!g_world) return fail(AbiError::NotInitialised);
    JPH::RefConst<JPH::Shape> sh = shapeTableGet(shape);
    if (!sh) return fail(AbiError::BadHandle);
    BodyParams bp;
    if (!parseBodyParams(p, n, bp)) return 0;
    JPH::EMotionType mt;
    switch (motionType) {
        case AVER_PHYS_MOTION_STATIC:    mt = JPH::EMotionType::Static;    break;
        case AVER_PHYS_MOTION_KINEMATIC: mt = JPH::EMotionType::Kinematic; break;
        case AVER_PHYS_MOTION_DYNAMIC:   mt = JPH::EMotionType::Dynamic;   break;
        default:
            AVER_WARN("[Physics] motion type {} is not one of static(0)/kinematic(1)/dynamic(2)", motionType);
            return fail(AbiError::InvalidArgument);
    }
    if (!std::isfinite(massKg) || (originCm && !allFiniteN(originCm, 3))) return fail(AbiError::InvalidArgument);
    JPH::Quat rot;
    if (!readJoltQuat(quatXyzw, rot)) return fail(AbiError::InvalidArgument);
    const Vec3 origin = originCm ? Vec3(originCm[0], originCm[1], originCm[2]) : Vec3(0.0f, 0.0f, 0.0f);

    const bool moving = mt != JPH::EMotionType::Static;
    bool boundsInertia = bp.inertiaFromBounds && massKg > 0.0f;
    bool placeholderMass = false;   // a kinematic body with no mass to derive: one Jolt never reads
    if (moving) {
        if (mt == JPH::EMotionType::Dynamic && sh->MustBeStatic()) {
            if (!isMeshLike(sh.GetPtr()) || !(massKg > 0.0f)) {
                AVER_WARN("[Physics] this shape cannot be dynamic (a plane, a heightfield, or a mesh given no mass)");
                return fail(AbiError::Unsupported);
            }
            sh = wrapMovableMesh(sh);
            boundsInertia = true;
        } else if (shapeMassKg(sh.GetPtr()) <= 0.0f) {
            if (isMeshLike(sh.GetPtr()) && massKg > 0.0f) { sh = wrapMovableMesh(sh); boundsInertia = true; }
            else placeholderMass = true;
        }
    }
    if (!shapeTableOwnerFree(sh.GetPtr())) {
        AVER_WARN("[Physics] this mutable compound already belongs to a body; build another");
        return fail(AbiError::InvalidArgument);
    }

    JPH::BodyCreationSettings s(sh.GetPtr(), JPH::RVec3(toJolt(origin)), rot, mt, Layers::encode(bp.layer, moving || bp.sensor));
    if (moving) {
        if (boundsInertia) {
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            s.mMassPropertiesOverride = boxMassProperties(*sh, massKg);
        } else if (placeholderMass) {
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            s.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sReplicate(1.0f), 1000.0f);
        } else if (massKg > 0.0f) {
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = massKg;
        }
    }
    s.mIsSensor = bp.sensor;
    s.mEnhancedInternalEdgeRemoval = bp.enhancedEdges;

    JPH::Body* body = bi().CreateBody(s);
    if (!body) { warnBodyLimit(); return fail(AbiError::AllocationFailed); }
    bi().AddBody(body->GetID(), moving && bp.activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, body->GetID());
    g_world->byId.emplace(body->GetID(), h);
    if (bp.sensor) g_world->sensors.emplace(h, true);
    shapeTableBindOwner(sh.GetPtr(), h);
    aver::setAbiError(AbiError::Ok);
    return h;
}

int32_t aver_phys_body_set_shape(int32_t body, int32_t shape, int32_t keepMass) {
    const JPH::BodyID* idp = findBody(body);
    if (!idp) return 0;
    const JPH::BodyID id = *idp;
    JPH::RefConst<JPH::Shape> next = shapeTableGet(shape);
    if (!next) return fail(AbiError::BadHandle);
    if (!reshapable(body, id)) return 0;

    const JPH::EMotionType mt = bi().GetMotionType(id);
    const float oldMass = bodyMassKg(id);
    if (mt == JPH::EMotionType::Dynamic && next->MustBeStatic()) {
        if (!isMeshLike(next.GetPtr()) || !(oldMass > 0.0f)) {
            AVER_WARN("[Physics] body {} is dynamic and this shape cannot be (a plane, a heightfield, or a mesh)", body);
            return fail(AbiError::Unsupported);
        }
        next = wrapMovableMesh(next);
    }
    if (!shapeTableOwnerFree(next.GetPtr())) {
        AVER_WARN("[Physics] this mutable compound already belongs to a body");
        return fail(AbiError::InvalidArgument);
    }
    // A shape with no mass of its own cannot give the body one, so the old mass stays whatever keepMass says.
    const float target = (keepMass != 0 || shapeMassKg(next.GetPtr()) <= 0.0f) ? oldMass : 0.0f;
    commitShape(id, next, target);
    shapeTableBindOwner(next.GetPtr(), body);
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_body_set_scale(int32_t body, const float* scaleXyz, int32_t keepMass) {
    const JPH::BodyID* idp = findBody(body);
    if (!idp) return 0;
    const JPH::BodyID id = *idp;
    if (!scaleXyz) return fail(AbiError::NullPointer);
    if (!allFiniteN(scaleXyz, 3) || std::abs(scaleXyz[0]) < 1.0e-4f || std::abs(scaleXyz[1]) < 1.0e-4f || std::abs(scaleXyz[2]) < 1.0e-4f) {
        AVER_WARN("[Physics] a body scale needs three non-zero finite components");
        return fail(AbiError::InvalidArgument);
    }
    if (!reshapable(body, id)) return 0;

    // ABSOLUTE: a body made by aver_phys_add_mesh_shape_body (or an earlier call here) wears a ScaledShape, and
    // the scale is applied to what is inside it.
    const JPH::RefConst<JPH::Shape> cur = bi().GetShape(id);
    if (!cur) return fail(AbiError::BadHandle);
    const JPH::Shape* inner = cur.GetPtr();
    JPH::Vec3 oldScale = JPH::Vec3::sReplicate(1.0f);
    if (cur->GetSubType() == JPH::EShapeSubType::Scaled) {
        const auto* sc = static_cast<const JPH::ScaledShape*>(cur.GetPtr());
        inner = sc->GetInnerShape();
        oldScale = sc->GetScale();
    }
    // The axis permutation without sign changes (see aver_phys_add_mesh_shape_body for the derivation).
    const JPH::Vec3 newScale(scaleXyz[1], scaleXyz[2], scaleXyz[0]);
    const JPH::Shape::ShapeResult res = inner->ScaleShape(newScale);
    if (res.HasError()) {
        AVER_WARN("[Physics] body {} scale: {}", body, res.GetError().c_str());
        return fail(AbiError::InvalidArgument);
    }
    const JPH::RefConst<JPH::Shape> next = res.Get();

    // Mass follows volume (density constant) unless the caller holds it.
    const float oldMass = bodyMassKg(id);
    const float om = shapeMassKg(cur.GetPtr()), nm = shapeMassKg(next.GetPtr());
    const float ratio = (om > 0.0f && nm > 0.0f)
        ? nm / om
        : std::abs(newScale.GetX() * newScale.GetY() * newScale.GetZ()) / std::abs(oldScale.GetX() * oldScale.GetY() * oldScale.GetZ());
    const float target = oldMass > 0.0f ? (keepMass != 0 ? oldMass : oldMass * ratio) : 0.0f;
    commitShape(id, next, target);
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_body_scale(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!outXyz) return fail(AbiError::NullPointer);
    const JPH::RefConst<JPH::Shape> cur = bi().GetShape(*id);
    if (!cur) return fail(AbiError::BadHandle);
    float s[3] = {1.0f, 1.0f, 1.0f};
    if (cur->GetSubType() == JPH::EShapeSubType::Scaled) {
        const JPH::Vec3 j = static_cast<const JPH::ScaledShape*>(cur.GetPtr())->GetScale();
        // Jolt (x, y, z) is engine (y, z, x) back again.
        s[0] = j.GetZ(); s[1] = j.GetX(); s[2] = j.GetY();
    }
    outXyz[0] = s[0]; outXyz[1] = s[1]; outXyz[2] = s[2];
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_body_origin(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!outXyz) return fail(AbiError::NullPointer);
    writeVec(outXyz, fromJolt(JPH::Vec3(bi().GetPosition(*id))));
    return 1;
}

int32_t aver_phys_body_set_enhanced_edges(int32_t body, int32_t on) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return fail(AbiError::BadHandle);
    if (!lock.GetBody().IsRigidBody()) return fail(AbiError::Unsupported);
    lock.GetBody().SetEnhancedInternalEdgeRemoval(on != 0);
    return 1;
}

// ---- mutable compound edits ----------------------------------------------------------------------------------

int32_t aver_phys_body_compound_add(int32_t body, int32_t child, const float* posCm, const float* quatXyzw, int32_t userData) {
    Editable e;
    if (!editable(body, e)) return -1;
    const JPH::RefConst<JPH::Shape> sub = shapeTableGet(child);
    if (!sub) { fail(AbiError::BadHandle); return -1; }
    if (bi().GetMotionType(e.id) != JPH::EMotionType::Static && sub->MustBeStatic()) {
        AVER_WARN("[Physics] a plane, heightfield or mesh cannot join a compound on a moving body");
        fail(AbiError::Unsupported);
        return -1;
    }
    if (posCm && !allFiniteN(posCm, 3)) { fail(AbiError::InvalidArgument); return -1; }
    JPH::Quat rot;
    if (!readJoltQuat(quatXyzw, rot)) { fail(AbiError::InvalidArgument); return -1; }
    const JPH::Vec3 pos = posCm ? toJolt(Vec3(posCm[0], posCm[1], posCm[2])) : JPH::Vec3::sZero();

    const JPH::Vec3 prevCom = e.mc->GetCenterOfMass();
    const float mass = bodyMassKg(e.id);
    uint32_t index = 0;
    {
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), e.id);
        if (!lock.Succeeded()) { fail(AbiError::BadHandle); return -1; }
        index = e.mc->AddShape(pos, rot, sub.GetPtr(), static_cast<uint32_t>(userData));
        e.mc->AdjustCenterOfMass();
    }
    commitEdit(e.id, *e.mc, prevCom, mass);
    aver::setAbiError(AbiError::Ok);
    return static_cast<int32_t>(index);
}

int32_t aver_phys_body_compound_remove(int32_t body, int32_t index) {
    Editable e;
    if (!editable(body, e)) return 0;
    const uint32_t count = e.mc->GetNumSubShapes();
    if (index < 0 || static_cast<uint32_t>(index) >= count) return fail(AbiError::OutOfRange);
    if (count == 1) {
        AVER_WARN("[Physics] body {}: the last child of a compound cannot be removed (swap the shape instead)", body);
        return fail(AbiError::Unsupported);
    }
    const JPH::Vec3 prevCom = e.mc->GetCenterOfMass();
    const float mass = bodyMassKg(e.id);
    {
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), e.id);
        if (!lock.Succeeded()) return fail(AbiError::BadHandle);
        e.mc->RemoveShape(static_cast<uint32_t>(index));
        e.mc->AdjustCenterOfMass();
    }
    commitEdit(e.id, *e.mc, prevCom, mass);
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_body_compound_set_child(int32_t body, int32_t index, const float* posCm, const float* quatXyzw) {
    Editable e;
    if (!editable(body, e)) return 0;
    if (index < 0 || static_cast<uint32_t>(index) >= e.mc->GetNumSubShapes()) return fail(AbiError::OutOfRange);
    if (posCm && !allFiniteN(posCm, 3)) return fail(AbiError::InvalidArgument);
    JPH::Quat rot;
    if (!readJoltQuat(quatXyzw, rot)) return fail(AbiError::InvalidArgument);

    // A NULL position or rotation keeps the child's current one. Positions are in the compound's origin frame,
    // so the current one is read back through the child's centre-of-mass offset.
    const JPH::CompoundShape::SubShape& cur = e.mc->GetSubShape(static_cast<uint32_t>(index));
    const JPH::Quat curRot = cur.GetRotation();
    const JPH::Vec3 curPos = cur.GetPositionCOM() + e.mc->GetCenterOfMass() - curRot * cur.mShape->GetCenterOfMass();
    const JPH::Vec3 pos = posCm ? toJolt(Vec3(posCm[0], posCm[1], posCm[2])) : curPos;
    const JPH::Quat newRot = quatXyzw ? rot : curRot;

    const JPH::Vec3 prevCom = e.mc->GetCenterOfMass();
    const float mass = bodyMassKg(e.id);
    {
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), e.id);
        if (!lock.Succeeded()) return fail(AbiError::BadHandle);
        e.mc->ModifyShape(static_cast<uint32_t>(index), pos, newRot);
        e.mc->AdjustCenterOfMass();
    }
    commitEdit(e.id, *e.mc, prevCom, mass);
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_body_compound_count(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return -1;
    const JPH::RefConst<JPH::Shape> s = bi().GetShape(*id);
    if (!s || s->GetType() != JPH::EShapeType::Compound) { aver::setAbiError(AbiError::Unsupported); return -1; }
    aver::setAbiError(AbiError::Ok);
    return static_cast<int32_t>(static_cast<const JPH::CompoundShape*>(s.GetPtr())->GetNumSubShapes());
}

// ---- collision triangles ------------------------------------------------------------------------------------

int32_t aver_phys_body_triangles(int32_t body, const float* regionMin, const float* regionMax,
                                 float* outXyz, int32_t* outSurface, int32_t maxTriangles) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!outXyz) return fail(AbiError::NullPointer);
    if (maxTriangles <= 0) return fail(AbiError::InvalidArgument);
    const JPH::TransformedShape ts = bi().GetTransformedShape(*id);
    if (!ts.mShape) return fail(AbiError::BadHandle);

    JPH::AABox box;
    if (regionMin && regionMax) {
        if (!allFiniteN(regionMin, 3) || !allFiniteN(regionMax, 3)) return fail(AbiError::InvalidArgument);
        // Engine x is Jolt's -z, so the two corners are not min and max: encapsulating sorts them.
        box.Encapsulate(toJolt(Vec3(regionMin[0], regionMin[1], regionMin[2])));
        box.Encapsulate(toJolt(Vec3(regionMax[0], regionMax[1], regionMax[2])));
    } else {
        box = ts.GetWorldSpaceBounds();
    }

    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    ts.CollectTransformedShapes(box, leaves);

    constexpr int kBatch = 64;
    JPH::Float3 verts[kBatch * 3];
    const JPH::PhysicsMaterial* mats[kBatch];
    int32_t written = 0;
    for (const JPH::TransformedShape& leaf : leaves.mHits) {
        JPH::Shape::GetTrianglesContext ctx;
        leaf.GetTrianglesStart(ctx, box, JPH::RVec3::sZero());
        for (;;) {
            const int got = leaf.GetTrianglesNext(ctx, kBatch, verts, mats);
            if (got <= 0) break;
            for (int t = 0; t < got; ++t) {
                if (written >= maxTriangles) { aver::setAbiError(AbiError::Ok); return written; }
                for (int v = 0; v < 3; ++v) {
                    const JPH::Float3& f = verts[t * 3 + v];
                    writeVec(outXyz + written * 9 + v * 3, fromJolt(JPH::Vec3(f.x, f.y, f.z)));
                }
                if (outSurface) outSurface[written] = surfaceIdOf(mats[t]);
                ++written;
            }
        }
    }
    aver::setAbiError(AbiError::Ok);
    return written;
}

// ---- vehicle chassis --------------------------------------------------------------------------------------

int32_t aver_phys_vehicle_set_chassis_shape(int32_t v, int32_t shape) {
    VehicleEntry* e = findVehicle(v);
    if (!e) return 0;
    if (e->constraint != nullptr) return fail(AbiError::Unsupported);   // the constraint holds the old chassis
    const JPH::RefConst<JPH::Shape> sh = shapeTableGet(shape);
    if (!sh) return fail(AbiError::BadHandle);
    if (sh->MustBeStatic() || shapeMassKg(sh.GetPtr()) <= 0.0f) {
        AVER_WARN("[Physics] vehicle {}: a chassis needs a shape with a mass (not a plane, mesh or heightfield)", v);
        return fail(AbiError::Unsupported);
    }
    if (!shapeTableOwnerFree(sh.GetPtr())) return fail(AbiError::InvalidArgument);

    // Rebuilt under the same handle like aver_phys_vehicle_set_chassis_boxes: mass and the requested centre of
    // mass come from the builder's numbers, the pose and entity stamp from the old body.
    const JPH::Vec3 com = toJolt(Vec3(e->comCm[0], e->comCm[1], e->comCm[2]));
    const JPH::RefConst<JPH::Shape> chassis = new JPH::OffsetCenterOfMassShape(sh.GetPtr(), com - sh->GetCenterOfMass());

    const auto old = g_world->bodies.find(e->body);
    if (old == g_world->bodies.end()) return fail(AbiError::BadHandle);
    const JPH::BodyID oldId = old->second;
    JPH::BodyCreationSettings s(chassis.GetPtr(), bi().GetPosition(oldId), bi().GetRotation(oldId),
                                JPH::EMotionType::Dynamic, Layers::encode(0, true));
    s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    s.mMassPropertiesOverride.mMass = e->massKg;
    s.mUserData = bi().GetUserData(oldId);
    JPH::Body* nb = bi().CreateBody(s);
    if (!nb) { warnBodyLimit(); return fail(AbiError::AllocationFailed); }
    bi().AddBody(nb->GetID(), JPH::EActivation::Activate);

    bi().RemoveBody(oldId);
    bi().DestroyBody(oldId);
    g_world->byId.erase(oldId);
    old->second = nb->GetID();
    g_world->byId[nb->GetID()] = e->body;

    e->boxes.clear();
    aver::setAbiError(AbiError::Ok);
    return 1;
}

} // extern "C"
