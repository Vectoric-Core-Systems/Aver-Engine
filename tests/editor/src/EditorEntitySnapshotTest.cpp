// Hand-run test for EditorEntitySnapshot.hpp/.cpp: the generic capture/restore pair
// describeEntity()/recreateFrom()/copySelection()/pasteClipboard()/duplicateSelection() in
// SandboxApp.cpp all build on. This is the headless proof that the mechanism undo-of-delete,
// redo-of-create, Copy, Paste and Duplicate actually share is correct in isolation: capture a live
// entity's components, destroy it, rebuild it from the snapshot, and check the rebuild is faithful.
// tests/editor/src/EditorUndoTest.cpp (Sandbox's own --undo-test) is what proves the SIX EDITOR
// COMMANDS themselves work end to end, through the real undo stack; this file proves the lower layer
// those commands are built on. Exit code = failure count.
#include "EditorEntitySnapshot.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <string>

using namespace aver;
using namespace aver::scene;
using aver::editor::EntitySnapshot;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Finds a captured component by type, or nullptr.
static const EntitySnapshot::Comp* findComp(const EntitySnapshot& s, u32 type) {
    for (const auto& c : s.components) if (c.type == type) return &c;
    return nullptr;
}

int main() {
    AVER_INFO("EditorEntitySnapshot test");
    World& w = World::instance();

    // ---- build a source entity with a mesh, a light and a tag bitmask, plus a custom object id ----
    Transform xf0;
    xf0.position = Vec3{100.0f, 200.0f, 300.0f};
    xf0.scale    = Vec3{2.0f, 2.0f, 2.0f};
    const Entity src = w.create("Meshes/snapshot_test.ocmesh", kInvalidEntity, xf0);
    check(src != kInvalidEntity, "source entity created");

    auto* mr = static_cast<CMeshRenderer*>(w.addComponent(src, kComponentMeshRenderer));
    check(mr != nullptr, "source entity got a CMeshRenderer");
    if (mr) {
        mr->mesh = 0xABCDEF0011223344ull;
        mr->material = 7;
        mr->flags = 0;          // deliberately NOT visible, and deliberately NOT dirty --
        mr->dirty = 0;          // instantiateEntity must force both back on (see its own comment)
        mr->aabbMin[0] = -3.0f; mr->aabbMin[1] = -4.0f; mr->aabbMin[2] = -5.0f;
        mr->aabbMax[0] =  3.0f; mr->aabbMax[1] =  4.0f; mr->aabbMax[2] =  5.0f;
    }
    auto* tags = static_cast<CTags*>(w.addComponent(src, kComponentTags));
    check(tags != nullptr, "source entity got a CTags");
    if (tags) tags->bits = 0x2A2Au;

    const u64 kCustomObjectId = 0xC0FFEEu;   // NOT fnv1a64 of the asset name, deliberately
    check(w.setObjectId(src, kCustomObjectId), "setObjectId accepted a custom value");
    check(w.objectId(src) == kCustomObjectId, "objectId reads back the custom value before capture");

    // ---------------------------------------------------------------------------- captureEntity()
    const EntitySnapshot snap = aver::editor::captureEntity(w, src);
    check(snap.asset == "Meshes/snapshot_test.ocmesh", "captureEntity records the asset name");
    check(snap.objectId == kCustomObjectId, "captureEntity records the (custom) object id");
    check(findComp(snap, kComponentLocal) == nullptr, "captureEntity does NOT generically copy CLocal");
    check(findComp(snap, kComponentWorld) == nullptr, "captureEntity does NOT copy CWorld (derived)");
    check(findComp(snap, kComponentHierarchy) == nullptr, "captureEntity does NOT copy CHierarchy (derived)");
    check(findComp(snap, kComponentName) == nullptr, "captureEntity does NOT copy CName (internal blob offsets)");
    const auto* capturedMr = findComp(snap, kComponentMeshRenderer);
    check(capturedMr != nullptr && capturedMr->bytes.size() == sizeof(CMeshRenderer),
          "captureEntity copied a CMeshRenderer of the right size");
    const auto* capturedTags = findComp(snap, kComponentTags);
    check(capturedTags != nullptr && capturedTags->bytes.size() == sizeof(CTags),
          "captureEntity copied a CTags of the right size");

    // Destroying the source is what a real Delete/Undo cycle does before recreateFrom() runs; do the
    // same here so the rebuild below is provably reading the SNAPSHOT, not the still-live original.
    w.destroy(src);
    w.flush();
    check(!w.valid(src), "the source entity is gone after destroy+flush");

    // ------------------------------------------------------------------------- instantiateEntity()
    Transform xf1;
    xf1.position = Vec3{9.0f, 8.0f, 7.0f};   // deliberately DIFFERENT from xf0 -- proves the
    xf1.scale    = Vec3{1.0f, 1.0f, 1.0f};   // transform is supplied at instantiate time, not captured
    const Entity rebuilt = aver::editor::instantiateEntity(w, snap, xf1, kInvalidEntity, /*restoreObjectId=*/true);
    check(rebuilt != kInvalidEntity, "instantiateEntity rebuilt an entity");
    check(rebuilt != src, "the rebuilt entity is a genuinely new handle, not the stale source one");

    const auto* rLoc = w.component<CLocal>(rebuilt, kComponentLocal);
    check(rLoc != nullptr && rLoc->xf.position.x == 9.0f && rLoc->xf.position.y == 8.0f && rLoc->xf.position.z == 7.0f,
          "the rebuilt entity uses the TRANSFORM PASSED IN, not the snapshot's original position");

    const auto* rMr = w.component<CMeshRenderer>(rebuilt, kComponentMeshRenderer);
    check(rMr != nullptr, "the rebuilt entity has a CMeshRenderer");
    if (rMr) {
        check(rMr->mesh == 0xABCDEF0011223344ull, "CMeshRenderer.mesh round-tripped byte-exact");
        check(rMr->material == 7, "CMeshRenderer.material round-tripped byte-exact");
        check(rMr->aabbMin[0] == -3.0f && rMr->aabbMax[2] == 5.0f, "CMeshRenderer AABB round-tripped byte-exact");
        check((rMr->flags & kMeshRendererVisible) != 0,
              "instantiateEntity forces the visible bit even though the source had it cleared");
        check(rMr->dirty == 1,
              "instantiateEntity forces dirty=1 (GPU-upload bookkeeping) even though the source had it cleared");
    }
    const auto* rTags = w.component<CTags>(rebuilt, kComponentTags);
    check(rTags != nullptr && rTags->bits == 0x2A2Au, "CTags round-tripped byte-exact");

    check(w.objectId(rebuilt) == kCustomObjectId,
          "restoreObjectId=true propagates the snapshot's (custom) object id");

    // --------------------------------------------------------- restoreObjectId=false (Paste/Duplicate)
    const Entity pasted = aver::editor::instantiateEntity(w, snap, xf1, kInvalidEntity, /*restoreObjectId=*/false);
    check(pasted != kInvalidEntity, "instantiateEntity with restoreObjectId=false still creates an entity");
    check(pasted != rebuilt, "the 'pasted' entity is distinct from the 'undone' one");
    check(w.objectId(pasted) != kCustomObjectId,
          "restoreObjectId=false does NOT clone the source's custom object id (Paste/Duplicate's own identity)");

    // ------------------------------------------------------- particles slice 5: CParticleEmitter.seed
    // Verifying, not assuming (this session's own standing rule): the generic mechanism above was
    // written before CParticleEmitter existed, and the component's own field registry entry alone
    // does not prove copy/paste treats it correctly -- see EditorEntitySnapshot.cpp's own comment on
    // why `seed` specifically needed a named touch-up, matching CMeshRenderer's `dirty` two blocks up.
    const Entity emitterSrc = w.create("ParticleEmitterSnapshotTest", kInvalidEntity, xf0);
    check(emitterSrc != kInvalidEntity, "particle emitter source entity created");
    auto* pe = static_cast<CParticleEmitter*>(w.addComponent(emitterSrc, kComponentParticleEmitter));
    check(pe != nullptr, "source entity got a CParticleEmitter");
    if (pe) {
        pe->effect = 0x1122334455667788ull;
        pe->age = 4.5f;
        pe->seed = 0xCAFEBABEu;   // as if ParticleSystem::tick had already assigned one
        pe->flags = kParticleEmitterStopped;
    }
    const EntitySnapshot peSnap = aver::editor::captureEntity(w, emitterSrc);
    const auto* capturedPe = findComp(peSnap, kComponentParticleEmitter);
    check(capturedPe != nullptr && capturedPe->bytes.size() == sizeof(CParticleEmitter),
          "captureEntity copied a CParticleEmitter of the right size");

    w.destroy(emitterSrc);
    w.flush();

    // restoreObjectId=false is the Paste/Duplicate shape -- the one this fix actually protects,
    // since Undo-of-delete (restoreObjectId=true) puts back the SAME logical entity a seed collision
    // cannot be wrong for.
    const Entity emitterPasted =
        aver::editor::instantiateEntity(w, peSnap, xf1, kInvalidEntity, /*restoreObjectId=*/false);
    check(emitterPasted != kInvalidEntity, "instantiateEntity rebuilt the particle emitter entity");
    const auto* rPe = w.component<CParticleEmitter>(emitterPasted, kComponentParticleEmitter);
    check(rPe != nullptr, "the rebuilt entity has a CParticleEmitter");
    if (rPe) {
        check(rPe->effect == 0x1122334455667788ull, "CParticleEmitter.effect round-tripped byte-exact");
        check(rPe->age == 4.5f, "CParticleEmitter.age round-tripped byte-exact (not a named exception)");
        check(rPe->flags == kParticleEmitterStopped, "CParticleEmitter.flags round-tripped byte-exact");
        check(rPe->seed == 0,
              "CParticleEmitter.seed is reset to 0 on paste, so the copy re-derives its OWN seed at its "
              "first tick instead of drawing the identical particle stream as its source");
    }

    // -------------------------------------------- restoreObjectId=true (Undo-of-delete / Redo-of-create)
    // ADVERSARIAL FOLLOW-UP: the check above only ever exercised restoreObjectId=false (Paste/
    // Duplicate). The seed=0 fixup lives in the SAME generic instantiateEntity() both branches share,
    // so "correct for paste" does not by itself prove "correct for undo" -- and a probe against the
    // pre-fix code showed it was NOT: seed came back 0 here too, even though effect/age/flags all
    // round-tripped byte-exact, meaning an undone delete re-derived a DIFFERENT particle stream than
    // the one on screen before the delete. instantiateEntity() now gates the reset on
    // !restoreObjectId (see its own comment); this is the regression check for that gate.
    const Entity emitterSrc2 = w.create("ParticleEmitterUndoSnapshotTest", kInvalidEntity, xf0);
    check(emitterSrc2 != kInvalidEntity, "second particle emitter source entity created");
    auto* pe2 = static_cast<CParticleEmitter*>(w.addComponent(emitterSrc2, kComponentParticleEmitter));
    check(pe2 != nullptr, "second source entity got a CParticleEmitter");
    if (pe2) {
        pe2->effect = 0x99AA99AA99AA99AAull;
        pe2->age    = 12.75f;
        pe2->seed   = 0x5EED1234u;   // as if ParticleSystem::tick had already assigned one
        pe2->flags  = 0;             // playing, not stopped
    }
    const EntitySnapshot peSnap2 = aver::editor::captureEntity(w, emitterSrc2);
    w.destroy(emitterSrc2);
    w.flush();

    const Entity emitterUndone =
        aver::editor::instantiateEntity(w, peSnap2, xf1, kInvalidEntity, /*restoreObjectId=*/true);
    check(emitterUndone != kInvalidEntity, "instantiateEntity(restoreObjectId=true) rebuilt the emitter");
    const auto* uPe = w.component<CParticleEmitter>(emitterUndone, kComponentParticleEmitter);
    check(uPe != nullptr, "the undone entity has a CParticleEmitter");
    if (uPe) {
        check(uPe->effect == 0x99AA99AA99AA99AAull, "undo: CParticleEmitter.effect round-tripped byte-exact");
        check(uPe->age == 12.75f, "undo: CParticleEmitter.age round-tripped byte-exact");
        check(uPe->seed == 0x5EED1234u,
              "undo: CParticleEmitter.seed round-trips byte-exact too (restoreObjectId=true is the SAME "
              "logical entity coming back, not a new one -- it must reproduce the same particle stream, "
              "unlike paste/duplicate above)");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
