#pragma once
// Find and Find References for the graph editor, as pure functions over fmt::OcGraphData. ImGui-free
// and header-only so tests/graphtools covers them with no window; the find bar and the right-click
// menu are thin glue in GraphEditor.cpp.
//
// Two different questions share the file because authors reach for them from the same place:
//   findInGraph       "where is the thing I half remember" -- text over ids, types, display names,
//                     attribute values, comment text, variable and function names, ranked.
//   findReferences    "what uses this" -- exact, by kind (variable, function, event, class), in one
//                     graph or across every .ocgraph under a project's Content directory.
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

// ---- text search -------------------------------------------------------------------------------------------

enum class FindScope : u8 { All, Nodes, Variables, Functions, Comments };

enum class FindTarget : u8 { Node, Variable, Function, Comment };

struct FindHit {
    FindTarget target = FindTarget::Node;
    std::string id;       // node id, variable name, function name or comment id
    std::string field;    // what matched: "id", "type", "name", "attr:<key>", "text"
    int rank = 2;         // 0 exact id, 1 name prefix, 2 substring
};

// Maps a node type to the label the palette shows for it ("AN_OnEvent" -> "On Event"). A hook so this
// header needs no catalog; GraphEditor passes the real one, a test passes its own.
using FindDisplayNameFn = std::function<std::string(const std::string& nodeType)>;

namespace findDetail {

inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// 0 when `hay` equals `needle`, 1 when it starts with it, 2 when it merely contains it, -1 for no match.
// Both already lower-case.
inline int rankOf(const std::string& hay, const std::string& needle) {
    if (hay == needle) return 0;
    const usize at = hay.find(needle);
    if (at == std::string::npos) return -1;
    return at == 0 ? 1 : 2;
}

// The value of every `key=value` token on a node line, with its key.
inline void forEachAttribute(const fmt::OcGraphNode& n, const std::function<void(const std::string&, const std::string&)>& fn) {
    for (const std::string& t : n.extraTokens) {
        const usize eq = t.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        fn(t.substr(0, eq), t.substr(eq + 1));
    }
}

inline std::string attr(const fmt::OcGraphNode& n, std::string_view key) {
    for (const std::string& t : n.extraTokens) {
        const usize eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (std::string_view(t).substr(0, eq) == key) return t.substr(eq + 1);
    }
    return {};
}

} // namespace findDetail

// Every match of `query` (case-insensitive substring) in the graph, best first. An exact node id ranks
// above a name that starts with the query, which ranks above one that merely contains it; ties keep
// graph order, so stepping through hits walks the file the way it reads. An empty query matches nothing.
inline std::vector<FindHit> findInGraph(const fmt::OcGraphData& g, std::string_view query, FindScope scope = FindScope::All,
                                        const FindDisplayNameFn& displayName = {}) {
    std::vector<FindHit> out;
    const std::string q = findDetail::lower(query);
    if (q.empty()) return out;

    const auto want = [&](FindScope s) { return scope == FindScope::All || scope == s; };

    if (want(FindScope::Nodes)) {
        for (const fmt::OcGraphNode& n : g.nodes) {
            FindHit best;
            best.id = n.id;
            best.rank = 99;
            const auto consider = [&](const std::string& hay, const char* field, bool idField) {
                int r = findDetail::rankOf(findDetail::lower(hay), q);
                if (r < 0) return;
                // Only the id can be "exact" in the ranking's first sense; an exact TYPE is a prefix match.
                if (r == 0 && !idField) r = 1;
                if (r < best.rank) { best.rank = r; best.field = field; }
            };
            consider(n.id, "id", true);
            consider(n.type, "type", false);
            if (displayName) consider(displayName(n.type), "name", false);
            findDetail::forEachAttribute(n, [&](const std::string& key, const std::string& value) {
                const int r = findDetail::rankOf(findDetail::lower(value), q);
                if (r < 0) return;
                const int rr = r == 0 ? 1 : r;
                if (rr < best.rank) { best.rank = rr; best.field = "attr:" + key; }
            });
            if (best.rank != 99) out.push_back(std::move(best));
        }
    }
    if (want(FindScope::Variables)) {
        for (const fmt::OcGraphVariable& v : g.variables) {
            const int r = findDetail::rankOf(findDetail::lower(v.name), q);
            if (r < 0) continue;
            out.push_back({FindTarget::Variable, v.name, "name", r == 0 ? 1 : r});
        }
    }
    if (want(FindScope::Functions)) {
        for (const fmt::OcGraphFunction& f : g.functions) {
            const int r = findDetail::rankOf(findDetail::lower(f.name), q);
            if (r < 0) continue;
            out.push_back({FindTarget::Function, f.name, "name", r == 0 ? 1 : r});
        }
    }
    if (want(FindScope::Comments)) {
        for (const fmt::OcGraphComment& c : g.comments) {
            const int r = findDetail::rankOf(findDetail::lower(c.text), q);
            if (r < 0) continue;
            out.push_back({FindTarget::Comment, c.id, "text", r == 0 ? 1 : r});
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const FindHit& a, const FindHit& b) { return a.rank < b.rank; });
    return out;
}

// The position inside a result list while an author steps through it with Enter and Shift+Enter.
struct FindCursor {
    std::vector<FindHit> hits;
    int index = -1;

