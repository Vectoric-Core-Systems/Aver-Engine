#include "aver/scene/Components.hpp"

#include "aver/core/Assert.hpp"
#include "aver/scene/Fields.hpp"
#include "aver/scene/World.hpp"

#include <cstddef>

// The eight built-in components and their field tables. Every one goes through the same public
// registration a script-declared component uses, so nothing about them is privileged beyond being
// registered first — which is the only reason their ids are constants.
//
// The offsets are hand-written with offsetof rather than generated, so there is nothing to keep in
// sync at build time; the .verify(sizeof(T)) terminator is what makes a wrong or missing one a
// load-time error naming the component rather than a corrupt read three layers away.
namespace aver::scene::detail {
namespace {

// Nested offsets are spelled as a sum rather than offsetof(CLocal, xf.position): the one-argument
// form of the member designator is the only one the standard requires to work.
constexpr u16 kLocalPosition = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, position));
constexpr u16 kLocalRotation = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, rotation));
constexpr u16 kLocalScale    = static_cast<u16>(offsetof(CLocal, xf) + offsetof(Transform, scale));

// A built-in whose table does not verify, or which lands on a different dense id than the constant
// the ABI publishes, is a defect in THIS file — so it aborts rather than returning a value nobody
// checks. A script-declared component takes the same registration path and only gets a logged
// rejection, because its table comes from outside the engine.
void expect(bool ok, u32 id, u32 want, const char* what) {
    AVER_ASSERTM(ok, what);
    AVER_ASSERTM(id == want, what);
}

} // namespace

void registerBuiltinComponents(World& world) {
    {
        auto b = world.registerComponent<CLocal>("CLocal");
        b.field("position", FieldKind::Vec3, kLocalPosition)
            .field("rotation", FieldKind::Quat, kLocalRotation)
            .field("scale", FieldKind::Vec3, kLocalScale)
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CLocal, rev)));
        expect(b.verify(sizeof(CLocal)), b.typeId(), kComponentLocal, "CLocal");
    }
    {
        auto b = world.registerComponent<CWorld>("CWorld");
        b.field("matrix", FieldKind::Mat4, static_cast<u16>(offsetof(CWorld, m)))
            .field("composedLocalRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedLocalRev)))
            .field("composedParentRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedParentRev)))
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, rev)));
        expect(b.verify(sizeof(CWorld)), b.typeId(), kComponentWorld, "CWorld");
    }
    {
        auto b = world.registerComponent<CHierarchy>("CHierarchy");
        b.field("parent", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, parent)))
            .field("firstChild", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, firstChild)))
            .field("nextSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, nextSibling)))
            .field("prevSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, prevSibling)))
            .field("depth", FieldKind::I32, static_cast<u16>(offsetof(CHierarchy, depth)));
        expect(b.verify(sizeof(CHierarchy)), b.typeId(), kComponentHierarchy, "CHierarchy");
    }
    {
        auto b = world.registerComponent<CName>("CName");
        b.field("objectId", FieldKind::I64, static_cast<u16>(offsetof(CName, objectId)))
            .field("offset", FieldKind::I32, static_cast<u16>(offsetof(CName, offset)))
            .field("len", FieldKind::I32, static_cast<u16>(offsetof(CName, len)));
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
            .field("dirty", FieldKind::I32, static_cast<u16>(offsetof(CMeshRenderer, dirty)));
        expect(b.verify(sizeof(CMeshRenderer)), b.typeId(), kComponentMeshRenderer, "CMeshRenderer");
    }
    {
        auto b = world.registerComponent<CLight>("CLight");
        b.field("kind", FieldKind::I32, static_cast<u16>(offsetof(CLight, kind)))
            .field("colour", FieldKind::Vec3, static_cast<u16>(offsetof(CLight, colour)))
            .field("intensityLux", FieldKind::F32, static_cast<u16>(offsetof(CLight, intensityLux)))
            .field("rangeCm", FieldKind::F32, static_cast<u16>(offsetof(CLight, rangeCm)))
            .field("innerCos", FieldKind::F32, static_cast<u16>(offsetof(CLight, innerCos)))
            .field("outerCos", FieldKind::F32, static_cast<u16>(offsetof(CLight, outerCos)));
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
}

} // namespace aver::scene::detail
