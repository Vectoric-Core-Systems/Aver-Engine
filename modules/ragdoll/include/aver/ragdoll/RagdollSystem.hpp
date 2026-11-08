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
#include "aver/core/HitEvent.hpp"
#include "aver/ragdoll/HitReaction.hpp"
#include "aver/ragdoll/PhysAnimProfile.hpp"
#include "aver/ragdoll/RagdollBuilder.hpp"
#include "aver/ragdoll/RagdollLife.hpp"

#include <memory>
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

    // ---- Physical animation (docs/physics/PHYSICAL_ANIMATION.md) ----
    // Everything here needs the ragdoll to exist AND to be built (start() builds at once when the entity has
    // been posed; otherwise the first pose the seam sees does). False before that.
    //
    // Per bone blend: the simulated pose is written over the animation with weight `weight(e) * boneBlend`.
    // Default 1 for every bone. 0 shows the animation for that bone whatever the simulation does.
    bool setBoneBlend(scene::Entity e, i32 bone, f32 blend);
    f32  boneBlend(scene::Entity e, i32 bone) const;
    // Per part drive (the part a bone is or rides on). Strength < 0 inherits setStrength.
    bool setPartStrength(scene::Entity e, i32 bone, f32 strength);
    bool setPartDamping(scene::Entity e, i32 bone, f32 damping);
    bool setPartTorqueScale(scene::Entity e, i32 bone, f32 scale);
    // A pinned part follows the animation exactly while the others simulate.
    bool setPinned(scene::Entity e, i32 bone, bool pinned);
    // PARTIAL-BODY SIMULATION: simulates the bone named by `boneNameHash` and everything below it; the rest is
    // pinned to the animation and shows no simulated pose. Hash 0 simulates the whole body again.
    bool simulateSubtree(scene::Entity e, u64 boneNameHash);
    // Ramps the whole-body physical animation weight to `weight` over `seconds`, starting a Powered ragdoll when
    // there is none and switching a Kinematic one to Powered.
    bool blendPhysicalAnimation(scene::Entity e, f32 weight, f32 seconds);

    // The entity's physical animation profile; null (or never set) uses the defaults. Resolved against the
    // skeleton when needed, so it may be set before the entity is posed.
    void setProfile(scene::Entity e, std::shared_ptr<const PhysAnimProfile> profile);
    // Reacts to a hit: the hit bone and its neighbours blend into the simulation along their envelopes, the hit
    // part gets the impulse, and the ragdoll goes away by itself when the envelopes have run out. An entity with
    // a ragdoll already running (started by the caller, or dying) only receives the impulse. A radial event
    // (radius > 0) reacts every bone within reach, weaker with distance. False when nothing could react.
    bool applyHit(const HitEvent& hit);
    // The reaction state of an entity whose ragdoll was started by applyHit, or null.
    const HitReaction* hitReaction(scene::Entity e) const;

    // Death and recovery. die(): Powered ragdoll, motor strength to zero over the profile's deathSeconds, then
    // Limp. getUp(): picks the montage by how the body lies (profile) and plays it, blending the simulation
    // out over the profile's getUpBlend; the entity's transform is not moved to where the body lies.
    bool die(scene::Entity e);
    bool getUp(scene::Entity e);
    // Alive for an entity with no ragdoll.
    Life life(scene::Entity e) const;
    // How the body lies now (Upright for an entity with no ragdoll).
    Facing facing(scene::Entity e) const;
    // The get-up montage player. Default plays the montage through AnimSystem::montages().
    using GetUpFn = void (*)(scene::Entity e, u64 montageId, f32 blendSeconds, void* user);
    void setGetUpHandler(GetUpFn fn, void* user) { getUpFn_ = fn; getUpUser_ = user; }

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

        // Physical animation. Per bone; sized when the ragdoll is built. `sent*` is what the physics has been given.
        std::vector<f32> boneBlend;
        std::vector<f32> pStrength, pDamping, pTorque, sentStrength, sentDamping, sentTorque;
        std::vector<char> pinned, sentPinned;
        std::shared_ptr<const PhysAnimProfile> profile;
        ResolvedPhysAnim resolved;
        bool haveResolved = false;
        bool reaction = false;       // started by applyHit: the envelopes drive blend, strength and pins
        HitReaction hr;
        struct Pending { i32 bone; Vec3 impulse; };
        std::vector<Pending> pending;
        LifeMachine life;
        i32 rootBone = -1;           // the root part's bone, and the build-time axes, for facing()
        Quat rootRot0;
        Vec3 up0{0, 0, 1}, fwd0{1, 0, 0};
    };

    static void modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose, void* user);
    void apply(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose);

    const RagdollDef* defFor(u64 skeletonId, const fmt::OcSkeleton& skel);
    bool build(scene::Entity e, State& s, const fmt::OcSkeleton& skel, const anim::Pose& pose);
    void syncPhysMode(State& s);
    void destroyPhysics(State& s);
    void setRate(State& s, f32 blendSeconds);
    void ensureDrive(State& s);
    void pushDrive(State& s);
    void pushStrength(State& s);
    bool resolveFor(scene::Entity e, State& s);
    void stepReaction(State& s, f32 dt);
    Facing facingOf(const State& s) const;

    scene::World* world_ = nullptr;
    anim::AnimSystem* anim_ = nullptr;
    bool installed_ = false;
    anim::PoseModifierFn prev_ = nullptr;
    void* prevUser_ = nullptr;
    BuildOptions opt_;
    u32 userLayer_ = 0;
    std::unordered_map<scene::Entity, State> states_;
    std::unordered_map<u64, RagdollDef> defs_;
    std::unordered_map<scene::Entity, std::shared_ptr<const PhysAnimProfile>> pendingProfiles_;
    GetUpFn getUpFn_ = nullptr;
    void* getUpUser_ = nullptr;
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
