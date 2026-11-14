#pragma once
// Model helpers for macros and reroute (knot) nodes, as pure functions of fmt::OcGraphData so a headless test
// drives them and GraphEditor only adds undo, selection and synthesised-pin bookkeeping around them.
//
// SUBGRAPH KEYS. The canvas shows one subgraph at a time: the event graph (empty key), a function (its name, as
// `func=` writes it) or a macro body. A macro body's key is its name behind a control character no function
// name has, so one string still says "where am I" and every `subgraphOf(n) == currentSubgraph_` test keeps working.
#include "GraphEditorGeometry.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace aver::editor {

inline std::string macroSubgraphKey(const std::string& macroName) { return std::string(1, '\x1f') + macroName; }
inline bool isMacroSubgraph(const std::string& key) { return !key.empty() && key[0] == '\x1f'; }
inline std::string macroNameOf(const std::string& key) { return isMacroSubgraph(key) ? key.substr(1) : std::string(); }
// What the tab and the Problems list show for a subgraph key.
inline std::string subgraphLabel(const std::string& key) {
    return key.empty() ? std::string("Event Graph") : isMacroSubgraph(key) ? "Macro: " + macroNameOf(key) : key;
}

// A node's owning subgraph: `macro=` for a macro body node, else `func=`, else the event graph.
inline std::string nodeSubgraphKey(const fmt::OcGraphNode& n) {
    const GraphNodeAttribute m = getNodeAttribute(n, "macro");
    if (m.found && !m.value.empty()) return macroSubgraphKey(m.value);
    const GraphNodeAttribute f = getNodeAttribute(n, "func");
    return f.found ? f.value : std::string();
}

// Puts `n` in the subgraph `key` names, replacing whichever owner it had.
inline void assignSubgraph(fmt::OcGraphNode& n, const std::string& key) {
    removeNodeAttribute(n, "func");
    removeNodeAttribute(n, "macro");
    if (key.empty()) return;
    if (isMacroSubgraph(key)) setNodeAttribute(n, "macro", macroNameOf(key));
    else setNodeAttribute(n, "func", key);
}

// ---- macros ------------------------------------------------------------------------------------------------

inline bool isMacroNodeType(const std::string& t) { return t == "MacroInput" || t == "MacroOutput" || t == "MacroInstance"; }

// The pins a macro's tunnel and instance nodes have: MacroInput's outputs are the macro's inputs, MacroOutput's
// inputs its outputs, an instance has both. Agrees with OcGraphParser.AddDefaultPins, which derives the same.
inline std::vector<fmt::OcGraphPin> macroNodePins(const fmt::OcGraphNode& n, const fmt::OcGraphMacro& m) {
    std::vector<fmt::OcGraphPin> out;
    if (n.type == "MacroInput") {
        for (const auto& p : m.inputs) out.push_back({p.name, p.type, true, ""});
    } else if (n.type == "MacroOutput") {
        for (const auto& p : m.outputs) out.push_back({p.name, p.type, false, ""});
    } else {
        for (const auto& p : m.inputs) out.push_back({p.name, p.type, false, ""});
        for (const auto& p : m.outputs) out.push_back({p.name, p.type, true, ""});
    }
    return out;
}

// ---- reroutes ----------------------------------------------------------------------------------------------

inline bool isRerouteNode(const fmt::OcGraphNode& n) { return n.type == "Reroute"; }

