#pragma once
// Generalizes the editor's "describe a live entity well enough to rebuild it" step -- previously
// SandboxApp.cpp's describeEntity()/recreateFrom(), which hardcoded exactly one component
// (CMeshRenderer) -- into a walk over WHATEVER components an entity actually carries. Undo of a
// delete, Copy, Paste and Duplicate all rebuild an entity from one of these, so a light, a camera, a
// skinned mesh, or a future script component now round-trips through all four instead of silently
// vanishing on the ones the old hardcoded pair never learned about.
//
// Deliberately free functions over scene::World, not a class: this is a stateless capture/restore
// pair. Giving it a class would only invite it to start owning things (an undo stack, a clipboard)
// that are the editor's business, not a snapshot's -- see SandboxApp.cpp's EditCmd/EditorClipboard
// (where the undo stack and clipboard actually live) for why they did NOT move here.
#if AVER_MODULE_SCENE
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"
#include "aver/scene/World.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aver::editor {

// Everything needed to recreate an entity, short of WHERE it goes (its transform is supplied
// separately at instantiate time -- see instantiateEntity below, and SandboxApp's own EditXform,
// which already carries a position/rotation/scale the rest of the undo system understands) and
// short of three components the World itself derives or manages, which a byte copy cannot honour:
//   - CWorld: fully recomputed by World::flush(); a captured copy would just be stale on arrival.
//   - CHierarchy: derived from setParent(). Nothing in this editor calls setParent() today (grep
//     across sandbox/ and modules/ finds zero call sites -- every entity is a root), so parent/child
//     round-tripping is a documented, currently-unreachable gap, not a silent one.
//   - CName: its objectId is the one externally-meaningful part (captured below, separately, via
//     World::objectId()/setObjectId()); its {offset,len} pair is a byte range into World's OWN
//     internal name blob and is not portable across a copy at all.
struct EntitySnapshot {
    std::string asset;     // the name passed to World::create() -- an .ocmesh path today

    // World::objectId(). WORTH BEING PRECISE ABOUT WHAT THIS IS: World::setName() computes it as
    // fnv1a64(name) (World.cpp), and OcWorld's own loader assigns placements the identical
    // fnv1a64(asset) by default (OcWorld.hpp) -- so for an entity nobody ever touched setObjectId()
    // on, this is "which ASSET the entity represents", not a per-placement unique id, and TWO
    // entities sharing an asset name legitimately share this value already, with or without this
    // struct's involvement. It stops being redundant with the name hash the moment something calls
    // World::setObjectId() to override it -- today that is chunk streaming's load path
    // (ChunkPayload.cpp), restoring a persisted value that may not match the CURRENT name hash
    // (e.g. after an asset rename). Capturing/restoring it here is what keeps a chunk-streamed
    // entity's overridden id intact across a delete+undo; see instantiateEntity()'s restoreObjectId
    // for why Paste/Duplicate deliberately do NOT propagate it the same way.
    u64 objectId = 0;

    // One other present component, byte-exact. Every registered type this entity carries besides
    // Local/World/Hierarchy/Name lands here, whatever it turns out to be -- this is what makes
    // capture forward-compatible with components this file has never heard of (CLight, CCamera,
    // CSkeletalMesh, CAnimator today; anything registered later for free).
    struct Comp { u32 type = 0; std::vector<std::byte> bytes; };
    std::vector<Comp> components;
};

// Captures `e`. Cheap: a couple hundred bytes for the entities the editor itself creates today
// (CMeshRenderer only), bounded at roughly 1KB for a hypothetical entity carrying every built-in
// component at once. Only ever called from a user action (Copy/Delete/Paste/Duplicate/Undo/Redo),
// never per frame, so there is no budget concern here regardless of size.
EntitySnapshot captureEntity(scene::World& world, scene::Entity e);

// Rebuilds an entity from a snapshot at `xf`, parented under `parent` (root by default -- see the
// CHierarchy note above). Reattaches every captured component by raw byte copy -- EXCEPT two named
// touch-ups afterward, not a general post-processing hook:
//   - CMeshRenderer: `dirty` is GPU-upload bookkeeping, not editor state, and a byte-for-byte copy of
//     an entity the renderer had already uploaded would restore dirty=0, and the rebuilt copy would
//     never reach the GPU. The visible bit is NOT touched -- it is AUTHORED data now (a level saves
//     it; see OcWorldPlacement::visible), so the byte copy just above is left to stand: Undo of a
//     delete, Copy/Paste and Duplicate all keep whatever visibility the source actually had, which
//     is the whole point of a byte-exact restore.
//   - CParticleEmitter: `seed` is derived, not authored -- ParticleSystem::tick assigns one from the
//     entity's own handle only while the field reads 0 (see ParticleSystem.cpp's seedFor). A raw copy
//     carries the SOURCE's already-nonzero seed onto a DIFFERENT entity, and two emitters seeded
//     identically draw bit-identical particle streams -- a duplicate that visibly moves in lockstep
//     with its original. Resetting to 0 makes the copy re-derive its own on its first tick. GATED ON
//     restoreObjectId=false (Paste/Duplicate) ONLY -- restoreObjectId=true (Undo/Redo) is the SAME
//     logical entity coming back, and must round-trip seed byte-exact like every other field, not
//     re-derive a different one from whatever handle undo happens to allocate.
//
// restoreObjectId: true propagates the snapshot's objectId onto the new entity via setObjectId();
// false leaves whatever World::create() assigned on its own (fnv1a64 of the name -- see
// EntitySnapshot::objectId's own comment for why that is usually the SAME value anyway). Undo-of-
// delete and redo-of-create (recreateFrom in SandboxApp.cpp) pass true: the point of undo is that
// the SAME logical entity comes back, including an objectId a chunk-streaming load may have
// overridden away from the name hash. Paste and Duplicate pass false: they are making a NEW, distinct
// placement, and in the one case where this is not a no-op -- the source's objectId was itself
// overridden -- forcibly cloning that override would leave two independently-selectable entities
// both claiming the SOURCE's persisted identity, which is the wrong outcome for "make another one".
scene::Entity instantiateEntity(scene::World& world, const EntitySnapshot& snap, const Transform& xf,
                                 scene::Entity parent = scene::kInvalidEntity, bool restoreObjectId = true);

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
