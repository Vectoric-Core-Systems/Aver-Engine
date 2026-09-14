#include "PhysicsSceneSync.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS

#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_shapes_abi.h"

namespace aver::editor {
namespace {

// This entity's own world transform, decomposed from CWorld. Every entity World::create() makes
// carries a CWorld, but nothing here trusts that of an entity that might have been built some
// other way -- identity is the same "nothing authored" answer a missing component already gives
// everywhere else in this codebase.
Transform worldTransformOf(scene::World& world, scene::Entity e) {
    const auto* cw = world.component<scene::CWorld>(e, scene::kComponentWorld);
    return cw ? transformFromMatrix(cw->m) : Transform{};
}

// local -> world for a POINT, translation included. This engine's Mat4 is ROW-VECTOR (see
// SoftBodyScene.cpp's own transformPoint, which makes the identical trip in the opposite
// direction): world = local * M means component j = sum_i(local[i] * M.m[i][j]) + M.m[3][j], the
// translation ROW rather than a translation column. Getting this backwards is silent at the
// origin -- both conventions agree there -- and only visible once the transform carries a
// rotation and a translation together.
Vec3 transformLocalPoint(const f32 localXyz[3], const Mat4& m) {
    const f32 lx = localXyz[0], ly = localXyz[1], lz = localXyz[2];
    return Vec3{
        lx * m.m[0][0] + ly * m.m[1][0] + lz * m.m[2][0] + m.m[3][0],
        lx * m.m[0][1] + ly * m.m[1][1] + lz * m.m[2][1] + m.m[3][1],
        lx * m.m[0][2] + ly * m.m[1][2] + lz * m.m[2][2] + m.m[3][2]};
}

// ---- pass 1: bodies -------------------------------------------------------------------------

// Builds CRigidBody's shape at `p`, STATIC where physics_shapes_abi.h/physics_abi.h actually
// offer a static creator and DYNAMIC everywhere else. There is no aver_phys_add_kinematic_*
// entry point for any shape, and physics_abi.h has no aver_phys_add_static_sphere at all -- so a
// KINEMATIC body, and a STATIC sphere, both come out of this as DYNAMIC and are corrected by the
// caller with one aver_phys_body_set_motion_type call afterward.
int32_t createShapeBody(const scene::CRigidBody& rb, const Vec3& p, scene::Entity e) {
    const bool wantStatic = rb.motionType == AVER_PHYS_MOTION_STATIC;
    switch (rb.shapeKind) {
    case scene::kRigidBodyShapeBox:
        return wantStatic
            ? aver_phys_add_static_box(p.x, p.y, p.z, rb.dims[0], rb.dims[1], rb.dims[2])
            : aver_phys_add_dynamic_box(p.x, p.y, p.z, rb.dims[0], rb.dims[1], rb.dims[2], rb.massKg);
    case scene::kRigidBodyShapeSphere:
        // wantStatic is handled by the caller demoting this DYNAMIC body afterward -- see above.
        return aver_phys_add_dynamic_sphere(p.x, p.y, p.z, rb.dims[0], rb.massKg);
    case scene::kRigidBodyShapeCapsule: {
        // CRigidBody stores radius plus the HALF-height of the CYLINDRICAL portion only (JPH's own
        // convention -- Components.hpp's field comment). physics_shapes_abi.h's capsule creators
        // instead take the TOTAL height including both hemispherical caps, and that file's own
        // comment gives the inverse of exactly this conversion: "a capsule's cylinder half-height
        // is therefore height/2 - radius". This is that relationship solved for height.
        const f32 totalHeight = 2.0f * (rb.dims[1] + rb.dims[0]);
        return wantStatic
            ? aver_phys_add_static_capsule(p.x, p.y, p.z, rb.dims[0], totalHeight)
            : aver_phys_add_dynamic_capsule(p.x, p.y, p.z, rb.dims[0], totalHeight, rb.massKg);
    }
    case scene::kRigidBodyShapeCylinder: {
        // dims[1] is a half-height (Components.hpp); the ABI's cylinder creators take the FULL
        // end-to-end height instead -- a cylinder has no caps of their own radius to add, unlike
        // the capsule above, so doubling is the whole conversion.
        const f32 totalHeight = 2.0f * rb.dims[1];
        return wantStatic
            ? aver_phys_add_static_cylinder(p.x, p.y, p.z, rb.dims[0], totalHeight)
            : aver_phys_add_dynamic_cylinder(p.x, p.y, p.z, rb.dims[0], totalHeight, rb.massKg);
    }
    default:
        AVER_WARN("[PhysicsSceneSync] entity {} has an unrecognised CRigidBody::shapeKind {}; no "
                  "body created", e, rb.shapeKind);
        return 0;
    }
}

// A sensor's shape, at `p`. Only box and sphere have a sensor creator in physics_abi.h -- there
// is no sensor capsule or cylinder -- so a capsule/cylinder sensor is refused outright rather than
// silently standing a SOLID body in for the trigger volume an author actually asked for.
int32_t createSensorBody(const scene::CRigidBody& rb, const Vec3& p, scene::Entity e) {
    switch (rb.shapeKind) {
    case scene::kRigidBodyShapeBox:
        return aver_phys_add_sensor_box(p.x, p.y, p.z, rb.dims[0], rb.dims[1], rb.dims[2]);
    case scene::kRigidBodyShapeSphere:
        return aver_phys_add_sensor_sphere(p.x, p.y, p.z, rb.dims[0]);
    default:
        AVER_WARN("[PhysicsSceneSync] entity {} is a sensor CRigidBody with shapeKind {}, which has "
                  "no sensor shape in this ABI (only box and sphere); no body created", e, rb.shapeKind);
        return 0;
    }
}

// Creates the body a CRigidBody names, at the entity's WORLD position and rotation, and applies
// every material/motion property the ABI exposes a live setter for. Returns 0 if nothing was
// created (an unrecognised shape, or a sensor shape the ABI does not offer).
int32_t createRigidBody(const scene::CRigidBody& rb, const Transform& xf, scene::Entity e) {
    const bool sensor = rb.isSensor != 0;
    const int32_t body = sensor ? createSensorBody(rb, xf.position, e) : createShapeBody(rb, xf.position, e);
    if (!body) return 0;

    // No shape creator above takes a rotation -- every one places its shape axis-aligned at
    // (cx,cy,cz) -- so an authored rotation is always a second call, the same way this ABI treats
    // position and rotation as two separate setters everywhere else (aver_phys_body_set_position /
    // aver_phys_body_set_rotation).
    const Quat& q = xf.rotation;
    aver_phys_body_set_rotation(body, q.x, q.y, q.z, q.w);
    aver_phys_body_set_layer(body, rb.collisionLayer);
    // Stamps the body with the entity that owns it, so a raycast or an overlap can report not
    // just THAT something was hit but WHAT (physics_abi.h's own words on aver_phys_set_entity).
    // Harmless on a sensor too: overlap reporting benefits from it exactly as a solid hit does.
    aver_phys_set_entity(body, static_cast<int32_t>(e));

    if (sensor) return body;   // a sensor has no material or motion type to set

    // Unconditional rather than only for the cases createShapeBody could not honour itself
    // (KINEMATIC, and a STATIC sphere): setting a body's own motion type to what it already is
    // costs nothing (physics_abi.h documents it as an ordinary setter, not a one-time choice) and
    // this way createShapeBody's own STATIC/DYNAMIC split never has to be re-derived here to know
    // which cases still need correcting.
    aver_phys_body_set_motion_type(body, rb.motionType);
    aver_phys_body_set_friction(body, scene::rigidBodyFriction(rb));
    aver_phys_body_set_restitution(body, rb.restitution);
    aver_phys_body_set_gravity_factor(body, scene::rigidBodyGravityFactor(rb));
    aver_phys_body_set_damping(body, rb.linearDamping, rb.angularDamping);
    return body;
}

// ---- pass 2: joints --------------------------------------------------------------------------

// One CJoint's constraint, built at world-space `anchor`/`primary`/`normal`. Returns 0 for an
// unrecognised jointType.
int32_t createJoint(const scene::CJoint& j, int32_t bodyA, int32_t bodyB, const Vec3& anchor,
                     const Vec3& primary, const Vec3& normal, scene::Entity e) {
    const float point[3] = {anchor.x, anchor.y, anchor.z};
    switch (j.jointType) {
    case scene::kJointTypeFixed: {
        const float axisX[3] = {primary.x, primary.y, primary.z};
        const float axisY[3] = {normal.x, normal.y, normal.z};
        return aver_phys_joint_fixed(bodyA, bodyB, point, axisX, axisY);
    }
    case scene::kJointTypePoint:
        return aver_phys_joint_point(bodyA, bodyB, point);
    case scene::kJointTypeDistance:
        // CJoint carries ONE anchor, not one per body, so both ends of the distance constraint
        // are the same world point -- the "rope tied at a shared point" case, which is the only
        // one a single anchorLocal field can express.
        return aver_phys_joint_distance(bodyA, bodyB, point, point, j.limitMin, j.limitMax);
    case scene::kJointTypeHinge: {
        const float hingeAxis[3] = {primary.x, primary.y, primary.z};
        const float normalAxis[3] = {normal.x, normal.y, normal.z};
        const int32_t joint = aver_phys_joint_hinge(bodyA, bodyB, point, hingeAxis, normalAxis,
                                                    j.limitMin, j.limitMax);
        // HINGE AND SLIDER ONLY have a motor (physics_joints_abi.h); `axis` is ignored for them
        // and may be 0. Off is the zero-safe default, so this only ever does anything for a joint
        // an author deliberately turned a motor on for.
        if (joint && j.motorState != scene::kJointMotorOff)
            aver_phys_joint_set_motor(joint, 0, j.motorState, j.motorTarget);
        return joint;
    }
    case scene::kJointTypeSlider: {
        const float sliderAxis[3] = {primary.x, primary.y, primary.z};
        const float normalAxis[3] = {normal.x, normal.y, normal.z};
        const int32_t joint = aver_phys_joint_slider(bodyA, bodyB, point, sliderAxis, normalAxis,
                                                     j.limitMin, j.limitMax);
        if (joint && j.motorState != scene::kJointMotorOff)
            aver_phys_joint_set_motor(joint, 0, j.motorState, j.motorTarget);
        return joint;
    }
    case scene::kJointTypeCone: {
        const float twistAxis[3] = {primary.x, primary.y, primary.z};
        // Cone only ever reads limitMax, as halfConeAngleRad (Components.hpp's own doc on
        // CJoint::limitMax); limitMin has no meaning for this type and is left unread here too.
        return aver_phys_joint_cone(bodyA, bodyB, point, twistAxis, j.limitMax);
    }
    default:
        AVER_WARN("[PhysicsSceneSync] entity {} has an unrecognised CJoint::jointType {}; no joint "
                  "created", e, j.jointType);
        return 0;
    }
}

// Walks every CJoint and builds its constraint. Split out of syncPhysicsFromScene only so the
// "why two passes" comment lives once, beside the call, rather than inside this loop too.
void syncJointsPass(scene::World& world, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = world.at(i);
        if (world.destroyPending(e)) continue;
        auto* j = world.component<scene::CJoint>(e, scene::kComponentJoint);
        if (!j || j->joint != 0) continue;   // no component, or already synced once

        // This joint's OWN end: a joint with no CRigidBody of its own has nothing on this side to
        // constrain. By the time this pass runs, pass 1 above has already tried to create a body
        // for every CRigidBody in the world, so a still-zero handle here means creation failed
        // (an unrecognised shape, most likely) rather than "not synced yet".
        const auto* rbA = world.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
        if (!rbA || rbA->body == 0) {
            AVER_WARN("[PhysicsSceneSync] entity {} has a CJoint but no live CRigidBody of its own; "
                      "joint skipped", e);
            continue;
        }
        const int32_t bodyA = rbA->body;

        // ZERO MEANS THE WORLD (CJoint::otherEntity's own doc, matching AVER_PHYS_WORLD_BODY,
        // which is also 0) -- an author who never set a target gets a joint anchored to an
        // immovable frame, not a lookup failure.
        int32_t bodyB = AVER_PHYS_WORLD_BODY;
        if (j->otherEntity != scene::kInvalidEntity) {
            const auto* rbB = world.component<scene::CRigidBody>(j->otherEntity, scene::kComponentRigidBody);
            if (!rbB || rbB->body == 0) {
                AVER_WARN("[PhysicsSceneSync] entity {}'s CJoint targets entity {}, which has no "
                          "live CRigidBody; joint skipped", e, j->otherEntity);
                continue;
            }
            bodyB = rbB->body;
        }

        const auto* cw = world.component<scene::CWorld>(e, scene::kComponentWorld);
        const Mat4 worldM = cw ? cw->m : Mat4::identity();

        // THE SINGLE MOST LIKELY THING TO GET WRONG HERE: anchorLocal is entity-LOCAL space
        // (Components.hpp: "survives a level move"), but every aver_phys_joint_* creator wants a
        // WORLD-space point at the moment of creation (physics_joints_abi.h: "EVERY POINT AND AXIS
        // IS WORLD-SPACE"). CJoint::anchorLocal's own comment says outright what has to happen:
        // "Whatever spawns the physics joint from this component multiplies this by the entity's
        // CWorld first" -- this is that multiplication, and it must not be skipped.
        const Vec3 anchorWorld = transformLocalPoint(j->anchorLocal, worldM);

        // primaryAxis/normalAxis are DIRECTIONS, not points: rotated by the entity's world
        // ROTATION only. Translation obviously does not apply to a direction, and unlike the
        // anchor point above, a non-uniform world SCALE must not apply either -- it would stretch
        // a hinge/slider axis into something no longer unit length, or no longer perpendicular to
        // its partner axis, and every joint creator above that takes two axes requires them
        // perpendicular. transformFromMatrix already decomposes scale OUT of the rotation it
        // returns, so rotating by that (and renormalising as insurance) is the safe transform; a
        // full point-style transform through worldM would not be.
        const Quat worldRot = transformFromMatrix(worldM).rotation;
        const Vec3 primaryWorld =
            worldRot.rotate(Vec3{j->primaryAxis[0], j->primaryAxis[1], j->primaryAxis[2]}).getSafeNormal();
        const Vec3 normalWorld =
            worldRot.rotate(Vec3{j->normalAxis[0], j->normalAxis[1], j->normalAxis[2]}).getSafeNormal();

        const int32_t joint = createJoint(*j, bodyA, bodyB, anchorWorld, primaryWorld, normalWorld, e);
        if (joint) j->joint = joint;
    }
}

} // namespace

