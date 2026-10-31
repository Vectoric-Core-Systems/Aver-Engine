#pragma once
// The POSE SEAM: a script-visible way to read and write the bones of an animated entity.
//
// Everything else that edits a pose (a control rig, an IK rig, a ragdoll) is data an asset or a system
// owns. A script had nothing: it could not set a bone, ask where a bone ended up, or hang something on one
// from outside the animation graph. This is that, and nothing more -- per-entity bone overrides applied as
// the last LAYER of the control-rig modifier, and reads of the finished pose.
//
// ORDER, for one entity, inside AnimSystem's single pose-modifier slot:
//     clip / state machine / montage -> CControlRig ops -> CIkRig -> POSE SEAM overrides -> ragdoll (chained)
// The seam sits after the rig and the IK so a script has the last word over authored motion, and before the
// ragdoll so a simulated pose still wins while one is blended in. Reads see the pose AFTER the whole chain
// (what was skinned); "before" reads see it just before the overrides were applied.
//
// REGISTERED DYNAMICALLY, like CControlRig: no change to Components.hpp or the scene ABI. The component marks an
// entity as taking overrides and holds the seam's master weight; the overrides themselves are runtime state
// (variable length, not savable) and go with the entity. Attached on the first write, so a script never attaches it.
//
// UNITS ARE THE ENGINE'S: centimetres, the skeleton's own axes, quaternions xyzw. A bone is a PARENT-LOCAL
// transform (Pose::local) unless the model-space flag is set, then it is a transform in the skeleton's model
// space (the entity's own frame).
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Types.hpp"

#include <unordered_map>
#include <vector>

namespace aver::scene { class World; }

namespace aver::anim {

// Which channels of a bone an override sets; the others keep what the pose already had.
inline constexpr u32 kPoseSeamPosition = 0x1;
inline constexpr u32 kPoseSeamRotation = 0x2;
inline constexpr u32 kPoseSeamScale    = 0x4;
inline constexpr u32 kPoseSeamAll      = 0x7;
// The values are a MODEL-space transform of the bone (children keep their local transforms and follow).
inline constexpr u32 kPoseSeamModel    = 0x8;
// Dropped after the tick that applied it (a pose a script writes every frame needs no clear).
inline constexpr u32 kPoseSeamOnce     = 0x10;

// CPoseSeam::flags
inline constexpr u32 kPoseSeamOff = 0x1;   // negative on purpose: a zero-filled attach is ON

struct CPoseSeam {
    f32 weight = 0.0f;   // master weight 0..1 on top of each override's own amount; 0 reads as 1
    u32 flags = 0;       // kPoseSeamOff
};
static_assert(sizeof(CPoseSeam) == 8, "CPoseSeam must be padding-free");

// Which pose a read returns.
enum class PoseReadSpace : u32 { Local = 0, Model = 1 };

class PoseSeamSystem {
public:
    // Registers CPoseSeam (idempotent) and returns its type id.
    static u32 registerComponent(scene::World& world);
    static u32 typeId();

    void init(scene::World& world, AnimSystem& anim) { world_ = &world; anim_ = &anim; }

    // ---- writes: they take effect at the next AnimSystem::tick ----
    // Sets the override of one bone. `flags` is kPoseSeam* channels | space | once; `amount` 0..1 blends the
    // override with the pose underneath (1 replaces it). False when the entity has no animated skeleton yet or
    // the bone is out of range. Attaches CPoseSeam when the entity has none.
    bool setBone(scene::Entity e, u32 bone, const Transform& t, u32 flags, f32 amount = 1.0f);
    // Drops one bone's override, or every bone's when `bone` is -1.
    bool clearBone(scene::Entity e, i32 bone);
    // The master weight and the off switch. Without a CPoseSeam both are refused.
    bool setWeight(scene::Entity e, f32 weight);
    bool setEnabled(scene::Entity e, bool enabled);
    // How many bones currently carry an override.
    u32 overrideCount(scene::Entity e) const;

    // ---- reads: the pose of the last tick ----
    // The finished pose of a bone (after the chain, ragdoll included), or, with `beforeSeam`, the pose just before
    // the seam's overrides were applied. False when the entity has no posed skeleton or the bone is out of range.
    bool readBone(scene::Entity e, u32 bone, PoseReadSpace space, bool beforeSeam, Transform& out) const;
    // The bone's rest transform (parent-local), and its parent (-1 for a root).
    bool restBone(scene::Entity e, u32 bone, Transform& out, i32& parent) const;
    u32 boneCount(scene::Entity e) const;
    // First bone with that name, or -1.
    i32 findBone(scene::Entity e, const std::string& name) const;
    const std::string* boneName(scene::Entity e, u32 bone) const;

    // The chain step (ControlRigSystem::applyTo calls it last). True when it changed the pose.
    bool apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose);

    // Drops every override and snapshot. `game::resetGameSystems` calls it.
    void clearRuntime() { ents_.clear(); }
    // Drops the state of entities that died (once per tick).
    void prune();

private:
    struct BoneOv {
        u32 flags = 0;      // 0 = none
        Transform t;
        f32 amount = 1.0f;
    };
    struct EntityRt {
        std::vector<BoneOv> bones;
        Pose before;        // the pose the overrides were applied to, last tick
        u32 count = 0;
        u64 seen = ~0ull;   // AnimSystem tick index of the last apply
    };

    scene::World* world_ = nullptr;
    AnimSystem* anim_ = nullptr;
    std::unordered_map<scene::Entity, EntityRt> ents_;
    std::vector<Mat4> model_;   // scratch
};

// The framework's pose provider (framework_pose_abi.h, aver_fw_set_pose_provider) with the PoseSeamSystem as `user`.
i32 poseProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI,
                 char* outStr, i32 outStrLen, void* user);

} // namespace aver::anim
