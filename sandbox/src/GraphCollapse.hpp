#pragma once
// Collapse to Function: turns a selection of nodes into a new FUNC and puts a call to it where the
// selection was, as a pure transformation of fmt::OcGraphData. ImGui-free and header-only so
// tests/graphtools covers it; GraphEditor::collapseSelectionToFunction wraps it in one undo step and
// keeps the canvas bookkeeping (positions overlay, selection, synthesised pins) in step.
//
// WHAT IT PROMISES: the graph does what it did. Every wire that crossed the selection's edge now goes
// through the new function's FuncEntry or FuncReturn and the CallFunc node, so values and control flow
// arrive where they used to. The C# test (CollapseFunctionTests) proves it by running both graphs.
//
// WHAT IT REFUSES, WITH A NAMED REASON, instead of guessing:
//   * a selection with more than one exec entry or exec exit (a function has one way in and one way
//     out; "selection has N exec exits; v1 supports one"),
//   * event and entry nodes, FuncEntry / FuncReturn, and Param / Self nodes (a function does not
//     receive the graph's parameters),
//   * a loop taken without its whole body, a node the graph hands back with OUT,
//   * control flow that nothing runs, and outputs with no unambiguous place to return from.
//
// ONE SEMANTIC DIFFERENCE, NAMED: a value wired in from OUTSIDE is pulled when the function is CALLED,
// not when the inner node that reads it runs. The two agree unless something earlier in the selection
// changes what that outside node reads (a SetVar before a GetVar feeding it), which is the same rule
// every function call in this language has.
#include "GraphEditorGeometry.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::editor {

struct CollapseOptions {
    // Whether a node TYPE is an event (OnTick, a custom event, ...). The palette knows (category
    // "Event"); without it the built-in list below stands in.
    std::function<bool(const std::string&)> isEventType;
};

struct CollapseResult {
    bool ok = false;
    std::string reason;                    // why not, when !ok
    std::string functionName;              // as declared (made unique)
    std::string callNodeId;
    std::string entryNodeId;
    std::string returnNodeId;              // empty when the function has no FuncReturn
    std::vector<std::string> movedIds;     // the nodes now inside the function
    f64 callX = 0.0, callY = 0.0;          // where the call node landed (the selection's centroid)
    f64 entryX = 0.0, entryY = 0.0;
    f64 returnX = 0.0, returnY = 0.0;
    bool pure = false;
};

namespace collapseDetail {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline fmt::OcGraphNode* findNode(fmt::OcGraphData& g, const std::string& id) {
    for (auto& n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}

inline const fmt::OcGraphPin* findPin(const fmt::OcGraphNode& n, const std::string& name, bool isOutput) {
    for (const auto& p : n.pins) if (p.name == name && p.isOutput == isOutput) return &p;
    return nullptr;
}

inline std::string subgraphOf(const fmt::OcGraphNode& n) {
    const GraphNodeAttribute a = getNodeAttribute(n, "func");
    return a.found ? a.value : std::string();
}

inline std::string uniqueNodeId(const fmt::OcGraphData& g, const std::string& type) {
    const std::string base = lower(type);
    for (int i = 1; i < 1000000; ++i) {
        const std::string candidate = base + std::to_string(i);
        bool taken = false;
        for (const auto& n : g.nodes) if (n.id == candidate) { taken = true; break; }
        if (!taken) return candidate;
    }
    return base + "_x";
}

inline bool builtinEventType(const std::string& type) {
    const std::string t = lower(type);
    return t == "onstart" || t == "ontick" || t == "onhit" || t == "customevent" || t == "an_onevent" ||
           t == "oninputaction";
}

inline bool isDataType(const std::string& t) { return t == "float" || t == "int" || t == "bool"; }

// A name fit for a FUNCIN / FUNCOUT record (one bare token).
inline std::string sanitize(std::string s) {
    for (char& c : s) if (std::isspace(static_cast<unsigned char>(c)) || c == '.') c = '_';
    return s.empty() ? std::string("value") : s;
}

inline std::string uniqueName(const std::string& want, std::set<std::string>& used) {
    std::string name = want;
    for (int i = 2; used.count(name) != 0; ++i) name = want + std::to_string(i);
    used.insert(name);
    return name;
}

} // namespace collapseDetail

inline CollapseResult collapseToFunction(fmt::OcGraphData& graph, const std::vector<std::string>& selectedIds, std::string name,
                                         const CollapseOptions& options = {}) {
    using namespace collapseDetail;
    CollapseResult r;
    const auto refuse = [&](std::string why) { r.ok = false; r.reason = std::move(why); return r; };

    // ---- the selection ---------------------------------------------------------------------------------
    std::unordered_set<std::string> inS;
    for (const auto& id : selectedIds)
        for (const auto& n : graph.nodes) if (n.id == id) { inS.insert(id); break; }
    if (inS.empty()) return refuse("select the nodes to collapse first");

    if (name.empty()) name = "CollapsedFunction";
    if (std::any_of(name.begin(), name.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }))
        return refuse("a function name cannot contain whitespace");

