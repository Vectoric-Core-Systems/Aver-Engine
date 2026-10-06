// Registers the fifteen built-in components and their field tables, through the same public API a
// script-declared component uses.
#include "aver/scene/Components.hpp"

#include "aver/core/Assert.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/World.hpp"

#include <cstddef>

namespace aver::scene::detail {
namespace {

// Nested offsets are a sum: only the one-argument form of offsetof is required to work.
constexpr u16 kLocalPosition = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, position));
constexpr u16 kLocalRotation = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, rotation));
constexpr u16 kLocalScale    = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, scale));

// Aborts unless the component verified and landed on the dense id the ABI publishes.
void expect(bool ok, u32 id, u32 want, const char* what) {
    AVER_ASSERTM(ok, what);
    AVER_ASSERTM(id == want, what);
}

} // namespace

// Registers every built-in component and its field table.
void registerBuiltinComponents(World& world) {
    {
        auto b = world.registerComponent<CLocal>("CLocal");
        b.field("position", FieldKind::Vec3, kLocalPosition)
            .field("rotation", FieldKind::Quat, kLocalRotation)
            .field("scale", FieldKind::Vec3, kLocalScale)
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CLocal, rev)), 0, /*readOnly*/ true);
        expect(b.verify(sizeof(CLocal)), b.typeId(), kComponentLocal, "CLocal");
    }
    {
        auto b = world.registerComponent<CWorld>("CWorld");
        b.field("matrix", FieldKind::Mat4, static_cast<u16>(offsetof(CWorld, m)), 0, /*readOnly*/ true)
            .field("composedLocalRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedLocalRev)), 0, true)
            .field("composedParentRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedParentRev)), 0, true)
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, rev)), 0, true);
        expect(b.verify(sizeof(CWorld)), b.typeId(), kComponentWorld, "CWorld");
    }
    {
        auto b = world.registerComponent<CHierarchy>("CHierarchy");
        b.field("parent", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, parent)), 0, /*readOnly*/ true)
            .field("firstChild", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, firstChild)), 0, true)
            .field("nextSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, nextSibling)), 0, true)
            .field("prevSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, prevSibling)), 0, true)
            .field("depth", FieldKind::I32, static_cast<u16>(offsetof(CHierarchy, depth)), 0, true);
        expect(b.verify(sizeof(CHierarchy)), b.typeId(), kComponentHierarchy, "CHierarchy");
    }
    {
        auto b = world.registerComponent<CName>("CName");
        b.field("objectId", FieldKind::I64, static_cast<u16>(offsetof(CName, objectId)))
            .field("offset", FieldKind::I32, static_cast<u16>(offsetof(CName, offset)), 0, /*readOnly*/ true)
            .field("len", FieldKind::I32, static_cast<u16>(offsetof(CName, len)), 0, true);
        expect(b.verify(sizeof(CName)), b.typeId(), kComponentName, "CName");
    }
    {
        auto b = world.registerComponent<CTags>("CTags");
        b.field("bits", FieldKind::I32, static_cast<u16>(offsetof(CTags, bits)));
        expect(b.verify(sizeof(CTags)), b.typeId(), kComponentTags, "CTags");
    }
    {
        auto b = world.registerComponent<CMeshRenderer>("CMeshRenderer");
        b.field("mesh", FieldKind::I64, static_cast<u16>(offsetof(CMeshRenderer, mesh)))
            .field("aabbMin", FieldKind::Vec3, static_cast<u16>(offsetof(CMeshRenderer, aabbMin)))
            .field("aabbMax", FieldKind::Vec3, static_cast<u16>(offsetof(CMeshRenderer, aabbMax)))
            .field("material", FieldKind::I32, static_cast<u16>(offsetof(CMeshRenderer, material)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CMeshRenderer, flags)))
            .field("dirty", FieldKind::I32, static_cast<u16>(offsetof(CMeshRenderer, dirty)), 0, /*readOnly*/ true);
        expect(b.verify(sizeof(CMeshRenderer)), b.typeId(), kComponentMeshRenderer, "CMeshRenderer");
    }
    {
        auto b = world.registerComponent<CLight>("CLight");
        b.field("kind", FieldKind::I32, static_cast<u16>(offsetof(CLight, kind)))
            .field("colour", FieldKind::Vec3, static_cast<u16>(offsetof(CLight, colour)))
            .field("intensityLux", FieldKind::F32, static_cast<u16>(offsetof(CLight, intensityLux)))
            .field("rangeCm", FieldKind::F32, static_cast<u16>(offsetof(CLight, rangeCm)))
            .field("innerCos", FieldKind::F32, static_cast<u16>(offsetof(CLight, innerCos)))
            .field("outerCos", FieldKind::F32, static_cast<u16>(offsetof(CLight, outerCos)))
            .field("widthCm", FieldKind::F32, static_cast<u16>(offsetof(CLight, widthCm)))
            .field("heightCm", FieldKind::F32, static_cast<u16>(offsetof(CLight, heightCm)))
            .field("iesProfile", FieldKind::I64, static_cast<u16>(offsetof(CLight, iesProfile)))
            .field("cookie", FieldKind::I64, static_cast<u16>(offsetof(CLight, cookie)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CLight, flags)))
            .field("sourceRadiusCm", FieldKind::F32, static_cast<u16>(offsetof(CLight, sourceRadiusCm)));
        expect(b.verify(sizeof(CLight)), b.typeId(), kComponentLight, "CLight");
    }
    {
        auto b = world.registerComponent<CCamera>("CCamera");
        b.field("fovYRad", FieldKind::F32, static_cast<u16>(offsetof(CCamera, fovYRad)))
            .field("nearCm", FieldKind::F32, static_cast<u16>(offsetof(CCamera, nearCm)))
            .field("farCm", FieldKind::F32, static_cast<u16>(offsetof(CCamera, farCm)))
            .field("priority", FieldKind::I32, static_cast<u16>(offsetof(CCamera, priority)));
        expect(b.verify(sizeof(CCamera)), b.typeId(), kComponentCamera, "CCamera");
    }
    // APPENDED, never inserted. Dense ids are registration order, so adding one anywhere but the end
    // shifts every id after it -- expect() aborts on the first mismatch, and anything that persisted
    // an id would silently address the wrong component.
    {
        auto b = world.registerComponent<CSkeletalMesh>("CSkeletalMesh");
        b.field("skeleton", FieldKind::I64, static_cast<u16>(offsetof(CSkeletalMesh, skeleton)))
            .field("boneCount", FieldKind::I32, static_cast<u16>(offsetof(CSkeletalMesh, boneCount)), 0, /*readOnly*/ true)
            .field("dirty", FieldKind::I32, static_cast<u16>(offsetof(CSkeletalMesh, dirty)), 0, true);
        expect(b.verify(sizeof(CSkeletalMesh)), b.typeId(), kComponentSkeletalMesh, "CSkeletalMesh");
    }
    {
        auto b = world.registerComponent<CAnimator>("CAnimator");
        b.field("clip", FieldKind::I64, static_cast<u16>(offsetof(CAnimator, clip)))
            .field("time", FieldKind::F32, static_cast<u16>(offsetof(CAnimator, time)))
            .field("speed", FieldKind::F32, static_cast<u16>(offsetof(CAnimator, speed)))
            .field("blendWeight", FieldKind::F32, static_cast<u16>(offsetof(CAnimator, blendWeight)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CAnimator, flags)));
        expect(b.verify(sizeof(CAnimator)), b.typeId(), kComponentAnimator, "CAnimator");
    }
    {
        auto b = world.registerComponent<CParticleEmitter>("CParticleEmitter");
        b.field("effect", FieldKind::I64, static_cast<u16>(offsetof(CParticleEmitter, effect)))
            .field("age", FieldKind::F32, static_cast<u16>(offsetof(CParticleEmitter, age)))
            .field("emitAccum", FieldKind::F32, static_cast<u16>(offsetof(CParticleEmitter, emitAccum)), 0, /*readOnly*/ true)
            .field("seed", FieldKind::I32, static_cast<u16>(offsetof(CParticleEmitter, seed)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CParticleEmitter, flags)));
        expect(b.verify(sizeof(CParticleEmitter)), b.typeId(), kComponentParticleEmitter, "CParticleEmitter");
    }
    {
        auto b = world.registerComponent<CAttachment>("CAttachment");
        b.field("socket", FieldKind::I64, static_cast<u16>(offsetof(CAttachment, socket)));
        expect(b.verify(sizeof(CAttachment)), b.typeId(), kComponentAttachment, "CAttachment");
    }
    {
        auto b = world.registerComponent<CSoftBody>("CSoftBody");
        b.field("maxDistanceCm", FieldKind::F32, static_cast<u16>(offsetof(CSoftBody, maxDistanceCm)))
            .field("compliance", FieldKind::F32, static_cast<u16>(offsetof(CSoftBody, compliance)))
            // READ-ONLY, and that is docs/CHUNKS.md 5.1 rather than a preference: `body` is a
            // process-local physics handle and means nothing in another run, so it must never reach
            // a serialised chunk. A read-only field cannot be written by the ABI or an editor, which
            // is what keeps it out.
            .field("body", FieldKind::I32, static_cast<u16>(offsetof(CSoftBody, body)), 0, /*readOnly*/ true)
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CSoftBody, flags)));
        expect(b.verify(sizeof(CSoftBody)), b.typeId(), kComponentSoftBody, "CSoftBody");
    }
    {
        auto b = world.registerComponent<CRigidBody>("CRigidBody");
        b.field("shapeKind", FieldKind::I32, static_cast<u16>(offsetof(CRigidBody, shapeKind)))
            .field("dims", FieldKind::Vec3, static_cast<u16>(offsetof(CRigidBody, dims)))
            .field("motionType", FieldKind::I32, static_cast<u16>(offsetof(CRigidBody, motionType)))
            .field("massKg", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, massKg)))
            .field("friction", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, friction)))
            .field("restitution", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, restitution)))
            .field("gravityFactor", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, gravityFactor)))
            .field("linearDamping", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, linearDamping)))
            .field("angularDamping", FieldKind::F32, static_cast<u16>(offsetof(CRigidBody, angularDamping)))
            .field("collisionLayer", FieldKind::I32, static_cast<u16>(offsetof(CRigidBody, collisionLayer)))
            .field("isSensor", FieldKind::Bool, static_cast<u16>(offsetof(CRigidBody, isSensor)))
            // READ-ONLY, and that is docs/CHUNKS.md 5.1 rather than a preference: `body` is a
            // process-local physics handle and means nothing in another run, so it must never reach
            // a serialised chunk. A read-only field cannot be written by the ABI or an editor, which
            // is what keeps it out.
            .field("body", FieldKind::I32, static_cast<u16>(offsetof(CRigidBody, body)), 0, /*readOnly*/ true);
        expect(b.verify(sizeof(CRigidBody)), b.typeId(), kComponentRigidBody, "CRigidBody");
    }
    {
        auto b = world.registerComponent<CJoint>("CJoint");
        b.field("jointType", FieldKind::I32, static_cast<u16>(offsetof(CJoint, jointType)))
            .field("otherEntity", FieldKind::Entity, static_cast<u16>(offsetof(CJoint, otherEntity)))
            .field("anchorLocal", FieldKind::Vec3, static_cast<u16>(offsetof(CJoint, anchorLocal)))
            .field("primaryAxis", FieldKind::Vec3, static_cast<u16>(offsetof(CJoint, primaryAxis)))
            .field("normalAxis", FieldKind::Vec3, static_cast<u16>(offsetof(CJoint, normalAxis)))
            .field("limitMin", FieldKind::F32, static_cast<u16>(offsetof(CJoint, limitMin)))
            .field("limitMax", FieldKind::F32, static_cast<u16>(offsetof(CJoint, limitMax)))
            .field("motorState", FieldKind::I32, static_cast<u16>(offsetof(CJoint, motorState)))
            .field("motorTarget", FieldKind::F32, static_cast<u16>(offsetof(CJoint, motorTarget)))
            // READ-ONLY for the identical reason as CRigidBody::body and CSoftBody::body: a
            // process-local physics handle must never reach a serialised chunk.
            .field("joint", FieldKind::I32, static_cast<u16>(offsetof(CJoint, joint)), 0, /*readOnly*/ true);
        expect(b.verify(sizeof(CJoint)), b.typeId(), kComponentJoint, "CJoint");
    }
}

} // namespace aver::scene::detail
