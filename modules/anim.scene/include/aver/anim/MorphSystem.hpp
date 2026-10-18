// CMorphTargets and the runtime behind it: turns animation curves, script and sequencer overrides, lip-sync
// and face-rig poses into ONE sparse list of final blend-shape weights per entity (a MorphFrame), and applies
// face-rig joint poses to the pose just before the pose modifier. It never sees a mesh: the renderer reads
// frame(e) and declares each entity's target names back through declareTargets.
//
// Owned by AnimSystem (animSystem().morphs()), which calls evaluate() once per animated entity, then
// tickLoose() and resolveInherit() at the tail of its tick. Design: docs/plans/wave9/MORPH_TARGETS.md 2.5.
#pragma once

#include "aver/anim/FaceRig.hpp"
#include "aver/anim/Morph.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/anim/Viseme.hpp"
#include "aver/scene/World.hpp"

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace aver::anim {

class AnimSystem;

// CMorphTargets::flags
inline constexpr u32 kMorphOff           = 1;   // no output at all
inline constexpr u32 kMorphInheritParent = 2;   // the animation layer is the nearest ancestor's final frame
inline constexpr u32 kMorphIgnoreAnim    = 4;   // animation curves are not read

// Dynamically registered, zero-fill safe. A raw attach (all zeros) reads as: no face rig (built-in ARKit
// defaults), no flags, master weight 1.
struct CMorphTargets {
    u64 faceRig = 0;            // .ocface ObjectId; 0 = built-in defaults only
    u32 flags = 0;
    f32 weight = 0.0f;          // master scale; 0 reads as 1 (the CAnimator.blendWeight convention); use kMorphOff for off
    u32 reserved[2] = {};
};
static_assert(sizeof(CMorphTargets) == 24, "CMorphTargets must be padding-free");

struct MorphWeight { u64 id; f32 w; };            // id = fnv1a64(target name)

// One entity's final weights: sorted by id, only |w| >= kMorphActiveEps, and only targets the mesh declares
// once it has declared any.
struct MorphFrame {
    std::vector<MorphWeight> active;
    u32 revision = 0;           // bumped whenever the set or a weight changed
    bool moving = false;        // changed in the last evaluation
};

struct MorphDecl { u64 id; f32 defaultWeight; };  // declared by the renderer from the mesh

class MorphSystem {
public:
    enum class Layer : u8 { Sequencer = 0, Script = 1 };

    explicit MorphSystem(AnimSystem& owner) : owner_(owner) {}

    // Registers CMorphTargets (idempotent) and returns its type id. AnimSystem::registerComponents calls it.
    static u32 registerComponent(scene::World& world);
    static u32 typeId();
    // Attaches CMorphTargets with a fresh value over the zero-filled bytes (weight 1).
    static CMorphTargets* attach(scene::World& world, scene::Entity e, u64 rig = 0, u32 flags = 0);

    // ---- the frame: AnimSystem calls these ----
    // One entity's weights. `skel` and `pose` are null for an entity with no rig: the CAnimator clip's weight
    // curves are then sampled here. With a pose, its curves are read and face-rig joint poses are applied to
    // pose->local (before the pose modifier, so IK and ragdoll see them).
    void evaluate(scene::World& world, scene::Entity e, const fmt::OcSkeleton* skel, Pose* pose, f32 dt);
    // True when evaluate() has anything to do for an entity AnimSystem has no other reason to evaluate: it
    // carries CMorphTargets, its pose carries weight curves, or it still holds weights, overrides or lip-sync
    // (so a face that stopped being driven is evaluated once more and goes back to neutral).
    bool needs(const scene::World& world, scene::Entity e, const Pose* pose) const;
    // CMorphTargets entities nothing evaluated this tick (script or lip-sync driven only).
    void tickLoose(scene::World& world, f32 dt);
    // kMorphInheritParent children take their ancestor's final frame; then prunes dead entities' state.
    void resolveInherit(scene::World& world);

    // ---- the renderer's side ----
    const MorphFrame* frame(scene::Entity e) const;
    // The mesh's targets, by name hash and default weight; an empty span (the entity retired) forgets them.
    void declareTargets(scene::Entity e, std::span<const MorphDecl> targets);

    // ---- overrides ----
    // `blendSec` ramps the value (and the override's strength) in; 0 is immediate. Script overrides persist
    // until cleared. Sequencer overrides belong to a track (`owner`) and go with releaseOwner.
    bool setTarget(scene::World& world, scene::Entity e, u64 id, f32 w, f32 blendSec, Layer layer = Layer::Script, u64 owner = 0);
    bool setPose(scene::World& world, scene::Entity e, u64 id, f32 w, f32 blendSec, Layer layer = Layer::Script, u64 owner = 0);
    // id 0 = every override of the layer; kindMask 1 targets, 2 poses, 3 both. Ramps the strength out over
    // blendSec, then removes.
    bool clear(scene::Entity e, u64 id, u32 kindMask, f32 blendSec, Layer layer = Layer::Script, u64 owner = 0);
    void releaseOwner(u64 owner);
    // The FINAL weight (0 when inactive). False when the entity has no frame.
    bool weightOf(scene::Entity e, u64 id, f32& out) const;
    bool setFaceRig(scene::World& world, scene::Entity e, u64 asset);
    // Makes the entity addressable: attaches CMorphTargets when it has a CMeshRenderer and none yet (what the
    // provider does before any set call) and creates its state. False for a dead entity or one with no mesh
    // and no component.
    bool ensure(scene::World& world, scene::Entity e);

    // ---- lip-sync ----
    bool lipSyncPlay(scene::World& world, scene::Entity e, u64 track, f32 rate, f32 start, f32 strength);
    void lipSyncStop(scene::Entity e, f32 blendOut);
    bool lipSyncSetTime(scene::Entity e, f32 t);
    bool lipSyncQueue(scene::Entity e, u64 symbol, f32 startFromNow, f32 duration, f32 intensity);
    bool lipSyncState(scene::Entity e, f32& time, f32& length, f32& strength, bool& playing) const;

    // Drops transient state (overrides, lip-sync) at a Play/level boundary; caches stay.
    void clearRuntime();
    // Drops loaded assets (rigs, tracks) and what points into them. AnimSystem::clear calls it.
    void clear();
    u32 evaluatedLastTick() const { return lastEvaluated_; }
    // Test and tool surface.
    u32 trackedEntities() const { return static_cast<u32>(states_.size()); }
    u32 declaredCount(scene::Entity e) const;
    // The rig pose count the entity's CMorphTargets resolves to (0 = built-in only).
    u32 rigPoseCount(scene::Entity e) const;

private:
    struct Override {
        u64 id = 0;
        u32 kind = 0;            // kPoseCurveMorph (target) or kPoseCurveFace (pose)
        Layer layer = Layer::Script;
        f32 value = 0.0f, target = 0.0f, valueRate = 0.0f;     // value moves to target at valueRate per second
        f32 alpha = 0.0f, alphaTarget = 1.0f, alphaRate = 0.0f;
        u64 owner = 0;
    };
    struct LipState {
        std::vector<VisemeEvent> events;     // the playing track, resolved against the entity's rig
        std::vector<VisemeEvent> queued;     // script-queued events, on the queue clock
        bool playing = false;                // a track
        bool clockSet = false;               // lipSyncSetTime ran this tick: do not also advance
        f32 time = 0.0f, rate = 1.0f, strength = 1.0f, length = 0.0f;
        f32 qclock = 0.0f;
        f32 fade = 1.0f, fadeRate = 0.0f;    // stop blend-out: fade falls to 0 at fadeRate per second
        bool fading = false;
        bool active() const { return playing || !queued.empty(); }
    };
    struct EntityState {
        FaceRigRuntime rt;
        u64 rigId = ~0ull;                   // the rig rt is bound to; ~0 = never bound
        const FaceRigAsset* rig = nullptr;
        std::vector<MorphDecl> declared;     // sorted by id
        bool declaredKnown = false;
        std::vector<Override> overrides;
        LipState lip;
        MorphFrame frame;
        u64 stamp = ~0ull;                   // the AnimSystem tick that last evaluated it
        // Set for kMorphInheritParent entities: the weights before master / clamp, finished by resolveInherit.
        bool inheritPending = false;
        f32 master = 1.0f;
        bool off = false;
        std::vector<MorphWeight> own;
    };

    EntityState* stateFor(scene::World& world, scene::Entity e, bool create);
    void bindRig(EntityState& st, u64 rigId);
    const FaceRigAsset* rigAsset(u64 id);
    const VisemeTrack* trackAsset(u64 id);
    bool setOverride(scene::World& world, scene::Entity e, u32 kind, u64 id, f32 w, f32 blendSec, Layer layer, u64 owner);
    void stepOverrides(EntityState& st, f32 dt);
    void stepLip(EntityState& st, f32 dt, std::vector<MorphWeight>& poseW);
    void finish(EntityState& st, std::vector<MorphWeight>& w);

    AnimSystem& owner_;
    std::unordered_map<scene::Entity, EntityState> states_;
    std::unordered_map<u64, std::unique_ptr<FaceRigAsset>> rigs_;     // a null entry = failed load, not retried
    std::unordered_map<u64, std::unique_ptr<VisemeTrack>> tracks_;
    std::vector<scene::Entity> inherit_;
    // Scratch, so a tick allocates nothing after warm-up.
    std::vector<MorphWeight> tw_, pw_;
    std::vector<FaceTargetOut> expanded_;
    std::vector<VisemeSample> lipA_, lipB_;
    Pose animScratch_;
    u32 evalCount_ = 0, lastEvaluated_ = 0;
};

// framework_morph_abi.h provider: aver_fw_set_morph_provider(&anim::morphProvider, &anim::animSystem().morphs()).
// Entities cross the ABI as int32 handles; SET_* and LIPSYNC_* auto-attach CMorphTargets when the entity has a
// CMeshRenderer and none yet, and refuse a dead entity or one with no mesh.
i32 morphProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI, void* user);

} // namespace aver::anim
