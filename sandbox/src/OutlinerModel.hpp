#pragma once
// The World Outliner's editor-side model: the type filter and the virtual folders. Plain data and string
// arithmetic, no ImGui or scene, so tests/editor/OutlinerModelTest runs it headless.
//
// Folders are EDITOR-ONLY, session state: the .ocworld format has no folder or group field and entities
// have no id that survives a reload, so they are neither saved with the level nor remembered between
// sessions. A folder is a '/'-separated path ("Lights/Indoor"); only root entities are assigned to one.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

// ---- type filter ----------------------------------------------------------------------------------------
enum class OutlinerType : u8 { Mesh, Light, Camera, Particles, Decal, Skeletal, Class, PlayerStart, Other, Count };

inline constexpr u32 kOutlinerTypeCount = static_cast<u32>(OutlinerType::Count);
inline constexpr u32 kOutlinerAllTypes = (1u << kOutlinerTypeCount) - 1u;

inline const char* outlinerTypeName(OutlinerType t) {
    switch (t) {
        case OutlinerType::Mesh:        return "Meshes";
        case OutlinerType::Light:       return "Lights";
        case OutlinerType::Camera:      return "Cameras";
        case OutlinerType::Particles:   return "Particles";
        case OutlinerType::Decal:       return "Decals";
        case OutlinerType::Skeletal:    return "Skeletal meshes";
        case OutlinerType::Class:       return "Class actors";
        case OutlinerType::PlayerStart: return "Player starts";
        default:                        return "Other";
    }
}
inline bool outlinerTypeShown(u32 mask, OutlinerType t) { return (mask >> static_cast<u32>(t)) & 1u; }
inline u32 outlinerTypeToggled(u32 mask, OutlinerType t) { return mask ^ (1u << static_cast<u32>(t)); }
inline bool outlinerTypeFiltering(u32 mask) { return (mask & kOutlinerAllTypes) != kOutlinerAllTypes; }

// ---- folders ---------------------------------------------------------------------------------------------
inline std::string folderParent(const std::string& path) {
    const auto at = path.rfind('/');
    return at == std::string::npos ? std::string() : path.substr(0, at);
}
inline std::string folderLeaf(const std::string& path) {
    const auto at = path.rfind('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}
// True when `path` is `ancestor` or lies below it. The empty ancestor is the root: everything is within it.
inline bool folderIsWithin(const std::string& path, const std::string& ancestor) {
    if (ancestor.empty()) return true;
    if (path.size() < ancestor.size() || path.compare(0, ancestor.size(), ancestor) != 0) return false;
    return path.size() == ancestor.size() || path[ancestor.size()] == '/';
}
// A folder leaf name: not empty, no '/', no control characters, no leading or trailing space.
inline bool folderNameValid(std::string_view leaf) {
    if (leaf.empty() || leaf.front() == ' ' || leaf.back() == ' ') return false;
    for (const char c : leaf)
        if (c == '/' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}
// `path` after the folder `from` was renamed or moved to `to` (a path below it follows).
inline std::string folderRebased(const std::string& path, const std::string& from, const std::string& to) {
    if (!folderIsWithin(path, from) || from.empty()) return path;
    return to + path.substr(from.size());
}
// Adds `path` and every ancestor to a sorted, duplicate-free folder list.
inline void folderAdd(std::vector<std::string>& folders, const std::string& path) {
    for (std::string p = path; !p.empty(); p = folderParent(p)) {
        const auto it = std::lower_bound(folders.begin(), folders.end(), p);
        if (it == folders.end() || *it != p) folders.insert(it, p);
    }
}
inline bool folderExists(const std::vector<std::string>& folders, const std::string& path) {
    return std::binary_search(folders.begin(), folders.end(), path);
}
// "New Folder", then "New Folder 2", ... under `parent`, whichever is free.
inline std::string folderUniquePath(const std::vector<std::string>& folders, const std::string& parent,
                                    const std::string& base = "New Folder") {
    const std::string prefix = parent.empty() ? std::string() : parent + "/";
    std::string cand = prefix + base;
    for (int n = 2; folderExists(folders, cand); ++n) cand = prefix + base + " " + std::to_string(n);
    return cand;
}

} // namespace aver::editor
