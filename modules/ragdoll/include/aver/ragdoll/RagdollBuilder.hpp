// RagdollBuilder: a skeleton's bind pose -> the per-bone capsules and joint limits a physics ragdoll
// is built from. Pure arithmetic on an fmt::OcSkeleton (no world, no physics), so it is testable
// with a hand-made rig.
//
// GENERAL BY CONSTRUCTION: nothing is keyed on a bone NAME. A bone gets a capsule from its origin
// toward its longest child; whether it is a limb or a finger is decided by its length against the
// skeleton's overall size. Names appear only in the optional "<skeleton>.ocragdoll" override file,
// which is how an author corrects the rule for one rig (see docs/physics/RAGDOLL_ANIMATION.md).
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ragdoll {

struct BuildOptions {
    // A bone shorter than this fraction of the skeleton's largest extent is a finger, a twist or a
    // helper: no part, it rides its ancestor.
    f32 tinyFraction = 0.035f;
    // A bone longer than this is a transform node (a "root" standing on the floor over the hips),
    // not a limb.
    f32 maxLengthFraction = 0.4f;
    f32 radiusPerLength = 0.18f;     // capsule radius = this * length ...
    f32 minRadiusFraction = 0.012f;  // ... clamped to these fractions of the extent
    f32 maxRadiusFraction = 0.07f;
    f32 swingDeg = 45.0f;            // default swing half-angle (both axes)
    f32 twistDeg = 30.0f;            // twist is tighter than swing
    f32 totalMassKg = 70.0f;
};

// One line of an override file, matched to a bone by name. Negative / unset means "keep the rule".
struct BoneOverride {
    std::string name;
    i32 enable = -1;                 // 0 off, 1 on (a leaf needs `length` to have a segment), -1 rule
    f32 radius = -1.0f;              // cm
    f32 length = -1.0f;              // cm; only used for a bone with no child (extends along parent->bone)
    bool hasLimits = false;
    f32 swing1 = 0, swing2 = 0, twist = 0;   // degrees
    // Authored per-bone data (all optional; the default is the rule above and the shared mass).
    i32 shape = -1;                  // kShape*: the part's shape, -1 rule (a capsule)
    bool hasOffset = false;
    Vec3 offset{0, 0, 0};            // cm, bone-local: moves the whole part (a sphere sits at the middle of its segment)
    f32 massKg = -1.0f;              // this part's own mass; the others share the rest of the total by volume
    f32 friction = -1.0f;            // body friction / restitution of this part; -1 keeps the world's default
    f32 bounce = -1.0f;
    i32 joint = -1;                  // kJoint*: how the part is joined to its parent part, -1 swing-twist (the rule)
};

// BoneOverride::shape and RagdollDef::shape.
inline constexpr i32 kShapeCapsule = 0, kShapeSphere = 1;
// BoneOverride::joint and RagdollDef::joint. Swing-twist is the rule; the others are limit triples of the same joint:
// a hinge keeps swing1 and locks the rest, fixed locks everything, a ball is free to 180 degrees on all three.
inline constexpr i32 kJointSwingTwist = 0, kJointHinge = 1, kJointFixed = 2, kJointBall = 3;

struct RagdollOverrides {
    f32 totalMassKg = 0.0f;          // 0 = not set
    std::vector<BoneOverride> bones;
};

// Text format, one directive per line, '#' starts a comment:
//   mass <kg>
//   bone <name> [on|off] [radius <cm>] [length <cm>] [limits <swing1> <swing2> <twist>]
//               [shape capsule|sphere] [offset <x> <y> <z>] [mass <kg>] [friction <f>] [bounce <r>]
//               [joint swingtwist|hinge|fixed|ball]
// Names have no spaces. False with `why` on a malformed line; `out` then holds what parsed before it.
bool parseRagdollOverrides(std::string_view text, RagdollOverrides& out, std::string* why = nullptr);
bool loadRagdollOverrides(const std::string& path, RagdollOverrides& out, std::string* why = nullptr);

// What the physics ABI wants, per bone (parents-before-children, as the skeleton is).
struct RagdollDef {
    u32 boneCount = 0;
    std::vector<i32> parents;        // skeleton parents, -1 for a root
    std::vector<f32> capsules;       // 7 per bone: ax ay az bx by bz radius (bone-local cm); radius <= 0 = no part
    std::vector<f32> limitsDeg;      // 3 per bone: swing1 swing2 twist
    std::vector<u64> nameHash;       // fnv1a64 of each bone name
    // Authored per-bone data, parallel to the bones; empty when no override file set any (then every part is as the rule makes it).
    std::vector<f32> massKg;         // < 0: by volume
    std::vector<f32> friction;       // < 0: the default
    std::vector<f32> bounce;         // < 0: the default
    std::vector<i32> joint;          // kJoint*
    f32 totalMassKg = 70.0f;
    f32 extent = 0.0f;               // the skeleton's largest bind-pose extent, cm
    u32 partCount = 0;
    i32 rootPart = -1;               // first bone with a part, or -1

    bool hasPart(u32 bone) const { return bone < boneCount && capsules[bone * 7 + 6] > 0.0f; }
    // The bone itself or its nearest ancestor with a part; -1 when none.
    i32 partOf(u32 bone) const;
    // The bone whose name hashes to `h`, or -1.
    i32 boneByHash(u64 h) const;
};

// Derives the ragdoll for `skel`. An invalid skeleton, or one with no bone big enough, yields
// partCount == 0 (and the caller must not create a ragdoll from it).
RagdollDef buildRagdoll(const fmt::OcSkeleton& skel, const BuildOptions& opt = {},
                        const RagdollOverrides* overrides = nullptr);

// "<dir>/<stem>.ocragdoll" for a skeleton file path.
std::string overridePathFor(const std::string& skeletonPath);

} // namespace aver::ragdoll
