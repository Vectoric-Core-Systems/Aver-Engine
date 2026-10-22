#pragma once
// The pure half of Project Settings > Physics > Surfaces and the material inspector's "Physical Surface" combo (SHAPES_MATERIALS phase F):
// the rules for the .ocsurf table the panel edits (duplicate names, hash collisions, the 4096 cap), the row edits, the text the
// panel saves, and the PHYSSURFACE patch of an .ocmat file. Header-only and ImGui-free so tests/editor can drive every rule;
// PhysicalSurfacePanel.cpp is the widget half.
#include "aver/core/SurfaceId.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcSurf.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aver::editor::surfmodel {

using IdFn = std::function<i32(std::string_view)>;

inline i32 defaultId(std::string_view name) { return aver::surfaceIdFromName(name); }

struct RowProblem {
    usize row = 0;
    std::string text;
};

// Every reason a row cannot be saved, per row: an empty or invalid name, a name used twice, two names whose 32-bit ids collide (the
// registry refuses the second), and the table's size. `idOf` is the hash; tests pass a colliding one.
inline std::vector<RowProblem> problems(const fmt::OcSurfData& d, const IdFn& idOf = defaultId) {
    std::vector<RowProblem> out;
    std::unordered_map<std::string, usize> byName;
    std::unordered_map<i32, usize> byId;
    for (usize i = 0; i < d.surfaces.size(); ++i) {
        const std::string& name = d.surfaces[i].name;
        if (name.empty()) { out.push_back({i, "The name is empty."}); continue; }
        if (!fmt::ocSurfNameValid(name)) { out.push_back({i, "'" + name + "' is not a valid name: letters, digits and underscore, up to 63."}); continue; }
        if (const auto it = byName.find(name); it != byName.end()) {
            out.push_back({i, "'" + name + "' is already row " + std::to_string(it->second + 1) + "."});
            continue;
        }
        byName.emplace(name, i);
        const i32 id = idOf(name);
        if (const auto it = byId.find(id); it != byId.end()) {
            out.push_back({i, "'" + name + "' hashes to the same id as '" + d.surfaces[it->second].name + "'; rename one."});
            continue;
        }
        byId.emplace(id, i);
    }
    if (d.surfaces.size() > fmt::kOcSurfMaxSurfaces)
        out.push_back({d.surfaces.size() - 1, "More than " + std::to_string(fmt::kOcSurfMaxSurfaces) + " surfaces."});
    return out;
}

inline bool canSave(const fmt::OcSurfData& d, const IdFn& idOf = defaultId) { return problems(d, idOf).empty(); }

// What Save writes: the canonical .ocsurf text, so the file the panel saves is the file a person would have written.
inline std::string saveText(const fmt::OcSurfData& d) { return fmt::writeOcSurf(d); }

// "Surface", "Surface2", ... the first name `base` + N that no row uses.
inline std::string uniqueName(const fmt::OcSurfData& d, std::string base = "Surface") {
    if (base.empty() || !fmt::ocSurfNameValid(base)) base = "Surface";
    const auto taken = [&](const std::string& n) {
        return std::any_of(d.surfaces.begin(), d.surfaces.end(), [&](const fmt::OcSurfRecord& r) { return r.name == n; });
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + std::to_string(i);
        if (!taken(n)) return n;
    }
}

// Appends an unset surface with a fresh name; the new row's index.
inline usize addRow(fmt::OcSurfData& d, const std::string& base = "Surface") {
    fmt::OcSurfRecord r;
    r.name = uniqueName(d, base);
    d.surfaces.push_back(std::move(r));
    return d.surfaces.size() - 1;
}

