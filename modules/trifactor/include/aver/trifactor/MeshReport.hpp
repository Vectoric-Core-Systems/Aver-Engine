// Per-mesh cook report: LOD ladder shape, group-seam cost, virtualization eligibility, and the
// builder-staleness model. Pure CPU; used by RelodTool and any editor surface that wants to say
// "this mesh was cooked by an older builder".
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"

#include <string>
#include <vector>

namespace aver::trifactor {

// How a file's stamped builderVersion compares with this build's kBuilderVersion. Unknown is a file
// from before the field existed (0 never means current); Newer is a file cooked by a later builder
// than this binary.
enum class BuilderStaleness : u8 { Current, Stale, Unknown, Newer };

BuilderStaleness classifyBuilder(u32 stored, u32 current = kBuilderVersion);
const char* stalenessLabel(BuilderStaleness s);
// One-line warning for a details panel; empty when the mesh is current.
std::string stalenessWarning(u32 stored, u32 current = kBuilderVersion);

// Whether a cooked mesh can take the virtualized (cluster) path, and the first reason it cannot.
struct Eligibility {
    bool ok = false;
    std::string why;   // empty when ok
};
Eligibility virtualizable(const fmt::OcMeshData& mesh);

// Group-seam cost of one LOD level. Edges are welded by position (duplicate vertices at UV or normal
// seams count once). `locked` edges are shared by triangles of two different simplification groups,
// which is what meshopt_SimplifyLockBorder keeps from collapsing when the next level is built; `open`
// edges have one triangle only (mesh border, independent of the partitioner).
struct LevelReport {
    u32 clusters = 0;
    u32 groups = 0;      // groups that simplified this level into the next (0 for the root)
    u64 triangles = 0;
    u64 edges = 0;
    u64 lockedEdges = 0;
    u64 openEdges = 0;
    f64 lockedRatio() const { return edges ? static_cast<f64>(lockedEdges) / static_cast<f64>(edges) : 0.0; }
};

struct LodReport {
    std::vector<LevelReport> levels;
    u64 lockedEdgesTotal = 0;   // over every level that has a next one
    u64 coarsestTriangles = 0;
};

LodReport measureLodDag(const LodDag& dag, const fmt::OcMeshData& mesh);

} // namespace aver::trifactor