    // Worked on a copy and committed only on success: a refusal late in the analysis must leave the
    // graph exactly as it was.
    fmt::OcGraphData g = graph;

    std::vector<const fmt::OcGraphNode*> sel;   // in graph order
    for (const auto& n : g.nodes) if (inS.count(n.id)) sel.push_back(&n);

    // One subgraph: a selection straddling the event graph and a function (or two functions) has no
    // single place to put the call.
    const std::string sub = subgraphOf(*sel.front());
    for (const auto* n : sel)
        if (subgraphOf(*n) != sub) return refuse("the selection spans more than one subgraph (the event graph and function '" +
                                                  (sub.empty() ? subgraphOf(*n) : sub) + "' cannot be collapsed together)");

    // ---- nodes that cannot move ------------------------------------------------------------------------
    for (const auto* n : sel) {
        const std::string t = lower(n->type);
        if (std::any_of(g.entryPoints.begin(), g.entryPoints.end(), [&](const auto& e) { return e.first == n->id; }))
            return refuse("node '" + n->id + "' is an entry point (ENTRY); an event cannot move into a function");
        if (options.isEventType ? options.isEventType(n->type) : builtinEventType(n->type))
            return refuse("node '" + n->id + "' (" + n->type + ") is an event; an event cannot move into a function");
        if (t == "funcentry" || t == "funcreturn")
            return refuse("node '" + n->id + "' (" + n->type + ") belongs to its own function; a function has exactly one entry and one return");
        if (t == "param" || t == "getparam" || t == "self")
            return refuse("node '" + n->id + "' (" + n->type + ") reads the graph's parameters, which a function does not receive");
        if (std::any_of(g.outputs.begin(), g.outputs.end(), [&](const auto& o) { return o.first == n->id; }))
            return refuse("node '" + n->id + "' is a graph OUT; the graph hands that value back from the event graph");
    }

    // ---- a loop keeps its whole body --------------------------------------------------------------------
    // The body of a While / ForEach is everything its `loop` exec pin leads to. Taking the loop and only
    // part of its body leaves a loop that repeats the wrong thing. (Taking part of a body WITHOUT the loop
    // is fine: the call simply runs on every pass, which is what the nodes did.)
    {
        std::unordered_map<std::string, std::vector<std::string>> execNext;   // node -> exec successors
        for (const auto& l : g.links) {
            const fmt::OcGraphNode* sn = nullptr;
            for (const auto& n : g.nodes) if (n.id == l.sourceNode) { sn = &n; break; }
            const fmt::OcGraphPin* sp = sn ? findPin(*sn, l.sourcePin, true) : nullptr;
            if (sp && lower(sp->type) == "exec") execNext[l.sourceNode].push_back(l.destNode);
        }
        for (const auto& loopNode : g.nodes) {
            const std::string t = lower(loopNode.type);
            if (t != "while" && t != "foreach") continue;
            std::unordered_set<std::string> body;
            std::vector<std::string> stack;
            for (const auto& l : g.links)
                if (l.sourceNode == loopNode.id && l.sourcePin == "loop") stack.push_back(l.destNode);
            while (!stack.empty()) {
                const std::string id = stack.back();
                stack.pop_back();
                if (id == loopNode.id || !body.insert(id).second) continue;
                for (const auto& nx : execNext[id]) stack.push_back(nx);
            }
            if (inS.count(loopNode.id) == 0) continue;
            for (const auto& b : body)
                if (inS.count(b) == 0)
                    return refuse("the selection takes loop '" + loopNode.id + "' without its whole body (node '" + b + "' is left behind)");
        }
    }

    // ---- what crosses the selection's edge -------------------------------------------------------------
    std::vector<usize> incoming, outgoing;       // link indices
    for (usize i = 0; i < g.links.size(); ++i) {
        const auto& l = g.links[i];
        const bool s = inS.count(l.sourceNode) != 0, d = inS.count(l.destNode) != 0;
        if (!s && d) incoming.push_back(i);
        else if (s && !d) outgoing.push_back(i);
    }

