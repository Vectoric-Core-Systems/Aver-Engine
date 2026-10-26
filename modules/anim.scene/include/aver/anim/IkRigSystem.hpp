#pragma once
// CIkRig -- an .ocikrig on an entity -- and the runtime that applies it: foot placement over a host-supplied
// ground probe, the pelvis shift, script-driven goals and the chain stack. It is a MEMBER of ControlRigSystem
// (never a second pose modifier): after an entity's .ocrig ops, ControlRigSystem::applyTo calls apply() here.
// Design: docs/plans/wave5/IK_RETARGETING.md 4.2 / 4.3 / 4.5 / 4.7.
//
// REGISTERED DYNAMICALLY like CControlRig (Components.hpp and the scene ABI stay put). ATTACH THROUGH attachIk,
// NEVER world.addComponent: the zero-fill leaves `weight` 0 and the rig inert.
//
// GOALS, RIG INPUTS AND FOOT SMOOTHING ARE TRANSIENT: not saved, rebuilt by the script after a load. A goal a
// script never touched contributes nothing; a released goal (or a bound goal whose target died) eases to alpha 0
// rather than freezing.
#include "aver/anim/IkRigSolver.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aver::scene { class World; }

namespace aver::anim {

class AnimSystem;

struct CIkRig {
    u64 asset = 0;       // .ocikrig ObjectId
    f32 weight = 0.0f;   // zero-filled reads 0 = off: attach through attachIk, or write weight every time
    u32 flags = 0;       // kIkRigFeetOff | kIkRigGoalsOff | kIkRigBodyOff
    u32 reserved[2] = {};
};
static_assert(sizeof(CIkRig) == 24, "CIkRig must be padding-free");

// Casts a ray DOWN (-Z) in world space from `originWorld` for `distanceCm` against the physics layers in
// `layerMask`; reports the first hit's point and unit normal. The host's implementation skips `self`'s own body.
// This is the only physics contact IK has: Aver.Anim.Scene gains no edge.
using GroundProbeFn = bool (*)(scene::Entity self, const Vec3& originWorld, f32 distanceCm, u32 layerMask,
                               Vec3& outPoint, Vec3& outNormal, void* user);

// One goal a script drives, by name hash.
struct IkGoalState {
    Vec3 pos{0, 0, 0};
    Quat rot = Quat::identity();
    bool hasRot = false;
    u8 space = 0;                    // 0 world, 1 entity model
    bool havePos = false;            // a position (or a bind) was supplied
    bool script = false;             // false until a script touches it: an untouched goal contributes nothing
    bool weightSet = false;          // SetIkGoalWeight was called (SetIkGoal then leaves the alpha alone)
    // Bound: follows a target entity's socket each tick.
    bool bound = false;
    bool everResolved = false;       // the target has been followed at least once (before that the goal just waits)
    scene::Entity target = scene::kInvalidEntity;
    u64 socket = 0;
    Vec3 off{0, 0, 0};
    Quat rotOff = Quat::identity();
    f32 alpha = 0.0f, alphaGoal = 0.0f, alphaRate = 0.0f;
    f32 errorCm = 0.0f;              // OUT: the last solve
    bool reached = false;
};

class IkRigRuntime {
public:
    static u32 registerComponent(scene::World& world);
    static u32 typeId();
    static CIkRig* attach(scene::World& world, scene::Entity e, u64 asset, f32 weight);

    void init(scene::World& world, AnimSystem& anim) { world_ = &world; anim_ = &anim; }
    void setGroundProbe(GroundProbeFn fn, void* user) { probe_ = fn; probeUser_ = user; }
    bool hasGroundProbe() const { return probe_ != nullptr; }

    // Solves the entity's rig on `pose`. False when nothing changed. `probeBudget` is the most rays this tick may
    // still cast across all entities (ControlRigSystem::maxFootProbesPerTick).
    bool apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, const CIkRig& c, u32 probeBudget);

