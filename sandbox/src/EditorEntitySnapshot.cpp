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
            // dirty=1 ONLY, not flags -- this used to force kMeshRendererVisible on too, which
            // silently overwrote the memcpy just above the instant it landed. That was fine while
            // the bit meant nothing outside the session, but it is now AUTHORED data a level saves
            // (OcWorldPlacement::visible; SandboxApp::authoredVisible/setAuthoredVisible), and the
            // memcpy already restored it byte-exact: Undo of a delete, Copy/Paste and Duplicate must
            // all keep whatever visibility the source actually had, not silently un-hide it. dirty
            // is different in kind -- pure GPU-upload bookkeeping the source's own byte copy would
            // otherwise carry over stale (possibly already 0, meaning "nothing to upload"), so it
            // still needs the explicit reseed a fresh entity's own EnsureMeshRenderer/SetVisible path
            // gives it.
            mr->dirty = 1;
        }
        // VERIFIED, NOT ASSUMED (particles slice 5's own instruction) -- and an adversarial re-check
        // (post-slice-5 review) found the first version of this fixup fired unconditionally, which
        // over-corrected: the generic byte-copy above is otherwise correct for CParticleEmitter --
        // there is no GPU handle to re-flag, matching CMeshRenderer's case, or blob offset to fix up,
        // matching why CName is excluded entirely -- BUT for restoreObjectId=false (Paste/Duplicate,
        // a NEW distinct entity) it carries the SOURCE entity's already-derived `seed` verbatim onto a
        // DIFFERENT entity. ParticleSystem::tick only ever assigns a seed when the field is still 0
        // (see ParticleSystem.cpp's seedFor), so a paste/duplicate landed here with seed nonzero skips
        // that assignment and both emitters' EmitterState::rng end up seeded identically -- bit-for-
        // bit the same spawn offsets, speeds and lifetimes every frame. A duplicated dust cloud would
        // visibly move in lockstep with its original instead of reading as a second, independent one.
        // Resetting to 0 makes the paste re-derive its own seed from ITS OWN entity handle at its
        // first tick, exactly as a freshly-placed emitter does.
        //
        // GATED ON !restoreObjectId, matching the objectId precedent immediately above in this same
        // function: restoreObjectId=true is Undo-of-delete / Redo-of-create -- the SAME logical entity
        // coming back, not a new one -- and every OTHER field on this component (effect, age, flags)
        // restores byte-exact for that case. Resetting seed unconditionally (the first version of this
        // fix) silently broke that for CParticleEmitter alone: an undone delete would re-derive a
        // fresh seed from whatever entity handle undo happened to allocate, so a deleted-then-undone
        // emitter's particle stream would visibly diverge from what was on screen before the delete,
        // even though nothing else about the entity changed. Confirmed with a standalone probe calling
        // this exact function with restoreObjectId=true before this gate existed: seed came back 0,
        // not the source's preserved value, while effect/age/flags all round-tripped byte-exact.
        if (c.type == scene::kComponentParticleEmitter && !restoreObjectId) {
            static_cast<scene::CParticleEmitter*>(dst)->seed = 0;
        }
    }
    return e;
}

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
