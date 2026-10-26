// The editor's side of the chunk cook (LARGE_WORLD 2.4.5, 2.4.8): World Settings > Level Streaming > Cook Chunks, and the freshness
// check Standalone Game runs before it launches.
//
// THE EDITOR NEVER PLAYS THE COOKED SET IN-PROCESS. It edits the level's own objects (records, pins, write-back, undo), which a cooked
// set cannot be edited through; and a level loaded from the cooked set is the SLIM level, which saving would write over the source.
// So "Use cooked chunks" is the Standalone Game path: a separate Aver Engine Runtime reads the level and its `chunks=` set from disk
// exactly as a packaged game does. GameLevel::useCookedChunks is off in the editor and on in the runtime.
//
// New file, not new members of a big translation unit: the Debug COFF section cap (the memory index) rules out growing SandboxApp*.cpp.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE
#  include "aver/core/Hash.hpp"
#  include "aver/world/AuthoredChunkSource.hpp"
#  include "aver/world/ChunkCook.hpp"

#  include <filesystem>

namespace aver {

namespace {

// <project>/Binaries/Chunks/<stem>, and the project-relative text the level's `chunks=` carries.
struct CookTarget {
    std::string dir;
    std::string relative;
};

CookTarget cookTarget(const std::string& projectDir, const std::string& levelPath) {
    namespace fs = std::filesystem;
    CookTarget t;
    const std::string stem = fs::path(levelPath).stem().string();
    t.dir = world::cookDirFor(projectDir, stem);
    std::error_code ec;
    t.relative = fs::relative(fs::path(t.dir), fs::path(projectDir), ec).generic_string();
    if (ec || t.relative.empty()) t.relative = fs::path(t.dir).generic_string();
    return t;
}

} // namespace

// Saves the level (with `chunks=` set, so the source names its set), re-reads what it just wrote, and cooks THAT: the hash in the
// index has to be of the text a runtime will parse.
bool SandboxApp::cookLevelChunks(std::string& report) {
    namespace fs = std::filesystem;
    if (!project_.valid() || levelPath_.empty()) { report = "no level is open"; return false; }
    const CookTarget target = cookTarget(project_.dir, levelPath_);
    if (levelHeader_.stream.chunksPath != target.relative) {
        levelHeader_.stream.chunksPath = target.relative;
        markLevelUnsaved();
    }
    if (!saveLevel(levelPath_)) { report = "the level could not be saved"; return false; }
    markLevelSaved();

    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(levelPath_, w, &why)) { report = why; return false; }

    world::CookInput in;
    std::error_code ec;
    in.levelAsset = fs::relative(fs::path(levelPath_), fs::path(project_.contentDir()), ec).generic_string();
    if (ec || in.levelAsset.empty()) in.levelAsset = fs::path(levelPath_).filename().generic_string();
    in.levelStem = fs::path(levelPath_).stem().string();
    in.contentDir = project_.contentDir();
    in.projectDir = project_.dir;
    in.library = &level_.assets();   // the same parse cache the level's instances use
    in.meshBounds = [this](const std::string& asset, Vec3& lo, Vec3& hi) {
        const std::pair<Vec3, Vec3>* b = content_.boundsFor(fnv1a64(std::string_view(asset)));
        if (!b) return false;
        lo = b->first;
        hi = b->second;
        return true;
    };
    world::CookReport rep;
    if (!world::cookAuthoredLevel(w, in, world::CookOptions{}, rep, &why)) { report = why; return false; }

    report = std::to_string(rep.chunkOwned) + " objects in " + std::to_string(rep.chunks) + " chunks (" +
             std::to_string(rep.regions) + " region file" + (rep.regions == 1 ? "" : "s") + ", " +
             std::to_string(rep.bytes / (1024 * 1024)) + " MB), " + std::to_string(rep.levelGlobal) + " stay with the level, " +
             std::to_string(rep.wide) + " wide";
    if (!rep.unknownBoundsAssets.empty())
        report += "; " + std::to_string(rep.unknownBoundsAssets.size()) + " mesh(es) without bounds stay with the level";
    for (const std::string& m : rep.warnings) AVER_WARN("[Cook] {}", m);
    AVER_INFO("[Cook] {}: {}", in.levelStem, report);
    return true;
}

// Before a Standalone launch: the runtime streams the cooked set only while it is current (a stale set is skipped with a warning),
// so a level that names one gets it re-cooked here when the saved level has moved on. False when the cook fails (no launch).
bool SandboxApp::refreshCookedChunksForPlay() {
    if (levelHeader_.stream.chunksPath.empty() || !project_.valid() || levelPath_.empty()) return true;   // this level streams the old way
    namespace fs = std::filesystem;
    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(levelPath_, w, &why)) return true;   // the launch reports an unreadable level itself

    const CookTarget target = cookTarget(project_.dir, levelPath_);
    world::AuthoredChunkSource src;
    world::CookInput in;
    std::error_code ec;
    in.levelAsset = fs::relative(fs::path(levelPath_), fs::path(project_.contentDir()), ec).generic_string();
    in.levelStem = fs::path(levelPath_).stem().string();
    in.contentDir = project_.contentDir();
    in.projectDir = project_.dir;
    in.library = &level_.assets();
    if (src.open(target.dir, nullptr) && world::cookSourceHash(w, in, world::CookOptions{}) == src.sourceHash()) return true;
    src.close();   // a set cannot be replaced under open files

    std::string report;
    const bool ok = cookLevelChunks(report);
    cookChunksReport_ = ok ? "Re-cooked for Play: " + report : "Failed: " + report;
    if (!ok) setUpgradeStatus("Cooking the level's chunks failed: " + report, editor::NotifySeverity::Error);
    return ok;
}

// World Settings > Level Streaming > Cooked chunks.
void SandboxApp::buildCookChunksSettings() {
    ImGui::Separator();
    ImGui::TextDisabled("Cooked chunks");
    fmt::OcStreamSettings& st = levelHeader_.stream;
    ImGui::TextDisabled("Set: %s", st.chunksPath.empty() ? "(not cooked)" : st.chunksPath.c_str());
    if (ImGui::Button("Cook Chunks")) {
        std::string report;
        cookChunksReport_ = cookLevelChunks(report) ? report : "Failed: " + report;
    }
    uiReg_.track("worldSettings.cookChunks");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Saves the level, then cuts its objects into 16 m chunks under Binaries/Chunks/<level> and\n"
                          "adds chunks= to the level. A game (and Play > Standalone Game) streams the level from\n"
                          "that set, which can be far larger than one float frame; this editor keeps editing the\n"
                          "level's own objects. The set is derived data: cook again after editing.");
    if (!st.chunksPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Forget Set")) {
            st.chunksPath.clear();
            markLevelUnsaved();
            cookChunksReport_.clear();
        }
        uiReg_.track("worldSettings.forgetCookedChunks");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Removes chunks= from the level, so a game streams it from its objects again.\n"
                              "The cooked files stay on disk.");
    }
    if (!cookChunksReport_.empty()) ImGui::TextDisabled("%s", cookChunksReport_.c_str());
}

} // namespace aver

#endif // AVER_MODULE_SCENE
