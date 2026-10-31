#pragma once
// POSE-TO-POSE RETARGET: an entity whose pose is another entity's pose carried across a bone map.
//
// The clip retargeter bakes a whole clip for another skeleton (AnimRetarget.hpp). This is the same retargeter (Retarget.hpp)
// run on a live pose: the source entity plays whatever it plays (a clip, a state machine, a ragdoll, script bone poses) and the
// entity with a CPoseFollow takes the result onto its own skeleton through a .ocretarget, every tick. A character driven by another
// character's performance, a mirror rig, a puppet.
//
// IT IS A POSE SOURCE, not a modifier: the followed pose replaces the entity's clip pose, and the control rig, IK, the script pose
// seam and a ragdoll still run on top of it. It chains the pose source already installed (state machines, motion matching) and
// stands in front of it for the entities that carry the component.
//
// ORDER AND LATENCY. AnimSystem poses entities in pool order, so a source that is posed later in the same tick is read as of the
// previous tick: one frame behind. Give the source a lower animator-creation order to follow it in step; the bone map and the rest
// poses do the retargeting, not the timing.
//
// REGISTERED DYNAMICALLY like CControlRig: no change to Components.hpp or the scene ABI.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/Retarget.hpp"

#include <map>
#include <memory>
#include <tuple>

namespace aver::scene { class World; }

namespace aver::anim {

struct CPoseFollow {
    u64 asset = 0;                              // .ocretarget ObjectId: the bone map
    scene::Entity source = scene::kInvalidEntity;   // the entity whose pose is followed
    f32 weight = 0.0f;                          // 0..1 over the entity's own pose; zero-filled reads 0 = off, attach through attach()
    u32 flags = 0;
    u32 reserved = 0;
};
static_assert(sizeof(CPoseFollow) == 24, "CPoseFollow must be padding-free");

class PoseFollowSystem {
public:
    static u32 registerComponent(scene::World& world);
    static u32 typeId();
    // Attaches over the zero-filled bytes, weight defaulting to 1. nullptr when the component cannot be added.
    static CPoseFollow* attach(scene::World& world, scene::Entity e, u64 retargetAsset, scene::Entity source, f32 weight = 1.0f);

    // Chains in front of the installed pose source. Idempotent.
    void install(AnimSystem& anim, scene::World& world);
    static void uninstall(AnimSystem& anim);

    // Drops the bound retargeters (content changed).
    void clear() { bindings_.clear(); epoch_ = 0; }
    // How many entities followed their source on the last tick.
    u32 appliedLastTick() const { return applied_; }

private:
    struct Binding {
        Retargeter rt;
        bool ok = false;
    };
    using Key = std::tuple<u64, u64, u64>;   // asset, source skeleton, target skeleton

    static bool sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user);
    bool apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose);
    Binding* bindingFor(u64 asset, u64 srcSkelId, u64 dstSkelId);

    scene::World* world_ = nullptr;
    AnimSystem* anim_ = nullptr;
    PoseSourceFn prev_ = nullptr;
    void* prevUser_ = nullptr;
    std::map<Key, std::unique_ptr<Binding>> bindings_;
    u32 epoch_ = 0;
    u64 tickSeen_ = ~0ull;
    u32 applied_ = 0, appliedThis_ = 0;
    Pose tmp_;
};

// The process-wide system, like controlRigSystem() and animSystem() (one pose source slot, one World).
PoseFollowSystem& poseFollowSystem();

} // namespace aver::anim
