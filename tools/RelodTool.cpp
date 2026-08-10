// RelodTool -- reports the LOD ladder Trifactor WOULD build for an .ocmesh today, against the one
// the file already carries.
//
// WHY THIS EXISTS. A cooked .ocmesh carries LOD 0's geometry AND the coarser ladder Trifactor built
// from it. Change the simplifier and every ladder already on disk is stale -- and the only way to
// see what the new one would look like was to re-import the original source asset, which for a
// project that ships cooked meshes (Electric Dreams has 69 .ocmesh files and not one .gltf) does not
// exist any more. The ladder is derivable from LOD 0 alone, so needing the source to inspect it was
// never a real requirement, just a missing tool.
//
// REPORTS ONLY. It does not write. Rebuilding a ladder in place is a lossy, irreversible rewrite of
// somebody's art asset, and the conversion back into the file's own meshlet/coarserLods form lives
// in ConvertTool's anonymous namespace rather than in Trifactor -- so the write path wants that
// conversion lifted into the module first, deliberately, rather than duplicated here in a hurry.
// What this answers is the question that has to come first anyway: is the new ladder better?
//
//     RelodTool.exe <file-or-directory>

#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

namespace {

struct Totals { u32 files = 0, failed = 0, improved = 0; u64 oldCoarsest = 0, newCoarsest = 0; };

// Triangles in one level of the DAG, summed over its clusters. Cluster::triangles holds LOCAL
// indices, three per triangle, so the triangle count is its size divided by three.
u64 levelTris(const trifactor::LodDag& dag, u32 level) {
    u64 n = 0;
    for (const u32 cid : dag.levels[level]) n += dag.clusters[cid].triangles.size() / 3;
    return n;
}

void relod(const std::filesystem::path& path, Totals& t) {
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path.string(), md, &why)) {
        AVER_WARN("[Relod] {}: {}", path.filename().string(), why);
        ++t.failed;
        return;
    }
    ++t.files;

    const u32 lod0Tris = static_cast<u32>(md.indices.size() / 3);
    const u32 oldLevels = md.lodCount();
    const u64 oldCoarsest = md.coarserLods.empty()
        ? lod0Tris
        : md.coarserLods.back().indices.size() / 3;

    // FROM LOD 0 ONLY. The coarserLods already in the file are the stale output being compared
    // against; feeding them back in would compound the old simplifier's decisions into the new
    // ladder instead of redoing them. buildClusters/buildLodHierarchy read only positions+indices.
    fmt::OcMeshData src;
    src.positions = md.positions;
    src.normals   = md.normals;
    src.uvs       = md.uvs;
    src.indices   = md.indices;
    src.boundsMin = md.boundsMin;
    src.boundsMax = md.boundsMax;
    src.flags     = md.flags;

    trifactor::LodDag dag;
    if (!trifactor::buildClusters(src, dag, &why)) {
        AVER_WARN("[Relod] {}: buildClusters: {}", path.filename().string(), why);
        ++t.failed;
        return;
    }
    if (!trifactor::buildLodHierarchy(src, dag, &why))
        AVER_WARN("[Relod] {}: buildLodHierarchy: {}", path.filename().string(), why);

    const u32 newLevels = dag.levelCount();
    const u64 newCoarsest = newLevels ? levelTris(dag, newLevels - 1) : lod0Tris;

    t.oldCoarsest += oldCoarsest;
    t.newCoarsest += newCoarsest;
    if (newCoarsest < oldCoarsest) ++t.improved;

    AVER_INFO("[Relod] {:<34} LOD0 {:>7} | was {:>7} ({:>2} lv, {:>5.1f}x) -> now {:>7} ({:>2} lv, {:>5.1f}x)",
              path.filename().string(), lod0Tris,
              oldCoarsest, oldLevels,
              oldCoarsest ? static_cast<f64>(lod0Tris) / static_cast<f64>(oldCoarsest) : 0.0,
              newCoarsest, newLevels,
              newCoarsest ? static_cast<f64>(lod0Tris) / static_cast<f64>(newCoarsest) : 0.0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_ERROR("usage: RelodTool <file-or-directory>");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::error_code ec;
    Totals t;

    if (std::filesystem::is_directory(root, ec)) {
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".ocmesh") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) relod(f, t);
    } else {
        relod(root, t);
    }

    AVER_INFO("[Relod] {} file(s), {} failed, {} would get a coarser floor -- "
              "summed coarsest level {} -> {} triangles",
              t.files, t.failed, t.improved, t.oldCoarsest, t.newCoarsest);
    AVER_INFO("[Relod] nothing was written; this tool only reports.");
    return t.failed ? 1 : 0;
}
