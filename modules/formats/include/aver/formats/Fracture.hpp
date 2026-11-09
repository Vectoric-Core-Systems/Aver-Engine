#pragma once
// Voronoi brittle fracture of a closed mesh (W7 of docs/plans/WAVE2_UE.md): seed patterns, the cell of every seed (the intersection of its bisector
// half-spaces, found with a bounded neighbour search), each cell clipped against the closed source with csg::Prepared, an inside material and planar UVs
// on the new faces, and the contact graph between the pieces. Plain doubles in the caller's space (the same convention as Csg.hpp), no engine type, no
// device. A fixed seed gives the same pieces on every run: the generators use their own PRNG, never the platform's distributions.
#include "aver/core/Types.hpp"
#include "aver/formats/Csg.hpp"

#include <array>
#include <vector>

namespace aver::fmt::fracture {

using Vec3d = std::array<double, 3>;

// ---- seed patterns (deterministic for a fixed `seed`) ----
// `count` seeds spread uniformly through the solid (rejection against the mesh, so none sit outside it).
std::vector<Vec3d> seedsUniform(const csg::Mesh& src, int count, u64 seed);
// Concentric shells about `center` out to `maxRadius`: shell k (1..rings) holds perShell * k seeds, plus one at the centre. jitter 0..1 of the spacing.
std::vector<Vec3d> seedsRadial(const Vec3d& center, double maxRadius, int rings, int perShell, double jitter, u64 seed);
// A lattice of `cell`-sized bricks over `bounds`; every other row along Y is offset by `stagger` (0..1) of a brick, every other layer along Z likewise.
std::vector<Vec3d> seedsBrick(const csg::Bounds& bounds, const Vec3d& cell, double stagger, double jitter, u64 seed);
// `count` seeds inside the solid, dense around `center` and sparser outward (the fall-off an impact leaves).
std::vector<Vec3d> seedsClustered(const csg::Mesh& src, const Vec3d& center, int count, u64 seed);

struct Options {
    int insideMaterial = -1;          // material of the new faces; -1 = one past the highest material the source uses
    double uvScale = 1.0;             // planar UVs on the inside faces: units of source space per UV unit is 1 / uvScale
    double minVolumeFraction = 1e-9;  // a piece below this fraction of the source volume is dropped as a sliver
};

struct Piece {
    csg::Mesh mesh;                   // closed, outward-wound; the new faces carry the inside material
    u32 seedIndex = 0;
    double volume = 0, area = 0;
    double centroid[3] = {0, 0, 0};
};

struct Link {
    u32 a = 0, b = 0;                 // piece indices, a < b
    double area = 0;                  // the shared face area
};

struct Result {
    std::vector<Piece> pieces;
    std::vector<Link> links;
    int insideMaterial = 0;
};

// The source must be closed and outward-wound. Seeds outside the solid are allowed (their cell may still reach into it).
Result voronoi(const csg::Mesh& src, const std::vector<Vec3d>& seeds, const Options& opt = {});

// ---- connectivity ----
struct Graph {
    u32 count = 0;                    // pieces
    std::vector<Link> links;
    std::vector<u8> anchor;           // 1 = fixed to the world; size == count
};
Graph makeGraph(const Result& r);
// Anchors every piece with a vertex on or behind the plane n . x = offset by at most `tolerance` (a floor, a wall).
void anchorByPlane(Graph& g, const Result& r, const Vec3d& normal, double offset, double tolerance);
// Connected components over the links that are not broken (broken[i] != 0). Returns a dense id per piece, numbered by the lowest piece index in it.
std::vector<u32> components(u32 pieceCount, const std::vector<Link>& links, const std::vector<u8>& broken, u32* outCount = nullptr);

// Volume, area and centroid of a closed mesh.
void measure(const csg::Mesh& m, double& volume, double& area, double centroid[3]);
// True when the outward area vectors sum to zero (a closed surface), robust to the T-junctions the BSP leaves.
bool isClosed(const csg::Mesh& m, double tolerance = 1e-6);

}  // namespace aver::fmt::fracture
