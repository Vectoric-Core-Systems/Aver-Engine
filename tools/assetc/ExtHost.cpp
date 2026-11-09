#include "AssetC.hpp"

namespace assetc {

// ---- the model importer an extension calls (the service "aver.asset.import-model") ----
//
// One model file (glTF, GLB, FBX, OBJ) written as meshes, materials, textures, skeletons and clips under
// <contentDir>/<outSubdir>, returning each mesh's content path and where it sits in the model. The JSON artifact
// lines the importer emits go to stdout like any other run; the summary line is the extension command's own.
struct ImportedModelPart {
    std::string meshAsset;   // content-relative path of the .ocmesh
    std::string name;        // the source node's name
    double x = 0, y = 0, z = 0;
    int node = -1;           // the source file's node index; -1 when the format has none (FBX, OBJ)
};

struct ImportedModel {
    bool ok = false;
    std::string error;
    std::vector<ImportedModelPart> parts;
    std::vector<std::string> notes;   // anything the importer dropped
};

ImportedModel importModelFile(const std::string& input, const std::string& contentDir, const std::string& outSubdir,
                              const std::string& base) {
    ImportedModel out;
    const std::string outDir = contentDir + "/" + outSubdir;
    createDirectories(outDir);
    const std::string ext = extOf(input);
    RunStats stats;
    std::vector<std::string> stems;
    std::vector<ScenePlacement> placements;
    ModelConvertArgs margs;
    margs.input = input; margs.outDir = outDir; margs.base = base; margs.contentDir = contentDir;
    margs.stemsOut = &stems; margs.placementsOut = &placements;

    int code = exitCode(ExitCode::Failed);
    std::vector<std::string> positionNames;
    std::vector<int> positionNodes;   // parallel to positionNames: the glTF node a placement came from, -1 when unknown
    if (ext == ".gltf" || ext == ".glb") {
        fmt::GltfImportResult res;
        std::string why;
        if (!fmt::importGltf(input, res, {}, &why)) { out.error = why; return out; }
        code = convertImportedModel(margs, res, stats);
        for (const fmt::GltfPlacement& p : res.placements) { positionNames.push_back(p.name); positionNodes.push_back(p.node); }
        if (placements.empty())
            for (usize i = 0; i < res.meshes.size(); ++i) {
                placements.push_back(ScenePlacement{i32(i), Vec3{0, 0, 0}});
                positionNames.push_back(i < res.meshNames.size() ? res.meshNames[i] : std::string{});
                positionNodes.push_back(-1);
            }
        for (const std::string& u : res.unsupported) out.notes.push_back("unsupported in the model: " + u);
    }
#if AVER_HAVE_FBX
    else if (ext == ".fbx") {
        fmt::FbxImportResult res;
        std::string why;
        if (!fmt::importFbx(input, res, fmt::FbxImportOptions{}, &why)) { out.error = why; return out; }
        code = convertImportedModel(margs, res, stats);
        for (const fmt::GltfPlacement& p : res.placements) positionNames.push_back(p.name);
        if (placements.empty())
            for (usize i = 0; i < res.meshes.size(); ++i) {
                placements.push_back(ScenePlacement{i32(i), Vec3{0, 0, 0}});
                positionNames.push_back(i < res.meshNames.size() ? res.meshNames[i] : std::string{});
            }
        for (const std::string& u : res.unsupported) out.notes.push_back("unsupported in the model: " + u);
    }
#endif
    else if (ext == ".obj") {
        fmt::ObjImportResult res;
        std::string why;
        if (!fmt::importObj(input, res, {}, &why)) { out.error = why; return out; }
        if (res.meshes.empty()) { out.error = "no meshes"; return out; }
        {
            std::vector<fmt::ImportedMaterial> mats;
            std::vector<fmt::ImportedImage> imgs;
            std::vector<std::string> mwarn;
            const usize slash = input.find_last_of("/\\");
            const std::string baseDir = slash == std::string::npos ? std::string() : input.substr(0, slash);
            fmt::objMaterialsToImported(res.materials, baseDir, mats, imgs, &mwarn);
            for (const std::string& w : mwarn) out.notes.push_back(w);
            cookAndRewriteSlots(mats, imgs, contentDir, base, res.meshes, 0);
        }
        std::vector<MeshItem> items;
        for (usize i = 0; i < res.meshes.size(); ++i)
            items.push_back(MeshItem{i < res.meshNames.size() ? res.meshNames[i] : std::string{}, res.meshes[i], -1});
        code = writeMeshItems(input, outDir, base, items, false, 0.0f, stats, &stems) ? exitCode(ExitCode::Failed) : exitCode(ExitCode::Ok);
        for (usize i = 0; i < res.meshes.size(); ++i) {
            placements.push_back(ScenePlacement{i32(i), Vec3{0, 0, 0}});
            positionNames.push_back(i < res.meshNames.size() ? res.meshNames[i] : std::string{});
        }
    } else {
        out.error = "not a model file the importers read (" + ext + ")";
        return out;
    }

    if (code != exitCode(ExitCode::Ok)) { out.error = "the importer reported a failure for " + input; return out; }
    for (usize i = 0; i < placements.size(); ++i) {
        const ScenePlacement& p = placements[i];
        if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size() || stems[usize(p.meshIndex)].empty()) continue;
        ImportedModelPart part;
        part.meshAsset = outSubdir + "/" + stems[usize(p.meshIndex)] + ".ocmesh";
        part.name = i < positionNames.size() ? positionNames[i] : std::string{};
        part.node = i < positionNodes.size() ? positionNodes[i] : -1;
        part.x = p.position.x; part.y = p.position.y; part.z = p.position.z;
        out.parts.push_back(std::move(part));
    }
    out.ok = !out.parts.empty();
    if (!out.ok) out.error = "the model produced no meshes";
    return out;
}

