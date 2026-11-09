#include "AssetC.hpp"

#include "aver/anim/Retarget.hpp"   // `retarget`

namespace assetc {

namespace {

// ---- retarget: carry clips authored for one skeleton onto another (docs/ANIM_IK_RETARGET.md) --------------
//
// AverAssetC retarget <asset.ocretarget> --clips <file|dir>... --out <dir> [--suffix S] [--fps N] [--overwrite]
//                     [--content-dir <dir>]
//
// The retargeter names its two skeletons by content-relative path; they are found by walking up from the asset
// until <dir>/<path> exists (or under --content-dir). One result line per clip: ok+verified for a written clip
// (reloaded and validated), ok with "skipped":true for a clip that is not for the source skeleton or is additive /
// an object clip, and a failure otherwise. Exit 1 only on a failure.
std::string findContentFile(const std::string& assetPath, const std::string& contentDir, const std::string& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!contentDir.empty()) {
        const fs::path p = fs::path(contentDir) / rel;
        return fs::exists(p, ec) ? p.string() : std::string();
    }
    for (fs::path dir = fs::absolute(fs::path(assetPath), ec).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const fs::path p = dir / rel;
        if (fs::exists(p, ec)) return p.string();
        if (dir == dir.parent_path()) break;
    }
    return {};
}

} // namespace

int runRetarget(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::string assetPath, outDir, suffix, contentDir;
    std::vector<std::string> clipArgs;
    anim::RetargetOptions opt;
    bool overwrite = false, haveSuffix = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--clips") {
            while (i + 1 < argc && argv[i + 1][0] != '-') clipArgs.push_back(argv[++i]);
        }
        else if (a == "--out" && i + 1 < argc)         outDir = argv[++i];
        else if (a == "--suffix" && i + 1 < argc)      { suffix = argv[++i]; haveSuffix = true; }
        else if (a == "--fps" && i + 1 < argc)         opt.fps = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--content-dir" && i + 1 < argc) contentDir = argv[++i];
        else if (a == "--overwrite")                   overwrite = true;
        else if (!a.empty() && a[0] != '-' && assetPath.empty()) assetPath = a;
        else { AVER_ERROR("[retarget] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (assetPath.empty() || outDir.empty() || clipArgs.empty() || !(opt.fps >= 1.0f)) {
        AVER_ERROR("[retarget] usage: AverAssetC retarget <asset.ocretarget> --clips <file|dir>... --out <dir> "
                   "[--suffix S] [--fps N] [--overwrite] [--content-dir <dir>]");
        return exitCode(ExitCode::Usage);
    }

    anim::RetargetAsset asset;
    std::string why;
    if (!anim::loadRetarget(assetPath, asset, &why)) {
        AVER_ERROR("[retarget] cannot load '{}': {}", assetPath, why);
        return exitCode(ExitCode::Failed);
    }
    const std::string srcSkel = findContentFile(assetPath, contentDir, asset.source);
    const std::string dstSkel = findContentFile(assetPath, contentDir, asset.target);
    if (srcSkel.empty() || dstSkel.empty()) {
        AVER_ERROR("[retarget] cannot find the skeleton '{}' (use --content-dir)", srcSkel.empty() ? asset.source : asset.target);
        return exitCode(ExitCode::Failed);
    }
    if (!haveSuffix) suffix = "_" + anim::pathStem(asset.target);

    std::vector<std::string> clips;
    for (const std::string& c : clipArgs) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) {
            std::vector<std::string> inDir;
            for (const auto& e : fs::directory_iterator(c, ec))
                if (e.is_regular_file() && e.path().extension() == ".ocanim") inDir.push_back(e.path().string());
            std::sort(inDir.begin(), inDir.end());
            clips.insert(clips.end(), inDir.begin(), inDir.end());
        } else {
            clips.push_back(c);
        }
    }

    RunStats stats;
    for (const std::string& clip : clips) {
        const anim::RetargetBatchResult r = anim::retargetFolder(asset, srcSkel, dstSkel, {clip}, outDir, suffix, opt, overwrite);
        std::string reason;
        for (const anim::RetargetIssue& i : r.issues)
            if (i.kind != anim::RetargetIssue::Kind::Info) reason += (reason.empty() ? "" : "; ") + i.text;
        if (r.written == 1 && !r.files.empty()) {
            fmt::OcAnimation back;
            std::string verr;
            const bool verified = fmt::loadOcAnim(r.files[0], back, &verr) && back.valid();
            emitArtifact(clip, r.files[0], "retarget", true, verified, verified ? std::string() : "written clip did not reload: " + verr, stats);
        } else if (r.skipped > 0 && r.failed == 0) {
            emit("{\"schemaVersion\":1,\"input\":\"" + jsonEscape(clip) + "\",\"output\":\"\",\"kind\":\"retarget\",\"ok\":true,"
                 "\"verified\":false,\"skipped\":true,\"error\":\"" + jsonEscape(reason) + "\"}");
        } else {
            emitArtifact(clip, "", "retarget", false, false, reason.empty() ? "retarget failed" : reason, stats);
        }
    }
    const int code = stats.failed > 0 ? 1 : 0;
    emitSummary(assetPath, stats, code);
    return exitCode(code ? ExitCode::Failed : ExitCode::Ok);
}

} // namespace assetc
