// Editor: model import (FBX and glTF). See ModelImportDialog.hpp. The writer below is the body of the old
// importGltfToDir (SandboxContentBrowser.cpp) moved here so FBX and glTF share it; the three additions
// (clip skeleton stems, the level's start camera, the record) are inert for glTF.

#include "ModelImportDialog.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"
#if AVER_MODULE_PBR
#include "aver/formats/ImportCook.hpp"
#endif

#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "ShellIntegration.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace aver {
namespace fsys = std::filesystem;

namespace {

ModelRecycleFn g_recycler = nullptr;

bool recycleFile(const std::string& path) {
    if (g_recycler) return g_recycler(path);
#if AVER_WITH_IMGUI
    return editor::moveToRecycleBin(path);
#else
    return false;
#endif
}

std::string lowerOf(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A name safe as a file stem on Windows (the invalid characters become '_'); never empty.
std::string sanitiseName(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (s.empty()) s = "unnamed";
    return s;
}

std::string leafName(const std::string& path) { return fsys::path(path).filename().string(); }

std::string countsText(const ModelImportSummary& s) {
    std::string c = std::to_string(s.meshesWritten) + " mesh(es), " + std::to_string(s.rigsWritten) +
                    " skeleton(s) and " + std::to_string(s.clipsWritten) + " clip(s)";
    if (s.materialsWritten || s.texturesWritten)
        c += ", with " + std::to_string(s.materialsWritten) + " material(s) and " + std::to_string(s.texturesWritten) + " texture(s)";
    return c;
}

} // namespace

// ---- routing -------------------------------------------------------------------------------------

const std::vector<ModelExtRoute>& modelExtensionRoutes() {
    static const std::vector<ModelExtRoute> kRoutes = {
        {".gltf", ModelFormat::Gltf}, {".glb", ModelFormat::Gltf}, {".fbx", ModelFormat::Fbx}};
    return kRoutes;
}

ModelFormat modelFormatOfPath(const std::string& path) {
    const std::string ext = lowerOf(fsys::path(path).extension().string());
    for (const ModelExtRoute& r : modelExtensionRoutes()) if (ext == r.ext) return r.format;
    return ModelFormat::None;
}

const char* modelFormatName(ModelFormat f) {
    return f == ModelFormat::Gltf ? "gltf" : f == ModelFormat::Fbx ? "fbx" : "";
}

void setModelImportRecycler(ModelRecycleFn fn) { g_recycler = fn; }

// ---- the shared writer ---------------------------------------------------------------------------

bool writeImportedModel(fmt::GltfImportResult& res, const std::string& src, const std::string& destDir,
                        const std::string& contentDir, bool overwrite, ModelImportSummary& out, std::string* why) {
    std::error_code ec;
    fsys::create_directories(destDir, ec);
    const std::string srcName = leafName(src);
    const std::string stem = fsys::path(src).stem().string();
    std::string werr;

    for (const std::string& u : res.unsupported) {
        AVER_WARN("[Import] '{}' contains {} - not imported", srcName, u);
        ++out.notes;
    }

#if AVER_MODULE_PBR
    // Must run BEFORE the meshes below are written: cookImportedMaterials rewrites each mesh's
    // materialSlots to the cooked stems, and that has to land before saveOcMesh serialises a mesh or the
    // .ocmesh would name a material never written under that name (AverAssetC's cookAndRewriteSlots has the
    // same ordering). A no-op when contentDir is empty. maxTexture 0 = no cap.
    if (!contentDir.empty() && (!res.materials.empty() || !res.images.empty())) {
        std::vector<std::string> matWarn;
        std::string matErr;
        u32 materialsWritten = 0, texturesWritten = 0;
        if (!fmt::cookImportedMaterials(res.materials, res.images, contentDir, stem, res.meshes,
                                        /*maxTexture=*/0, overwrite, &matWarn, &matErr,
                                        &materialsWritten, &texturesWritten)) {
            AVER_WARN("[Import] materials: {}", matErr);
        } else {
            out.materialsWritten = materialsWritten;
            out.texturesWritten = texturesWritten;
            for (const std::string& w : matWarn) AVER_WARN("[Import] materials: {}", w);
            if (materialsWritten || texturesWritten)
                AVER_INFO("[Import] {} -> {} material(s), {} texture(s) under {}", srcName, materialsWritten,
                          texturesWritten, contentDir);
            // The material files, for the record: the cooked slot names are <contentDir>/Materials/<stem>.ocmat.
            std::unordered_set<std::string> seen;
            for (const fmt::OcMeshData& m : res.meshes)
                for (const std::string& slot : m.materialSlots) {
                    if (!seen.insert(slot).second) continue;
                    const fsys::path mat = fsys::path(contentDir) / "Materials" / (slot + ".ocmat");
                    if (!fsys::exists(mat, ec)) continue;
                    const fsys::path rel = mat.lexically_normal().lexically_relative(fsys::path(destDir).lexically_normal());
                    if (!rel.empty()) out.outputs.push_back({"material", rel.generic_string()});
                }
        }
    }
#else
    (void)contentDir;
#endif

    // Parallel to res.meshes: stem each mesh was written under, or empty if skipped (invalid, or name
    // taken). The scene level below reuses these rather than re-deriving the naming rule.
    std::vector<std::string> stems(res.meshes.size());
    for (usize i = 0; i < res.meshes.size(); ++i) {
        fmt::OcMeshData& m = res.meshes[i];
        if (!m.valid()) { AVER_WARN("[Import] mesh {} came out empty and was skipped", i); continue; }

        std::string base = i < res.meshNames.size() && !res.meshNames[i].empty() ? res.meshNames[i] : stem;
        if (res.meshes.size() > 1 && base == stem) base += "_" + std::to_string(i);
        for (char& c : base) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                                 c == '"' || c == '<' || c == '>' || c == '|') c = '_';

        const std::string outFile = destDir + AVER_PATH_SEP + base + ".ocmesh";
        if (fsys::exists(outFile, ec)) {
            // Overwrite-aware skip: the true output name is only known here, which is why `overwrite`
            // reaches this far rather than the caller deleting a guess at what import would write.
            if (overwrite) {
                if (!recycleFile(outFile)) { AVER_WARN("[Import] '{}.ocmesh' could not be replaced", base); continue; }
                AVER_INFO("[Import] '{}.ocmesh' already existed - replaced", base);
            } else {
                AVER_WARN("[Import] '{}.ocmesh' already exists - not overwritten", base);
                continue;
            }
        }
        if (!fmt::saveOcMesh(outFile, m, &werr)) { AVER_WARN("[Import] {}", werr); continue; }
        AVER_INFO("[Import] {} -> {} ({} verts, {} tris)", srcName, base + ".ocmesh", m.vertexCount(), m.indices.size() / 3);
        stems[i] = base;
        out.outputs.push_back({"mesh", base + ".ocmesh"});
        ++out.meshesWritten;
    }

