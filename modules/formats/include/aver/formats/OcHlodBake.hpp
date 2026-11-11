// Dynamic HLOD bake (docs/LEVEL_STREAMING.md, plan W2): one merged, simplified proxy mesh per streaming cell, recorded in the level's
// .ocstream (`HLOD` records) with the hash of what it was built from, so only the cells whose content changed are rebuilt.
// Pure CPU: Core + Formats + Platform. AverAssetC `hlod` and the editor's background bake both come here.
//
// A proxy's vertices are relative to its cell's minimum corner (cx * cellCm, cy * cellCm, 0); the runtime places it there.
// Proxies are not content: no body, never in the ray-tracing structure, and not selectable (the renderer side's rule).
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcMeshMerge.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/formats/OcWorld.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::fmt {

// Bump when the same level would bake a different proxy; part of every cell hash.
inline constexpr u32 kOcHlodBakeVersion = 1;

struct OcHlodOptions {
    // 0 = the level's own STREAM values.
    f32 cellCm = 0.0f;
    f32 hlodCm = 0.0f;
    // Per-cell triangle budget: the screen-area heuristic below, clamped to these.
    u32 minTriangles = 32;
    u32 maxTriangles = 3000;
    f32 viewportPx = 1080.0f;
    f32 fovYDeg = 60.0f;
    f32 dropSmallerCm = 0.0f;
    bool dropEnclosed = false;
    // Rebuild every cell even when its hash matches.
    bool force = false;
    // A content-relative asset's mesh; null reads <contentDir>/<asset>.
    std::function<bool(const std::string& asset, OcMeshData& out)> meshLoader;
    // Reduces a merged mesh to at most `budget` triangles (trifactor::simplifyToBudget in the tools); null = merge only.
    std::function<bool(OcMeshData& mesh, u32 budget)> simplify;
    // By error instead (runtime generation): when maxErrorCm > 0 and this is set, the proxy keeps whatever triangles a geometric error
    // of maxErrorCm needs (trifactor::simplifyToError) and the triangle budget is not used.
    f32 maxErrorCm = 0.0f;
    std::function<bool(OcMeshData& mesh, f32 maxErrorCm)> simplifyError;
    // Polled between cells (a background bake); true stops with what was finished written.
    std::function<bool()> cancel;
    std::function<void(u32 done, u32 total)> progress;
};

struct OcHlodReport {
    u32 cells = 0;            // cells with eligible content
    u32 built = 0, fresh = 0; // rebuilt / hash matched and left alone
    u32 removed = 0;          // proxies of cells that no longer qualify
    u32 failed = 0;
    u32 placementsMerged = 0, placementsExcluded = 0;   // excluded: class, vehicle, animated, hidden, layers, no mesh
    u32 excludedByLayer = 0;
    u32 trianglesIn = 0, trianglesOut = 0;
    bool cancelled = false;
    std::vector<std::string> warnings;
};

// The triangles a cell's proxy is worth at `hlodCm`: its projected area in pixels over ~32 px per triangle, never more than the source
// has, clamped to [minTriangles, maxTriangles].
u32 hlodTargetTriangles(u32 sourceTriangles, f32 cellCm, f32 hlodCm, const OcHlodOptions& opt);

// W10-6: a placement feeds a proxy only if every layer it is in is always on -- defined, initially Activated, not `runtime=1`, and the
// same for each ancestor. A name that is not a defined layer is ignored (it loads as no layer).
bool hlodLayersAllowed(const std::vector<std::string>& layerNames, const std::vector<OcDataLayer>& defs);

// A hash of a mesh's geometry and material slots (part of every cell hash that uses it).
u64 hashHlodMeshContent(const OcMeshData& m);

// Bakes every cell of `w` (placements need world bounds or are placed by position; run the stream bake first) and rewrites the HLOD
// records of w.stream.dataPath. Sets w.stream.hlodCm when it was 0; the caller saves the level. False with `why` if there is no
// streaming data to extend or it cannot be written.
bool bakeHlod(OcWorldData& w, const std::string& levelStem, const std::string& contentDir, const std::string& projectDir,
              const OcHlodOptions& opt, OcHlodReport& report, std::string* why = nullptr);

// The pieces of one cell, for a caller that builds proxies on its own schedule (runtime generation for procedurally generated chunks
// uses the same merger on the generation worker). `items` already carry world transforms.
struct OcHlodCellBuild {
    OcMeshData mesh;
    u32 trianglesIn = 0;
};
bool buildHlodProxy(const std::vector<OcMergeItem>& items, f64 originX, f64 originY, f32 cellCm, f32 hlodCm,
                    const OcHlodOptions& opt, OcHlodCellBuild& out, std::string* why = nullptr);

// Runtime generation's plan: the level's eligible placements (bakeHlod's rule: a visible static mesh in always-on layers, no class,
// vehicle or animation) grouped by streaming cell, each with its world matrix (ocMergeMatrix form, parents composed in f64), without
// reading a mesh. Cells sorted by (cy, cx); placement indices are positions in w.placements.
// A placement whose bounds are wider than this many cells on x or y (a road, a ground tile) is left out of runtime proxies: it streams
// by its nearest edge, so it is resident whenever any part of it is near, and merged into a far cell's proxy it would fight itself.
// world::PlacementStreamer::kHlodWideCells keeps it out of the ring's content the same way.
inline constexpr f32 kOcHlodWideCells = 1.5f;
struct OcHlodPlan {
    struct Cell {
        i32 cx = 0, cy = 0;
        std::vector<u32> placements;
    };
    std::vector<Cell> cells;
    std::vector<f64> matrices;   // 12 per placement of w (zero for an ineligible one)
};
void planHlodCells(const OcWorldData& w, f32 cellCm, OcHlodPlan& out);

} // namespace aver::fmt
