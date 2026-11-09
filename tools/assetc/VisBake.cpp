#include "AssetC.hpp"

#include "aver/anim/Viseme.hpp"     // `visbake`

namespace assetc {

// `visbake <track.ocvis> --rig <x.ocface> --out <clip.ocanim> [--fps N]`: a lip-sync track turned into an ordinary
// curve-only clip (pose.viseme_* curves), so a montage, a state machine or a sequencer animation track can play speech
// with no new playback code (docs/plans/wave9/MORPH_TARGETS.md 2.6). The rig supplies phoneme overrides and the envelope.
int runVisBake(int argc, char** argv) {
    std::string trackPath, rigPath, outPath;
    f32 fps = 30.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rig" && i + 1 < argc)      rigPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--fps" && i + 1 < argc) fps = static_cast<f32>(std::atof(argv[++i]));
        else if (!a.empty() && a[0] != '-' && trackPath.empty()) trackPath = a;
        else { AVER_ERROR("[visbake] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (trackPath.empty() || outPath.empty() || !(fps >= 1.0f)) {
        AVER_ERROR("[visbake] usage: AverAssetC visbake <track.ocvis> --rig <x.ocface> --out <clip.ocanim> [--fps N]");
        return exitCode(ExitCode::Usage);
    }

    RunStats stats;
    anim::VisemeTrack track;
    std::vector<std::string> diag;
    std::string why;
    if (!anim::loadVisemeTrack(trackPath, track, &diag, &why)) {
        emitArtifact(trackPath, outPath, "visbake", false, false, why, stats);
        emitSummary(trackPath, stats, 1);
        return exitCode(ExitCode::Failed);
    }
    for (const std::string& d : diag) AVER_WARN("[visbake] {}: {}", trackPath, d);
    anim::FaceRigAsset rig;
    const anim::FaceRigAsset* rigPtr = nullptr;
    if (!rigPath.empty()) {
        if (!anim::loadFaceRig(rigPath, rig, &why)) {
            emitArtifact(trackPath, outPath, "visbake", false, false, "cannot load the face rig: " + why, stats);
            emitSummary(trackPath, stats, 1);
            return exitCode(ExitCode::Failed);
        }
        rigPtr = &rig;
    }
    // Symbols neither a phoneme nor a viseme are skipped by the baker; say which, once.
    {
        std::vector<anim::VisemeEvent> probe = track.events;
        std::vector<std::string> unknown;
        anim::resolveVisemeEvents(probe, rigPtr, track.set == anim::VisemeSet::Viseme, &unknown);
        for (const std::string& s : unknown) AVER_WARN("[visbake] '{}' is not a phoneme or a viseme; its events are skipped", s);
    }
    const fmt::OcAnimation clip = anim::bakeVisemeTrack(track, rigPtr, fps);
    if (clip.curves.empty()) {
        emitArtifact(trackPath, outPath, "visbake", false, false, "no event resolved to a viseme", stats);
        emitSummary(trackPath, stats, 1);
        return exitCode(ExitCode::Failed);
    }
    if (!fmt::saveOcAnim(outPath, clip, &why)) {
        emitArtifact(trackPath, outPath, "visbake", false, false, why, stats);
        emitSummary(trackPath, stats, 1);
        return exitCode(ExitCode::Failed);
    }
    fmt::OcAnimation back;
    std::string verr;
    const bool verified = fmt::loadOcAnim(outPath, back, &verr) && back.valid() && back.curves.size() == clip.curves.size();
    emitArtifact(trackPath, outPath, "visbake", true, verified, verified ? std::string() : "the baked clip did not reload: " + verr, stats,
                 "\"curves\":" + std::to_string(clip.curves.size()) + ",\"durationSeconds\":" + std::to_string(clip.duration));
    const int code = stats.failed > 0 ? 1 : 0;
    emitSummary(trackPath, stats, code);
    return exitCode(code ? ExitCode::Failed : ExitCode::Ok);
}

} // namespace assetc