    // Lower-cased, so names are judged as collisions the way the file system judges them.
    const auto lower = [](std::string s) { return lowerOf(std::move(s)); };

    // The skeletal clips' file stems, decided once: the object clips below must not take one of them and
    // the clip loop at the bottom writes under exactly these names.
    std::vector<std::string> clipBases(res.animations.size());
    for (usize i = 0; i < res.animations.size(); ++i) {
        clipBases[i] = i < res.animationNames.size() && !res.animationNames[i].empty()
                     ? res.animationNames[i] : (stem + "_clip" + std::to_string(i));
        clipBases[i] = sanitiseName(clipBases[i]);
    }
    // The skeletons' file stems, also decided once: a clip's skeletonRef names its skeleton by stem.
    std::vector<std::string> skelStems(res.skeletons.size());
    for (usize i = 0; i < res.skeletons.size(); ++i) {
        std::string base = i < res.skeletonNames.size() && !res.skeletonNames[i].empty() ? res.skeletonNames[i] : stem;
        if (res.skeletons.size() > 1) base += "_" + std::to_string(i);
        skelStems[i] = sanitiseName(base);
    }

    // destDir as a content-relative prefix ("Meshes/Cars/"; empty for the Content root itself). A PLACE path
    // resolves against the Content root, not the level's own folder. No prefix (no contentDir, or destDir
    // outside it) = no level can be written, the same rule as AverAssetC's writeSceneLevel.
    std::string contentPrefix;
    bool havePrefix = false;
    if (!contentDir.empty()) {
        std::error_code relEc;
        const std::string rel = fsys::relative(destDir, contentDir, relEc).generic_string();
        if (!relEc && !rel.empty() && rel != ".." && rel.rfind("../", 0) != 0) {
            havePrefix = true;
            contentPrefix = rel == "." ? std::string() : rel + "/";
        }
    }

