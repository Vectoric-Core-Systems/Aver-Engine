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
        // position/rotation/scale are authored; rev is the transform's own revision, bumped by the
        // write path, so it is read-only over the generic ABI.
        b.field("position", FieldKind::Vec3, kLocalPosition)
            .field("rotation", FieldKind::Quat, kLocalRotation)
            .field("scale", FieldKind::Vec3, kLocalScale)
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CLocal, rev)), 0, /*readOnly*/ true);
        expect(b.verify(sizeof(CLocal)), b.typeId(), kComponentLocal, "CLocal");
    }
    {
        auto b = world.registerComponent<CWorld>("CWorld");
        // CWorld is entirely DERIVED — the propagation pass owns every byte of it — so all of it is
        // read-only over the generic ABI. A written world matrix is overwritten on the next flush;
        // a written revision desyncs the compare the pass runs.
        b.field("matrix", FieldKind::Mat4, static_cast<u16>(offsetof(CWorld, m)), 0, /*readOnly*/ true)
            .field("composedLocalRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedLocalRev)), 0, true)
            .field("composedParentRev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, composedParentRev)), 0, true)
            .field("rev", FieldKind::I32, static_cast<u16>(offsetof(CWorld, rev)), 0, true);
        expect(b.verify(sizeof(CWorld)), b.typeId(), kComponentWorld, "CWorld");
    }
    {
        auto b = world.registerComponent<CHierarchy>("CHierarchy");
        // The links and depth are managed by setParent (which keeps the sibling chain and the
        // topological order consistent); writing one raw would desync them, so the whole component is
        // read-only over the generic ABI. Reparenting goes through aver_scene_set_parent.
        b.field("parent", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, parent)), 0, /*readOnly*/ true)
            .field("firstChild", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, firstChild)), 0, true)
            .field("nextSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, nextSibling)), 0, true)
            .field("prevSibling", FieldKind::Entity, static_cast<u16>(offsetof(CHierarchy, prevSibling)), 0, true)
            .field("depth", FieldKind::I32, static_cast<u16>(offsetof(CHierarchy, depth)), 0, true);
        expect(b.verify(sizeof(CHierarchy)), b.typeId(), kComponentHierarchy, "CHierarchy");
    }
    {
        auto b = world.registerComponent<CName>("CName");
        // objectId is authored identity; offset/len are the cursor into the world name blob, managed
        // by setName. They are read-only over the generic ABI: a written offset is a slice cursor
        // pointing wherever the writer chose, which is exactly the out-of-bounds name read this closes.
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
            // dirty is upload bookkeeping the GPU path reads and clears, not authored state.
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
