#pragma once
// Finding the levels in a project, so the editor can offer them.
//
// WHY THIS EXISTS AS ITS OWN HEADER. File > Open Level was never a picker: it called loadStartMap(),
// which reopens the project's ONE start map and nothing else, so there was no way to reach a second
// level from inside the editor at all -- and a double-click on a level in the Content Browser fell
// through to openWithShell(), i.e. it opened the .ocworld in Notepad. Both need the same list, and a
// list is pure logic over a directory, which is the half of a picker that can be tested without a
// window. SandboxApp.cpp is add_executable-only with no library half, so anything a test must reach
// lives in a header it can include -- the same arrangement EditorEuler, GraphEditorGeometry and
// EditorPrefs already use.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace aver::editor {

// One level found under a project's content root.
struct LevelEntry {
    std::string relPath;   // content-relative, forward slashes: "Maps/Default.ocmap"
    std::string name;      // the stem, for display: "Default"
    std::string absPath;   // what to hand loadLevel
};

// BOTH EXTENSIONS, and that is not a detail. `.ocmap` is the legacy grammar and `.ocworld` the
// current one, but which grammar a file actually uses is decided by its CONTENT, not its name --
// fmt::levelFileIsLegacyOcmap exists precisely because ElectricDreams ships an `.ocmap` that is pure
// OCWORLD. A picker that offered only one extension would hide half a project's levels.
//
// DELIBERATELY NOT assets::assetTypeFromPath: its AssetType::Map case matches `.ocmap` only and has
// no branch for `.ocworld`, so filtering a directory walk through it would silently drop every level
// the editor's own Save Level As has ever written.
inline bool isLevelPath(const std::string& path) {
    const usize slash = path.find_last_of("/\\");
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const usize dot = file.find_last_of('.');
    // A STEM IS REQUIRED, not just an extension: a file literally named ".ocmap" has none, and the
    // picker names each level by its stem -- so offering it would put a blank row in the list.
    if (dot == std::string::npos || dot == 0) return false;
    std::string ext = file.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ocmap" || ext == ".ocworld";
}

// Directories a content walk must not descend into. All three are DERIVED or version-control state:
// they hold no authored level, and Chunks/ in particular can hold tens of thousands of streamed
// files in a project that has ever run a scatter -- walking it would make the picker's first frame
// take visibly long for nothing.
inline bool isDerivedDir(const std::string& name) {
    return name == "DerivedDataCache" || name == "Chunks" || name == ".git";
}

// Every level under `contentDir`, sorted by its content-relative path.
//
// SORTED, AND NOT BY MTIME OR "RECENT". There is no recent-levels list anywhere in the editor and
// EditorPrefs' own header refuses project-scoped state, so there is nothing to be recent WITH; a
// stable alphabetical order is the one a person can predict and scan. Levels live under Maps/ by
// convention -- that is where Save Level As writes and where a project's STARTMAP points -- so they
// group there naturally without the sort needing to know about it.
inline std::vector<LevelEntry> listLevels(const std::string& contentDir) {
    std::vector<LevelEntry> out;
    if (contentDir.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(contentDir, ec) || !std::filesystem::is_directory(contentDir, ec))
        return out;

    // Hand-rolled recursion rather than recursive_directory_iterator, because skipping a subtree
    // needs disable_recursion_pending() on the iterator and that is easy to get subtly wrong; this
    // says what it does.
    std::vector<std::filesystem::path> stack{std::filesystem::path(contentDir)};
    while (!stack.empty()) {
        const std::filesystem::path dir = stack.back();
        stack.pop_back();
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const std::filesystem::path p = it->path();
            if (it->is_directory(ec)) {
                if (!isDerivedDir(p.filename().string())) stack.push_back(p);
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (!isLevelPath(p.filename().string())) continue;

            std::string rel = std::filesystem::relative(p, contentDir, ec).string();
            if (ec || rel.empty()) continue;
            for (char& c : rel) if (c == '\\') c = '/';
            out.push_back(LevelEntry{rel, p.stem().string(), p.string()});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const LevelEntry& a, const LevelEntry& b) { return a.relPath < b.relPath; });
    return out;
}

} // namespace aver::editor
