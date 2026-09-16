// Command-line tool: imports a glTF/GLB file and writes the .oc* assets it contains.
//
// It writes the WHOLE TRIPLE -- mesh, skeleton, clips -- rather than only the first mesh, because a
// rig that arrives without its skeleton and its animation is not an importable asset, it is a static
// mesh with some unreachable extra streams. That was the state of this tool until skinning had a
// consumer, and it is why nothing downstream could be tested against a real file.
#include "aver/formats/GltfImport.hpp"
#if AVER_HAVE_MATERIAL_COOK
// Reached only through the link interface, the same way Aver.Trifactor is below: the header pulls
// in pbr::MaterialDesc, which does not exist in a tree built with AVER_MODULE_PBR off.
#include "aver/formats/MaterialCook.hpp"
#endif
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/core/Log.hpp"

// Clustering is entirely OPTIONAL: ConvertTool must still import and write the .oc* triple with
// AVER_MODULE_TRIFACTOR=OFF (the tree's default -- see modules/trifactor/CMakeLists.txt on why it
// is off by default). AVER_MODULE_TRIFACTOR reaches this translation unit only through the link
// interface (tests/formats/CMakeLists.txt's `if(TARGET Aver.Trifactor)` block), the same mechanism
// Runtime/src/GameContent.cpp uses for AVER_MODULE_PBR/AVER_MODULE_SCENE.
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

// UNGUARDED, and it was not. <algorithm> sat inside the block above while the multi-mesh merge
// below -- which has nothing to do with Trifactor -- uses std::min/std::max, so a build with the
// module off failed on a header the merge never asked for. The same guard-scoping shape that has
// been fixed seventeen times in this tree.
#include <algorithm>
#include <string>

using namespace aver;

#if AVER_MODULE_TRIFACTOR
namespace {
// Builds the FULL LOD hierarchy (buildClusters for LOD 0, buildLodHierarchy for every coarser level)
// and persists all of it into `m` via aver::trifactor::packLodDag. Returns false (mesh saved without
// meshlets, exactly as if Trifactor were absent) only when buildClusters itself fails or packLodDag
// refuses the converted hierarchy -- a clustering failure on some pathological input is not a reason
// to fail an otherwise-good import, and ConvertTool's job is "wire it", not "referee it".
// buildLodHierarchy failing (it does not, on any input buildClusters accepted -- see its own doc
// comment) is deliberately non-fatal: the mesh still saves with LOD 0 only, same as before this
// function existed.
//
// THE CONVERSION ITSELF USED TO LIVE HERE, as this function's own private toMeshlets/toIndices plus
// the packing logic now inlined below. It has moved into Aver.Trifactor (see
// aver::trifactor::packLodDag's own doc comment in ClusterBuilder.hpp for the full reasoning on why
// that module, not this one or Aver.Formats, is where it belongs) precisely so this tool and
// RelodTool's write path call the exact same code rather than risk two copies drifting apart --
// which is why this function is now three lines instead of sixty.
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;

    std::string hierWhy;
    if (!aver::trifactor::buildLodHierarchy(m, dag, &hierWhy))
        AVER_WARN("buildLodHierarchy: {} (saving LOD 0 only)", hierWhy);

    return aver::trifactor::packLodDag(dag, m, why);
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
    std::string contentDir;   // --content-dir: where Materials/ and Textures/ live
    int positional = argc;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--lod") {
            lodRatio = static_cast<f32>(std::atof(argv[i + 1]));
            if (i < positional) positional = i;
        } else if (a == "--content-dir") {
            // A SEPARATE FLAG FROM THE OUTPUT DIRECTORY, deliberately. The out-directory is where
            // meshes go and is routinely a scratch path; the content root is where the engine looks
            // for Materials/ and Textures/. Inferring one from the other would be a second fragile
            // convention. Absent, materials and textures are skipped and the tool says so.
            contentDir = argv[i + 1];
            if (i < positional) positional = i;
        }
    }
    argc = positional;   // hide the flags from the positional reads below

    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(argv[1], res, {}, &why)) { AVER_ERROR("import: {}", why); return 1; }
    for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
    if (res.meshes.empty()) { AVER_ERROR("no meshes"); return 1; }

    std::string dir = argv[2];
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    const std::string base = argc > 3 ? std::string(argv[3]) : stemOf(argv[1]);

    // ---- the materials and their textures ----
    // BEFORE THE MESH IS WRITTEN, and the order is load-bearing. Cooking renames each material to a
    // prefixed, collision-free stem, and the mesh's materialSlots have to be rewritten to match --
    // if the .ocmesh is serialised first it bakes in the OLD name and points at a material that is
    // not on disk under that name.
