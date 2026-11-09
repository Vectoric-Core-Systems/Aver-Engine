#include "AssetC.hpp"

#if AVER_HAVE_AUDIO_IMPORT
#include "aver/formats/OcAudio.hpp"
#endif

namespace assetc {

// ---- a model file's whole conversion, shared by glTF and FBX ----
//
// WHAT HAD BEEN THE glTF BRANCH OF main(), moved here verbatim so FBX takes exactly the same road: materials
// cooked and the mesh slots rewritten, one .ocmesh per mesh (or merged), object clips, the scene level, the
// skeletons, then the clips. Two additions, both inert for glTF: the level opens at the file's first camera,
// and a clip names the skeleton it animates (FbxImportResult::animationSkeleton).
int convertImportedModel(const ModelConvertArgs& args, fmt::GltfImportResult& res, RunStats& stats) {
    const std::string& input = args.input;
    const std::string& outDir = args.outDir;
    const std::string& base = args.base;
    const std::string& contentDir = args.contentDir;
    const bool merge = args.merge;
    const f32 lodRatio = args.lodRatio;
    const u32 maxTexture = args.maxTexture;
    const bool embedded = args.placementsOut != nullptr;
    std::string why;
    for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
    if (res.meshes.empty() && res.skeletons.empty() && res.animations.empty() && res.objectAnimations.empty()) {
        emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
        if (!embedded) emitSummary(input, stats, 1);
        return exitCode(ExitCode::Failed);
    }

    // ---- materials and their textures, BEFORE the meshes are copied into `items` ----
    //
    // THIS TOOL, NOT JUST THE HARNESS. The cook shipped first in tests/formats ConvertTool,
    // which is the dev harness; this is the compiler the launcher actually invokes, so until
    // now a glTF imported by the product arrived with its materials thrown away while the same
    // file imported by the harness did not.
    cookAndRewriteSlots(res.materials, res.images, contentDir, base, res.meshes, maxTexture);

    std::vector<MeshItem> items;
    items.reserve(res.meshes.size());
    for (usize i = 0; i < res.meshes.size(); ++i) {
        items.push_back(MeshItem{
            i < res.meshNames.size() ? res.meshNames[i] : std::string{},
            res.meshes[i],
            i < res.meshSkinIndex.size() ? res.meshSkinIndex[i] : -1});
    }

    std::vector<std::string> stems;
    bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats, &stems);
    std::vector<ScenePlacement> scene;
    scene.reserve(res.placements.size());
    for (const fmt::GltfPlacement& p : res.placements) scene.push_back(ScenePlacement{p.meshIndex, p.position});

