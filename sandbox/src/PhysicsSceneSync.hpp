#pragma once
// Turns the two data-only physics components (CRigidBody, CJoint -- see Components.hpp) into a
// live Jolt body or constraint, and back. A component nothing reads is the shape this codebase
// keeps shipping and finding unused; this is what makes CRigidBody/CJoint mean something at run
// time. NOTHING CALLS THESE YET -- wiring them into level load is the orchestrator's job, not
// this file's.
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS

namespace aver::editor {

// Creates a physics body for every entity carrying a CRigidBody, and a constraint for every
// CJoint, writing the resulting handles back into the components (CRigidBody::body,
// CJoint::joint). An entity whose handle is already non-zero is left untouched, so calling this a
// second time cannot create a duplicate body or joint.
//
// BODIES BEFORE JOINTS, NECESSARILY: a CJoint names its other end by Entity and needs that
// entity's CRigidBody::body to already be a live handle before a constraint between the two can
// be built -- see the .cpp for why that forces two full passes rather than one.
void syncPhysicsFromScene();

// Destroys every body and joint syncPhysicsFromScene created, and zeroes CRigidBody::body /
// CJoint::joint back to 0 as it does -- the same "a non-zero handle means it already exists"
// contract the create side relies on to stay idempotent.
void clearPhysicsFromScene();

} // namespace aver::editor

#endif // AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
