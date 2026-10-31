// RagdollSystem: turns a skinned entity's skeleton into a physics ragdoll and back.
//
// It lives in AnimSystem's pose-modifier seam (the same one the control rig uses) and CHAINS: install
// keeps whatever modifier was there and calls it first, so a rig's IK still runs and the ragdoll sees
// the pose it produced. In kinematic and powered modes that animated pose, taken to world space
// through the entity's transform, is the ragdoll's target; in limp and powered modes the simulated
// pose is written back over the animated one (world -> local), blended by a weight that ramps in and
// out over the seconds the caller asked for.
//
// THE ENTITY'S OWN TRANSFORM IS NOT MOVED. The skin follows the simulated parts wherever they go, but
// CLocal stays where it was, so culling bounds and sockets-by-entity do not follow a ragdoll that has
// fallen away from its origin.
#pragma once

#include "aver/anim/AnimSystem.hpp"
#include "aver/ragdoll/RagdollBuilder.hpp"

#include <unordered_map>
#include <vector>

namespace aver::ragdoll {

// Matches AVER_PHYS_RAGDOLL_* in physics_ragdoll_abi.h.
enum class RagdollMode : i32 { Kinematic = 0, Limp = 1, Powered = 2 };

class RagdollSystem {
public:
    // Installs this as `anim`'s pose modifier, chaining `prev` (called first). When `prev` is null
    // the modifier already installed is captured instead, which needs AnimSystem::poseModifier().
    // Returns false and installs NOTHING when a modifier exists that cannot be chained (replacing it
    // would silently turn the control rig off).
    bool install(anim::AnimSystem& anim, scene::World& world,
                 anim::PoseModifierFn prev = nullptr, void* prevUser = nullptr);
    // Puts the chained modifier back.
    void uninstall();

    // Advances blend weights, finishes stops, and drops ragdolls whose entity died. Call once a frame
    // BEFORE animSystem().tick, after the physics step.
    void tick(f32 dt);

    // Begins (or retargets) a ragdoll on `e`, blending the simulated pose in over `blendSeconds`.
    // The ragdoll is built from the entity's current animated pose. False when `e` has no skeleton or
    // the skeleton yields no parts.
    bool start(scene::Entity e, RagdollMode mode, f32 blendSeconds);
    // Blends back to animation over `blendSeconds`, then destroys the ragdoll. 0 destroys now.
    bool stop(scene::Entity e, f32 blendSeconds);
    // The requested mode, or -1 when `e` has no ragdoll (also after a stop has finished).
    i32  mode(scene::Entity e) const;
    // Powered-mode motor strength, 0..1.
    bool setStrength(scene::Entity e, f32 strength);
    // A world-space impulse (kg*cm/s) on the part of the bone whose name hashes (fnv1a64) to
    // `boneNameHash`, or on the root part for 0, at the part's centre.
    bool addImpulse(scene::Entity e, u64 boneNameHash, const Vec3& impulse);

    // Current write-back weight 0..1, or -1 when none. For tests and tools.
    f32  weight(scene::Entity e) const;
    // The physics ragdoll handle, or 0 (none, or not built yet).
    i32  physicsHandle(scene::Entity e) const;
    u32  activeCount() const { return static_cast<u32>(states_.size()); }

    // The ragdoll definition for an entity's skeleton, or nullptr until one has been built.
    const RagdollDef* definition(scene::Entity e) const;

    BuildOptions& buildOptions() { return opt_; }
    void setUserLayer(u32 layer) { userLayer_ = layer; }
    // Destroys every ragdoll.
    void clear();

private:
    struct State {
        RagdollMode mode = RagdollMode::Limp;
        i32 handle = 0;              // physics ragdoll; 0 until built
        i32 physMode = -1;           // the mode the physics ragdoll is actually in
        f32 weight = 0.0f;
        f32 goal = 1.0f;
        f32 rate = 1e9f;             // weight per second
        f32 strength = 0.5f;
        f32 scale = 1.0f;            // the entity's uniform world scale when last posed
        bool stopping = false;
        u64 skeleton = 0;
        const RagdollDef* def = nullptr;
        std::vector<f32> tpos, trot, ppos, prot;
        bool massApplied = false;    // the authored per-part masses are in (they wait for the first simulated mode)
    };

    static void modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose, void* user);
    void apply(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose);

    const RagdollDef* defFor(u64 skeletonId, const fmt::OcSkeleton& skel);
    bool build(scene::Entity e, State& s, const fmt::OcSkeleton& skel, const anim::Pose& pose);
    void syncPhysMode(State& s);
    void destroyPhysics(State& s);
    void setRate(State& s, f32 blendSeconds);

    scene::World* world_ = nullptr;
    anim::AnimSystem* anim_ = nullptr;
    bool installed_ = false;
    anim::PoseModifierFn prev_ = nullptr;
    void* prevUser_ = nullptr;
    BuildOptions opt_;
    u32 userLayer_ = 0;
    std::unordered_map<scene::Entity, State> states_;
    std::unordered_map<u64, RagdollDef> defs_;
};

RagdollSystem& ragdollSystem();

// The framework relay's provider signatures (framework_abi.h, aver_fw_set_ragdoll_provider), with
// the RagdollSystem as `user`. Declared here, not in the framework header, so this module need not
// link the framework: the composition root hands them across.
i32 providerStart(i32 entity, i32 mode, f32 blendSeconds, void* user);
i32 providerStop(i32 entity, f32 blendSeconds, void* user);
i32 providerMode(i32 entity, void* user);
i32 providerSetStrength(i32 entity, f32 strength, void* user);
i32 providerAddImpulse(i32 entity, i64 boneNameHash, f32 ix, f32 iy, f32 iz, void* user);

} // namespace aver::ragdoll