    // ---- object clips, BEFORE the level so a placement only names a clip that was written ----
    //
    // The motion of a plain glTF node (a car on a route, a fan) as <base>_<node>.ocanim beside the
    // skeletal clips. Names are taken after the skeletal clips', so a node named like an armature
    // action cannot overwrite it.
    {
        // Compared lower-cased: Windows treats "Car" and "car" as ONE file, so the second clip
        // would overwrite the first.
        const auto fileKey = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::unordered_set<std::string> taken;
        for (usize i = 0; i < res.animations.size(); ++i)
            taken.insert(fileKey(safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                      "Clip" + std::to_string(i))));
        for (usize i = 0; i < res.objectAnimations.size(); ++i) {
            std::string name = safe(i < res.objectAnimationNames.size() ? res.objectAnimationNames[i] : "",
                                    "ObjectClip" + std::to_string(i));
            if (!taken.insert(fileKey(name)).second) {
                const std::string root = name;
                for (int n = 2; !taken.insert(fileKey(name = root + "_" + std::to_string(n))).second; ++n) {}
            }
            const fmt::OcAnimation& clip = res.objectAnimations[i];
            const std::string p = outDir + "/" + base + "_" + name + ".ocanim";
            if (!fmt::saveOcAnim(p, clip, &why)) {
                emitArtifact(input, p, "animation", false, false, why, stats);
                anyFailed = true;
                continue;
            }
            emitArtifact(input, p, "animation", true, true, {}, stats,
                         "\"durationSeconds\":" + std::to_string(clip.duration));
            const i32 pl = i < res.objectAnimationPlacement.size() ? res.objectAnimationPlacement[i] : -1;
            if (pl >= 0 && usize(pl) < scene.size()) {
                // A placement plays ONE clip, so the FIRST one the file lists keeps it; a later clip
                // for the same node is still written (it can be picked by hand) but says so here.
                if (scene[usize(pl)].animClip.empty()) {
                    scene[usize(pl)].animClip = base + "_" + name;
                } else {
                    AVER_WARN("'{}' is animated by more than one glTF animation: its placement plays '{}'; "
                              "'{}' was written but not attached", res.placements[usize(pl)].name,
                              scene[usize(pl)].animClip, base + "_" + name);
                }
            }
        }
    }
    // The first camera of the file is where the level opens (FBX carries them; glTF has none, so this is inert there).
    SceneCamera startCamera;
    if (!res.cameras.empty()) {
        const fmt::ImportedCamera& c = res.cameras.front();
        startCamera = SceneCamera{c.position, c.yawDeg, c.pitchDeg};
        AVER_INFO("level camera: the file's {} at ({:.0f}, {:.0f}, {:.0f}) yaw {:.1f} pitch {:.1f}", c.name,
                  c.position.x, c.position.y, c.position.z, c.yawDeg, c.pitchDeg);
    }
    if (embedded) {
        *args.placementsOut = scene;
        if (args.stemsOut) *args.stemsOut = stems;
    } else if (!writeSceneLevel(outDir, contentDir, base, scene, stems, res.cameras.empty() ? nullptr : &startCamera)) {
        anyFailed = true;
    }

    bool anySkinned = false;
    for (const fmt::OcMeshData& mesh : res.meshes) if (mesh.hasSkin()) { anySkinned = true; break; }
    if (anySkinned && res.skeletons.empty()) {
        AVER_WARN("the file carries skin but no skin node, so its joint indices address a "
                  "skeleton that was not written");
    }

    // ---- skeleton(s), named after `base` so a clip's skeletonRef resolves by stem ----
    for (usize i = 0; i < res.skeletons.size(); ++i) {
        const std::string p = outDir + "/" + base + (i == 0 ? "" : std::to_string(i)) + ".ocskel";
        if (!fmt::saveOcSkel(p, res.skeletons[i], &why)) {
            emitArtifact(input, p, "skeleton", false, false, why, stats);
            anyFailed = true;
        } else {
            emitArtifact(input, p, "skeleton", true, true, {}, stats,
                         "\"bones\":" + std::to_string(res.skeletons[i].bones.size()));
        }
    }

    // ---- clips ----
    for (usize i = 0; i < res.animations.size(); ++i) {
        fmt::OcAnimation clip = res.animations[i];
        // A clip names its skeleton by the file stem written above: skeleton i is <base><i>, skeleton 0 is <base>.
        // A curve-only clip (blend-shape weights on a file with no skeleton) names none.
        const usize skelIdx = i < res.animationSkeleton.size() && res.animationSkeleton[i] > 0 ? usize(res.animationSkeleton[i]) : 0;
        if (!clip.tracks.empty() || !res.skeletons.empty())
            clip.skeletonRef = base + (skelIdx == 0 ? std::string() : std::to_string(skelIdx));
        const std::string name = safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                      "Clip" + std::to_string(i));
        const std::string p = outDir + "/" + base + "_" + name + ".ocanim";
        if (!fmt::saveOcAnim(p, clip, &why)) {
            emitArtifact(input, p, "animation", false, false, why, stats);
            anyFailed = true;
        } else {
            emitArtifact(input, p, "animation", true, true, {}, stats,
                         "\"durationSeconds\":" + std::to_string(clip.duration) + ",\"curves\":" + std::to_string(clip.curves.size()));
        }
    }

    if (!embedded) emitSummary(input, stats, anyFailed ? 1 : 0);
    return exitCode(anyFailed ? ExitCode::Failed : ExitCode::Ok);
}

