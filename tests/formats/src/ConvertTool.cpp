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
#endif

#include <string>

using namespace aver;

#if AVER_MODULE_TRIFACTOR
namespace {
// Converts buildClusters' LOD-0 output into the on-disk OcMeshMeshlet shape and appends it to `m`.
// Returns false (mesh saved without meshlets, exactly as if Trifactor were absent) rather than
// aborting the whole conversion -- a clustering failure on some pathological input is not a reason
// to fail an otherwise-good import, and ConvertTool's job is "wire it", not "referee it".
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;

    m.meshlets.clear();
    m.meshlets.reserve(dag.levels.empty() ? 0 : dag.levels[0].size());
    for (const u32 cid : (dag.levels.empty() ? std::vector<u32>{} : dag.levels[0])) {
        const aver::trifactor::Cluster& c = dag.clusters[cid];
        fmt::OcMeshMeshlet ml;
        ml.vertices  = c.vertices;
        ml.triangles = c.triangles;
        ml.sphereCenter = c.bounds.sphereCenter;
        ml.sphereRadius = c.bounds.sphereRadius;
        ml.coneApex     = c.bounds.coneApex;
        ml.coneAxis[0]  = c.bounds.coneAxis[0];
        ml.coneAxis[1]  = c.bounds.coneAxis[1];
        ml.coneAxis[2]  = c.bounds.coneAxis[2];
        ml.coneCutoff   = c.bounds.coneCutoff;
        m.meshlets.push_back(std::move(ml));
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
        AVER_ERROR("usage: ConvertTool <in.gltf|in.glb> <out-directory> [base-name]");
        return 2;
    }
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
    // Best-effort: a mesh too small/degenerate to cluster (see TrifactorTest's degenerate cases)
    // still gets saved, just without an MLET chunk -- the mesh is not lost over an optional feature.
    if (!addMeshlets(m, &why)) {
        AVER_WARN("clustering '{}': {} (saving without meshlets)", res.meshNames[0], why);
    } else {
        AVER_INFO("clustered '{}': {} meshlets", res.meshNames[0], m.meshlets.size());
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
