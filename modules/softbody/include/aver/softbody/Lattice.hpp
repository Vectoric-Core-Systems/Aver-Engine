// Lattice: the voxel tetrahedral cage of a jelly (docs/plans/wave11/SOFTBODY_HAIR.md 2.4), and the embedding that makes
// the render mesh follow it.
//
// A closed triangle mesh is filled with the cells of a regular grid whose centre is inside it; each kept cube is split into
// six tetrahedra by the Freudenthal (Kuhn) subdivision around its main diagonal, the same diagonal in every cube, so
// neighbouring cubes agree on their shared face and the lattice has no cracks. The boundary triangles of the tetrahedra
// are the Jolt faces. Render vertices are embedded by plain tetrahedral barycentric coordinates (not harmonic or
// mean-value coordinates, the choice SOFTBODY_DEPTH 2.19 also made).
//
// Pure arithmetic on arrays: no scene, no GPU, no Jolt. Positions are in whatever space the mesh is in (mesh-local cm for
// the scene layer); the node positions the physics body simulates are the caller's transform of `nodes`.
#pragma once

#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::softbody {

struct LatticeSpec {
    f32 cellCm = 12.0f;        // the cube edge
    u32 maxNodes = 2048;       // more is refused (Lattice::overCap)
    f32 snapCm = 0.0f;         // boundary nodes within this of the surface move onto it (clamped to 0.45 of a cell)
};

struct Lattice {
    std::vector<Vec3> nodes;           // rest positions, after snapping
    std::vector<u32> tets;             // 4 node indices per tetrahedron
    // 3 node indices per boundary triangle, wound so that (C - A) x (B - A) points OUT of the volume: the winding the
    // physics ABI (pressure, buoyancy, aver_phys_sbx_volume) expects, which is the reverse of the render mesh's.
    std::vector<u32> surfaceFaces;
    std::vector<u32> edges;            // 2 per unique tetrahedron edge, ascending
    f32 restVolumeCm3 = 0.0f;          // sum of |tetrahedron volume|
    f32 cellCm = 0.0f;                 // the cell edge the lattice was built with
    u32 surfaceNodeCount = 0;          // nodes on at least one boundary face
    bool overCap = false;              // refused because it needs more than LatticeSpec::maxNodes nodes
    std::string error;                 // the reason a build was refused
};

// Signed volume of the tetrahedron (a, b, c, d): dot(cross(b - a, c - a), d - a) / 6.
f32 tetSignedVolume(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d);

// True when, after welding vertices closer than weldCm, every edge of the triangle soup is shared an even number of times
// (a closed surface; a mesh with a UV seam is still closed). False for fewer than four triangles.
bool meshIsClosed(const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount, f32 weldCm = 0.01f);

// positions: 3 floats per vertex; indices: 3 per triangle. False with `out.error` set for an open mesh, an empty lattice
// (the cell is larger than the mesh) or one over the node cap. Deterministic.
bool buildLattice(const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount, const LatticeSpec& spec, Lattice& out);

// Per render vertex: the tetrahedron it sits in (else the nearest, with extrapolated weights each capped at 2 in
// magnitude) and its 4 barycentric weights, summing to 1.
struct Embedding {
    std::vector<u32> tet;
    std::vector<f32> bary;
};
void embedMesh(const Lattice& lattice, const f32* positions, u32 vertexCount, Embedding& out);

// out[i] = sum of weight x node over the vertex's tetrahedron; nodeWorld has one entry per lattice node.
void embeddedPositions(const Lattice& lattice, const Embedding& emb, const Vec3* nodeWorld, u32 vertexCount, Vec3* out);

}  // namespace aver::softbody
