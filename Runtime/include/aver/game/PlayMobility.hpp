#pragma once
// Which entities MOVE during a play session, so the renderer can keep them out of what it bakes.
//
// Voxi's GI volume is a bake: it is revoxelised from scratch whenever any draw it holds changes
// (VoxiRenderer::giSnapshotUnchanged). In the editor that is right, since a moved object is an edit.
// In Play it cost more than the rest of the frame: the FirstPerson viewmodel is parented to the
// camera, so it moved every frame the player looked around and the whole volume was rebuilt every
// frame. MEASURED on PTTest/NewSponza (2026-09-28): voxelise 0.43 -> 9.00 ms, GI shadow 0.11 -> 3.40
// ms, frame 13.4 -> 30.5 ms, the gate rebuilding on 64 of 64 ticks on "world transforms".
//
// THE RULE is Unreal's Movable mobility, inferred instead of authored:
//  - Outside a session nothing is movable, so editor edits keep rebuilding GI.
//  - An entity alive when the session began is static until its world transform first changes, and
//    movable for the rest of the session after that. Sticky, so a crate that falls and settles costs
//    one rebuild, not one per start and stop.
//  - An entity alive at the start with a CAnimator whose clip is an OBJECT clip (kOcAnimObject) and no
//    CSkeletalMesh (an object-animated car, boat or fan) is movable from the start: it is going to
//    move, so waiting for the first move would only spend a rebuild finding that out. An animator on
//    any other clip never moves the entity's own transform and gets no such head start.
//  - An entity the host names through seedMovable (a physics-driven traffic car) is movable from the
//    start for the same reason: it is going to move, and its first move would cost the same rebuild.
//  - An entity born during the session (spawned actor, projectile, streamed chunk) is provisional:
//    movable until it has held one world transform for kSettleFrames frames, then static. A
//    streamed wall settles and joins the bake; a projectile never does.
//  - Everything under the possessed pawn (body, camera, viewmodel) is always movable.
//
// A movable draw still casts ray-traced shadows, shows in reflections and receives GI. It is only
// left out of the volume and the GI shadow map.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"

#include <vector>

namespace aver::scene { class World; }

namespace aver::game {

class PlayMobility {
public:
    // Frames an entity born during the session must hold still before it counts as static.
    static constexpr u32 kSettleFrames = 30;

    // Starts a session. Call BEFORE the session spawns anything (before aver_fw_begin_play): every
    // entity alive now is level content, everything created afterwards is born during the session.
    void begin(const scene::World& w);
    // Ends the session. Nothing is movable afterwards.
    void end();
    bool active() const { return active_; }

    // Declares level entities that a system OTHER than an object clip is about to move (the
    // physics cars of world::VehicleSystem) movable from their first frame, so each skips the GI
    // rebuild its first move would cost. Call after begin(), once the system that moves them has
    // been built. An entity born during the session, a dead one and a repeat are left as they are,
    // and nothing happens while inactive.
    void seedMovable(const std::vector<scene::Entity>& entities);

    // Once per rendered frame, before the first drawWorld of that frame. drawWorld runs more than
    // once per frame (depth prepass and colour), and the second call must see the first's answers
    // rather than count another frame of stillness.
    void beginFrame(scene::Entity movableRoot);

    // True when `e`'s draws must stay out of the GI bake this frame. `world` is the matrix the walk
    // draws the entity with, and must be `w.worldMatrix(e)` itself: the matrix is only hashed and
    // compared when World::worldRevision(e) has moved since the last look, so an unchanged revision
    // is taken to mean an unchanged matrix. Always false while inactive.
    //
    // COST, because this runs once per visible mesh entity per frame (about 33,000 in NeonDistrict): an
    // entity already movable returns at once, and every other one costs a slot load, one revision
    // compare and one membership test against the pawn's subtree, which is marked once per frame.
    bool movable(const scene::World& w, scene::Entity e, const Mat4& world);

    // Entities judged movable so far this session, for the log line saying the rule is doing
    // something.
    u32 movableCount() const { return movableCount_; }

private:
    enum class State : u8 { Unknown, Seeded, Static, Provisional, Movable };
    // 32 bytes, so the whole table of a 50,000-entity level is walked in 1.6 MB; the fields are
    // ordered to keep it that way.
    struct Slot {
        scene::Entity e = scene::kInvalidEntity;
        // World::worldRevision(e) when worldHash was taken. While it still matches, the matrix has not
        // been recomposed, so neither hashing it nor comparing the hash can say anything new.
        u32 worldRev = 0;
        u16 stillFrames = 0;
        State state = State::Unknown;
        u64 lastSeenFrame = 0;
        u64 worldHash = 0;
    };
    static_assert(kSettleFrames <= 0xFFFF, "Slot::stillFrames counts up to kSettleFrames");

    Slot& slotFor(scene::Entity e);
    // Rebuilds pawnTree_ and pawnMark_ from root_, once per frame (the first movable() after beginFrame).
    void markPawnSubtree(const scene::World& w);
    // True when `e` is root_ or beneath it, as of this frame's mark.
    bool inPawnSubtree(scene::Entity e) const;
    static u64 hashWorld(const Mat4& m);

    std::vector<Slot> slots_;
    scene::Entity root_ = scene::kInvalidEntity;
    // root_ and everything beneath it, as of frame pawnTreeFrame_. pawnMark_ holds the same set by entity
    // index (the handle living at that index, else kInvalidEntity), so asking "is this entity under the
    // pawn" is one load, where walking its ancestors cost a valid() and a parent() per hop for every
    // entity every frame. The set is a handful of entities, so marking it costs nothing.
    std::vector<scene::Entity> pawnTree_;
    std::vector<scene::Entity> pawnMark_;
    std::vector<scene::Entity> pawnStack_;   // markPawnSubtree's scratch
    u64 pawnTreeFrame_ = 0;                  // the frame_ the mark was built for; 0 = not yet
    u64 frame_ = 0;
    u32 movableCount_ = 0;
    bool active_ = false;
};

} // namespace aver::game

#endif