void syncPhysicsFromScene() {
    if (!aver_phys_ready()) {
        AVER_WARN("[PhysicsSceneSync] the physics world is not running; nothing synced");
        return;
    }
    scene::World& world = scene::World::instance();
    const u32 n = world.count();

    // ---- pass 1: bodies ----
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = world.at(i);
        if (world.destroyPending(e)) continue;
        auto* rb = world.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
        if (!rb || rb->body != 0) continue;   // no component, or already synced once
        rb->body = createRigidBody(*rb, worldTransformOf(world, e), e);
    }

    // ---- pass 2: joints ----
    // NECESSARILY AFTER pass 1 above, and not merged into it: a CJoint names its other end by
    // Entity and needs THAT entity's CRigidBody::body to already be a live handle before a
    // constraint between the two can be built. A single combined pass would create a joint
    // against a body that had not been made yet whenever its target entity happened to be walked
    // first in world order -- which entity that is depends on creation order, not on anything a
    // level author controls, so this would fail unpredictably rather than never.
    syncJointsPass(world, n);
}

void clearPhysicsFromScene() {
    scene::World& world = scene::World::instance();
    const u32 n = world.count();

    // Joints before bodies, mirroring "bodies before joints" on the way up: a joint referencing a
    // body this loop is about to remove must be torn down while both its ends still exist, not
    // dangling from a body that already went away.
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = world.at(i);
        auto* j = world.component<scene::CJoint>(e, scene::kComponentJoint);
        if (!j || j->joint == 0) continue;
        aver_phys_joint_remove(j->joint);
        j->joint = 0;
    }
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = world.at(i);
        auto* rb = world.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
        if (!rb || rb->body == 0) continue;
        aver_phys_remove_body(rb->body);
        rb->body = 0;
    }
}

} // namespace aver::editor

#endif // AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
