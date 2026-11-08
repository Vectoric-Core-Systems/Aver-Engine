#pragma once
// The Content Browser's editor-side organisation: asset type filter chips, favourites and collections.
// Pure data and text, no ImGui or filesystem, so tests/editor/ContentCollectionsTest runs headless.
//
// Favourites and collections are references (content-relative paths), never copies, and live in a small
// per-project file (<project>/Saved/ContentCollections.txt), not in the editor prefs (which hold nothing about
// a project) and not in any engine format.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

// ---- type chips -----------------------------------------------------------------------------------------
enum class CbType : u8 { Mesh, Texture, Material, Audio, Animation, Graph, Level, Effects, UI, Script, Data, Other, Count };
inline constexpr u32 kCbTypeCount = static_cast<u32>(CbType::Count);

inline const char* cbTypeName(CbType t) {
    switch (t) {
        case CbType::Mesh:      return "Meshes";
        case CbType::Texture:   return "Textures";
        case CbType::Material:  return "Materials";
        case CbType::Audio:     return "Audio";
        case CbType::Animation: return "Animation";
        case CbType::Graph:     return "Graphs";
        case CbType::Level:     return "Levels";
        case CbType::Effects:   return "Effects";
        case CbType::UI:        return "UI";
        case CbType::Script:    return "Code";
        case CbType::Data:      return "Data";
        default:                return "Other";
    }
}

// The chip an asset belongs to, from its lower-case extension with the dot.
inline CbType cbTypeForExt(std::string_view e) {
    const auto in = [&](std::initializer_list<std::string_view> l) { return std::find(l.begin(), l.end(), e) != l.end(); };
    if (in({".ocmesh", ".gltf", ".glb", ".obj", ".fbx", ".usd", ".usda", ".usdc", ".ocbeam"})) return CbType::Mesh;
    if (in({".png", ".jpg", ".jpeg", ".tga", ".webp", ".svg", ".exr", ".hdr", ".bmp", ".dds"})) return CbType::Texture;
    if (in({".ocmat", ".ocsurf"})) return CbType::Material;
    if (in({".ocaudio", ".wav", ".ogg", ".mp3", ".flac", ".ocsnd"})) return CbType::Audio;
    if (in({".ocanim", ".ocskel", ".ocblend", ".ocasm", ".ocmontage", ".ocikrig", ".ocretarget", ".ocrig", ".ocface",
            ".ocvis", ".ocmotion", ".ocmbake", ".ocseq"})) return CbType::Animation;
    if (in({".ocgraph", ".ocbt", ".ocst", ".oceqs", ".ocso", ".ocgas"})) return CbType::Graph;
    if (in({".ocworld", ".ocmap", ".ocnav", ".ocprefab"})) return CbType::Level;
    if (in({".ocparticle", ".ocfoliage", ".ocinst", ".ocsoft", ".ochair"})) return CbType::Effects;
    if (in({".ocui", ".ocfonts", ".occaptions", ".ocstrings"})) return CbType::UI;
    if (in({".cs", ".cpp", ".cxx", ".cc", ".hpp", ".hxx", ".h", ".hlsl", ".fs", ".fsproj", ".csproj"})) return CbType::Script;
    if (in({".json", ".csv", ".md", ".ocinput", ".ocvehicle", ".ocshape"})) return CbType::Data;
    return CbType::Other;
}

inline constexpr u32 kCbAllTypes = (1u << kCbTypeCount) - 1u;   // mask 0 or all = no type filter
inline bool cbTypeFiltering(u32 mask) { return mask != 0 && (mask & kCbAllTypes) != kCbAllTypes; }
// A chip filter is "any of the lit chips"; a folder always passes.
inline bool cbTypePasses(u32 mask, bool isDir, std::string_view ext) {
    if (isDir || !cbTypeFiltering(mask)) return true;
    return (mask >> static_cast<u32>(cbTypeForExt(ext))) & 1u;
}
inline u32 cbTypeToggled(u32 mask, CbType t) { return mask ^ (1u << static_cast<u32>(t)); }