namespace rerouteDetail {
inline const fmt::OcGraphNode* node(const fmt::OcGraphData& g, const std::string& id) {
    for (const auto& n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}
inline const fmt::OcGraphPin* pin(const fmt::OcGraphData& g, const std::string& id, const std::string& name, bool isOutput) {
    const fmt::OcGraphNode* n = node(g, id);
    if (!n) return nullptr;
    for (const auto& p : n->pins) if (p.name == name && p.isOutput == isOutput) return &p;
    return nullptr;
}
inline bool isReroute(const fmt::OcGraphData& g, const std::string& id) {
    const fmt::OcGraphNode* n = node(g, id);
    return n && isRerouteNode(*n);
}
} // namespace rerouteDetail

// Every reroute reachable from `id` through links whose two ends are both reroutes (the chain a type travels along).
inline std::vector<std::string> rerouteChain(const fmt::OcGraphData& g, const std::string& id) {
    std::vector<std::string> chain{id};
    std::unordered_set<std::string> seen{id};
    for (usize i = 0; i < chain.size(); ++i) {
        for (const auto& l : g.links) {
            if (!rerouteDetail::isReroute(g, l.sourceNode) || !rerouteDetail::isReroute(g, l.destNode)) continue;
            const std::string* other = l.sourceNode == chain[i] ? &l.destNode : l.destNode == chain[i] ? &l.sourceNode : nullptr;
            if (other && seen.insert(*other).second) chain.push_back(*other);
        }
    }
    return chain;
}

// The type a chain carries, from the first link joining it to a pin that is not a reroute's; empty for a chain
// nothing real is wired to yet, which accepts any first link.
inline std::string rerouteChainType(const fmt::OcGraphData& g, const std::vector<std::string>& chain) {
    const std::unordered_set<std::string> in(chain.begin(), chain.end());
    for (const auto& l : g.links) {
        const bool srcIn = in.count(l.sourceNode) != 0, dstIn = in.count(l.destNode) != 0;
        if (srcIn == dstIn) continue;
        const fmt::OcGraphPin* p = srcIn ? rerouteDetail::pin(g, l.destNode, l.destPin, false)
                                         : rerouteDetail::pin(g, l.sourceNode, l.sourcePin, true);
        if (p) return p->type;
    }
    return {};
}

inline void retypeReroutes(fmt::OcGraphData& g, const std::vector<std::string>& chain, const std::string& type) {
    for (auto& n : g.nodes) {
        if (!isRerouteNode(n) || std::find(chain.begin(), chain.end(), n.id) == chain.end()) continue;
        if (n.pins.empty()) {
            n.pins.push_back({"in", type, false, ""});
            n.pins.push_back({"out", type, true, ""});
        }
        for (auto& p : n.pins) p.type = type;
    }
}

// Retypes the free reroute chain(s) at either end of a link about to be made, so the ordinary type check then
// passes; a chain that already carries a type is left alone (a mismatch is then refused as any other). Returns
// the ids it retyped.
inline std::vector<std::string> prepareRerouteConnect(fmt::OcGraphData& g, const std::string& srcNode, const std::string& srcPin,
                                                      const std::string& dstNode, const std::string& dstPin) {
    std::vector<std::string> changed;
    const bool srcR = rerouteDetail::isReroute(g, srcNode), dstR = rerouteDetail::isReroute(g, dstNode);
    const auto retype = [&](const std::vector<std::string>& chain, const std::string& type) {
        if (type.empty()) return;
        retypeReroutes(g, chain, type);
        changed.insert(changed.end(), chain.begin(), chain.end());
    };
    if (srcR && !dstR) {
        const auto chain = rerouteChain(g, srcNode);
        const fmt::OcGraphPin* p = rerouteDetail::pin(g, dstNode, dstPin, false);
        if (p && rerouteChainType(g, chain).empty()) retype(chain, p->type);
    } else if (!srcR && dstR) {
        const auto chain = rerouteChain(g, dstNode);
        const fmt::OcGraphPin* p = rerouteDetail::pin(g, srcNode, srcPin, true);
        if (p && rerouteChainType(g, chain).empty()) retype(chain, p->type);
    } else if (srcR && dstR) {
        const auto cs = rerouteChain(g, srcNode), cd = rerouteChain(g, dstNode);
        if (std::find(cs.begin(), cs.end(), dstNode) != cs.end()) return changed;   // already one chain
        const std::string ts = rerouteChainType(g, cs), td = rerouteChainType(g, cd);
        const fmt::OcGraphPin* sp = rerouteDetail::pin(g, srcNode, "out", true);
        if (ts.empty() && td.empty()) { if (sp) retype(cd, sp->type); }
        else if (ts.empty()) retype(cs, td);
        else if (td.empty()) retype(cd, ts);
    }
    return changed;
}

// Splits link `linkIndex` with a new reroute at (x, y): source -> reroute.in and reroute.out -> destination, the
// reroute typed as the wire is and living in the subgraph its source node does. Returns the reroute's id (the
// caller makes it unique), or empty when the link or its source pin is missing.
inline std::string insertRerouteOnLink(fmt::OcGraphData& g, usize linkIndex, f64 x, f64 y, const std::string& newId) {
    if (linkIndex >= g.links.size()) return {};
    const fmt::OcGraphLink link = g.links[linkIndex];
    const fmt::OcGraphPin* sp = rerouteDetail::pin(g, link.sourceNode, link.sourcePin, true);
    const fmt::OcGraphNode* sn = rerouteDetail::node(g, link.sourceNode);
    if (!sp || !sn) return {};
    fmt::OcGraphNode r;
    r.id = newId;
    r.type = "Reroute";
    r.x = x;
    r.y = y;
    r.hasPosition = true;
    r.pins.push_back({"in", sp->type, false, ""});
    r.pins.push_back({"out", sp->type, true, ""});
    assignSubgraph(r, nodeSubgraphKey(*sn));
    g.nodes.push_back(std::move(r));
    g.links[linkIndex] = fmt::OcGraphLink{link.sourceNode, link.sourcePin, newId, "in"};
    g.links.insert(g.links.begin() + static_cast<isize>(linkIndex) + 1, fmt::OcGraphLink{newId, "out", link.destNode, link.destPin});
    return newId;
}

} // namespace aver::editor