    // OBJECT CLIPS: the motion of a plain node (a car on a route, a fan), written BEFORE the level so a
    // placement only names a clip that reached disk. <stem>_<node>.ocanim, so none reads as one of the
    // armature's own clips. placementClip[i] = destDir-relative file of the clip placement i plays, "" for none.
    std::vector<std::string> placementClip(res.placements.size());
    {
        std::unordered_set<std::string> taken;
        for (const std::string& b : clipBases) taken.insert(lower(b));
        for (usize i = 0; i < res.objectAnimations.size(); ++i) {
            std::string name = i < res.objectAnimationNames.size() ? res.objectAnimationNames[i] : std::string();
            if (name.empty()) name = "ObjectClip" + std::to_string(i);
            name = sanitiseName(name);
            std::string clipStem = stem + "_" + name;
            if (!taken.insert(lower(clipStem)).second) {
                const std::string root = clipStem;
                for (int n = 2; !taken.insert(lower(clipStem = root + "_" + std::to_string(n))).second; ++n) {}
            }

            const std::string outFile = destDir + AVER_PATH_SEP + clipStem + ".ocanim";
            const bool clipExists = fsys::exists(outFile, ec);
            if (clipExists && !overwrite) {
                AVER_WARN("[Import] '{}.ocanim' already exists - not overwritten", clipStem);
                continue;
            }
            if (clipExists && !recycleFile(outFile)) {
                AVER_WARN("[Import] '{}.ocanim' could not be replaced", clipStem);
                continue;
            }
            if (!fmt::saveOcAnim(outFile, res.objectAnimations[i], &werr)) {
                AVER_WARN("[Import] {}", werr);
                continue;
            }
            AVER_INFO("[Import] {} -> {}.ocanim (object clip, {:.2f}s)", srcName, clipStem, res.objectAnimations[i].duration);
            out.outputs.push_back({"anim", clipStem + ".ocanim"});
            ++out.clipsWritten;

            // First clip written for a placement wins; a node animated in several animations gets the others as files only.
            const i32 pl = i < res.objectAnimationPlacement.size() ? res.objectAnimationPlacement[i] : -1;
            if (pl < 0 || usize(pl) >= placementClip.size()) {
                AVER_INFO("[Import] '{}.ocanim' animates a node with no mesh (a route): nothing plays it "
                          "until a placement names it", clipStem);
            } else if (placementClip[usize(pl)].empty()) {
                placementClip[usize(pl)] = clipStem + ".ocanim";
            } else {
                AVER_INFO("[Import] '{}.ocanim' animates the same mesh as '{}': the level plays the first, "
                          "assign this one by hand", clipStem, placementClip[usize(pl)]);
            }
        }
    }
    bool anyClipBound = false;
    for (const std::string& c : placementClip) if (!c.empty()) { anyClipBound = true; break; }

