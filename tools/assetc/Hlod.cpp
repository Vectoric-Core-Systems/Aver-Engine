#include "AssetC.hpp"

// Clustering is entirely OPTIONAL, exactly as in ConvertTool.cpp: this tool must still import and
// write .ocmesh with AVER_MODULE_TRIFACTOR=OFF (the tree's default), so every use of it is behind
// this guard, reached only through the link interface (`if(TARGET Aver.Trifactor)` below).
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

namespace assetc {

// ---- hlod: per-cell proxy meshes for the HLOD ring (docs/LEVEL_STREAMING.md) -----------------------------------
//
// Needs the level's streaming data (`stream`). Merges the static placements of each cell into one mesh, simplifies it to a
// per-cell triangle budget (Trifactor when built in) and records it in the .ocstream; only cells whose content hash changed rebuild.
int runHlod(int argc, char** argv) {
    std::string levelPath, contentDir, projectDir;
    f32 hlodM = 0.0f;
    fmt::OcHlodOptions opt;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--content-dir" && i + 1 < argc)       contentDir = argv[++i];
        else if (a == "--project-dir" && i + 1 < argc)  projectDir = argv[++i];
        else if (a == "--hlod" && i + 1 < argc)         hlodM = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--max-tris" && i + 1 < argc)     opt.maxTriangles = static_cast<u32>(std::atoi(argv[++i]));
        else if (a == "--min-tris" && i + 1 < argc)     opt.minTriangles = static_cast<u32>(std::atoi(argv[++i]));
        else if (a == "--drop-smaller" && i + 1 < argc) opt.dropSmallerCm = static_cast<f32>(std::atof(argv[++i])) * 100.0f;
        else if (a == "--drop-enclosed")                opt.dropEnclosed = true;
        else if (a == "--force")                        opt.force = true;
        else if (!a.empty() && a[0] != '-' && levelPath.empty()) levelPath = a;
        else { AVER_ERROR("[hlod] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (levelPath.empty() || contentDir.empty() || opt.maxTriangles < opt.minTriangles) {
        AVER_ERROR("[hlod] usage: AverAssetC hlod <level.ocworld> --content-dir <dir> [--project-dir <dir>] [--hlod m] [--max-tris n] "
                   "[--min-tris n] [--drop-smaller m] [--drop-enclosed] [--force]");
        return exitCode(ExitCode::Usage);
    }
    if (projectDir.empty()) projectDir = std::filesystem::path(contentDir).parent_path().string();
    opt.hlodCm = hlodM * 100.0f;
#if AVER_MODULE_TRIFACTOR
    opt.simplify = [](fmt::OcMeshData& m, u32 budget) {
        u32 got = 0;
        std::string why;
        const bool ok = aver::trifactor::simplifyToBudget(m, budget, &got, &why);
        if (!ok) AVER_WARN("[hlod] simplify: {} (kept {} triangles)", why, got);
        return got > 0;
    };
#endif

    fmt::OcWorldData w;
    std::string err;
    if (!fmt::loadOcworld(levelPath, w, &err)) {
        AVER_ERROR("[hlod] cannot load '{}': {}", levelPath, err);
        return exitCode(ExitCode::Failed);
    }
    if (!w.stream.enabled) {
        AVER_ERROR("[hlod] '{}' has no STREAM record: run `AverAssetC stream` first", levelPath);
        return exitCode(ExitCode::Failed);
    }
    {   // bounds place each placement in its cell: take them from the streaming data
        fmt::OcStreamData sd;
        if (!w.stream.dataPath.empty() && fmt::loadOcStream((std::filesystem::path(projectDir) / w.stream.dataPath).string(), sd))
            fmt::applyOcStreamBounds(w, sd);
    }
    fmt::OcHlodReport rep;
    const std::string stem = std::filesystem::path(levelPath).stem().string();
    if (!fmt::bakeHlod(w, stem, contentDir, projectDir, opt, rep, &err) || !fmt::saveOcworld(levelPath, w, &err)) {
        AVER_ERROR("[hlod] {}", err);
        return exitCode(ExitCode::Failed);
    }
    AVER_INFO("[hlod] {}: {} cell(s): {} built, {} up to date, {} removed, {} failed; {} placement(s) merged, {} excluded ({} by data layer); "
              "{} -> {} triangles", levelPath, rep.cells, rep.built, rep.fresh, rep.removed, rep.failed, rep.placementsMerged,
              rep.placementsExcluded, rep.excludedByLayer, rep.trianglesIn, rep.trianglesOut);
    for (const std::string& m : rep.warnings) AVER_WARN("[hlod] {}", m);
    return exitCode(rep.failed ? ExitCode::Failed : ExitCode::Ok);
}

} // namespace assetc