    // Pin types, from the nodes' own records.
    const auto srcPinOf = [&](const fmt::OcGraphLink& l) -> const fmt::OcGraphPin* {
        for (const auto& n : g.nodes) if (n.id == l.sourceNode) return findPin(n, l.sourcePin, true);
        return nullptr;
    };
    const auto dstPinOf = [&](const fmt::OcGraphLink& l) -> const fmt::OcGraphPin* {
        for (const auto& n : g.nodes) if (n.id == l.destNode) return findPin(n, l.destPin, false);
        return nullptr;
    };
    std::vector<usize> execIn, execOut, dataIn, dataOut;
    for (const usize i : incoming) {
        const auto* sp = srcPinOf(g.links[i]);
        if (!sp) return refuse("link " + g.links[i].sourceNode + "." + g.links[i].sourcePin + " names a pin its node has no record of");
        (lower(sp->type) == "exec" ? execIn : dataIn).push_back(i);
    }
    for (const usize i : outgoing) {
        const auto* sp = srcPinOf(g.links[i]);
        if (!sp) return refuse("link " + g.links[i].sourceNode + "." + g.links[i].sourcePin + " names a pin its node has no record of");
        (lower(sp->type) == "exec" ? execOut : dataOut).push_back(i);
    }

    if (execIn.size() > 1)
        return refuse("selection has " + std::to_string(execIn.size()) + " exec entries; v1 supports one");
    if (execOut.size() > 1)
        return refuse("selection has " + std::to_string(execOut.size()) + " exec exits; v1 supports one");
    if (execIn.empty() && !execOut.empty())
        return refuse("selection exits by exec but nothing outside runs it; wire an exec link into it first");

    const bool pure = execIn.empty() && execOut.empty();
    if (pure) {
        for (const auto* n : sel)
            for (const auto& p : n->pins)
                if (lower(p.type) == "exec")
                    return refuse("node '" + n->id + "' has exec pins but nothing in or out of the selection runs it; "
                                  "select the chain it belongs to, or wire it in first");
    }

    // ---- the function's signature ----------------------------------------------------------------------
    fmt::OcGraphFunction fn;
    fn.pure = pure;
    std::set<std::string> usedNames;
    std::unordered_map<std::string, std::string> inputName;    // "node.pin" (outside source) -> function input
    std::unordered_map<std::string, std::string> outputName;   // "node.pin" (inside source)  -> function output

    for (const usize i : dataIn) {
        const auto& l = g.links[i];
        const std::string key = l.sourceNode + "." + l.sourcePin;
        if (inputName.count(key)) continue;
        const std::string type = lower(srcPinOf(l)->type);
        if (!isDataType(type)) return refuse("the wire from " + key + " carries a " + type + ", which a function pin cannot");
        const std::string pn = uniqueName(sanitize(l.sourceNode + "_" + l.sourcePin), usedNames);
        inputName[key] = pn;
        fn.inputs.push_back({pn, type});
    }
    for (const usize i : dataOut) {
        const auto& l = g.links[i];
        const std::string key = l.sourceNode + "." + l.sourcePin;
        if (outputName.count(key)) continue;
        const std::string type = lower(srcPinOf(l)->type);
        if (!isDataType(type)) return refuse("the wire from " + key + " carries a " + type + ", which a function pin cannot");
        const std::string pn = uniqueName(sanitize(l.sourceNode + "_" + l.sourcePin), usedNames);
        outputName[key] = pn;
        fn.outputs.push_back({pn, type});
    }

    // Where an impure function with outputs returns FROM: the exec exit, or failing that the one exec
    // output nothing is wired to (the end of the chain). Anything else has no honest answer.
    std::string returnFromNode, returnFromPin;
    if (!pure && !fn.outputs.empty()) {
        if (execOut.size() == 1) {
            returnFromNode = g.links[execOut[0]].sourceNode;
            returnFromPin = g.links[execOut[0]].sourcePin;
        } else {
            int dangling = 0;
            for (const auto* n : sel)
                for (const auto& p : n->pins) {
                    if (!p.isOutput || lower(p.type) != "exec") continue;
                    const bool wired = std::any_of(g.links.begin(), g.links.end(), [&](const fmt::OcGraphLink& l) {
                        return l.sourceNode == n->id && l.sourcePin == p.name;
                    });
                    if (wired) continue;
                    ++dangling;
                    returnFromNode = n->id;
                    returnFromPin = p.name;
                }
            if (dangling != 1)
                return refuse("the selection returns values but has " + std::to_string(dangling) +
                              " places its exec chain can end; it needs exactly one (or one exec exit) to return from");
        }
    }

    // ---- a name -----------------------------------------------------------------------------------------
    {
        const auto taken = [&](const std::string& n) {
            return std::any_of(g.functions.begin(), g.functions.end(), [&](const fmt::OcGraphFunction& f) {
                return lower(f.name) == lower(n);
            });
        };
        std::string unique = name;
        for (int i = 2; taken(unique); ++i) unique = name + std::to_string(i);
        fn.name = unique;
    }
    r.functionName = fn.name;
    r.pure = pure;