    // The scene layout: written when there is more than one placement, one of them plays an object clip,
    // or the file carried a camera. A single static PLACE record is not worth a file. Needed because the
    // importers no longer weld node translations into vertices.
    const bool wantLevel = res.placements.size() > 1 || anyClipBound || !res.cameras.empty();
    bool levelWritten = false;
    if (wantLevel && !havePrefix) {
        AVER_WARN("[Import] no scene layout was written: mesh paths in a level are relative to the "
                  "project's Content folder, and '{}' is {}", destDir,
                  contentDir.empty() ? "not inside a project" : "outside it");
    } else if (wantLevel) {
        fmt::OcWorldData w;
        w.name = stem;
        for (usize pi = 0; pi < res.placements.size(); ++pi) {
            const fmt::GltfPlacement& p = res.placements[pi];
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size()) continue;
            if (stems[usize(p.meshIndex)].empty()) continue;
            fmt::OcWorldPlacement op;
            op.asset = contentPrefix + stems[usize(p.meshIndex)] + ".ocmesh";
            op.x = p.position.x; op.y = p.position.y; op.z = p.position.z;
            if (!placementClip[pi].empty()) op.animClip = contentPrefix + placementClip[pi];
            w.placements.push_back(std::move(op));
        }
        // The first camera of the file is where the level opens (the editor camera's own convention).
        if (!res.cameras.empty()) {
            const fmt::ImportedCamera& cam = res.cameras[0];
            w.hasCamera = true;
            w.camX = cam.position.x; w.camY = cam.position.y; w.camZ = cam.position.z;
            w.camYaw = cam.yawDeg; w.camPitch = cam.pitchDeg;
            w.camSpeed = 0.0;
        }
        const std::string lvl = destDir + AVER_PATH_SEP + stem + ".ocworld";
        const bool lvlExists = fsys::exists(lvl, ec);
        if (w.placements.empty()) {
            // Nothing to say; not worth a file.
        } else if (lvlExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocworld' already exists - not overwritten, so the scene layout was not written", stem);
        } else if ((lvlExists && !recycleFile(lvl)) || !fmt::saveOcworld(lvl, w, &werr)) {
            AVER_WARN("[Import] could not write the scene layout: {}", werr);
        } else {
            AVER_INFO("[Import] {} -> {}.ocworld ({} placement(s), the source scene's own layout)", srcName, stem, w.placements.size());
            out.outputs.push_back({"world", stem + ".ocworld"});
            levelWritten = true;
        }
    }
    // A clip no level names never plays.
    if (anyClipBound && !levelWritten)
        AVER_WARN("[Import] the object clip(s) were written but no scene layout was, so nothing plays "
                  "them until a placed mesh names one");

    // The rig. Skeleton stems are decided above.
    for (usize i = 0; i < res.skeletons.size(); ++i) {
        const std::string& base = skelStems[i];
        const std::string outFile = destDir + AVER_PATH_SEP + base + ".ocskel";
        const bool skelExists = fsys::exists(outFile, ec);
        if (skelExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocskel' already exists - not overwritten", base);
        } else if (skelExists && !recycleFile(outFile)) {
            AVER_WARN("[Import] '{}.ocskel' could not be replaced", base);
        } else if (!fmt::saveOcSkel(outFile, res.skeletons[i], &werr)) {
            AVER_WARN("[Import] {}", werr);
        } else {
            AVER_INFO("[Import] {} -> {} ({} bone(s))", srcName, base + ".ocskel", res.skeletons[i].bones.size());
            out.outputs.push_back({"skeleton", base + ".ocskel"});
            ++out.rigsWritten;
        }
    }
    for (usize i = 0; i < res.animations.size(); ++i) {
        const std::string& base = clipBases[i];
        const std::string outFile = destDir + AVER_PATH_SEP + base + ".ocanim";
        const bool animExists = fsys::exists(outFile, ec);
        // A clip names its skeleton by file stem when the importer knows which one it animates.
        fmt::OcAnimation clip = res.animations[i];
        if (i < res.animationSkeleton.size() && res.animationSkeleton[i] >= 0 && usize(res.animationSkeleton[i]) < skelStems.size())
            clip.skeletonRef = skelStems[usize(res.animationSkeleton[i])];
        if (animExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocanim' already exists - not overwritten", base);
        } else if (animExists && !recycleFile(outFile)) {
            AVER_WARN("[Import] '{}.ocanim' could not be replaced", base);
        } else if (!fmt::saveOcAnim(outFile, clip, &werr)) {
            AVER_WARN("[Import] {}", werr);
        } else {
            AVER_INFO("[Import] {} -> {} ({:.2f}s, {} track(s))", srcName, base + ".ocanim", clip.duration, clip.tracks.size());
            out.outputs.push_back({"anim", base + ".ocanim"});
            ++out.clipsWritten;
        }
    }
    (void)why;
    return true;
}

// ---- the record ----------------------------------------------------------------------------------

