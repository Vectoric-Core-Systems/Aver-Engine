// A skeleton's pose, and the matrices a skinning shader wants from it.
//
// Core and Formats only, and deliberately: posing is arithmetic on a bone hierarchy, so it is
// decidable with no GPU and no device. Nothing here knows what a mesh or a draw call is.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"   // kOcMeshInfluences: the influence width skinVertices reads

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

// Where a socket is, in the skeleton's MODEL space, given the model matrices poseToModel produced.
//
// THE POSED BONE, NOT THE REST BONE, which is the entire point of a socket: a grip in the hand has
// to travel with the hand. That is why this takes `model` rather than reading the skeleton's rest
// transforms -- pass poseToModel's output for this frame's pose and the socket follows the
// animation; pass a rest-pose model and it sits where the rig was authored.
//
// NOT poseToSkinning's output. Those carry the inverse bind, which exists to put a BIND-POSE VERTEX
// where it belongs; a socket offset is already expressed in the bone's own space and applying the
// inverse bind to it would send it somewhere meaningless. The two are easy to confuse and produce
// a result that is wrong in a way that still looks like a transform.
//
// False, leaving `out` untouched, when the socket names a bone outside `model`.
bool socketModelMatrix(const std::vector<Mat4>& model, const fmt::OcSocket& sock, Mat4& out);

// `a` toward `b` by `t`, per bone: lerp on translation and scale, shortest-arc slerp on rotation.
// `t` is clamped. Mismatched bone counts leave `out` as a copy of `a`.
void blendPose(const Pose& a, const Pose& b, f32 t, Pose& out);

// Layers an additive clip's pose onto a base. `additive` is the additive clip sampled at some time
// and `additiveRest` is that same clip's own reference pose -- the difference between them is what
// gets applied, scaled by `weight`. That is what makes an additive lean or breathe compose with
// whatever the base is doing instead of replacing it.
void addPose(const Pose& base, const Pose& additive, const Pose& additiveRest, f32 weight, Pose& out);

// Linear-blend skinning on the CPU, and the PARITY CONTRACT the GPU compute pass in
// Aver.Render.Skin matches term for term:
//
//   out = sum over the four influences of  w_i * (v * skin[j_i])
//
// with the weights taken AS AUTHORED -- the .ocmesh writer is what normalises them, so nothing here
// re-divides -- an influence whose weight is zero or whose bone index is out of range skipped, and
// a vertex with no surviving influence left at its rest position rather than collapsing to the
// origin. Normals are transformed by the same matrix as positions and renormalised, NOT by the
// inverse transpose: an approximation, shared deliberately by both sides, because a divergence
// here would be a parity bug rather than a visible improvement.
//
// `joints` and `weights` are four per vertex, as OcMeshData stores them. Mis-sized inputs leave
// the outputs empty.
void skinVertices(const std::vector<Mat4>& skin,
                  const std::vector<f32>& restPositions, const std::vector<f32>& restNormals,
                  const std::vector<u16>& joints, const std::vector<f32>& weights,
                  std::vector<f32>& outPositions, std::vector<f32>& outNormals);

// One rest-space box per bone: the extent of the vertices that bone meaningfully influences.
//
// Computed ONCE per mesh, so that the posed bounds can then be found in O(bones) instead of
// O(vertices) every frame. A bone no vertex reaches gets an EMPTY box, flagged by outUsed, because
// a bone with no geometry must contribute nothing rather than a degenerate point at its origin --
// a rig's unweighted leaf bones would otherwise drag the bounds around for free.
//
// `minWeight` is the influence below which a vertex is not counted for that bone. ZERO is the safe
// value and the one callers should pass: at zero this counts exactly the influences skinVertices
// counts, so the resulting bound provably contains every posed vertex. A HIGHER threshold buys
// tighter boxes -- a 0.001 weight on a distant bone barely moves the vertex, and counting it ties
// that bone's box to geometry on the other side of the character -- but it does so by giving up the
// guarantee, because the skinning still applies the weight this dropped. Raise it only where a
// slightly wrong bound is cheaper than a loose one, and never for culling something expensive.
void boneRestBounds(const std::vector<f32>& restPositions,
                    const std::vector<u16>& joints, const std::vector<f32>& weights,
                    u32 boneCount, f32 minWeight,
                    std::vector<Vec3>& outMin, std::vector<Vec3>& outMax, std::vector<u8>& outUsed);

// The posed bounding box, from those rest boxes and this frame's skinning matrices.
//
// CONSERVATIVE BY CONSTRUCTION, and the argument is worth stating because it is what makes an
// O(bones) answer legitimate: each bone's box is transformed and the AABB of all of them is taken.
// A skinned vertex is a weighted sum of its influences' transformed positions, each of which lies
// in its own transformed box; the AABB of the union is CONVEX, so it contains any such combination.
// The union of the boxes itself would not be -- it is not convex, and a blended vertex can sit in
// the gap between two of them.
//
// `restMin`/`restMax` are folded in as well, because a vertex whose weights do not sum to one is
// NOT a convex combination -- the contract skips zero weights and out-of-range indices rather than
// renormalising -- and a fully unweighted vertex stays at its rest position by the same rule.
void posedBounds(const std::vector<Vec3>& boneMin, const std::vector<Vec3>& boneMax,
                 const std::vector<u8>& used, const Mat4* skin, u32 boneCount,
                 const Vec3& restMin, const Vec3& restMax, Vec3& outMin, Vec3& outMax);

} // namespace aver::anim