    // ---- build ------------------------------------------------------------------------------------------
    f64 minX = sel.front()->x, maxX = sel.front()->x, minY = sel.front()->y, sumX = 0.0, sumY = 0.0;
    for (const auto* n : sel) {
        minX = std::min(minX, n->x);
        maxX = std::max(maxX, n->x);
        minY = std::min(minY, n->y);
        sumX += n->x;
        sumY += n->y;
        r.movedIds.push_back(n->id);
    }
    const f64 count = static_cast<f64>(sel.size());

    for (auto& n : g.nodes) if (inS.count(n.id)) setNodeAttribute(n, "func", fn.name);

    // The three new nodes, with the pins a function of this shape gives them (the same shapes
    // GraphEditor::resyncFunctionNodePins and OcGraphParser.AddDefaultPins derive).
    fmt::OcGraphNode entry;
    entry.id = uniqueNodeId(g, "FuncEntry");
    entry.type = "FuncEntry";
    entry.x = minX - 300.0; entry.y = minY;
    setNodeAttribute(entry, "func", fn.name);
    if (!pure) entry.pins.push_back({"then", "exec", true, ""});
    for (const auto& p : fn.inputs) entry.pins.push_back({p.name, p.type, true, ""});
    g.nodes.push_back(entry);

    std::string returnId;
    if (!fn.outputs.empty()) {
        fmt::OcGraphNode ret;
        ret.id = uniqueNodeId(g, "FuncReturn");
        ret.type = "FuncReturn";
        ret.x = maxX + 360.0; ret.y = minY;
        setNodeAttribute(ret, "func", fn.name);
        if (!pure) ret.pins.push_back({"exec", "exec", false, ""});
        for (const auto& p : fn.outputs) ret.pins.push_back({p.name, p.type, false, ""});
        returnId = ret.id;
        r.returnX = ret.x; r.returnY = ret.y;
        g.nodes.push_back(std::move(ret));
    }

    fmt::OcGraphNode call;
    call.id = uniqueNodeId(g, "CallFunc");
    call.type = "CallFunc";
    call.x = sumX / count; call.y = sumY / count;
    setNodeAttribute(call, "call", fn.name);
    if (!sub.empty()) setNodeAttribute(call, "func", sub);
    if (!pure) {
        call.pins.push_back({"exec", "exec", false, ""});
        call.pins.push_back({"then", "exec", true, ""});
    }
    for (const auto& p : fn.inputs) call.pins.push_back({p.name, p.type, false, ""});
    for (const auto& p : fn.outputs) call.pins.push_back({p.name, p.type, true, ""});
    g.nodes.push_back(call);

    r.entryNodeId = entry.id;
    r.returnNodeId = returnId;
    r.callNodeId = call.id;
    r.entryX = entry.x; r.entryY = entry.y;
    r.callX = call.x; r.callY = call.y;

    // ---- re-point the wires -----------------------------------------------------------------------------
    std::vector<fmt::OcGraphLink> links;
    std::set<std::string> callInFed;            // call inputs already given their outside source
    std::set<std::string> returnFed;            // function outputs already wired inside
    const auto add = [&](std::string sn, std::string sp, std::string dn, std::string dp) {
        links.push_back({std::move(sn), std::move(sp), std::move(dn), std::move(dp)});
    };

    std::unordered_set<usize> crossing;
    for (const usize i : incoming) crossing.insert(i);
    for (const usize i : outgoing) crossing.insert(i);
    for (usize i = 0; i < g.links.size(); ++i)
        if (!crossing.count(i)) links.push_back(g.links[i]);

    for (const usize i : dataIn) {
        const auto& l = g.links[i];
        const std::string& pn = inputName[l.sourceNode + "." + l.sourcePin];
        if (callInFed.insert(pn).second) add(l.sourceNode, l.sourcePin, call.id, pn);
        add(entry.id, pn, l.destNode, l.destPin);
    }
    if (!execIn.empty()) {
        const auto& l = g.links[execIn[0]];
        add(l.sourceNode, l.sourcePin, call.id, "exec");
        add(entry.id, "then", l.destNode, l.destPin);
    }
    for (const usize i : dataOut) {
        const auto& l = g.links[i];
        const std::string& pn = outputName[l.sourceNode + "." + l.sourcePin];
        if (returnFed.insert(pn).second) add(l.sourceNode, l.sourcePin, returnId, pn);
        add(call.id, pn, l.destNode, l.destPin);
    }
    if (!execOut.empty()) {
        const auto& l = g.links[execOut[0]];
        add(call.id, "then", l.destNode, l.destPin);
        // The exit pin runs the return (when there is one) instead of leaving the function's chain.
        if (!returnId.empty()) add(l.sourceNode, l.sourcePin, returnId, "exec");
    } else if (!pure && !returnId.empty()) {
        add(returnFromNode, returnFromPin, returnId, "exec");
    }
    g.links = std::move(links);
    g.functions.push_back(std::move(fn));

    graph = std::move(g);
    r.ok = true;
    return r;
}

} // namespace aver::editor
