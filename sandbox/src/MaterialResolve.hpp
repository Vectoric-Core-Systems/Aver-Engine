#pragma once
// Resolves an editor SURFACE NAME to the .ocmat path that should back it, and enumerates every
// surface name a project's material folders define -- the editor-side half of the documented
// convention (docs/SCRIPTING_API.md, docs/ARCHITECTURE.md, README): Content/Materials/*.cs is
// SOURCE, avermatc compiles it to Binaries/Materials/*.ocmat, and a NAME is looked up under
// Binaries first, then Content/Materials, then a content-relative path -- see
// GameContent::materialForSurface (modules/runtime.game/src/GameContent.cpp) for the runtime's own
// copy of this same order, which this file must keep matching without ever including it: the
// runtime is Aver.RuntimeGame and this is Sandbox, and nothing here should make the editor need it.
//
// HEADER-ONLY AND FILESYSTEM-ONLY, matching LevelList.hpp's own precedent in this same directory.
// SandboxApp.cpp's materialForSurface() and loadProjectMaterials() do more than what lives here --
// they parse the .ocmat, resolve its material graph and create a live pbr::MaterialHandle, all of
// which needs Aver.Pbr and cannot run headless. The part that actually decides WHICH FILE WINS --
// the part a moved-materials bug like this one lives in -- needs neither, so it is pulled out here
// where a test can drive it with nothing but a temp directory, and SandboxApp.cpp calls it rather
// than keeping a second copy of the same three-candidate order.
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace aver::editor {

// Lower-cases a file extension the same way the rest of the Content Browser does, so a Windows
// directory listing that hands back ".OCMAT" still matches.
inline std::string lowerFileExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// The candidate .ocmat paths for `name`, in the order both the runtime and the editor resolve
// them: the BUILT one under Binaries\Materials wins over a hand-authored one under Content\
// Materials, which wins over a bare content-relative path. `binariesDir`/`contentDir` may be empty
// (an unopened project); the candidates built from an empty root simply never exist.
inline std::array<std::string, 3> materialCandidatePaths(const std::string& binariesDir,
                                                          const std::string& contentDir,
                                                          const std::string& name) {
    return {
        binariesDir.empty() ? std::string() : binariesDir + "\\Materials\\" + name + ".ocmat",
        contentDir.empty()  ? std::string() : contentDir + "\\Materials\\" + name + ".ocmat",
        contentDir.empty()  ? std::string() : contentDir + "\\" + name,
    };
}

// The first candidate that exists on disk, or "" if none do -- what a surface named `name`
// resolves to. This is the WHOLE precedence rule: a name present under both Binaries\Materials and
// Content\Materials resolves to the Binaries one, because it is tried first and the loop stops at
// the first hit.
inline std::string resolveMaterialPath(const std::string& binariesDir, const std::string& contentDir,
                                        const std::string& name) {
    if (name.empty()) return {};
    for (const std::string& path : materialCandidatePaths(binariesDir, contentDir, name)) {
        if (path.empty()) continue;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return path;
    }
    return {};
}

// Every surface NAME with an .ocmat directly under EITHER Binaries\Materials or Content\Materials
// (non-recursive in each, since materialForSurface() only ever looks directly inside those two
// folders). A name present under both directories is returned exactly once -- resolveMaterialPath()
// above, not this enumeration, is what decides which FILE wins.
inline std::vector<std::string> projectMaterialStems(const std::string& binariesDir,
                                                      const std::string& contentDir) {
    std::unordered_set<std::string> stems;
    std::error_code ec;
    const std::string dirs[2] = {
        binariesDir.empty() ? std::string() : binariesDir + "\\Materials",
        contentDir.empty()  ? std::string() : contentDir + "\\Materials",
    };
    for (const std::string& dir : dirs) {
        if (dir.empty() || !std::filesystem::exists(dir, ec)) continue;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (lowerFileExt(it->path()) != ".ocmat") continue;
            stems.insert(it->path().stem().string());
        }
    }
    return std::vector<std::string>(stems.begin(), stems.end());
}

} // namespace aver::editor
