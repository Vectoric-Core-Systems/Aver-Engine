// See EditorEntitySnapshot.hpp for what this is and, more importantly, what it deliberately does
// NOT capture.
#include "EditorEntitySnapshot.hpp"
#if AVER_MODULE_SCENE
#include "aver/scene/Components.hpp"

#include <cstring>

namespace aver::editor {
namespace {

// Component types the World derives or manages itself, so a byte copy of one would be meaningless
// (CWorld), currently unreachable (CHierarchy -- see the header comment), or not portable at all
// (CName's blob offsets). Everything else present on the entity is captured generically.
bool isGenericallyCopyable(u32 type) {
    return type != scene::kComponentLocal && type != scene::kComponentWorld &&
           type != scene::kComponentHierarchy && type != scene::kComponentName;
}

} // namespace

EntitySnapshot captureEntity(scene::World& world, scene::Entity e) {
    EntitySnapshot snap;
    if (!world.valid(e)) return snap;
    snap.asset = world.name(e);
    snap.objectId = world.objectId(e);

    const u32 n = world.componentCount();
    for (u32 i = 0; i < n; ++i) {
        const u32 type = world.componentAt(i);
        if (!isGenericallyCopyable(type) || !world.hasComponent(e, type)) continue;
        const void* src = world.getComponent(e, type);
        const usize size = world.componentSize(type);
        if (!src || size == 0) continue;
        EntitySnapshot::Comp c;
        c.type = type;
        c.bytes.resize(size);
        std::memcpy(c.bytes.data(), src, size);
        snap.components.push_back(std::move(c));
    }
    return snap;
}

scene::Entity instantiateEntity(scene::World& world, const EntitySnapshot& snap, const Transform& xf,
                                 scene::Entity parent, bool restoreObjectId) {
    const scene::Entity e = world.create(snap.asset, parent, xf);
    if (e == scene::kInvalidEntity) return e;
    if (restoreObjectId && snap.objectId != 0) world.setObjectId(e, snap.objectId);

    for (const EntitySnapshot::Comp& c : snap.components) {
        if (c.bytes.empty()) continue;
        void* dst = world.addComponent(e, c.type);
        // A size mismatch means the component's layout changed between capture and restore (e.g. a
        // save loaded across an engine upgrade); refuse rather than write past what addComponent
        // handed back.
        if (!dst || world.componentSize(c.type) != c.bytes.size()) continue;
        std::memcpy(dst, c.bytes.data(), c.bytes.size());
        if (c.type == scene::kComponentMeshRenderer) {
            auto* mr = static_cast<scene::CMeshRenderer*>(dst);
            mr->flags |= scene::kMeshRendererVisible;
            mr->dirty = 1;
        }
    }
    return e;
}

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