fmt::ImportRecord makeImportRecord(const std::string& format, const std::string& src, const std::string& projectDir,
                                   const std::string& optionsText, const std::vector<fmt::ImportRecordOutput>& outputs) {
    fmt::ImportRecord r;
    r.format = format;
    r.options = optionsText;
    r.outputs = outputs;
    std::error_code ec;
    const fsys::path abs = fsys::absolute(fsys::path(src), ec).lexically_normal();
    std::string source = abs.generic_string();
    if (!projectDir.empty()) {
        const fsys::path proj = fsys::absolute(fsys::path(projectDir), ec).lexically_normal();
        const fsys::path rel = abs.lexically_relative(proj);
        if (!rel.empty() && *rel.begin() != "..") source = rel.generic_string();
    }
    r.source = source;
    const auto sz = fsys::file_size(abs, ec);
    r.sourceSize = ec ? 0 : static_cast<u64>(sz);
    ec.clear();
    const auto tm = fsys::last_write_time(abs, ec);
    r.sourceMTime = ec ? 0 : static_cast<i64>(tm.time_since_epoch().count());
    return r;
}

bool writeModelImportRecord(const std::string& destDir, const std::string& stem, fmt::ImportRecord rec,
                            std::string* why, std::string* writtenPath) {
    // Native separators, so the path matches what the caller builds with std::filesystem.
    const std::string path = fsys::path(fmt::importRecordPathFor(destDir, stem)).make_preferred().string();
    std::error_code ec;
    if (fsys::exists(path, ec)) {
        fmt::ImportRecord old;
        if (fmt::loadImportRecord(path, old, nullptr) && rec.extraLines.empty()) rec.extraLines = old.extraLines;
    }
    if (!fmt::saveImportRecord(path, rec, why)) return false;
    if (writtenPath) *writtenPath = path;
    return true;
}

bool importSourceChanged(const fmt::ImportRecord& rec, const std::string& projectDir) {
    const std::string src = fmt::importRecordSourcePath(rec, projectDir);
    std::error_code ec;
    if (!fsys::exists(src, ec)) return true;
    const auto sz = fsys::file_size(src, ec);
    if (!ec && rec.sourceSize != 0 && static_cast<u64>(sz) != rec.sourceSize) return true;
    ec.clear();
    const auto tm = fsys::last_write_time(src, ec);
    if (!ec && rec.sourceMTime != 0 && static_cast<i64>(tm.time_since_epoch().count()) != rec.sourceMTime) return true;
    return false;
}

std::vector<std::string> listImportRecords(const std::string& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (fsys::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lowerOf(it->path().extension().string()) == ".ocimport") found.push_back(it->path().string());
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool importFbxToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                    bool overwrite, const fmt::FbxImportOptions& opt, ModelImportSummary& out, std::string* why) {
    std::error_code dirEc;
    fsys::create_directories(destDir, dirEc);

    fmt::FbxImportResult res;
    std::string parseWhy;
    if (!fmt::importFbx(src, res, opt, &parseWhy)) {
        if (why) *why = parseWhy;
        return false;
    }
    {
        // The importer's own decisions (axes, unit, bind pose, what was dropped), one log line each.
        usize pos = 0;
        while (pos < res.report.size()) {
            usize eol = res.report.find('\n', pos);
            if (eol == std::string::npos) eol = res.report.size();
            if (eol > pos) { AVER_INFO("[Import] {}", res.report.substr(pos, eol - pos)); ++out.notes; }
            pos = eol + 1;
        }
    }
    if (!writeImportedModel(res, src, destDir, contentDir, overwrite, out, why)) return false;

    // The record, only when something was written: an import that found every file already there and
    // refused to replace them must not rewrite the options a later Reimport will use.
    if (out.meshesWritten || out.rigsWritten || out.clipsWritten) {
        std::string projectDir;
        if (!contentDir.empty()) projectDir = fsys::path(contentDir).lexically_normal().parent_path().string();
        if (!contentDir.empty() && !fsys::path(contentDir).has_filename())
            projectDir = fsys::path(contentDir).parent_path().parent_path().string();
        const fmt::ImportRecord rec = makeImportRecord("fbx", src, projectDir, fmt::fbxOptionsToText(opt), out.outputs);
        std::string recWhy;
        if (!writeModelImportRecord(destDir, fsys::path(src).stem().string(), rec, &recWhy, &out.recordPath))
            AVER_WARN("[Import] could not write the import record: {}", recWhy);
    }
    return true;
}