// `convert <input> --out-dir <dir> ...`: one model, OBJ, USD stage or audio file written as meshes, materials, a level and clips.
int runConvert(int argc, char** argv) {
    std::string input, outDir, baseOverride, contentDir;
    u32 maxTexture = 0;             // 0 = every texture through at its source resolution
    // WHERE A KEPT POINTINSTANCER INSTANCE GOES. FOLIAGE (the default) is what this comment used to
    // call the only option: a scattered stage declares millions of instances (Jungle Ruins: 8.7
    // million), and until .ocinst existed the level held one ENTITY per kept instance -- past ~16k
    // entities the frame was CPU-bound, so --max-instances defaulted to a number nowhere near the
    // stage's own count. Instanced foliage pays no such per-instance entity cost (ray-traced only,
    // one TLAS instance and no draw, no collision, not individually selectable -- see OcInstances.hpp
    // and docs/ASSET_IMPORT.md), so the budget below is sized for the FORMAT's own ceiling instead of
    // the entity system's, and --instances-as entities is what asks for the old behaviour verbatim.
    bool instancesAsEntities = false;
    // A USD stage's PointInstancer budget (UsdImportOptions). 0 on the command line lifts a limit;
    // left at 0 here and resolved AFTER argument parsing, once --instances-as is known, since the two
    // modes want different defaults (see just below the argument loop).
    u64 maxInstances = 0;
    u64 maxInstanceTris = 0;
    bool maxInstancesSet = false, maxInstanceTrisSet = false;
    // Densest around the stage's own camera -- the view it was built to be seen from -- within 100 m,
    // and any prototype with 2000 instances or fewer kept whole (see UsdImportOptions::focus).
    fmt::UsdInstanceFocus focusMode = fmt::UsdInstanceFocus::FirstCamera;
    Vec3 focusPoint{0, 0, 0};
    f32 focusRadius = 10000.0f;
    u64 keepAllBelow = 2000;
    std::vector<std::string> excludePrims;   // --exclude, repeatable and/or comma-separated
    std::string materialMapPath;              // --material-map
    bool materialMapLegacy = false;           // --material-map-legacy
    std::string instancesDir = "Instances";   // --instances-dir, content-relative
    std::string importReportPath;             // --import-report
    bool resolveCoplanarDrop = false;         // --resolve-coplanar drop-later
    std::vector<std::string> tracePrims;      // --trace-prims
    // FBX only (ignored for every other format): docs/FBX_IMPORT.md.
    f32 fbxScale = 100.0f, fbxUnitMeters = 0.0f;
    u32 fbxBakeFps = 30;
    std::string fbxRootMotion = "auto";
    bool fbxNoAnimations = false, fbxNoBlendShapes = false;
    bool merge = false;
    bool haveInput = false;
    f32 lodRatio = 0.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out-dir" && i + 1 < argc)      outDir = argv[++i];
        else if (a == "--base" && i + 1 < argc)    baseOverride = argv[++i];
        else if (a == "--merge")                   merge = true;
        else if (a == "--lod" && i + 1 < argc)      lodRatio = static_cast<f32>(std::atof(argv[++i]));
        // A SEPARATE FLAG FROM --out-dir, deliberately. The out-directory is where meshes go and is
        // routinely a scratch path; the content root is where the engine looks for Materials/ and
        // Textures/. Inferring one from the other would be a second fragile convention beside the
        // one kContentRelPrefix already admits is an assumption.
        else if (a == "--content-dir" && i + 1 < argc) contentDir = argv[++i];
        // --max-texture <n>: no imported texture wider or taller than n pixels. A 4K set costs 87 MB
        // of mips PER TEXTURE once the engine builds the chain, so three maps on each of a dozen
        // plants is a gigabyte of VRAM before anything else is in the scene.
        else if (a == "--max-texture" && i + 1 < argc) maxTexture = u32(std::atoi(argv[++i]));
        else if (a == "--instances-as" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "entities")     instancesAsEntities = true;
            else if (v == "foliage") instancesAsEntities = false;
            else {
                AVER_ERROR("--instances-as must be 'foliage' or 'entities', not '{}'", v);
                return exitCode(ExitCode::Usage);
            }
        }
        else if (a == "--max-instances" && i + 1 < argc) {
            maxInstances = std::strtoull(argv[++i], nullptr, 10);
            maxInstancesSet = true;
        }
        else if (a == "--max-instance-tris" && i + 1 < argc) {
            maxInstanceTris = std::strtoull(argv[++i], nullptr, 10);
            maxInstanceTrisSet = true;
        }
        // --focus camera | none | <x>,<y>  (engine centimetres)
        else if (a == "--focus" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "camera")    focusMode = fmt::UsdInstanceFocus::FirstCamera;
            else if (v == "none") focusMode = fmt::UsdInstanceFocus::Uniform;
            else {
                focusMode = fmt::UsdInstanceFocus::Point;
                const usize comma = v.find(',');
                focusPoint.x = static_cast<f32>(std::atof(v.substr(0, comma).c_str()));
                focusPoint.y = comma == std::string::npos ? 0.0f : static_cast<f32>(std::atof(v.substr(comma + 1).c_str()));
            }
        }
        else if (a == "--focus-radius" && i + 1 < argc) focusRadius = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--keep-all-below" && i + 1 < argc) keepAllBelow = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--material-map" && i + 1 < argc) materialMapPath = argv[++i];
        else if (a == "--material-map-legacy") materialMapLegacy = true;
        else if (a == "--import-report" && i + 1 < argc) importReportPath = argv[++i];
        else if (a == "--scale" && i + 1 < argc)         fbxScale = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--unit-meters" && i + 1 < argc)   fbxUnitMeters = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--bake-fps" && i + 1 < argc)      fbxBakeFps = static_cast<u32>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--no-animations")                 fbxNoAnimations = true;
        else if (a == "--no-blend-shapes")               fbxNoBlendShapes = true;
        else if (a == "--root-motion" && i + 1 < argc) {
            fbxRootMotion = argv[++i];
            if (fbxRootMotion != "off" && fbxRootMotion != "auto" && fbxRootMotion != "all") {
                AVER_ERROR("--root-motion must be 'off', 'auto' or 'all', not '{}'", fbxRootMotion);
                return exitCode(ExitCode::Usage);
            }
        }
        else if (a == "--instances-dir" && i + 1 < argc) {
            instancesDir = cleanContentRelDir(argv[++i]);
            if (instancesDir.empty()) {
                AVER_ERROR("--instances-dir must be a folder inside the content directory (relative, no '..')");
                return exitCode(ExitCode::Usage);
            }
        }
        else if (a == "--resolve-coplanar" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "drop-later")  resolveCoplanarDrop = true;
            else if (v == "report") resolveCoplanarDrop = false;
            else {
                AVER_ERROR("--resolve-coplanar must be 'report' or 'drop-later', not '{}'", v);
                return exitCode(ExitCode::Usage);
            }
        }
        else if (a == "--trace-prims" && i + 1 < argc) splitCommaList(argv[++i], tracePrims);
        else if (a == "--exclude" && i + 1 < argc) splitCommaList(argv[++i], excludePrims);
        else if (!haveInput) { input = a; haveInput = true; }
    }
    if (!haveInput || outDir.empty()) {
        AVER_ERROR("{}", kUsage);
        return exitCode(ExitCode::Usage);
    }
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    const std::string base = !baseOverride.empty() ? baseOverride : stemOf(input);

    // THE PER-MODE DEFAULTS, applied only where the command line left a budget unset.
    if (instancesAsEntities) {
        // UNCHANGED from every build before --instances-as existed.
        if (!maxInstancesSet)   maxInstances   = 12000;
        if (!maxInstanceTrisSet) maxInstanceTris = 150000000;
    } else {
        // FOLIAGE: instancing shares one BLAS per prototype, so the per-instance triangle cost that
        // motivated maxInstanceTris for entities does not apply here -- unlimited (0) is the default.
        if (!maxInstanceTrisSet) maxInstanceTris = 0;
        if (!maxInstancesSet) maxInstances = 4000000;
        // THE FORMAT'S OWN HARD CEILING, aver::voxi::VoxiRenderer::kMaxFoliageInstances
        // (modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp) -- duplicated here as a literal
        // rather than an include, since this tool does not and should not link Aver.Voxi for one
        // constant. 0 means "no limit" on the command line (UsdImportOptions' own contract), but
        // there is no such thing as unlimited FOLIAGE: a table past this many rows only wastes convert
        // time and disk, since setFoliage truncates past it (with its own warning) the moment the
        // level loads. Clamping here, rather than only at load, is what makes "what was kept" in the
        // log below the number that actually survives.
        constexpr u64 kMaxFoliageInstances = 8000000;
        if (maxInstances == 0 || maxInstances > kMaxFoliageInstances) maxInstances = kMaxFoliageInstances;
    }

    RunStats stats;
    const std::string ext = extOf(input);

    if (ext == ".gltf" || ext == ".glb") {
        fmt::GltfImportResult res;
        std::string why;
        if (!fmt::importGltf(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        ModelConvertArgs margs;
        margs.input = input; margs.outDir = outDir; margs.base = base; margs.contentDir = contentDir;
        margs.merge = merge; margs.lodRatio = lodRatio; margs.maxTexture = maxTexture;
        return convertImportedModel(margs, res, stats);
    }

#if AVER_HAVE_FBX
    // FBX (ufbx). The --scale / --unit-meters / --root-motion / --bake-fps / --no-* flags are FBX-only.
    if (ext == ".fbx") {
        fmt::FbxImportOptions fopt;
        fopt.scale = fbxScale;
        fopt.unitMetersOverride = fbxUnitMeters;
        fopt.bakeFps = fbxBakeFps;
        fopt.importAnimations = !fbxNoAnimations;
        fopt.importBlendShapes = !fbxNoBlendShapes;
        if (fbxRootMotion == "off") fopt.rootMotion = fmt::FbxImportOptions::RootMotion::Off;
        else if (fbxRootMotion == "all") fopt.rootMotion = fmt::FbxImportOptions::RootMotion::All;
        else fopt.rootMotion = fmt::FbxImportOptions::RootMotion::Auto;
        fmt::FbxImportResult res;
        std::string why;
        const bool parsed = fmt::importFbx(input, res, fopt, &why);
        if (!importReportPath.empty()) {
            std::ofstream rf(aver::nativePath(importReportPath), std::ios::binary);
            if (rf) {
                if (!parsed) rf << "IMPORT FAILED: " << why << '\n';
                rf << res.report;
                for (const std::string& u : res.unsupported) rf << "unsupported: " << u << '\n';
            }
        }
        if (!parsed) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        ModelConvertArgs margs;
        margs.input = input; margs.outDir = outDir; margs.base = base; margs.contentDir = contentDir;
        margs.merge = merge; margs.lodRatio = lodRatio; margs.maxTexture = maxTexture;
        return convertImportedModel(margs, res, stats);
    }
#endif

    if (ext == ".obj") {
        fmt::ObjImportResult res;
        std::string why;
        if (!fmt::importObj(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }

        // ---- the .mtl's materials, which until now were parsed and then dropped ----
        //
        // The map_* paths a .mtl writes are relative to the .obj's OWN directory, not to the
        // project content root, so that is what the converter resolves against. Warnings from it
        // name any texture the file points at and does not have.
        {
            std::vector<fmt::ImportedMaterial> mats;
            std::vector<fmt::ImportedImage> imgs;
            std::vector<std::string> mwarn;
            const usize slash = input.find_last_of("/\\");
            const std::string baseDir = slash == std::string::npos ? std::string() : input.substr(0, slash);
            fmt::objMaterialsToImported(res.materials, baseDir, mats, imgs, &mwarn);
            for (const std::string& w : mwarn) AVER_WARN("materials: {}", w);
            cookAndRewriteSlots(mats, imgs, contentDir, base, res.meshes, maxTexture);
        }

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                                     res.meshes[i], -1});
        }
        const bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return exitCode(anyFailed ? ExitCode::Failed : ExitCode::Ok);
    }

    if (ext == ".usd" || ext == ".usda" || ext == ".usdc") {
        fmt::UsdImportResult res;
        std::string why;
        // THE WHOLE STAGE: sublayers, binary layers, references and PointInstancers. A single text
        // layer imports exactly as importUsd would read it.
        fmt::UsdImportOptions uopt;
        uopt.maxInstances = maxInstances;
        uopt.maxInstanceTriangles = maxInstanceTris;
        uopt.focus = focusMode;
        uopt.focusPoint = focusPoint;
        uopt.focusRadius = focusRadius;
        uopt.keepAllBelow = keepAllBelow;
        uopt.excludePrims = excludePrims;
        uopt.tracePrims = tracePrims;
        uopt.resolveCoplanar = resolveCoplanarDrop ? fmt::UsdCoplanarPolicy::DropLater : fmt::UsdCoplanarPolicy::Report;
        // Instanced foliage is thinned by what can be SEEN at a distance (a tree stays dense far out,
        // moss does not): UsdImportOptions::focusBySize. Entities keep the per-prototype share.
        uopt.focusBySize = !instancesAsEntities;
        if (!fmt::importUsdStage(input, res, uopt, &why)) {
            // What the composer saw is the diagnosis of a failed import, so it is reported too.
            for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
            if (!importReportPath.empty()) {
                std::ofstream rf(aver::nativePath(importReportPath), std::ios::binary);
                if (rf) rf << "IMPORT FAILED: " << why << '\n' << fmt::usdImportReportText(res);
            }
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (!importReportPath.empty()) {
            std::ofstream rf(aver::nativePath(importReportPath), std::ios::binary);
            if (rf) {
                rf << fmt::usdImportReportText(res);
                AVER_INFO("--import-report: wrote {}", importReportPath);
            } else {
                AVER_WARN("--import-report: cannot write {}", importReportPath);
            }
        }
        if (res.instancing.instancers) {
            AVER_INFO("PointInstancers: {} instancer(s), {} prototype mesh(es), {} of {} instance(s) kept "
                      "(--max-instances {}, --max-instance-tris {}, --instances-as {})",
                      res.instancing.instancers, res.instancing.prototypes, res.instancing.keptInstances,
                      res.instancing.sourceInstances, maxInstances, maxInstanceTris,
                      instancesAsEntities ? "entities" : "foliage");
            if (res.instancing.focused)
                AVER_INFO("  focus ({:.0f}, {:.0f}), radius {:.0f} cm: {} of {} instance(s) inside it kept",
                          res.instancing.focusPoint.x, res.instancing.focusPoint.y, res.instancing.focusRadius,
                          res.instancing.keptInFocus, res.instancing.sourceInFocus);
            for (const fmt::UsdPrototypeStats& p : res.instancing.perPrototype) {
                const std::string name = p.meshIndex >= 0 && usize(p.meshIndex) < res.meshNames.size()
                                             ? res.meshNames[usize(p.meshIndex)] : std::string("?");
                if (p.fullDensityRadius > 0.0f)
                    AVER_INFO("  prototype {}: {} triangle(s), ~{:.0f} cm radius, {} of {} instance(s) kept, "
                              "full density within {:.0f} m", name, p.triangles, p.radius, p.kept, p.instances,
                              p.fullDensityRadius / 100.0f);
                else
                    AVER_INFO("  prototype {}: {} triangle(s), ~{:.0f} cm radius, {} of {} instance(s) kept", name,
                              p.triangles, p.radius, p.kept, p.instances);
            }
        }
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }

        // The importer has already resolved each mesh's `rel material:binding` to the material's own
        // name, so this rewrites those names to the cooked stems exactly as the glTF path does. A
        // mesh whose slot stayed empty -- no binding, or one behind a reference this importer does
        // not compose -- is untouched, and the import said so under `unsupported`.
        cookAndRewriteSlots(res.materials, res.images, contentDir, base, res.meshes, maxTexture);
        if (!materialMapPath.empty()) {
            fmt::MaterialMap mmap;
            if (!fmt::loadMaterialMap(materialMapPath, materialMapLegacy, mmap)) {
                emitArtifact(input, {}, "mesh", false, false, "cannot read --material-map " + materialMapPath, stats);
                emitSummary(input, stats, 2);
                return exitCode(ExitCode::Usage);
            }
            for (const std::string& w : mmap.warnings) AVER_WARN("--material-map {}: {}", materialMapPath, w);
            fmt::MaterialMapReport mmReport;
            const fmt::MaterialMapStats mmStats = fmt::applyMaterialMap(res.meshes, res.meshNames, mmap, &mmReport);
            const std::string mmReportPath = outDir + "/" + base + ".materialmap.report.txt";
            createDirectories(outDir);
            std::ofstream rf(aver::nativePath(mmReportPath), std::ios::binary);
            if (rf) {
                rf << fmt::materialMapReportText(mmap, mmReport, mmStats, input);
                AVER_INFO("--material-map: report written to {}", mmReportPath);
            } else {
                AVER_WARN("--material-map: cannot write the report {}", mmReportPath);
            }
        }

        // A stage spread over several folders is written the same way, one subfolder per source
        // folder; a single-folder stage stays flat.
        std::vector<std::string> groups = res.meshGroups;
        groups.resize(res.meshes.size());
        bool grouped = false;
        for (const std::string& g : groups) if (!g.empty() && g != groups.front()) { grouped = true; break; }

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            // THE LEAF OF THE PRIM PATH, not the whole path. UsdImportResult::meshNames holds
            // "/root/Grass_B_02/Grass_B_02" because that is what identifies a prim; safe() then
            // strips the slashes for a filename and produces rootGrass_B_02Grass_B_02.ocmesh, which
            // is unreadable and gets worse the deeper a scene nests. writeMeshItems already
            // de-duplicates a stem it has used before, so two prims sharing a leaf name are still
            // told apart -- by a numeric suffix rather than by a path nobody can read.
            std::string name = i < res.meshNames.size() ? res.meshNames[i] : std::string{};
            const usize leaf = name.find_last_of('/');
            if (leaf != std::string::npos) name = name.substr(leaf + 1);
            // MOVED, not copied: a composed stage's terrain alone can be gigabytes of vertices.
            items.push_back(MeshItem{std::move(name), std::move(res.meshes[i]), -1, grouped ? groups[i] : std::string{}});
        }
        std::vector<std::string> stems;
        bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats, &stems);
        std::vector<ScenePlacement> scene;
        std::vector<ScenePlacement> foliageInstances;
        usize exactPlacements = 0;
        scene.reserve(res.placements.size());
        for (const fmt::UsdPlacement& p : res.placements) {
            ScenePlacement sp{p.meshIndex, p.position,
                              Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}, p.scale};
            // AN INSTANCE IS SCATTER -- grass, moss, a sapling -- and a collision body per blade would
            // cost physics thousands of bodies for surfaces nobody should be stopped by. The stage's
            // own meshes (terrain, ruins, water) keep the default. An empty `name` is exactly how
            // UsdPlacement marks a PointInstancer instance (see its own header comment).
            const bool isInstancerInstance = p.name.empty();
            sp.collide = !isInstancerInstance;
            // SHEAR OR A MIRROR: a PLACE line holds position+Euler+scale and cannot carry it, so the
            // exact matrix goes to the .ocinst whatever the instance count or --instances-as says.
            if (p.exact) {
                sp.exact = true;
                for (int k = 0; k < 16; ++k) sp.matrix[k] = p.matrix[k];
                foliageInstances.push_back(sp);
                ++exactPlacements;
                continue;
            }
            // FOLIAGE, NOT AN ENTITY, unless --instances-as entities asked for the old behaviour: a
            // kept instance goes into its own list instead of `scene`, so writeSceneLevel writes it
            // into the level's .ocinst table and a FOLIAGE record rather than a PLACE/PLACEG line.
            if (isInstancerInstance && !instancesAsEntities) foliageInstances.push_back(sp);
            else scene.push_back(sp);
        }
        if (exactPlacements)
            AVER_INFO("{} placement(s) carry shear or a mirror and were written as instanced foliage "
                      "(no collision, not individually selectable)", exactPlacements);
        SceneCamera cam;
        if (!res.cameras.empty()) {
            const fmt::UsdCameraPose& c = res.cameras.front();
            cam = SceneCamera{c.position, c.yawDeg, c.pitchDeg};
            AVER_INFO("level camera: the stage's {} at ({:.0f}, {:.0f}, {:.0f}) yaw {:.1f} pitch {:.1f}", c.name,
                      c.position.x, c.position.y, c.position.z, c.yawDeg, c.pitchDeg);
        }
        SceneSun sun;
        if (res.sun.found) {
            sun = SceneSun{res.sun.direction, res.sun.angularDiameterDeg};
            const f32 elev = std::asin(std::fmax(-1.0f, std::fmin(1.0f, res.sun.direction.z))) * 57.2957795f;
            const f32 azim = std::atan2(res.sun.direction.y, res.sun.direction.x) * 57.2957795f;
            AVER_INFO("level sun: from {} -- elevation {:.1f}, azimuth {:.1f} degrees", res.sun.source, elev, azim);
        }
        if (!writeSceneLevel(outDir, contentDir, base, scene, stems, res.cameras.empty() ? nullptr : &cam,
                             res.sun.found ? &sun : nullptr,
                             foliageInstances.empty() ? nullptr : &foliageInstances, instancesDir))
            anyFailed = true;
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return exitCode(anyFailed ? ExitCode::Failed : ExitCode::Ok);
    }