inline bool removeRow(fmt::OcSurfData& d, usize i) {
    if (i >= d.surfaces.size()) return false;
    d.surfaces.erase(d.surfaces.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

// Moves row `i` by `delta` places (file order is display order and nothing else); false at an end.
inline bool moveRow(fmt::OcSurfData& d, usize i, int delta) {
    if (i >= d.surfaces.size()) return false;
    const long to = static_cast<long>(i) + delta;
    if (to < 0 || to >= static_cast<long>(d.surfaces.size())) return false;
    std::swap(d.surfaces[i], d.surfaces[static_cast<usize>(to)]);
    return true;
}

// The names a combo offers, in table order.
inline std::vector<std::string> names(const fmt::OcSurfData& d) {
    std::vector<std::string> out;
    out.reserve(d.surfaces.size());
    for (const fmt::OcSurfRecord& r : d.surfaces) out.push_back(r.name);
    return out;
}

// The rows of `now` the registry has to hear about: new names, and names whose numbers changed since `saved`. (Register is an upsert,
// so telling it about an unchanged row is harmless; this is what the status line counts.)
inline std::vector<usize> changedRows(const fmt::OcSurfData& saved, const fmt::OcSurfData& now) {
    std::vector<usize> out;
    for (usize i = 0; i < now.surfaces.size(); ++i) {
        const fmt::OcSurfRecord& n = now.surfaces[i];
        const auto it = std::find_if(saved.surfaces.begin(), saved.surfaces.end(), [&](const fmt::OcSurfRecord& r) { return r.name == n.name; });
        if (it == saved.surfaces.end() || it->friction != n.friction || it->restitution != n.restitution || it->tag != n.tag) out.push_back(i);
    }
    return out;
}

// ---- the .ocmat PHYSSURFACE record -------------------------------------------------------------------------------------------

// `ocmatText` with its PHYSSURFACE record set to `name` (removed when `name` is empty). Every other line is left byte for byte, so a
// material with a GRAPH{} block or hand-written comments keeps them (parse-and-rewrite would drop what the format skips). The record
// is written the way OcMat's writer writes it: after a blank line, at the end of the file. The line ending of the file is kept.
inline std::string patchPhysSurface(const std::string& ocmatText, const std::string& name) {
    const bool crlf = ocmatText.find("\r\n") != std::string::npos;
    const char* eol = crlf ? "\r\n" : "\n";
    std::vector<std::string> lines;
    usize pos = 0;
    while (pos < ocmatText.size()) {
        usize nl = ocmatText.find('\n', pos);
        const bool hasNl = nl != std::string::npos;
        if (!hasNl) nl = ocmatText.size();
        std::string line = ocmatText.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    const auto isRecord = [](const std::string& l) {
        usize i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
        static constexpr std::string_view kWord = "PHYSSURFACE";
        if (l.size() < i + kWord.size()) return false;
        for (usize k = 0; k < kWord.size(); ++k) {
            char c = l[i + k];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (c != kWord[k]) return false;
        }
        return l.size() == i + kWord.size() || l[i + kWord.size()] == ' ' || l[i + kWord.size()] == '\t';
    };
    lines.erase(std::remove_if(lines.begin(), lines.end(), isRecord), lines.end());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();   // the blank line before the record is ours to place
    std::string out;
    for (const std::string& l : lines) { out += l; out += eol; }
    if (!name.empty()) {
        out += eol;
        out += "PHYSSURFACE " + name;
        out += eol;
    }
    return out;
}

// The name a PHYSSURFACE record states in `ocmatText`, "" when there is none.
inline std::string readPhysSurface(const std::string& ocmatText) {
    usize pos = 0;
    std::string found;
    while (pos < ocmatText.size()) {
        usize nl = ocmatText.find('\n', pos);
        if (nl == std::string::npos) nl = ocmatText.size();
        std::string line = ocmatText.substr(pos, nl - pos);
        pos = nl + 1;
        usize i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::string low = line.substr(i, 11);
        for (char& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (low != "physsurface") continue;
        if (i + 11 < line.size() && line[i + 11] != ' ' && line[i + 11] != '\t' && line[i + 11] != '\r') continue;
        usize j = i + 11;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
        usize k = j;
        while (k < line.size() && line[k] != ' ' && line[k] != '\t' && line[k] != '\r') ++k;
        if (k > j) found = line.substr(j, k - j);   // the last record wins, as the parser's does
    }
    return found;
}

} // namespace aver::editor::surfmodel

#if AVER_WITH_IMGUI
namespace aver::editor {

// Project Settings > Physics > Surfaces: the table of Content/Physics/Surfaces.ocsurf below the physics settings. `contentDir` is the
// project's content directory ("" draws a hint). Save writes the file and registers the rows with the physics module, so friction
// edits are live in Play. PhysicalSurfacePanel.cpp.
void drawPhysicalSurfaceTable(const std::string& contentDir, float dpi);

// The material inspector's Physical Surface combo for the material named `materialName`: edits the PHYSSURFACE record of the .ocmat file
// the collision build reads (Binaries/Materials first, then Content/Materials). Draws a hint when the material has no file.
void drawMaterialPhysSurface(const std::string& binariesDir, const std::string& contentDir, const std::string& materialName);

} // namespace aver::editor
#endif