// ---- favourites and collections -------------------------------------------------------------------------
struct CbCollection { std::string name; std::vector<std::string> paths; };

struct CbCollections {
    std::vector<std::string> favourites;
    std::vector<CbCollection> collections;

    bool isFavourite(const std::string& p) const { return std::find(favourites.begin(), favourites.end(), p) != favourites.end(); }
    // Returns the new state.
    bool toggleFavourite(const std::string& p) {
        const auto it = std::find(favourites.begin(), favourites.end(), p);
        if (it != favourites.end()) { favourites.erase(it); return false; }
        favourites.push_back(p);
        return true;
    }
    CbCollection* find(const std::string& name) {
        for (CbCollection& c : collections) if (c.name == name) return &c;
        return nullptr;
    }
    // False for an empty, multi-line or taken name.
    bool create(const std::string& name) {
        if (name.empty() || name.find_first_of("\r\n\t") != std::string::npos || find(name)) return false;
        collections.push_back({name, {}});
        return true;
    }
    bool remove(const std::string& name) {
        for (auto it = collections.begin(); it != collections.end(); ++it)
            if (it->name == name) { collections.erase(it); return true; }
        return false;
    }
    bool add(const std::string& name, const std::string& p) {
        CbCollection* c = find(name);
        if (!c || std::find(c->paths.begin(), c->paths.end(), p) != c->paths.end()) return false;
        c->paths.push_back(p);
        return true;
    }
    bool removeFrom(const std::string& name, const std::string& p) {
        CbCollection* c = find(name);
        if (!c) return false;
        const auto it = std::find(c->paths.begin(), c->paths.end(), p);
        if (it == c->paths.end()) return false;
        c->paths.erase(it);
        return true;
    }
    // A path moved or renamed on disk: exact entries and anything under a moved folder follow.
    void rebase(const std::string& from, const std::string& to) {
        const auto fix = [&](std::string& p) {
            if (p == from) p = to;
            else if (p.size() > from.size() && p.compare(0, from.size(), from) == 0 && p[from.size()] == '/') p = to + p.substr(from.size());
        };
        for (std::string& p : favourites) fix(p);
        for (CbCollection& c : collections) for (std::string& p : c.paths) fix(p);
    }
};

// Content-relative key from an absolute path: forward slashes, no leading separator. Empty when the path
// is not under `contentDir`.
inline std::string cbContentKey(std::string full, std::string contentDir) {
    for (char& c : full) if (c == '\\') c = '/';
    for (char& c : contentDir) if (c == '\\') c = '/';
    while (!contentDir.empty() && contentDir.back() == '/') contentDir.pop_back();
    if (full.size() <= contentDir.size() + 1 || full.compare(0, contentDir.size(), contentDir) != 0 || full[contentDir.size()] != '/')
        return {};
    return full.substr(contentDir.size() + 1);
}

// Text form: `fav <path>`, then `col <name>` followed by `item <path>` lines. Malformed lines are skipped.
inline std::string cbSerialize(const CbCollections& c) {
    std::string out = "# Content Browser favourites and collections (editor-only)\n";
    for (const std::string& p : c.favourites) out += "fav " + p + "\n";
    for (const CbCollection& col : c.collections) {
        out += "col " + col.name + "\n";
        for (const std::string& p : col.paths) out += "item " + p + "\n";
    }
    return out;
}

inline CbCollections cbParse(std::string_view text) {
    CbCollections c;
    CbCollection* cur = nullptr;
    usize at = 0;
    while (at < text.size()) {
        usize end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const usize sp = line.find(' ');
        if (sp == std::string_view::npos || sp + 1 >= line.size()) continue;
        const std::string_view key = line.substr(0, sp);
        const std::string val(line.substr(sp + 1));
        if (key == "fav") { if (!c.isFavourite(val)) c.favourites.push_back(val); }
        else if (key == "col") { c.create(val); cur = c.find(val); }
        else if (key == "item" && cur) { if (std::find(cur->paths.begin(), cur->paths.end(), val) == cur->paths.end()) cur->paths.push_back(val); }
    }
    return c;
}

} // namespace aver::editor
