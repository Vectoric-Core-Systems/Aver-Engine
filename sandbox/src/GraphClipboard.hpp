#pragma once
// The graph editor's shared clipboard, minus ImGui: what a copy puts on it, how text is read back, and
// what a paste does to the graph it lands in. ImGui-free and header-only so tests/graphtools covers it;
// GraphEditor.cpp adds only the OS-clipboard calls (ImGui::SetClipboardText / GetClipboardText).
//
// THE CLIP IS TEXT: the line `AVER_NODE_CLIP 1`, then the same records writeOcgraph emits for just the
// copied nodes, the links with both ends among them, and the VAR and FUNC records those nodes name. It is
// kept twice -- on the OS clipboard (so a paste reaches another editor tab, survives the source tab
// closing and an editor restart, and reads as ordinary .ocgraph in a text editor) and in a process-wide
// store (so it still works where there is no OS clipboard to ask, i.e. every headless test).
#include "GraphEditorGeometry.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::editor {

inline constexpr const char* kClipMagic = "AVER_NODE_CLIP";

struct ClipPayload {
    std::string domain;                              // the source graph's DOMAIN text; empty = gameplay
    std::vector<fmt::OcGraphNode> nodes;
    std::vector<fmt::OcGraphLink> links;             // only links with both ends in `nodes`
    std::vector<fmt::OcGraphVariable> variables;     // the declarations the nodes' var= name
    std::vector<fmt::OcGraphFunction> functions;     // the declarations the nodes' call= name
    bool empty() const { return nodes.empty(); }
};

// Writes the clip text. The records are writeOcgraph's own, so the grammar has one definition.
inline std::string serializeClip(const std::vector<fmt::OcGraphNode>& nodes, const std::vector<fmt::OcGraphLink>& links,
                                 const std::vector<fmt::OcGraphVariable>& vars, const std::vector<fmt::OcGraphFunction>& functions,
                                 std::string_view domain = {}) {
    fmt::OcGraphData g;
    g.domain = std::string(domain);
    g.name = "clipboard";
    g.nodes = nodes;
    g.links = links;
    g.variables = vars;
    g.functions = functions;
    return std::string(kClipMagic) + " 1\n" + fmt::writeOcgraph(g);
}

inline std::string serializeClip(const ClipPayload& p) {
    return serializeClip(p.nodes, p.links, p.variables, p.functions, p.domain);
}

// Reads clip text back. Nothing for text that does not begin with the magic line (any other clipboard
// content), for a version this build does not know, and for a body that does not parse.
inline std::optional<ClipPayload> parseClip(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    const std::string_view magic(kClipMagic);
    if (text.substr(0, magic.size()) != magic) return std::nullopt;

    const usize nl = text.find('\n');
    std::string_view head = text.substr(magic.size(), nl == std::string_view::npos ? std::string_view::npos : nl - magic.size());
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) head.remove_prefix(1);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t' || head.back() == '\r')) head.remove_suffix(1);
    if (head != "1") return std::nullopt;
    if (nl == std::string_view::npos) return std::nullopt;

    fmt::OcGraphData g;
    if (!fmt::parseOcgraph(text.substr(nl + 1), g, nullptr)) return std::nullopt;
    if (g.nodes.empty()) return std::nullopt;

    ClipPayload p;
    p.domain = g.domain;
    p.nodes = std::move(g.nodes);
    p.links = std::move(g.links);
    p.variables = std::move(g.variables);
    p.functions = std::move(g.functions);
    return p;
}