    // Once per tick (the first apply of a new AnimSystem tick): resets the probe counter and drops state of dead entities.
    void beginTick(u64 tickIndex);
    u32 probesLastTick() const { return probesLast_; }

    // Loaded rigs and bound forms. Call when content changes.
    void clear();
    // Goals, foot state and smoothing of every entity.
    void clearRuntime();
    // Floating-origin shift (LARGE_WORLD 2.2.5): every goal held in WORLD space (space 0, set by a script or resolved from a
    // bound socket) -= delta. Model-space goals, goal rotations and the model-space foot state are invariant.
    void rebase(const Vec3& delta);

    // The rig asset for `asset`, loaded on first use; nullptr when it fails (reported once).
    const IkRigAsset* assetFor(u64 asset);

    // ---- the script surface (the provider relays to these) ----
    // False when the entity has no CIkRig or the rig declares no such goal / foot: a typo is visible, not stored.
    bool setGoal(scene::Entity e, u64 goal, i32 space, const Vec3& pos, const Quat* rot);
    bool bindGoal(scene::Entity e, u64 goal, scene::Entity target, u64 socket, const Vec3& off, const Quat* rotOff);
    bool setGoalWeight(scene::Entity e, u64 goal, f32 weight, f32 blendSeconds);
    bool clearGoal(scene::Entity e, u64 goal);                         // goal 0 = every goal
    bool goalState(scene::Entity e, u64 goal, f32* alphaErrorReached) const;
    bool setFootWeight(scene::Entity e, u64 foot, f32 weight, f32 blendSeconds);   // foot 0 = every foot
    bool footState(scene::Entity e, u64 foot, f32* offsetPlantNormal) const;       // foot 0 = the pelvis

private:
    struct Info {
        IkRigAsset asset;
        bool ok = false;
        std::unordered_set<u64> goals, feet;
    };
    struct Bound {
        IkRigBound bound;
        bool warned = false;
    };
    struct FootRt { f32 alpha = 1.0f, goal = 1.0f, rate = 0.0f; };
    struct EntityRt {
        std::unordered_map<u64, IkGoalState> goals;
        FootRt all;
        std::unordered_map<u64, FootRt> feet;
        const Bound* bound = nullptr;
        u64 asset = 0;
        IkRigState state;
        IkRigReport report;
    };
    struct GroundCtx;

    const Info* infoFor(scene::Entity e, const CIkRig*& c) const;
    const Bound* boundFor(u64 asset, u64 skeletonId, const fmt::OcSkeleton& skel);
    bool resolveBound(const IkGoalState& g, Vec3& posWorld, Quat& rotWorld) const;
    static bool groundThunk(const Vec3& originModel, f32 distance, u32 layerMask, Vec3& pointModel, Vec3& normalModel, void* user);
    static bool curveThunk(u64 nameHash, f32& out, void* user);
    static void stepAlpha(f32& alpha, f32 goal, f32 rate, f32 dt);

    scene::World* world_ = nullptr;
    AnimSystem* anim_ = nullptr;
    GroundProbeFn probe_ = nullptr;
    void* probeUser_ = nullptr;
    std::unordered_map<u64, std::unique_ptr<Info>> infos_;
    std::map<std::pair<u64, u64>, std::unique_ptr<Bound>> bounds_;
    std::unordered_map<scene::Entity, EntityRt> ents_;
    IkScratch scratch_;
    std::vector<IkGoalValue> goalVals_;
    std::vector<f32> footAlphas_;
    u64 tickSeen_ = ~0ull;
    u32 probesThis_ = 0, probesLast_ = 0;
    std::unordered_set<u64> warnedMismatch_;
};

// The framework's IK provider (framework_ik_abi.h, aver_fw_set_ik_provider), with the ControlRigSystem as `user`.
// Declared here so the framework never links this module; the composition root hands it across:
// aver_fw_set_ik_provider(&anim::ikProvider, &anim::controlRigSystem()).
i32 ikProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI, void* user);

} // namespace aver::anim