// importModelFile's result as the plain structs aver_ext.h defines. Everything the arrays point at lives in `hostData`, which
// hostFreeModelResult releases.
struct ModelResultStorage {
    std::string error;
    std::vector<std::string> strings;   // meshAsset and name of every part, then the notes
    std::vector<AverExtModelPart> parts;
    std::vector<const char*> notes;
};

int32_t hostImportModel(const AverExtModelArgs* args, AverExtModelResult* out) {
    if (!args || !out) return 1;
    ImportedModel m = importModelFile(args->input ? args->input : "", args->contentDir ? args->contentDir : "",
                                      args->outSubdir ? args->outSubdir : "", args->base ? args->base : "");
    auto* s = new ModelResultStorage();
    s->error = m.error;
    s->strings.reserve(m.parts.size() * 2 + m.notes.size());
    for (const ImportedModelPart& p : m.parts) { s->strings.push_back(p.meshAsset); s->strings.push_back(p.name); }
    for (const std::string& n : m.notes) s->strings.push_back(n);
    for (usize i = 0; i < m.parts.size(); ++i) {
        const ImportedModelPart& p = m.parts[i];
        s->parts.push_back(AverExtModelPart{s->strings[i * 2].c_str(), s->strings[i * 2 + 1].c_str(), p.x, p.y, p.z, int32_t(p.node)});
    }
    for (usize i = 0; i < m.notes.size(); ++i) s->notes.push_back(s->strings[m.parts.size() * 2 + i].c_str());
    out->ok = m.ok ? 1 : 0;
    out->error = s->error.c_str();
    out->parts = s->parts.data();
    out->partCount = uint32_t(s->parts.size());
    out->notes = s->notes.data();
    out->noteCount = uint32_t(s->notes.size());
    out->hostData = s;
    return m.ok ? 0 : 1;
}

void hostFreeModelResult(AverExtModelResult* r) {
    if (!r) return;
    delete static_cast<ModelResultStorage*>(r->hostData);
    r->hostData = nullptr;
    r->parts = nullptr; r->partCount = 0; r->notes = nullptr; r->noteCount = 0; r->error = "";
}

// The extensions installed beside the executable, with this tool's services on offer (aver_ext_services.h). Scanned when a subcommand is not
// one of ours, or for --help.
void openExtensions(ext::Registry& registry) {
    static const AverExtImportModelService importModelService = {sizeof(AverExtImportModelService), hostImportModel, hostFreeModelResult};
    ext::ServiceRegistry& services = ext::ServiceRegistry::instance();
    services.remove(AVER_EXT_SERVICE_IMPORT_MODEL, AVER_EXT_SERVICE_IMPORT_MODEL_VERSION);
    services.add(AVER_EXT_SERVICE_IMPORT_MODEL, AVER_EXT_SERVICE_IMPORT_MODEL_VERSION, &importModelService);
    registry.scanDefault();
}

// One usage line per command an installed extension lists.
std::string extensionUsage(const ext::Registry& registry) {
    std::string text;
    for (const ext::Extension& e : registry.extensions()) {
        if (!e.ready()) continue;
        for (const ext::CommandInfo& c : e.commands) {
            text += "\n       AverAssetC " + c.name + (c.usage.empty() ? std::string() : " " + c.usage);
            text += "\n                          " + (c.summary.empty() ? std::string("a command of the extension") : c.summary) + " [extension " + e.name + "]";
        }
    }
    return text;
}

} // namespace assetc