// ---- Reimport ------------------------------------------------------------------------------------

bool findReimportTarget(const std::string& filePath, const std::string& projectDir, ReimportTarget& out) {
    const fsys::path p(filePath);
    std::string recordPath;
    if (lowerOf(p.extension().string()) == ".ocimport") {
        recordPath = filePath;
    } else {
        const std::string name = p.filename().string();
        for (const std::string& rp : listImportRecords(p.parent_path().string())) {
            fmt::ImportRecord r;
            if (fmt::loadImportRecord(rp, r, nullptr) && fmt::importRecordHasOutput(r, name)) { recordPath = rp; break; }
        }
    }
    if (recordPath.empty()) return false;
    fmt::ImportRecord rec;
    if (!fmt::loadImportRecord(recordPath, rec, nullptr) || rec.format != "fbx") return false;
    out.recordPath = recordPath;
    out.record = rec;
    out.sourcePath = fmt::importRecordSourcePath(rec, projectDir);
    std::error_code ec;
    out.sourceExists = fsys::exists(out.sourcePath, ec);
    return true;
}

bool reimportModel(const ReimportTarget& t, const std::string& contentDir, const fmt::FbxImportOptions* overrideOpts,
                   ModelImportSummary& out, std::string* why) {
    if (!t.sourceExists) {
        if (why) *why = "the source is missing: " + t.sourcePath;
        return false;
    }
    fmt::FbxImportOptions opt;
    if (overrideOpts) opt = *overrideOpts;
    else if (!fmt::fbxOptionsFromText(t.record.options, opt, why)) return false;
    const std::string destDir = fsys::path(t.recordPath).parent_path().string();
    return importFbxToDir(t.sourcePath, destDir, contentDir, /*overwrite=*/true, opt, out, why);
}

// ---- the Import dialog, Reimport menu and options modal (UI) -------------------------------------

#if AVER_WITH_IMGUI
namespace {

struct PanelState {
    fmt::FbxImportOptions opt;
    std::string path;
    bool armed = false;
    std::string armedPath;
    fmt::FbxImportOptions armedOpt;
};
PanelState& panel() { static PanelState s; return s; }

struct PopupState {
    bool requestOpen = false;
    ReimportTarget target;
    fmt::FbxImportOptions opt;
    // A one-shot cache of the context menu's lookup: the menu draws every frame while it is open.
    std::string cachedPath;
    f64 cachedAt = -1.0;
    bool cachedFound = false;
    ReimportTarget cachedTarget;
};
PopupState& popup() { static PopupState s; return s; }

ModelReimportResult g_result;
bool g_haveResult = false;

// The option controls, shared by the Import dialog and the Import options modal. True when one changed.
bool drawOptionFields(fmt::FbxImportOptions& o) {
    bool changed = false;
    changed |= ImGui::DragFloat("Scale (cm per metre)", &o.scale, 0.5f, 0.001f, 100000.0f, "%.3f");
    changed |= ImGui::DragFloat("Unit override (m per file unit, 0 = the file's)", &o.unitMetersOverride, 0.001f, 0.0f, 1000.0f, "%.4f");
    changed |= ImGui::Checkbox("Meshes", &o.importMeshes);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Skeletons", &o.importSkeletons);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Animations", &o.importAnimations);
    changed |= ImGui::Checkbox("Materials and textures", &o.importMaterials);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Blend shapes", &o.importBlendShapes);
    int rm = static_cast<int>(o.rootMotion);
    static const char* const kRootMotion[] = {"Off", "Auto", "All"};
    if (ImGui::Combo("Root motion", &rm, kRootMotion, 3)) {
        o.rootMotion = static_cast<fmt::FbxImportOptions::RootMotion>(rm);
        changed = true;
    }
    int fps = static_cast<int>(o.bakeFps);
    if (ImGui::DragInt("Bake FPS", &fps, 1.0f, 1, 240)) { o.bakeFps = static_cast<u32>(fps); changed = true; }
    changed |= ImGui::Checkbox("Reduce keys", &o.reduceKeys);
    changed |= ImGui::Checkbox("File colours are linear", &o.fileColoursAreLinear);
    return changed;
}