#if AVER_HAVE_MATERIAL_COOK
    if (!contentDir.empty() && (!res.materials.empty() || !res.images.empty())) {
        fmt::MaterialCookOptions copt;
        copt.contentDir = contentDir;
        copt.assetBase  = base;
        copt.overwriteExisting = true;   // a cook tool writing to a stated directory replaces
        fmt::MaterialCookResult cres;
        std::vector<std::string> cwarn;
        std::string cerr;
        if (!fmt::cookMaterials(res.materials, res.images, copt, cres, &cwarn, &cerr)) {
            AVER_ERROR("materials: {}", cerr);
        } else {
            for (const std::string& w : cwarn) AVER_WARN("materials: {}", w);
            for (usize i = 0; i < res.materials.size() && i < cres.materialSlotNames.size(); ++i) {
                if (cres.materialSlotNames[i].empty()) continue;   // did not cook; leave the old name
                const std::string& from = res.materials[i].name;
                const std::string& to   = cres.materialSlotNames[i];
                for (fmt::OcMeshData& m : res.meshes)
                    for (std::string& slot : m.materialSlots)
                        if (slot == from) slot = to;
            }
            AVER_INFO("wrote {} material(s) and {} texture(s) under {}",
                      cres.materialsWritten, cres.texturesWritten, contentDir);
        }
    } else if (contentDir.empty() && (!res.materials.empty() || !res.images.empty())) {
        AVER_WARN("this file has {} material(s) and {} image(s); pass --content-dir <dir> to write "
                  "them, otherwise only geometry is imported",
                  res.materials.size(), res.images.size());
    }
#else
    if (!res.materials.empty() || !res.images.empty())
        AVER_WARN("this build has no PBR module, so the file's {} material(s) and {} image(s) were "
                  "not imported; geometry only", res.materials.size(), res.images.size());
    (void)contentDir;
