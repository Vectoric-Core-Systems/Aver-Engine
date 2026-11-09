#include "AssetC.hpp"

#if AVER_HAVE_COOK
#include "aver/world/ChunkCook.hpp"  // `cook-chunks`: the authored-level chunk cook (the pure half of Aver.World, compiled into the tool)
#endif

#if AVER_HAVE_COOK
namespace assetc {

// ---- cook-chunks: an authored level into streamable region files (LARGE_WORLD 2.4.5) --------------------------------------
//
// The set goes to <project>/Binaries/Chunks/<level stem>/ and the level's STREAM record gets `chunks=` naming it, so a runtime
// streams the level from the set and falls back to its records if the set ever goes stale. It is world::cookAuthoredLevel, the
// function the editor's World Settings > Level Streaming > Cook Chunks calls, and it is deterministic (two cooks of one level are
// byte-identical), so running it at packaging is idempotent. The cook reads the level as f64 text and never builds a scene.
//
//   --check       write nothing: exit 0 when the set is present and current, 1 when it is missing or stale
//   --no-header   do not add `chunks=` to the level
int runCookChunks(int argc, char** argv) {
    std::string levelPath, contentDir, projectDir, outDir;
    f64 chunkCm = static_cast<f64>(world::kDefaultChunkSizeCm), wideM = 64.0;
    bool check = false, noHeader = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--content-dir" && i + 1 < argc)       contentDir = argv[++i];
        else if (a == "--project-dir" && i + 1 < argc)  projectDir = argv[++i];
        else if (a == "--out" && i + 1 < argc)          outDir = argv[++i];
        else if (a == "--chunk" && i + 1 < argc)        chunkCm = std::atof(argv[++i]);
        else if (a == "--wide" && i + 1 < argc)         wideM = std::atof(argv[++i]);
        else if (a == "--check")                        check = true;
        else if (a == "--no-header")                    noHeader = true;
        else if (!a.empty() && a[0] != '-' && levelPath.empty()) levelPath = a;
        else { AVER_ERROR("[cook-chunks] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (levelPath.empty() || contentDir.empty() || chunkCm < 1.0 || wideM <= 0.0) {
        AVER_ERROR("[cook-chunks] usage: AverAssetC cook-chunks <level.ocworld> --content-dir <dir> [--project-dir <dir>] "
                   "[--out <dir>] [--chunk <cm>] [--wide <m>] [--check] [--no-header]");
        return exitCode(ExitCode::Usage);
    }
    namespace fs = std::filesystem;
    if (projectDir.empty()) projectDir = fs::path(contentDir).parent_path().string();
    const std::string stem = fs::path(levelPath).stem().string();
    if (outDir.empty()) outDir = world::cookDirFor(projectDir, stem);
    std::error_code ec;
    std::string chunksValue = fs::relative(fs::path(outDir), fs::path(projectDir), ec).generic_string();
    if (ec || chunksValue.empty() || chunksValue.rfind("..", 0) == 0) chunksValue = fs::path(outDir).generic_string();
    std::string levelAsset = fs::relative(fs::path(levelPath), fs::path(contentDir), ec).generic_string();
    if (ec || levelAsset.empty() || levelAsset.rfind("..", 0) == 0) levelAsset = fs::path(levelPath).filename().generic_string();

    world::CookInput in;
    in.levelAsset = levelAsset;
    in.levelStem = stem;
    in.contentDir = contentDir;
    in.projectDir = projectDir;
    world::CookOptions opt;
    opt.chunkSizeCm = static_cast<i32>(chunkCm);
    opt.wideCm = wideM * 100.0;
    opt.outDir = outDir;

    fmt::OcWorldData w;
    std::string err;
    if (!fmt::loadOcworld(levelPath, w, &err)) {
        AVER_ERROR("[cook-chunks] cannot load '{}': {}", levelPath, err);
        return exitCode(ExitCode::Failed);
    }
    if (check) {
        world::AuthoredChunkSource src;
        if (!src.open(outDir, &err)) {
            AVER_WARN("[cook-chunks] {}: no cooked set ({})", stem, err);
            return exitCode(ExitCode::Failed);
        }
        const u64 now = world::cookSourceHash(w, in, opt);
        if (now != src.sourceHash()) {
            AVER_WARN("[cook-chunks] {}: the cooked set is stale", stem);
            return exitCode(ExitCode::Failed);
        }
        AVER_INFO("[cook-chunks] {}: the cooked set at {} is current", stem, outDir);
        return exitCode(ExitCode::Ok);
    }
    // The header first, then the cook reads the level back from disk: the hash must be of the text a runtime will parse.
    if (!noHeader && w.stream.chunksPath != chunksValue) {
        w.stream.chunksPath = chunksValue;
        if (!fmt::saveOcworld(levelPath, w, &err)) {
            AVER_ERROR("[cook-chunks] cannot write '{}': {}", levelPath, err);
            return exitCode(ExitCode::Failed);
        }
        w = fmt::OcWorldData{};
        if (!fmt::loadOcworld(levelPath, w, &err)) {
            AVER_ERROR("[cook-chunks] cannot reload '{}': {}", levelPath, err);
            return exitCode(ExitCode::Failed);
        }
    }
    world::CookReport rep;
    if (!world::cookAuthoredLevel(w, in, opt, rep, &err)) {
        AVER_ERROR("[cook-chunks] {}: {}", stem, err);
        return exitCode(ExitCode::Failed);
    }
    AVER_INFO("[cook-chunks] {}: {} placement(s) -> {} chunk-owned in {} chunk(s) / {} region(s), {} level-global "
              "(class {}, vehicle {}, sequence {}, no bounds {}), {} wide, {:.1f} MB, {:.0f} ms, hash {:016x}",
              stem, rep.flatPlacements, rep.chunkOwned, rep.chunks, rep.regions, rep.levelGlobal, rep.globalClass,
              rep.globalVehicle, rep.globalSequence, rep.globalUnbounded + rep.globalUnpackable, rep.wide,
              static_cast<f64>(rep.bytes) / (1024.0 * 1024.0), rep.elapsedMs, rep.sourceHash);
    for (const std::string& m : rep.warnings) AVER_WARN("[cook-chunks] {}", m);
    for (const std::string& a : rep.unknownBoundsAssets) AVER_WARN("[cook-chunks] no bounds for '{}'", a);
    AVER_INFO("[cook-chunks] set written to {}", rep.dir);
    return exitCode(ExitCode::Ok);
}

} // namespace assetc
#endif // AVER_HAVE_COOK