// What a copy of `selected` takes from `g`: the nodes in graph order, the links wholly inside them (a
// half-copied wire would dangle or point the copy back at the original), and the declarations they name.
inline ClipPayload collectClip(const fmt::OcGraphData& g, const std::vector<std::string>& selected) {
    ClipPayload p;
    p.domain = g.domain;
    const auto isSelected = [&](const std::string& id) {
        return std::find(selected.begin(), selected.end(), id) != selected.end();
    };
    for (const fmt::OcGraphNode& n : g.nodes)
        if (isSelected(n.id)) p.nodes.push_back(n);
    for (const fmt::OcGraphLink& l : g.links)
        if (isSelected(l.sourceNode) && isSelected(l.destNode)) p.links.push_back(l);

    for (const fmt::OcGraphNode& n : p.nodes) {
        const std::string var = getNodeAttribute(n, "var").value;
        if (!var.empty()) {
            const bool have = std::any_of(p.variables.begin(), p.variables.end(), [&](const fmt::OcGraphVariable& v) { return v.name == var; });
            if (!have)
                for (const fmt::OcGraphVariable& v : g.variables) if (v.name == var) { p.variables.push_back(v); break; }
        }
        const std::string call = getNodeAttribute(n, "call").value;
        if (!call.empty()) {
            const bool have = std::any_of(p.functions.begin(), p.functions.end(), [&](const fmt::OcGraphFunction& f) { return f.name == call; });
            if (!have)
                for (const fmt::OcGraphFunction& f : g.functions) if (f.name == call) { p.functions.push_back(f); break; }
        }
    }
    return p;
}

// The process-wide copy of the clip text -- see the file header for why there are two homes.
struct ClipStore {
    std::string text;
};
inline ClipStore& clipStore() {
    static ClipStore s;
    return s;
}

// ---- paste ---------------------------------------------------------------------------------------------------

// What a paste will do to a target graph, decided before anything is touched.
struct PastePlan {
    bool ok = false;
    std::string refusal;                             // why not, when !ok
    std::vector<usize> keep;                         // indices into the payload's nodes that will be pasted
    std::vector<fmt::OcGraphVariable> declare;       // VARs the target lacks, with the type and default they carried
    std::vector<std::string> typeConflicts;          // VARs the target has under another type (left alone; Problems flags them)
    std::vector<std::string> droppedCalls;           // payload node ids: calls to a function the target lacks
    std::vector<std::string> droppedFunctionNodes;   // payload node ids: FuncEntry / FuncReturn (one per function)
};

inline std::string clipLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline PastePlan planPaste(const ClipPayload& p, const fmt::OcGraphData& target) {
    PastePlan plan;
    if (p.empty()) { plan.refusal = "the clipboard holds no nodes"; return plan; }

    // Across languages is refused outright: a gameplay Add takes two scalars, a material Add two float3s,
    // and neither compiler can do anything sane with the other's nodes.
    fmt::OcGraphData probe;
    probe.domain = p.domain;
    const fmt::OcGraphDomain from = fmt::ocGraphDomainOf(probe);
    const fmt::OcGraphDomain to = fmt::ocGraphDomainOf(target);
    if (from != to) {
        const auto name = [](fmt::OcGraphDomain d) {
            const std::string_view n = fmt::ocGraphDomainName(d);
            return n.empty() ? std::string("unknown") : std::string(n);
        };
        plan.refusal = "cannot paste " + name(from) + " nodes into a " + name(to) + " graph";
        return plan;
    }

    for (usize i = 0; i < p.nodes.size(); ++i) {
        const fmt::OcGraphNode& n = p.nodes[i];
        const std::string type = clipLower(n.type);
        if (type == "funcentry" || type == "funcreturn") { plan.droppedFunctionNodes.push_back(n.id); continue; }
        if (type == "callfunc") {
            const std::string callee = getNodeAttribute(n, "call").value;
            const bool there = std::any_of(target.functions.begin(), target.functions.end(),
                                           [&](const fmt::OcGraphFunction& f) { return f.name == callee; });
            if (!there) { plan.droppedCalls.push_back(n.id); continue; }
        }
        plan.keep.push_back(i);
    }
    if (plan.keep.empty()) {
        plan.refusal = !plan.droppedCalls.empty() ? "every copied node calls a function this graph does not have"
                                                  : "nothing on the clipboard can be pasted here";
        return plan;
    }

    // A variable a pasted node reads or writes must exist, or the paste hands the author a graph that
    // does not compile. Declared with the type and default it carried, in the same undo step.
    for (const usize i : plan.keep) {
        const std::string var = getNodeAttribute(p.nodes[i], "var").value;
        if (var.empty()) continue;
        const fmt::OcGraphVariable* has = nullptr;
        for (const fmt::OcGraphVariable& v : target.variables) if (v.name == var) { has = &v; break; }
        const fmt::OcGraphVariable* carried = nullptr;
        for (const fmt::OcGraphVariable& v : p.variables) if (v.name == var) { carried = &v; break; }
        if (has) {
            if (carried && clipLower(carried->type) != clipLower(has->type) &&
                std::find(plan.typeConflicts.begin(), plan.typeConflicts.end(), var) == plan.typeConflicts.end())
                plan.typeConflicts.push_back(var);
            continue;
        }
        if (!carried) continue;   // the source never declared it either; Problems will say so
        const bool queued = std::any_of(plan.declare.begin(), plan.declare.end(), [&](const fmt::OcGraphVariable& v) { return v.name == var; });
        if (!queued) plan.declare.push_back(*carried);
    }
    plan.ok = true;
    return plan;
}

