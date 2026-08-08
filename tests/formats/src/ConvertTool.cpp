// Command-line tool: imports a glTF/GLB file and writes the .oc* assets it contains.
//
// It writes the WHOLE TRIPLE -- mesh, skeleton, clips -- rather than only the first mesh, because a
// rig that arrives without its skeleton and its animation is not an importable asset, it is a static
// mesh with some unreachable extra streams. That was the state of this tool until skinning had a
// consumer, and it is why nothing downstream could be tested against a real file.
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/core/Log.hpp"

// Clustering is entirely OPTIONAL: ConvertTool must still import and write the .oc* triple with
// AVER_MODULE_TRIFACTOR=OFF (the tree's default -- see modules/trifactor/CMakeLists.txt on why it
// is off by default). AVER_MODULE_TRIFACTOR reaches this translation unit only through the link
// interface (tests/formats/CMakeLists.txt's `if(TARGET Aver.Trifactor)` block), the same mechanism
// modules/runtime.game/src/GameContent.cpp uses for AVER_MODULE_PBR/AVER_MODULE_SCENE.
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#include <algorithm>
#endif

#include <string>

using namespace aver;

#if AVER_MODULE_TRIFACTOR
namespace {
// Converts one LOD level of a DAG into the on-disk OcMeshMeshlet shape.
std::vector<fmt::OcMeshMeshlet> toMeshlets(const aver::trifactor::LodDag& dag, u32 level) {
    std::vector<fmt::OcMeshMeshlet> out;
    if (level >= dag.levels.size()) return out;
    out.reserve(dag.levels[level].size());
    for (const u32 cid : dag.levels[level]) {
        const aver::trifactor::Cluster& c = dag.clusters[cid];
        fmt::OcMeshMeshlet ml;
        ml.vertices     = c.vertices;
        ml.triangles    = c.triangles;
        ml.sphereCenter = c.bounds.sphereCenter;
        ml.sphereRadius = c.bounds.sphereRadius;
        ml.coneApex     = c.bounds.coneApex;
        ml.coneAxis[0]  = c.bounds.coneAxis[0];
        ml.coneAxis[1]  = c.bounds.coneAxis[1];
        ml.coneAxis[2]  = c.bounds.coneAxis[2];
        ml.coneCutoff   = c.bounds.coneCutoff;
        out.push_back(std::move(ml));
    }
    return out;
}

// Converts one LOD level's clusters back into a plain GLOBAL-index triangle list -- the level's own
// index buffer, for the raster fallback and for OcMeshLod::indices. A cluster's `triangles` are
// LOCAL indices into its own `vertices` (the on-disk MLET shape); this undoes that, mirroring
// Aver.Trifactor's own appendGlobalTriangles (ClusterBuilder.cpp, private to that TU).
std::vector<u32> toIndices(const aver::trifactor::LodDag& dag, u32 level) {
    std::vector<u32> out;
    if (level >= dag.levels.size()) return out;
    for (const u32 cid : dag.levels[level]) {
        const aver::trifactor::Cluster& c = dag.clusters[cid];
        for (usize t = 0; t + 2 < c.triangles.size(); t += 3) {
            out.push_back(c.vertices[c.triangles[t + 0]]);
            out.push_back(c.vertices[c.triangles[t + 1]]);
            out.push_back(c.vertices[c.triangles[t + 2]]);
        }
    }
    return out;
}

// Builds the FULL LOD hierarchy (buildClusters for LOD 0, buildLodHierarchy for every coarser level)
// and persists all of it into `m`: LOD 0 into meshlets (as before), LOD 1+ into coarserLods, each
// with its own index buffer and its ScreenErrorThreshold converted from the DAG's geometric error
// (aver::trifactor::toScreenErrorThreshold). Returns false (mesh saved without meshlets, exactly as
// if Trifactor were absent) only when buildClusters itself fails or the converted screen error is
// not monotonic -- a clustering failure on some pathological input is not a reason to fail an
// otherwise-good import, and ConvertTool's job is "wire it", not "referee it". buildLodHierarchy
// failing (it does not, on any input buildClusters accepted -- see its own doc comment) is
// deliberately non-fatal: the mesh still saves with LOD 0 only, same as before this function existed.
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;

