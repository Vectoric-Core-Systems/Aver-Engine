// A skeleton's pose, and the matrices a skinning shader wants from it.
//
// Core and Formats only, and deliberately: posing is arithmetic on a bone hierarchy, so it is
// decidable with no GPU and no device. Nothing here knows what a mesh or a draw call is.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/formats/OcAnim.hpp"

#include <vector>

namespace aver::anim {

// One local transform per bone, in the skeleton's own bone order.
struct Pose {
    std::vector<Transform> local;

    u32  boneCount() const { return static_cast<u32>(local.size()); }
    bool matches(const fmt::OcSkeleton& s) const { return local.size() == s.bones.size(); }
};

// Fills `out` with the skeleton's authored rest pose. Every sampler seeds from this, so a bone that
// no clip animates keeps its rest transform rather than collapsing to the identity.
void restPose(const fmt::OcSkeleton& skel, Pose& out);

// Local transforms to MODEL space, walking each bone through its parents. Requires the skeleton's
// parents-before-children ordering, which OcSkeleton::valid() is what checks.
void poseToModel(const fmt::OcSkeleton& skel, const Pose& p, std::vector<Mat4>& outModel);

// The matrices a skinning shader consumes: model-space bone times its inverse bind. A vertex in bind
// pose transformed by these lands exactly where it started, which is the property restPose gives.
void poseToSkinning(const fmt::OcSkeleton& skel, const Pose& p, std::vector<Mat4>& outSkin);

// `a` toward `b` by `t`, per bone: lerp on translation and scale, shortest-arc slerp on rotation.
// `t` is clamped. Mismatched bone counts leave `out` as a copy of `a`.
void blendPose(const Pose& a, const Pose& b, f32 t, Pose& out);

// Layers an additive clip's pose onto a base. `additive` is the additive clip sampled at some time
// and `additiveRest` is that same clip's own reference pose -- the difference between them is what
// gets applied, scaled by `weight`. That is what makes an additive lean or breathe compose with
// whatever the base is doing instead of replacing it.
void addPose(const Pose& base, const Pose& additive, const Pose& additiveRest, f32 weight, Pose& out);

} // namespace aver::anim