#endif

    // ---- the mesh ----
    // EVERY mesh in the file, merged into one, not just res.meshes[0].
    //
    // Taking the first one silently discarded most of most real assets, and did it without a word:
    // a glTF from any scanning or scattering library is routinely a SCENE of several meshes, and
    // this tool imported one of them and reported success. Measured against the source polycounts,
    // 11 of 15 Poly Haven assets came in short -- moss_01 arrived as 24 triangles out of 246,170,
    // and rock_moss_set_01 as 11,000 out of 63,127. Nothing failed; the .ocmesh was simply a
    // fragment, and the only way to notice was to already know what the number should have been.
    //
    // MERGED rather than one file per mesh, because these are single objects to whoever authored
    // them -- a "rock moss set" is one prop with several parts, and a scatter palette wants to place
    // it as one thing. Each source mesh becomes its own submesh, so the parts stay addressable and
    // keep their own material slots.
    //
    // A copy, not a const ref: clustering (when built) mutates the meshlets field in place, and the
    // alternative -- a second OcMeshData just for the clustered case -- would make the
    // AVER_MODULE_TRIFACTOR=OFF and =ON code paths save two DIFFERENT objects, which is exactly the
    // kind of divergence that only shows up once someone diffs the two builds' output.
    fmt::OcMeshData m = res.meshes[0];
    usize mergedCount = 1;
    // The skin every mesh merged into `m` so far shares (an index into res.skeletons), or -1 if
    // `m` itself is unskinned. Fixed at mesh 0's own skin: a same-skin merge never changes which
    // skeleton the combined joints/weights streams address, so later comparisons keep comparing
    // against this rather than something that could drift.
    const i32 accSkin = res.meshSkinIndex.empty() ? -1 : res.meshSkinIndex[0];
    for (usize mi = 1; mi < res.meshes.size(); ++mi) {
        const fmt::OcMeshData& src = res.meshes[mi];
        if (src.positions.empty() || src.indices.empty()) continue;

        // SKIN CANNOT BE MERGED BLIND -- unless the two meshes' JOINTS_0 indices already agree.
        // Two meshes bound to DIFFERENT skeletons address their JOINTS_0 through different index
        // spaces, so concatenating them would silently bind vertices to the wrong bones -- a rig
        // that looks intact and animates wrongly, which is why that case still refuses and keeps
        // only what was merged before it. But two meshes bound to the SAME skeleton (same
        // res.meshSkinIndex, which importGltf's importSkins() only assigns equal when it proved
        // the underlying skins are identical -- see GltfImport.cpp's own comment on why) are
        // already expressed in one shared bone-index space with zero remapping needed, so their
        // joints/weights streams merge exactly like positions/normals/uvs below.
        const i32 srcSkin = mi < res.meshSkinIndex.size() ? res.meshSkinIndex[mi] : -1;
        const bool sameSkeleton = src.hasSkin() && m.hasSkin() && accSkin >= 0 && srcSkin == accSkin;
        if ((src.hasSkin() || m.hasSkin()) && !sameSkeleton) {
            AVER_WARN("'{}' is skinned by a different skeleton than the {} mesh(es) merged so far; "
                      "merging it would remap its joints wrongly, so only {} of {} meshes were imported",
                      res.meshNames[mi], mergedCount, mergedCount, res.meshes.size());
            break;
        }

        const u32 base = m.vertexCount();
        const u32 firstIndex = static_cast<u32>(m.indices.size());
        m.positions.insert(m.positions.end(), src.positions.begin(), src.positions.end());
        m.normals.insert(m.normals.end(), src.normals.begin(), src.normals.end());
        m.uvs.insert(m.uvs.end(), src.uvs.begin(), src.uvs.end());
        if (sameSkeleton) {
            m.joints.insert(m.joints.end(), src.joints.begin(), src.joints.end());
            m.weights.insert(m.weights.end(), src.weights.begin(), src.weights.end());
        }
        for (const u32 idx : src.indices) m.indices.push_back(idx + base);

        // The source's material slots move across with it, and its submeshes are re-pointed at the
        // merged buffers. A submesh keeping its old slot index would silently repaint the part with
        // whatever material happened to sit at that index in the first mesh.
        const u32 slotBase = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.insert(m.materialSlots.end(), src.materialSlots.begin(), src.materialSlots.end());
        if (src.submeshes.empty()) {
            m.submeshes.push_back(fmt::OcMeshSubmesh{res.meshNames[mi], slotBase, firstIndex,
                                                     static_cast<u32>(src.indices.size()),
                                                     base, src.vertexCount()});
        } else {
            for (fmt::OcMeshSubmesh sm : src.submeshes) {
                sm.materialSlot += slotBase;
                sm.indexStart   += firstIndex;
                sm.baseVertex   += base;
                m.submeshes.push_back(std::move(sm));
            }
        }

        m.boundsMin = Vec3{std::min(m.boundsMin.x, src.boundsMin.x), std::min(m.boundsMin.y, src.boundsMin.y),
                           std::min(m.boundsMin.z, src.boundsMin.z)};
        m.boundsMax = Vec3{std::max(m.boundsMax.x, src.boundsMax.x), std::max(m.boundsMax.y, src.boundsMax.y),
                           std::max(m.boundsMax.z, src.boundsMax.z)};
        ++mergedCount;
    }

    AVER_INFO("imported '{}'{}: {} verts, {} tris, skin {}, bounds ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
              res.meshNames[0],
              mergedCount > 1 ? " (+" + std::to_string(mergedCount - 1) + " more merged)" : "",
              m.vertexCount(), m.indices.size() / 3, m.hasSkin() ? "yes" : "no",
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