    std::string hierWhy;
    if (!aver::trifactor::buildLodHierarchy(m, dag, &hierWhy))
        AVER_WARN("buildLodHierarchy: {} (saving LOD 0 only)", hierWhy);

    // Cluster::error is raw/relative (meshopt units, see ClusterBuilder.hpp); worldExtentScale is the
    // ONE constant that turns every cluster's error in this mesh's DAG into an absolute (cm) error --
    // see Aver.Trifactor's own comment on why that is safe to compute once per mesh.
    const f32 scale = aver::trifactor::worldExtentScale(m);
    std::string monoWhy;
    if (!aver::trifactor::validateScreenErrorMonotonic(dag, scale, &monoWhy)) {
        if (why) *why = "screen-error monotonicity broke after conversion: " + monoWhy;
        return false;
    }

    m.meshlets = toMeshlets(dag, 0);
    m.coarserLods.clear();
    for (u32 level = 1; level < dag.levelCount(); ++level) {
        fmt::OcMeshLod lod;
        lod.indices  = toIndices(dag, level);
        lod.meshlets = toMeshlets(dag, level);
        // Every cluster newly created at this level shares the SAME propagatedError (buildLodHierarchy
        // assigns it once per group, to every cluster the group's re-split produced), so max() over
        // the level is defensive rather than strictly necessary -- it stays correct even if a future
        // change to buildLodHierarchy ever let that stop being true.
        f32 rawError = 0.0f;
        for (u32 cid : dag.levels[level]) rawError = std::max(rawError, dag.clusters[cid].error);
        lod.screenErrorThreshold = aver::trifactor::toScreenErrorThreshold(rawError, scale);
        m.coarserLods.push_back(std::move(lod));
    }
    return true;
}
} // namespace
#endif

namespace {

// Strips a directory and an extension, so <out>/<stem>.ocskel sits beside <out>/<stem>.ocmesh.
std::string stemOf(const std::string& path) {
    usize a = path.find_last_of("/\\");
    a = (a == std::string::npos) ? 0 : a + 1;
    const usize b = path.find_last_of('.');
    return path.substr(a, (b == std::string::npos || b < a) ? std::string::npos : b - a);
}

// A name safe to hang on a file, so an unnamed or oddly-named glTF node cannot escape into a path.
std::string safe(const std::string& in, const std::string& fallback) {
    std::string out;
    for (char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out.push_back(c);
    return out.empty() ? fallback : out;
}

} // namespace

// Converts argv[1] into argv[2] (a directory), naming everything after argv[3] or the source stem.
// Returns 0 on success, 1 on a conversion error, 2 on bad usage.
int main(int argc, char** argv) {
    if (argc < 3) {
        AVER_ERROR("usage: ConvertTool <in.gltf|in.glb> <out-directory> [base-name] [--lod <ratio>]");
        return 2;
    }
    // --lod <ratio> decimates to roughly that fraction of the triangles at cook time. Parsed out of
    // argv before the positional arguments are read, so it can be written anywhere on the line and
    // [base-name] does not accidentally swallow it.
    f32 lodRatio = 0.0f;
    int positional = argc;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--lod") {
            lodRatio = static_cast<f32>(std::atof(argv[i + 1]));
            positional = (positional == argc) ? i : positional;
        }
    }
    argc = positional;   // hide the flag from the positional reads below

    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(argv[1], res, {}, &why)) { AVER_ERROR("import: {}", why); return 1; }
    for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
    if (res.meshes.empty()) { AVER_ERROR("no meshes"); return 1; }

    std::string dir = argv[2];
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    const std::string base = argc > 3 ? std::string(argv[3]) : stemOf(argv[1]);

    // ---- the mesh ----
    // A copy, not a const ref to res.meshes[0]: clustering (when built) mutates the meshlets field
    // in place, and the alternative -- a second OcMeshData just for the clustered case -- would make
    // the AVER_MODULE_TRIFACTOR=OFF and =ON code paths save two DIFFERENT objects, which is exactly
    // the kind of divergence that only shows up once someone diffs the two builds' output.
    fmt::OcMeshData m = res.meshes[0];
    AVER_INFO("imported '{}': {} verts, {} tris, skin {}, bounds ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
              res.meshNames[0], m.vertexCount(), m.indices.size() / 3, m.hasSkin() ? "yes" : "no",
              m.boundsMin.x, m.boundsMin.y, m.boundsMin.z, m.boundsMax.x, m.boundsMax.y, m.boundsMax.z);

