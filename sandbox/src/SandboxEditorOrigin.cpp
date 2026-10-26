// The editor's order-80 participant and everything around a floating-origin shift that belongs to the editor (LARGE_WORLD 2.7).
//
//   - the gesture gate: a shift is refused while an edit gesture holds positions across frames, and ended when the camera is so
//     far out that it cannot wait (EditorOriginParticipant.hpp);
//   - the rebasers: undo and redo, clipboard, placeholder objects, the Play-start snapshots, the foliage brush's placed list
//     (EditorOriginRebase.hpp);
//   - the world.* console host and the overlays a shift stales;
//   - the save guard: the editor writes level records from the live scene frame, so with the origin off zero a save would write
//     positions a region off. It is refused, with the way back, until the editor writes absolute positions.
//
// New file, not new members of a big translation unit: the Debug COFF section cap rules out growing SandboxApp*.cpp.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE

#include "EditorOriginRebase.hpp"

namespace aver {

void SandboxApp::initEditorOrigin() {
    using namespace editor::originRebase;
    editor::EditorOriginParticipant& eo = editorOrigin_;

    // ---- the gesture gate (prepare) ----
    // A transform gesture (gizmo drag, nudge, Details drag) holds the pre-gesture transforms. Cancelling puts them back in the OLD
    // frame, before the shift, and closes the gesture without an undo entry.
    eo.addGesture("a transform edit is open",
        [this] { return editBeforeValid_ || dragging_ || nudgeEditOpen_; },
        [this] {
            scene::World& w = scene::World::instance();
            if (editBeforeValid_) {
                setSelectedXform(editBefore_);
                for (const auto& [id, xf] : multiMoveBefore_) {
                    const scene::Entity e = entityForEdit(id);
                    if (e != scene::kInvalidEntity && w.valid(e)) w.setLocalTransform(e, xf);
                }
            }
            editBeforeValid_ = false;
            multiMoveBefore_.clear();
            dragging_ = false;
            activeAxis_ = -1;
            nudgeEditOpen_ = false;
            rotDragDeg_ = 0.0f;
            rotAppliedDeg_ = 0.0f;
        });
    eo.addGesture("a selection rectangle is being dragged",
        [this] { return marqueeArmed_ || marqueeActive_; },
        [this] { marqueeArmed_ = false; marqueeActive_ = false; });
#if AVER_MODULE_LANDSCAPE
    // A stroke is ENDED, not abandoned: its undo entry is pushed for what was already painted, as when the mode changes mid-stroke.
    eo.addGesture("a sculpt stroke is open", [this] { return sculpting_; }, [this] { endSculptStroke(); });
#if AVER_WITH_IMGUI
    eo.addGesture("a paint stroke is open",
        [this] { return painting_ || landscape_.weightStrokeActive(); },
        [this] { endPaintStroke(); });
    eo.addGesture("a foliage stroke is open",
        [this] { return foliageStroking_; },
        [this] {
            foliageStroking_ = false;
            if (!foliageBatch_.batchIds.empty()) pushEdit(std::move(foliageBatch_));
            foliageBatch_ = EditCmd{};
        });
#if AVER_MODULE_FLUIDS
    eo.addGesture("a water edit is open",
        [this] { return waterGestureOpen_ || waterDrag_; },
        [this] { waterGestureEnd("Edit water"); waterDrag_ = false; });
#endif
#endif
#endif

    // ---- the rebasers (commit) ----
    eo.addRebaser("undo and redo", [this](const world::ShiftEvent& ev) {
        rebaseStacks(undoStack_, redoStack_, ev.deltaF, scene::kInvalidEntity);
    });
    eo.addRebaser("clipboard", [this](const world::ShiftEvent& ev) { rebaseClipboard(clipboard_, ev.deltaF); });
    eo.addRebaser("placeholder objects", [this](const world::ShiftEvent& ev) {
        for (MeshObj& o : objects_) shiftPos(o.pos, ev.deltaF);
    });
    eo.addRebaser("play snapshots", [this](const world::ShiftEvent& ev) {
        scene::World& w = scene::World::instance();
        rebasePlaySnapshot(playWorldSnapshot_, ev.deltaF,
                           [&w](scene::Entity e) { return w.valid(e) && w.parent(e) == scene::kInvalidEntity; });
#if AVER_MODULE_FRAMEWORK
        for (PlayClassSaved& s : playClassSnapshot_) shiftPos(s.xf.position, ev.deltaF);   // class roots
#endif
        if (playFromHere_) shiftPos(*playFromHere_, ev.deltaF);
    });
#if AVER_MODULE_LANDSCAPE && AVER_MODULE_SCENE && AVER_WITH_IMGUI
    eo.addRebaser("foliage brush", [this](const world::ShiftEvent& ev) {
        for (FoliagePlaced& f : foliagePlaced_) shiftPos(f.pos, ev.deltaF);
    });
#endif

    game::OriginBindings& b = origin_.bindings();
    b.editorPrepare = [this](const world::ShiftEvent& ev) { return editorOrigin_.prepare(ev); };
    b.editorCommit = [this](const world::ShiftEvent& ev) { editorOrigin_.commit(ev); };

    // ---- the world.* console ----
    worldHost_.origin = &origin_;
    worldHost_.camPos = &camPos_;
    worldHost_.onTeleport = [this] { streaming_.resetVelocityTracking(); };   // a jump, not a one-frame flight to here
    editor::worldConsoleHostSlot() = &worldHost_;
}

// After a shift committed (slot S0, which has the Engine): the overlays built from world positions are rebuilt. The collider overlay
// follows by its signature (rebuildColliderOverlay mixes the origin generation in); per-frame overlays read the entities.
void SandboxApp::afterOriginShift(Engine& e) {
#if AVER_MODULE_SYNAPSE
    // buildNavOverlay emits cell corners from the grid's own origin, which the Synapse participant has just moved.
    if (navMesh_) rebuildNavOverlay(e);
#else
    (void)e;
#endif
}

bool SandboxApp::originBlocksSave(const char* what) {
    const world::OriginCm o = origin_.origin();
    if (o == world::OriginCm{}) return false;
    static u32 warnedGeneration = ~0u;
    if (warnedGeneration != origin_.generation()) {
        warnedGeneration = origin_.generation();
        AVER_WARN("[Editor] {} refused: the world origin is at region ({}, {}), and the editor writes level positions in the scene frame. "
                  "Return to the first region with world.tp 0 0 0, then save.",
                  what, o.x / world::kOriginStepCm, o.y / world::kOriginStepCm);
    }
    cbStatus_ = "Cannot save away from the first region: world.tp 0 0 0 first";
    return true;
}

} // namespace aver

#endif // AVER_MODULE_SCENE
