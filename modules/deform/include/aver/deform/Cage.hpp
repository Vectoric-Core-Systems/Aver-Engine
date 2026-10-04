// Sub-bone vertex deformation: a cage of deformation nodes finer than the skeleton, and a binding
// from mesh vertices to it.
//
// Bone skinning can only move a vertex the way its bones move -- every vertex sharing a bone's
// influence moves together. A cage puts nodes BETWEEN and AROUND the bones, so two vertices on the
// same bone can move differently: a dent, a bulge, a crease, cloth pushed by a hand.
//
// Core, Formats and Anim only. Generating and applying a cage is arithmetic on points, so all of it
// is decidable with no GPU.
#pragma once

#include "aver/anim/Pose.hpp"
#include "aver/core/Math.hpp"

#include <vector>

namespace aver::deform {

// Bind weights per vertex. Four matches the GPU path's planned R32_SINT/R32_FLOAT stride and is
// enough for a smooth field; it is a constant rather than a knob so the buffers have one layout.
inline constexpr u32 kCageInfluences = 4;

// A deformation node: where it rests, and which bone (if any) it was generated along.
struct CageNode {
    Vec3 rest{0, 0, 0};
    i32  bone = -1;    // the bone this node subdivides, or -1 for a lattice node
    f32  alongBone = 0.0f;   // 0 at the parent joint, 1 at the bone's own joint
};

// The nodes themselves. Displacements are supplied per frame against this rest set.
struct Cage {
    std::vector<CageNode> nodes;
    u32 nodeCount() const { return static_cast<u32>(nodes.size()); }
};

// Per vertex: which nodes move it, and by how much. Weights are PRE-NORMALISED and sum to one --
// the deform never re-divides, which is the contract the GPU port has to match byte for byte.
struct CageBinding {
    std::vector<i32> nodeIndex;   // vertexCount * kCageInfluences, -1 for an unused slot
    std::vector<f32> weight;      // vertexCount * kCageInfluences

    u32  vertexCount() const { return static_cast<u32>(nodeIndex.size() / kCageInfluences); }
    // True when the two arrays agree in length and every weight set sums to one.
    bool valid() const;
};

// How a cage is generated from a mesh.
struct CageOptions {
    // Nodes placed ALONG each bone segment, over and above the joint itself. This is the number the
    // whole module is named for: 0 gives one node per joint and reproduces bone-level granularity,
    // and every step above it buys deformation detail the skeleton cannot express.
    u32 subdivisionsPerBone = 2;
    // Lattice resolution, for a mesh with no skeleton. Nodes are placed on a regular grid over the
    // mesh bounds, expanded by `latticePadding` so surface vertices are enclosed rather than edge
    // cases.
    u32 latticeX = 3, latticeY = 3, latticeZ = 3;
    f32 latticePadding = 0.02f;   // fraction of the bounds' extent
    // Influence radius as a fraction of the mean node spacing. Larger is smoother and less local.
    f32 influenceRadius = 1.75f;
};

// Runtime knobs, and the order they apply in is the CPU/GPU parity contract from
// docs/rendering/RENDERING.md 7.2: blend -> mirrorY -> xGain -> clamp-to-MaxD -> rest+disp.
struct DeformParams {
    bool mirrorY = false;          // the OpenConstructor cage convention
    f32  gain = 1.0f;
    f32  maxDisplacement = 1e30f;  // centimetres; clamps the blended displacement's LENGTH
};

// Places nodes along every bone segment, subdividing each one. Bone positions come from the
// skeleton's REST pose, so the cage is authored in bind space like the mesh is.
void generateSubBoneCage(const fmt::OcSkeleton& skel, const CageOptions& opt, Cage& out);

// Places nodes on a regular grid over `boundsMin`..`boundsMax`. For a mesh with no skeleton, and as
// an outer shell around one that has.
void generateLatticeCage(const Vec3& boundsMin, const Vec3& boundsMax, const CageOptions& opt, Cage& out);

// Binds vertices to the cage: the nearest kCageInfluences nodes, weighted by a falloff that reaches
// zero at the furthest of them, then normalised. A vertex sitting exactly on a node binds to it
// alone. A vertex the cage does not reach binds to its single nearest node at full weight, so the
// partition of unity holds everywhere and no vertex is ever left unmoved by a rigid motion.
void bindVerticesToCage(const Cage& cage, const f32* positions, u32 vertexCount,
                        const CageOptions& opt, CageBinding& out);

// Generate and bind in one call: what an importer runs once, on the mesh it has just read.
// `skel` may be null, in which case a lattice is generated from the mesh's own bounds.
void buildCageForMesh(const fmt::OcSkeleton* skel, const f32* positions, u32 vertexCount,
                      const CageOptions& opt, Cage& outCage, CageBinding& outBinding);

// rest + blended node displacement, per the parity contract. `nodeDisp` is 3 floats per cage node.
// `restPositions` and `outPositions` are 3 floats per vertex and may alias.
void deformVertices(const Cage& cage, const CageBinding& binding, const f32* nodeDisp,
                    const f32* restPositions, u32 vertexCount, const DeformParams& params,
                    f32* outPositions);

} // namespace aver::deform