#if AVER_MODULE_TRIFACTOR
    // Decimation BEFORE clustering, necessarily: simplifyMesh rewrites the index buffer and clears
    // any meshlets, so clustering first would only throw that work away.
    if (lodRatio > 0.0f) {
        const usize before = m.indices.size() / 3;
        if (!aver::trifactor::simplifyMesh(m, lodRatio, &why)) {
            AVER_WARN("--lod {}: {} (saving at full density)", lodRatio, why);
        } else {
            AVER_INFO("simplified to {:.1f}%: {} -> {} tris ({})", double(lodRatio) * 100.0,
                      before, m.indices.size() / 3, why);
        }
    }

    // Best-effort: a mesh too small/degenerate to cluster (see TrifactorTest's degenerate cases)
    // still gets saved, just without an MLET chunk -- the mesh is not lost over an optional feature.
    if (!addMeshlets(m, &why)) {
        AVER_WARN("clustering '{}': {} (saving without meshlets)", res.meshNames[0], why);
    } else {
        AVER_INFO("clustered '{}': {} LOD(s), {} meshlets at LOD 0", res.meshNames[0], m.lodCount(), m.meshlets.size());
        for (usize i = 0; i < m.coarserLods.size(); ++i)
            AVER_INFO("  LOD {}: {} tris, {} meshlets, screenError {:.4f}", i + 1,
                      m.coarserLods[i].indices.size() / 3, m.coarserLods[i].meshlets.size(),
                      m.coarserLods[i].screenErrorThreshold);
    }
#endif

    const std::string meshPath = dir + "/" + base + ".ocmesh";
    if (!fmt::saveOcMesh(meshPath, m, &why)) { AVER_ERROR("save mesh: {}", why); return 1; }
    AVER_INFO("wrote {}", meshPath);

    fmt::OcMeshData back;
    if (!fmt::loadOcMesh(meshPath, back, &why)) { AVER_ERROR("reload mesh: {}", why); return 1; }
    // Reported rather than assumed: the skin is the one stream that used to be dropped silently,
    // and "it reloaded" is not the same claim as "it reloaded with its rig intact".
    AVER_INFO("reloaded: {} verts, {} tris, skin {}",
              back.vertexCount(), back.indices.size() / 3, back.hasSkin() ? "yes" : "no");
    if (m.hasSkin() && !back.hasSkin()) { AVER_ERROR("the skin did not survive the round trip"); return 1; }

    // ---- the skeleton. Named after the base rather than after the skin, because a clip's
    //      skeletonRef is resolved by FILE STEM and the two have to agree. ----
    for (usize i = 0; i < res.skeletons.size(); ++i) {
        const std::string p = dir + "/" + base + (i == 0 ? "" : std::to_string(i)) + ".ocskel";
        if (!fmt::saveOcSkel(p, res.skeletons[i], &why)) { AVER_ERROR("save skeleton: {}", why); return 1; }
        AVER_INFO("wrote {} ({} bones)", p, res.skeletons[i].bones.size());
    }
    if (m.hasSkin() && res.skeletons.empty())
        AVER_WARN("the mesh carries skin but the file had no skin node, so its joint indices "
                  "address a skeleton that was not written");

    // ---- the clips ----
    for (usize i = 0; i < res.animations.size(); ++i) {
        fmt::OcAnimation clip = res.animations[i];
        clip.skeletonRef = base;   // resolved by stem beside the clip; see AnimEditor's findSkeleton
        const std::string name = safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                      "Clip" + std::to_string(i));
        const std::string p = dir + "/" + base + "_" + name + ".ocanim";
        if (!fmt::saveOcAnim(p, clip, &why)) { AVER_ERROR("save clip: {}", why); return 1; }
        AVER_INFO("wrote {} ({:.2f}s, {} tracks, skeletonRef '{}')",
                  p, clip.duration, clip.tracks.size(), clip.skeletonRef);
    }

    return 0;
}