void runReimport(const ReimportTarget& t, const std::string& contentDir, const fmt::FbxImportOptions* ov) {
    ModelReimportResult r;
    r.destDir = fsys::path(t.recordPath).parent_path().string();
    const std::string name = leafName(t.sourcePath);
    std::string why;
    if (!reimportModel(t, contentDir, ov, r.summary, &why)) {
        r.ok = false;
        r.title = "Could not reimport " + name;
        r.body = why;
    } else {
        r.ok = true;
        r.title = "Reimported " + name;
        r.body = countsText(r.summary);
        if (r.summary.notes) r.body += " (" + std::to_string(r.summary.notes) + " notes, see Output Log)";
    }
    g_result = std::move(r);
    g_haveResult = true;
}

} // namespace

void modelImportDialogReset() { panel() = PanelState{}; }

void drawFbxImportOptionsPanel(const char* sourcePath) {
    PanelState& st = panel();
    if (!sourcePath || modelFormatOfPath(sourcePath) != ModelFormat::Fbx) { st.path.clear(); return; }
    st.path = sourcePath;
    ImGui::Separator();
    ImGui::TextUnformatted("FBX options");
    drawOptionFields(st.opt);
    ImGui::TextDisabled("Dropping a file on the Content Browser imports with the defaults.");
    ImGui::Separator();
}

void modelImportArmDialogOptions(const char* sourcePath) {
    PanelState& st = panel();
    if (!sourcePath || modelFormatOfPath(sourcePath) != ModelFormat::Fbx) { st.armed = false; return; }
    st.armed = true;
    st.armedPath = sourcePath;
    st.armedOpt = st.opt;
}

void modelImportDisarm() { panel().armed = false; }

bool modelImportTakeArmedOptions(const std::string& sourcePath, fmt::FbxImportOptions& out) {
    PanelState& st = panel();
    if (!st.armed || st.armedPath != sourcePath) return false;
    out = st.armedOpt;
    return true;
}

void modelReimportMenuItems(const std::string& fullPath, const std::string& projectDir, const std::string& contentDir) {
    PopupState& ps = popup();
    const f64 now = ImGui::GetTime();
    if (ps.cachedPath != fullPath || now - ps.cachedAt > 0.5) {
        ps.cachedPath = fullPath;
        ps.cachedAt = now;
        ps.cachedFound = findReimportTarget(fullPath, projectDir, ps.cachedTarget);
    }
    if (!ps.cachedFound) return;
    const ReimportTarget& t = ps.cachedTarget;
    ImGui::Separator();
    ImGui::BeginDisabled(!t.sourceExists);
    if (ImGui::MenuItem("Reimport")) runReimport(t, contentDir, nullptr);
    if (ImGui::MenuItem("Import options...")) {
        ps.target = t;
        ps.opt = fmt::FbxImportOptions{};
        std::string ignored;
        fmt::fbxOptionsFromText(t.record.options, ps.opt, &ignored);
        ps.requestOpen = true;
    }
    ImGui::EndDisabled();
    if (!t.sourceExists) ImGui::TextDisabled("Source missing: %s", t.sourcePath.c_str());
    else if (importSourceChanged(t.record, projectDir)) ImGui::TextDisabled("The source has changed since this import.");
}

void drawModelImportPopups(const std::string& projectDir, const std::string& contentDir) {
    (void)projectDir;
    PopupState& ps = popup();
    if (ps.requestOpen) {
        ImGui::OpenPopup("Import options##model");
        ps.requestOpen = false;
    }
    if (!ImGui::BeginPopupModal("Import options##model", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Source: %s", ps.target.sourcePath.c_str());
    drawOptionFields(ps.opt);
    ImGui::Separator();
    if (ImGui::Button("Reimport")) {
        runReimport(ps.target, contentDir, &ps.opt);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool takeModelReimportResult(ModelReimportResult& out) {
    if (!g_haveResult) return false;
    out = std::move(g_result);
    g_result = ModelReimportResult{};
    g_haveResult = false;
    return true;
}
#endif // AVER_WITH_IMGUI

} // namespace aver
