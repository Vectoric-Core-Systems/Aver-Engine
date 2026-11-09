#include "AssetC.hpp"

namespace assetc {

// ---- stream: generate a level's streaming data (docs/LEVEL_STREAMING.md) -------------------------
//
// The level gets STREAM settings, a data= reference and placement ids; bounds and cell-sorted foliage go
// to <project>/Binaries/Streaming (fmt::bakeOcStream, shared with the editor's Regenerate button).
int runStream(int argc, char** argv) {
    std::string levelPath, contentDir, projectDir;
    f32 cellM = 64.0f, loadM = 250.0f, evictM = 300.0f, foliageCellM = 64.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--content-dir" && i + 1 < argc)       contentDir = argv[++i];
        else if (a == "--project-dir" && i + 1 < argc)  projectDir = argv[++i];
        else if (a == "--cell" && i + 1 < argc)         cellM = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--load" && i + 1 < argc)         loadM = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--evict" && i + 1 < argc)        evictM = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--foliage-cell" && i + 1 < argc) foliageCellM = static_cast<f32>(std::atof(argv[++i]));
        else if (!a.empty() && a[0] != '-' && levelPath.empty()) levelPath = a;
        else { AVER_ERROR("[stream] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (levelPath.empty() || contentDir.empty() || cellM <= 0 || foliageCellM <= 0 || loadM <= 0 ||
        evictM <= loadM) {
        AVER_ERROR("[stream] usage: AverAssetC stream <level.ocworld> --content-dir <dir> [--project-dir <dir>] "
                   "[--cell m] [--load m] [--evict m (> load)] [--foliage-cell m]");
        return exitCode(ExitCode::Usage);
    }
    if (projectDir.empty()) projectDir = std::filesystem::path(contentDir).parent_path().string();

    fmt::OcWorldData w;
    std::string err;
    if (!fmt::loadOcworld(levelPath, w, &err)) {
        AVER_ERROR("[stream] cannot load '{}': {}", levelPath, err);
        return exitCode(ExitCode::Failed);
    }
    w.stream.enabled = true;
    w.stream.cellCm = cellM * 100.0f;
    w.stream.loadCm = loadM * 100.0f;
    w.stream.evictCm = evictM * 100.0f;
    fmt::OcStreamBakeOptions opt;
    opt.foliageCellCm = foliageCellM * 100.0f;
    fmt::OcStreamBakeReport rep;
    const std::string stem = std::filesystem::path(levelPath).stem().string();
    if (!fmt::bakeOcStream(w, stem, contentDir, projectDir, opt, rep, &err) || !fmt::saveOcworld(levelPath, w, &err)) {
        AVER_ERROR("[stream] {}", err);
        return exitCode(ExitCode::Failed);
    }
    std::string dirs;
    for (const std::string& d : w.stream.lazyDirs) dirs += (dirs.empty() ? "" : ",") + d;
    AVER_INFO("[stream] {}: {} placements ({} with bounds, {} new ids), foliage {} instances in {} cells of {} "
              "table(s), data {}, lazy dirs: {}", levelPath, rep.placements, rep.withBounds, rep.newIds,
              rep.foliageInstances, rep.foliageCells, rep.foliageTables, w.stream.dataPath,
              dirs.empty() ? "(none)" : dirs);
    return exitCode(ExitCode::Ok);
}

} // namespace assetc