#if AVER_HAVE_AUDIO_IMPORT
    if (fmt::isImportableAudio(input)) {
        audio::SoundData sound;
        fmt::AudioImportResult imp = fmt::audioImportFile(input, sound);
        if (!imp.ok) {
            emitArtifact(input, {}, "audio", false, false, imp.error, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        const std::string path = outDir + "/" + base + ".ocaudio";
        std::string why;
        if (!fmt::saveOcAudio(path, sound, stemOf(input), &why)) {
            emitArtifact(input, path, "audio", false, false, why, stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        audio::SoundData back;
        const bool reloaded = fmt::loadOcAudio(path, back, &why);
        if (!reloaded || back.frames() != sound.frames() || back.channels != sound.channels) {
            emitArtifact(input, path, "audio", true, false,
                         !reloaded && !why.empty() ? why : "reloaded audio does not match the source",
                         stats);
            emitSummary(input, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        emitArtifact(input, path, "audio", true, true, {}, stats,
                     "\"frames\":" + std::to_string(back.frames())
                     + ",\"channels\":" + std::to_string(back.channels)
                     + ",\"sampleRate\":" + std::to_string(back.sampleRate));
        emitSummary(input, stats, 0);
        return exitCode(ExitCode::Ok);
    }
#endif

    AVER_ERROR("unsupported input format: {}", input);
    emitArtifact(input, {}, "unknown", false, false, "unsupported input format", stats);
    emitSummary(input, stats, 2);
    return exitCode(ExitCode::Usage);
}

} // namespace assetc
