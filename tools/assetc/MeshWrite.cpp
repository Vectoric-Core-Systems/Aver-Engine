#include "AssetC.hpp"

#if AVER_HAVE_MATERIAL_COMPILE
// Under the SAME guard as the `material` subcommand, and reusing its macro rather than adding a
// second one: both need exactly Aver.Formats.Material to be linked, and two names for one condition
// is a worse thing to keep in step than one name doing two jobs.
//
// ImportCook.hpp, not MaterialCook.hpp directly: cookAndRewriteSlots below is now a thin wrapper
// around fmt::cookImportedMaterials, which does the opacity fold, the texture-size cap and the call
// into cookMaterials itself. See ImportCook.hpp for why that policy layer lives in Aver.Formats.Material
// rather than in this tool.
#include "aver/formats/ImportCook.hpp"
#endif

// Clustering is entirely OPTIONAL, exactly as in ConvertTool.cpp: this tool must still import and
// write .ocmesh with AVER_MODULE_TRIFACTOR=OFF (the tree's default), so every use of it is behind
// this guard, reached only through the link interface (`if(TARGET Aver.Trifactor)` below).
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

namespace assetc {

namespace {

// ---- optional decimation + clustering, ported from ConvertTool.cpp unchanged ----

#if AVER_MODULE_TRIFACTOR
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;
    std::string hierWhy;
    if (!aver::trifactor::buildLodHierarchy(m, dag, &hierWhy))
        AVER_WARN("buildLodHierarchy: {} (saving LOD 0 only)", hierWhy);
    return aver::trifactor::packLodDag(dag, m, why);
}

// Best-effort, exactly like ConvertTool: a mesh too small/degenerate to cluster still gets saved,
// just without an MLET chunk.
void applyLodAndClustering(fmt::OcMeshData& m, f32 lodRatio, const std::string& label) {
    std::string why;
    if (lodRatio > 0.0f) {
        const usize before = m.indices.size() / 3;
        if (!aver::trifactor::simplifyMesh(m, lodRatio, &why)) {
            AVER_WARN("--lod {}: {} (saving '{}' at full density)", lodRatio, why, label);
        } else {
            AVER_INFO("simplified '{}' to {:.1f}%: {} -> {} tris", label, double(lodRatio) * 100.0,
                      before, m.indices.size() / 3);
        }
    }
    // Said before it starts: a dense mesh (scanned foliage, a photogrammetry rock) takes minutes here, and a run that prints nothing reads as hung.
    AVER_INFO("clustering '{}': {} tris, building meshlets and LODs", label, m.indices.size() / 3);
    const auto t0 = std::chrono::steady_clock::now();
    if (!addMeshlets(m, &why)) {
        AVER_WARN("clustering '{}': {} (saving without meshlets)", label, why);
    } else {
        AVER_INFO("clustered '{}': {} LOD(s), {} meshlets at LOD 0, {:.1f} s", label, m.lodCount(), m.meshlets.size(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
}
#endif

// --material-map: rules and matching live in aver/formats/MaterialMap.hpp.

// ---- writing and verifying one mesh, ported from ConvertTool.cpp unchanged ----

// Writes one .ocmesh, reloads it, and confirms the skin (the one stream that used to be dropped
// silently) survived the round trip. Reports the outcome as one JSON line; never throws.
bool writeAndVerifyMesh(const std::string& input, const std::string& path, const fmt::OcMeshData& m,
                        RunStats& stats) {
    std::string why;
    if (m.submeshes.size() > 255) {   // a merged USD prefab of many unmaterialled parts
        fmt::OcMeshData merged = m;
        const usize before = merged.submeshes.size();
        fmt::coalesceAdjacentSubmeshes(merged);
        AVER_INFO("{}: {} submeshes merged to {} (neighbours naming the same material)", path, before,
                  merged.submeshes.size());
        if (merged.submeshes.size() <= 255) return writeAndVerifyMesh(input, path, merged, stats);
    }
    if (!fmt::saveOcMesh(path, m, &why)) {
        emitArtifact(input, path, "mesh", false, false, why, stats);
        return false;
    }
    fmt::OcMeshData back;
    if (!fmt::loadOcMesh(path, back, &why)) {
        emitArtifact(input, path, "mesh", true, false, "reload failed: " + why, stats);
        return false;
    }
    if (m.hasSkin() && !back.hasSkin()) {
        emitArtifact(input, path, "mesh", true, false, "the skin did not survive the round trip", stats);
        return false;
    }
    std::string extra = "\"vertices\":" + std::to_string(back.vertexCount())
        + ",\"triangles\":" + std::to_string(back.indices.size() / 3)
        + ",\"hasSkin\":" + (back.hasSkin() ? "true" : "false")
        + ",\"morphTargets\":" + std::to_string(back.morphTargets.size());
    emitArtifact(input, path, "mesh", true, true, {}, stats, extra);
    return true;
}


// Merges every item into the first, following ConvertTool.cpp's exact rule: two items resolving to
// the SAME skin index share one bone-index space and merge their joints/weights too; a different (or
// one-sided) skin would remap joints wrongly, so the merge stops there instead, keeping only what
// was already merged. See ConvertTool.cpp for the full reasoning -- this is that algorithm, lifted
// out so both tools call the same logic rather than risk two copies drifting apart.
fmt::OcMeshData mergeAll(const std::vector<MeshItem>& items, usize& mergedCount, std::string* warn) {
    fmt::OcMeshData m = items[0].data;
    mergedCount = 1;
    const i32 accSkin = items[0].skinIndex;
    for (usize mi = 1; mi < items.size(); ++mi) {
        const fmt::OcMeshData& src = items[mi].data;
        if (src.positions.empty() || src.indices.empty()) continue;

        const i32 srcSkin = items[mi].skinIndex;
        const bool sameSkeleton = src.hasSkin() && m.hasSkin() && accSkin >= 0 && srcSkin == accSkin;
        if ((src.hasSkin() || m.hasSkin()) && !sameSkeleton) {
            if (warn) {
                *warn = "'" + items[mi].name + "' is skinned by a different skeleton than the "
                    + std::to_string(mergedCount) + " mesh(es) merged so far; only "
                    + std::to_string(mergedCount) + " of " + std::to_string(items.size())
                    + " meshes were merged";
            }
            break;
        }

        const u32 base = m.vertexCount();
        const u32 firstIndex = static_cast<u32>(m.indices.size());
        m.positions.insert(m.positions.end(), src.positions.begin(), src.positions.end());
        m.normals.insert(m.normals.end(), src.normals.begin(), src.normals.end());
        m.uvs.insert(m.uvs.end(), src.uvs.begin(), src.uvs.end());
        // R1 extra streams: a part without one gets neutral values (white, no tangent -- w 0 makes the shader derive it -- UV1 0).
        {
            const auto mergeExtra = [&](std::vector<f32>& dst, bool dstHas, const std::vector<f32>& s, bool srcHas, usize comps,
                                        const f32* fill) {
                if (!dstHas && !srcHas) return;
                if (!dstHas) {
                    dst.clear();
                    for (u32 v = 0; v < base; ++v) dst.insert(dst.end(), fill, fill + comps);
                }
                if (srcHas) dst.insert(dst.end(), s.begin(), s.end());
                else for (u32 v = 0; v < src.vertexCount(); ++v) dst.insert(dst.end(), fill, fill + comps);
            };
            static const f32 kWhite[4] = {1, 1, 1, 1}, kNone[4] = {0, 0, 0, 0};
            const bool hadColor = m.colors.size() == usize(base) * 4, hadTan = m.tangents.size() == usize(base) * 4,
                       hadUv1 = m.uvs1.size() == usize(base) * 2;
            mergeExtra(m.colors, hadColor, src.colors, src.hasColor(), 4, kWhite);
            mergeExtra(m.tangents, hadTan, src.tangents, src.hasTangent(), 4, kNone);
            mergeExtra(m.uvs1, hadUv1, src.uvs1, src.hasUv1(), 2, kNone);
        }
        if (sameSkeleton) {
            m.joints.insert(m.joints.end(), src.joints.begin(), src.joints.end());
            m.weights.insert(m.weights.end(), src.weights.begin(), src.weights.end());
        }
        for (const u32 idx : src.indices) m.indices.push_back(idx + base);
        fmt::appendMorphTargets(m, src, base);   // blend-shape entries index vertices

        const u32 slotBase = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.insert(m.materialSlots.end(), src.materialSlots.begin(), src.materialSlots.end());
        if (src.submeshes.empty()) {
            m.submeshes.push_back(fmt::OcMeshSubmesh{items[mi].name, slotBase, firstIndex,
                                                     static_cast<u32>(src.indices.size()), base,
                                                     src.vertexCount()});
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
    return m;
}


// Fills 12 contiguous floats at `out` with the row-vector world transform OcInstances.hpp's own
// convention describes, for one kept USD placement written as FOLIAGE instead of a PLACE/PLACEG entity.
//
// BUILT FROM THE QUATERNION AND SCALE DIRECTLY, or from the exact matrix when the placement has shear or
// a mirror. This used to go through world::eulerDegFromQuat / quatFromEulerDeg to reproduce what a PLACE
// line gets, but that round trip is only lossy at gimbal lock, where it gives the ENTITY the wrong
// orientation; a foliage row has no reason to inherit that. Transform::toMatrix() is the same
// scale*rotate*translate row-vector matrix world::instantiate builds for a root placement, so dropping
// its trailing (0,0,0,1) column is OcInstances.hpp's t[r*3+c] = M.m[r][c] convention.
void foliageInstanceTransform(const ScenePlacement& p, f32* out) {
    if (p.exact) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                out[r * 3 + c] = p.matrix[r * 4 + c];
        return;
    }
    Transform xf;
    xf.position = p.position;
    xf.rotation = p.rotation;
    xf.scale = p.scale;
    const Mat4 m = xf.toMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = m.m[r][c];
}

} // namespace


// Writes a level that puts the imported meshes back where the source scene had them.
//
// WHY THIS EXISTS. The importer used to bake a glTF node's translation into its vertices, so a
// 115-piece scene imported as 115 meshes each carrying its own world position in its geometry: the
// pieces lined up if you placed them all at the origin, and every one of them had its pivot metres
// away from itself. Taking the translation out fixes the pivot but throws the scene away unless
// somebody writes it down. This is somebody writing it down.
//
// CONTENT-RELATIVE, resolved the way the editor resolves it (contentDir + "\\" + ref), which is why
// this needs --content-dir. Without one there is no way to express the mesh path so a level can find
// it, and the level is skipped with a line saying so rather than written full of paths that resolve
// to nothing.
bool writeSceneLevel(const std::string& outDir, const std::string& contentDir,
                     const std::string& base,
                     const std::vector<ScenePlacement>& placements,
                     const std::vector<std::string>& stems,
                     const SceneCamera* camera, const SceneSun* sun,
                     // Kept USD PointInstancer instances written as FOLIAGE (the default) rather than
                     // as PLACE/PLACEG entities, and sheared or mirrored (exact) placements in any
                     // mode. Null or empty when there are none; under --instances-as entities the
                     // other instances are folded into `placements` instead.
                     const std::vector<ScenePlacement>* foliageInstances,
                     // Content-relative folder the .ocinst goes in ("Instances" by default).
                     const std::string& instancesDir) {
    const bool haveFoliage = foliageInstances && !foliageInstances->empty();
    if (placements.empty() && !haveFoliage) return true; // a single-object file needs no level
    if (contentDir.empty()) {
        AVER_WARN("{} placement(s) were recovered from the scene graph, but --content-dir was not "
                  "given, so a mesh path cannot be made content-relative and no level was written. "
                  "The meshes are correct and centred; place them by hand, or re-run with "
                  "--content-dir.", placements.size() + (haveFoliage ? foliageInstances->size() : 0));
        return true;
    }

    std::error_code ec;
    std::filesystem::path rel =
        std::filesystem::relative(std::filesystem::path(outDir), std::filesystem::path(contentDir), ec);
    if (ec || rel.empty() || rel.generic_string().rfind("..", 0) == 0) {
        AVER_WARN("--out-dir is not inside --content-dir, so mesh paths cannot be made "
                  "content-relative; no level was written");
        return true;
    }
    std::string prefix = rel.generic_string();
    if (prefix == ".") prefix.clear(); else prefix += "/";

    fmt::OcWorldData w;
    w.name = base;
    for (const ScenePlacement& p : placements) {
        if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size()) continue;
        const std::string& stem = stems[usize(p.meshIndex)];
        if (stem.empty()) continue;                      // merged away, or failed to write
        fmt::OcWorldPlacement op;
        op.asset = prefix + stem + ".ocmesh";
        op.x = p.position.x; op.y = p.position.y; op.z = p.position.z;
        // Through the level format's own Euler encoding, so the loader's quatFromEulerDeg gives back
        // exactly this rotation.
        const Vec3 e = world::eulerDegFromQuat(p.rotation);
        op.roll = e.x; op.pitch = e.y; op.yaw = e.z;
        op.sx = p.scale.x; op.sy = p.scale.y; op.sz = p.scale.z;
        op.collide = p.collide;
        if (!p.animClip.empty()) op.animClip = prefix + p.animClip + ".ocanim";
        w.placements.push_back(std::move(op));
    }
    if (w.placements.empty() && !haveFoliage) return true;
    if (camera) {
        // The editor opens the level here instead of framing the whole of it; speed 0 = the user's own.
        w.hasCamera = true;
        w.camX = camera->position.x; w.camY = camera->position.y; w.camZ = camera->position.z;
        w.camYaw = camera->yawDeg; w.camPitch = camera->pitchDeg;
        w.camSpeed = 0.0;
    }
    if (sun) {
        // Direction and disc size only: colour, illuminance and the sky stay the engine's physical
        // defaults, whose atmosphere reddens and dims a low sun by itself.
        w.hasSun = true;
        w.sunDir[0] = sun->direction.x; w.sunDir[1] = sun->direction.y; w.sunDir[2] = sun->direction.z;
        w.sunAngularDeg = sun->angularDeg;
    }

    // Into <content>/Maps, where the editor's level list looks, rather than beside the meshes: a
    // .ocworld sitting in a Meshes folder is findable by nothing.
    const std::string mapsDir = contentDir + "/Maps";
    createDirectories(mapsDir);
    // NEVER OVERWRITE A LEVEL THAT IS ALREADY THERE. The level is named after the SOURCE FILE, so
    // importing a "JungleRuins.gltf" into a project that already has a JungleRuins.ocworld would
    // replace a level somebody has been building with a bare list of freshly imported meshes. An
    // import is not a thing anyone expects to destroy their work, and this one runs unattended from
    // the launcher. Suffixing is the boring, recoverable answer.
    std::string path = mapsDir + "/" + base + ".ocworld";
    if (fileExists(path)) {
        int n = 2;
        std::string alt;
        do { alt = mapsDir + "/" + base + "_" + std::to_string(n++) + ".ocworld"; }
        while (fileExists(alt) && n < 1000);
        AVER_WARN("{} already exists and was NOT touched; the imported scene went to {} instead",
                  path, alt);
        path = alt;
    }
    // THE .ocinst CARRIES THE (possibly suffixed) STEM OF THE .ocworld IT WAS BUILT FOR -- a re-import that
    // lands as "JungleRuins_2.ocworld" gets "JungleRuins_2.ocinst", not a table misnamed after the level
    // that did NOT get overwritten. `path` already carries that stem, decided by the dedup loop above.
    const std::string levelStem = stemOf(path);
    if (haveFoliage) {
        // GROUP BY PROTOTYPE MESH ASSET. Two passes rather than one: the first counts each prototype's
        // instances (and records the order prototypes are first seen in, so the groups in the file
        // read in a stable, deterministic order run after run), which is what lets the second pass
        // write every instance straight into its final slot in ONE allocation -- no growing buffer, no
        // second bulk copy, the same "no per-instance allocation" contract OcInstances.hpp's own
        // fast-path comment asks of a caller building a multi-million-row table.
        std::vector<i32> order;
        std::unordered_map<i32, u32> counts;
        for (const ScenePlacement& p : *foliageInstances) {
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size() || stems[usize(p.meshIndex)].empty())
                continue;                                   // merged away, or failed to write
            auto [it, inserted] = counts.try_emplace(p.meshIndex, 0u);
            if (inserted) order.push_back(p.meshIndex);
            ++it->second;
        }

        fmt::OcInstanceData inst;
        std::unordered_map<i32, u32> baseOf, cursorOf;
        u32 running = 0;
        for (const i32 mi : order) {
            const u32 count = counts[mi];
            // CAST-SHADOW BY DEFAULT: OcInstances.hpp's kOcInstanceFlagCastShadow is not yet an
            // authorable choice anywhere in this pipeline, so every group this importer writes sets
            // it -- foliage that could not cast a shadow at all would be a visible regression against
            // the entities it replaces, which always could.
            inst.groups.push_back(fmt::OcInstanceGroup{prefix + stems[usize(mi)] + ".ocmesh",
                                                       fmt::kOcInstanceFlagCastShadow, running, count});
            baseOf[mi] = running;
            cursorOf[mi] = 0;
            running += count;
        }
        inst.transforms.resize(usize(running) * 12);
        for (const ScenePlacement& p : *foliageInstances) {
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size() || stems[usize(p.meshIndex)].empty())
                continue;
            u32& cursor = cursorOf[p.meshIndex];
            foliageInstanceTransform(p, &inst.transforms[usize(baseOf[p.meshIndex] + cursor) * 12]);
            ++cursor;
        }

        // Under the level's stem, but never over a table that is already there (a different level's, or
        // one whose .ocworld was deleted): the same _2 rule the level itself follows.
        const std::string instDir = contentDir + "/" + instancesDir;
        createDirectories(instDir);
        std::string instStem = levelStem;
        for (int n = 2; fileExists(instDir + "/" + instStem + ".ocinst") && n < 1000; ++n)
            instStem = levelStem + "_" + std::to_string(n);
        const std::string instPath = instDir + "/" + instStem + ".ocinst";
        std::string instWhy;
        if (!fmt::saveOcInstances(instPath, inst, &instWhy)) {
            AVER_WARN("could not write the foliage instance table {}: {}", instPath, instWhy);
            return false;
        }
        // Content-relative, exactly like every other asset path this function writes (`op.asset`
        // above) -- but relative to `contentDir` directly, NOT through `prefix` (`outDir`'s own
        // relative path, the Meshes folder).
        w.foliageFiles.push_back(instancesDir + "/" + instStem + ".ocinst");

        std::error_code sizeEc;
        const std::uintmax_t bytes = std::filesystem::file_size(instPath, sizeEc);
        AVER_INFO("wrote {}: {} group(s), {} instance(s), {:.1f} MiB -- ray-traced foliage, no "
                  "collision, not individually selectable",
                  instPath, inst.groups.size(), running,
                  sizeEc ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    std::string why;
    if (!fmt::saveOcworld(path, w, &why)) {
        AVER_WARN("could not write the scene level {}: {}", path, why);
        return false;
    }
    AVER_INFO("wrote {} with {} placement(s) -- open it to see the source scene reassembled",
              path, w.placements.size());
    return true;
}

// Writes `items` as one merged .ocmesh (--merge) or one .ocmesh per item (the default), naming each
// output after its own mesh name -- sanitised, and de-duplicated within this run so two same-named
// source meshes cannot overwrite one another in the staging directory. Shared by all three mesh
// formats; glTF calls this and then separately writes its skeleton/clips, since OBJ and USD have
// neither.
// `outStems`, when given, receives the stem each item was written under, PARALLEL TO `items` --
// empty for an item that was merged away. The scene writer needs it because the stem is decided
// here, by safe()/dedup, and re-deriving it there would be a second implementation of a naming rule
// that only has to disagree once to write a level full of dangling mesh paths.
bool writeMeshItems(const std::string& input, const std::string& outDir, const std::string& base,
                    const std::vector<MeshItem>& items, bool merge, f32 lodRatio, RunStats& stats,
                    std::vector<std::string>* outStems) {
    bool anyFailed = false;
    if (outStems) outStems->assign(items.size(), std::string{});
    if (merge && items.size() > 1) {
        usize mergedCount = 0;
        std::string warn;
        fmt::OcMeshData m = mergeAll(items, mergedCount, &warn);
        if (!warn.empty()) AVER_WARN("{}", warn);
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, base);
#endif
        if (!writeAndVerifyMesh(input, outDir + "/" + base + ".ocmesh", m, stats)) anyFailed = true;
        // Merged: one file, so there is no per-item stem to report and a scene cannot be rebuilt
        // from it. Left empty rather than pointing every placement at the merged mesh, which would
        // stamp the whole model once per placement.
        return anyFailed;
    }

    std::vector<std::string> used;
    usize emptied = 0;
    for (usize i = 0; i < items.size(); ++i) {
        // Every submesh dropped (a material map's '-' rules): nothing to write, and not a failure.
        // Its stem stays empty, so its placements are skipped.
        if (items[i].data.indices.empty() && items[i].data.submeshes.empty()) { ++emptied; continue; }
        fmt::OcMeshData m = items[i].data;
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, items[i].name.empty() ? base : items[i].name);
#else
        (void)lodRatio;
#endif
        std::string stem = items.size() == 1 ? base : safe(items[i].name, base + std::to_string(i));
        // The stem carries the subfolder, so de-duplication and the level's asset path both see it.
        if (!items[i].subdir.empty()) {
            const std::string sub = safe(items[i].subdir, "Group");
            createDirectories(outDir + "/" + sub);
            stem = sub + "/" + stem;
        }
        std::string candidate = stem;
        int suffix = 1;
        while (std::find(used.begin(), used.end(), candidate) != used.end())
            candidate = stem + "_" + std::to_string(++suffix);
        used.push_back(candidate);
        if (!writeAndVerifyMesh(input, outDir + "/" + candidate + ".ocmesh", m, stats)) anyFailed = true;
        else if (outStems) (*outStems)[i] = candidate;
    }
    if (emptied) AVER_INFO("{} mesh(es) had every submesh dropped by the material map; not written", emptied);
    return anyFailed;
}


// Thin wrapper around fmt::cookImportedMaterials (aver/formats/ImportCook.hpp), which now owns the
// cook itself: opacity folding, the texture-size cap, the call into fmt::cookMaterials, and the
// mesh material-slot rewrite. MOVED there because the editor's own import needs the identical policy
// and could not reach into this tool's .cpp for it. What stays here is what is genuinely this TOOL's:
// the geometry-only fallback when no --content-dir was given, and turning the shared function's
// out-params back into this tool's own AVER_INFO/AVER_WARN lines so its console output is unchanged.
//
// A no-op when `contentDir` is empty, which is the launcher-less case: geometry still imports, and
// the caller is told what it is leaving behind rather than losing it silently.
void cookAndRewriteSlots(std::vector<fmt::ImportedMaterial>& materials,
                         std::vector<fmt::ImportedImage>& images,
                         const std::string& contentDir, const std::string& base,
                         std::vector<fmt::OcMeshData>& meshes, u32 maxTexture) {
    if (materials.empty() && images.empty()) return;
    // Only the READABLE images are worth counting at the user: an entry whose bytes could not be
    // loaded is carried so a second material naming the same missing file is not retried, and
    // reporting it here would name a texture the cook is never going to write either way.
    usize readable = 0;
    for (const fmt::ImportedImage& img : images) if (img.ok) ++readable;
#if AVER_HAVE_MATERIAL_COMPILE
    if (contentDir.empty()) {
        AVER_WARN("this file has {} material(s) and {} image(s); pass --content-dir <dir> to "
                  "write them, otherwise only geometry is imported",
                  materials.size(), readable);
        return;
    }
    std::vector<std::string> cwarn;
    std::string cerr;
    u32 materialsWritten = 0, texturesWritten = 0;
    // overwriteExisting = true: a stated output directory is a directive here -- this tool is
    // invoked per asset by the launcher, and a re-import that silently kept the old material would
    // be a worse surprise than one that replaced it. Exactly this tool's previous, hardcoded choice.
    if (!fmt::cookImportedMaterials(materials, images, contentDir, base, meshes, maxTexture,
                                    /*overwriteExisting=*/true, &cwarn, &cerr,
                                    &materialsWritten, &texturesWritten)) {
        AVER_WARN("materials: {}", cerr);
        return;
    }
    for (const std::string& w : cwarn) AVER_WARN("materials: {}", w);
    AVER_INFO("wrote {} material(s) and {} texture(s) under {}",
              materialsWritten, texturesWritten, contentDir);
#else
    (void)contentDir; (void)base; (void)meshes; (void)maxTexture;
    AVER_WARN("this build has no PBR module, so the file's {} material(s) and {} image(s) were not "
              "imported; geometry only", materials.size(), readable);
#endif
}

} // namespace assetc