// The banner text for what a paste did beyond pasting nodes; empty when it was uneventful.
inline std::string describePaste(const PastePlan& plan) {
    const auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (usize i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s;
    };
    std::string msg;
    const auto add = [&](const std::string& part) { msg += (msg.empty() ? "" : "; ") + part; };
    if (!plan.declare.empty()) {
        std::vector<std::string> names;
        for (const auto& v : plan.declare) names.push_back(v.name);
        add("declared variables " + list(names));
    }
    if (!plan.typeConflicts.empty()) add("variables with a different type here: " + list(plan.typeConflicts));
    if (!plan.droppedCalls.empty()) add("skipped calls to functions this graph lacks: " + list(plan.droppedCalls));
    if (!plan.droppedFunctionNodes.empty()) add("skipped function entry/return nodes: " + list(plan.droppedFunctionNodes));
    return msg.empty() ? msg : "Pasted; " + msg;
}

struct PasteResult {
    PastePlan plan;
    std::vector<std::string> newIds;                              // in payload order
    std::unordered_map<std::string, std::string> oldToNew;
};

// Applies the plan to `target`: declares the missing variables, appends the kept nodes under fresh ids
// (from `makeId`, which must consult `target` -- nodes are appended one at a time so it sees the earlier
// ones) with the copied set's top-left landing on (atX, atY), and re-points the internal links at the
// copies. A node lands in `subgraph` (a function name; empty = the event graph). ENTRY records and the
// canvas positions overlay are the caller's, because they need the palette and the editor's own state.
template <class MakeId>
PasteResult applyPaste(fmt::OcGraphData& target, const ClipPayload& p, f64 atX, f64 atY, const std::string& subgraph, MakeId&& makeId) {
    PasteResult r;
    r.plan = planPaste(p, target);
    if (!r.plan.ok) return r;

    for (const fmt::OcGraphVariable& v : r.plan.declare) target.variables.push_back(v);

    f64 ox = p.nodes[r.plan.keep[0]].x, oy = p.nodes[r.plan.keep[0]].y;
    for (const usize i : r.plan.keep) {
        ox = std::min(ox, p.nodes[i].x);
        oy = std::min(oy, p.nodes[i].y);
    }
    for (const usize i : r.plan.keep) {
        const fmt::OcGraphNode& src = p.nodes[i];
        fmt::OcGraphNode n = src;
        n.id = makeId(src.type);
        n.x = atX + (src.x - ox);
        n.y = atY + (src.y - oy);
        n.hasPosition = true;
        if (!subgraph.empty()) setNodeAttribute(n, "func", subgraph);
        else removeNodeAttribute(n, "func");
        r.oldToNew[src.id] = n.id;
        r.newIds.push_back(n.id);
        target.nodes.push_back(std::move(n));
    }
    for (const fmt::OcGraphLink& l : p.links) {
        const auto a = r.oldToNew.find(l.sourceNode);
        const auto b = r.oldToNew.find(l.destNode);
        if (a == r.oldToNew.end() || b == r.oldToNew.end()) continue;   // an end was dropped: no link
        fmt::OcGraphLink nl = l;
        nl.sourceNode = a->second;
        nl.destNode = b->second;
        target.links.push_back(std::move(nl));
    }
    return r;
}

} // namespace aver::editor