    void reset(std::vector<FindHit> next) {
        hits = std::move(next);
        index = hits.empty() ? -1 : 0;
    }
    const FindHit* current() const {
        return index >= 0 && index < static_cast<int>(hits.size()) ? &hits[static_cast<usize>(index)] : nullptr;
    }
    // Wraps at both ends: past the last hit is the first.
    const FindHit* next() {
        if (hits.empty()) return nullptr;
        index = (index + 1) % static_cast<int>(hits.size());
        return current();
    }
    const FindHit* previous() {
        if (hits.empty()) return nullptr;
        const int n = static_cast<int>(hits.size());
        index = (index <= 0 ? n : index) - 1;
        return current();
    }
    // "3/12", or "0/0" with nothing found.
    std::string label() const {
        return std::to_string(hits.empty() ? 0 : index + 1) + "/" + std::to_string(hits.size());
    }
};

// ---- references --------------------------------------------------------------------------------------------

enum class RefKind : u8 { Variable, Function, Event, Class };

struct RefHit {
    std::string nodeId;
    std::string what;     // a short verb for the list: "reads", "writes", "calls", "defines", "fires", ...
};

// Every node in `g` that refers to `name` as a `kind`, in graph order, each node once:
//   Variable  var=name on GetVar ("reads"), SetVar ("writes") or anything else that carries one ("uses").
//   Function  call=name on a call ("calls"), and the FuncEntry of that function ("defines").
//   Event     a CustomEvent named name ("defines"), event=name on FireEvent ("fires"), AN_OnEvent
//             ("handles"), AN_DispatchEvent ("dispatches") or any other node ("uses"), and the node an
//             ENTRY record binds to that event name ("handles").
//   Class     class=name ("spawns" on Spawn, otherwise "uses").
inline std::vector<RefHit> findReferences(const fmt::OcGraphData& g, RefKind kind, std::string_view name) {
    std::vector<RefHit> out;
    if (name.empty()) return out;
    const auto add = [&](const std::string& id, const char* what) {
        for (const RefHit& h : out) if (h.nodeId == id) return;
        out.push_back({id, what});
    };
    const auto isType = [](const fmt::OcGraphNode& n, const char* t) { return findDetail::lower(n.type) == t; };

    for (const fmt::OcGraphNode& n : g.nodes) {
        switch (kind) {
        case RefKind::Variable:
            if (findDetail::attr(n, "var") == name)
                add(n.id, isType(n, "getvar") ? "reads" : isType(n, "setvar") ? "writes" : "uses");
            break;
        case RefKind::Function:
            if (findDetail::attr(n, "call") == name) add(n.id, "calls");
            else if (isType(n, "funcentry") && findDetail::attr(n, "func") == name) add(n.id, "defines");
            break;
        case RefKind::Event: {
            if (isType(n, "customevent") && findDetail::attr(n, "name") == name) { add(n.id, "defines"); break; }
            if (findDetail::attr(n, "event") == name)
                add(n.id, isType(n, "fireevent") ? "fires" : isType(n, "an_onevent") ? "handles"
                          : isType(n, "an_dispatchevent") ? "dispatches" : "uses");
            break;
        }
        case RefKind::Class:
            if (findDetail::attr(n, "class") == name) add(n.id, isType(n, "spawn") ? "spawns" : "uses");
            break;
        }
    }
    if (kind == RefKind::Event) {
        for (const auto& e : g.entryPoints)
            if (e.second == name) add(e.first, "handles");
    }
    return out;
}

// ---- across a project --------------------------------------------------------------------------------------

struct ProjectRefHit {
    std::string file;     // absolute path of the .ocgraph
    std::string nodeId;
    std::string what;
};

// Scans every gameplay *.ocgraph under `contentDir` (recursively, files in path order so the list is
// stable) and returns where `name` is used as a `kind`. A file that does not parse is skipped, not an
// error: Find References is advice, and one broken graph must not hide the rest.
inline std::vector<ProjectRefHit> findInProject(const std::string& contentDir, RefKind kind, std::string_view name) {
    std::vector<ProjectRefHit> out;
    if (name.empty() || contentDir.empty()) return out;

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(contentDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".ocgraph") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        fmt::OcGraphData g;
        if (!fmt::loadOcgraph(file.string(), g, nullptr)) continue;
        if (fmt::ocGraphDomainOf(g) != fmt::OcGraphDomain::Gameplay) continue;
        for (const RefHit& h : findReferences(g, kind, name))
            out.push_back({file.string(), h.nodeId, h.what});
    }
    return out;
}

// The .ocgraph under `contentDir` whose NAME record is `graphName` (the first, in path order), or empty.
// For a breakpoint that trips in a graph with no tab open: the managed side knows the graph only by name.
inline std::string findGraphFileByName(const std::string& contentDir, const std::string& graphName) {
    if (contentDir.empty() || graphName.empty()) return {};
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(contentDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".ocgraph") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        fmt::OcGraphData g;
        if (!fmt::loadOcgraph(file.string(), g, nullptr)) continue;
        // A graph with no NAME record is "untitled" to the managed side.
        if ((g.name.empty() ? std::string("untitled") : g.name) == graphName) return file.string();
    }
    return {};
}

} // namespace aver::editor
