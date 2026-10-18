// GraphCollapseTest -- Collapse to Function (sandbox/src/GraphCollapse.hpp), driven through the REAL
// GraphEditor::collapseSelectionToFunction()/save() path -- the exact calls the node menu makes. AVER_WITH_IMGUI
// is not defined for this target, so no window is needed.
//
// What it proves, with files on a real filesystem:
//   * a selection with one exec in, one exec out, data in and data out becomes a function with the right
//     signature, a FuncEntry, a FuncReturn and a call node, every crossing wire re-pointed through them;
//   * a pure selection becomes a pure function (no exec pins anywhere);
//   * every refusal names its reason and changes nothing -- and costs no undo step;
//   * one Ctrl+Z puts the original back;
//   * node ids stay unique, and the saved file reparses.
// It also WRITES collapse_test.output.ocgraph next to the C# fixtures, so Aver.Graph.Tests'
// CollapseFunctionTests compiles and RUNS what this editor actually produced and compares its variables
// with the original graph's.
#include "GraphEditor.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeFile(const std::string& path, const std::string& text) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static std::string scratchDir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::current_path();
    return (base / "AverGraphCollapseTest").string();
}

static std::string scratch(const std::string& name, const std::string& text) {
    const std::string path = scratchDir() + "/" + name;
    writeFile(path, text);
    return path;
}

static bool hasLink(const fmt::OcGraphData& g, const std::string& sn, const std::string& sp, const std::string& dn, const std::string& dp) {
    for (const auto& l : g.links)
        if (l.sourceNode == sn && l.sourcePin == sp && l.destNode == dn && l.destPin == dp) return true;
    return false;
}

static const fmt::OcGraphNode* nodeById(const fmt::OcGraphData& g, const std::string& id) {
    for (const auto& n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}

static bool idsUnique(const fmt::OcGraphData& g) {
    std::set<std::string> seen;
    for (const auto& n : g.nodes) if (!seen.insert(n.id).second) return false;
    return true;
}

// ================================================================================ the C# fixture =
static void testCollapseWithExecAndData() {
    AVER_INFO("=== one exec in, one exec out, data in, data out -> an impure function, wires re-pointed ===");
    const std::string original = readFile(AVER_COLLAPSE_FIXTURE);
    check(!original.empty(), "the fixture was read (the path resolved)");
    const std::string path = scratch("collapse_test.ocgraph", original);

    GraphEditor ed(path);
    const std::string before = ed.serializeForSave();
    const usize nodes0 = ed.graph().nodes.size();
    const usize links0 = ed.graph().links.size();
    check(nodes0 == 10 && links0 == 10, "the fixture has 10 nodes and 10 links, got " + std::to_string(nodes0) + "/" + std::to_string(links0));

    ed.selectNodes({"c2", "c3", "mul", "addn", "st"});
    check(ed.selectedNodes().size() == 5, "five nodes are selected");
    const CollapseResult r = ed.collapseSelectionToFunction("ComputeScore");
    check(r.ok, "the collapse is accepted (reason '" + r.reason + "')");
    if (!r.ok) return;

    const fmt::OcGraphData& g = ed.graph();
    check(r.functionName == "ComputeScore" && !r.pure, "an impure function named ComputeScore");
    check(g.functions.size() == 1 && g.functions[0].name == "ComputeScore", "one FUNC record");
    if (g.functions.size() == 1) {
        const auto& fn = g.functions[0];
        check(!fn.pure, "not declared pure (it carries the exec chain)");
        check(fn.inputs.size() == 1 && fn.inputs[0].name == "gv_value" && fn.inputs[0].type == "float",
              "one input, named for the crossing source pin: gv_value (float)");
        check(fn.outputs.size() == 1 && fn.outputs[0].name == "addn_result" && fn.outputs[0].type == "float",
              "one output, named for the crossing pin inside: addn_result (float)");
    }

    check(g.nodes.size() == nodes0 + 3, "three nodes were added: entry, return, call");
    check(r.entryNodeId == "funcentry1" && r.returnNodeId == "funcreturn1" && r.callNodeId == "callfunc1",
          "with ids that cannot collide: " + r.entryNodeId + ", " + r.returnNodeId + ", " + r.callNodeId);
    check(idsUnique(g), "every node id is still unique");
    check(r.movedIds.size() == 5, "five nodes moved");
    for (const char* id : {"c2", "c3", "mul", "addn", "st"}) {
        const fmt::OcGraphNode* n = nodeById(g, id);
        check(n && getNodeAttribute(*n, "func").value == "ComputeScore", std::string(id) + " now lives in the function");
    }
    for (const char* id : {"tick", "gv", "c10", "add2", "sl"}) {
        const fmt::OcGraphNode* n = nodeById(g, id);
        check(n && !getNodeAttribute(*n, "func").found, std::string(id) + " stayed in the event graph");
    }
    const fmt::OcGraphNode* call = nodeById(g, r.callNodeId);
    check(call && getNodeAttribute(*call, "call").value == "ComputeScore" && !getNodeAttribute(*call, "func").found,
          "the call node calls it, from the event graph");

    // Every wire that crossed the selection's edge goes through the new nodes now.
    check(hasLink(g, "tick", "exec", "callfunc1", "exec"), "tick.exec -> the call's exec in");
    check(hasLink(g, "gv", "value", "callfunc1", "gv_value"), "the outside value -> the call's input");
    check(hasLink(g, "callfunc1", "then", "sl", "exec"), "the call's exec out -> where the chain used to continue");
    check(hasLink(g, "callfunc1", "addn_result", "add2", "a"), "the call's output -> the reader outside");
    check(hasLink(g, "funcentry1", "then", "st", "exec"), "inside: FuncEntry runs the first node");
    check(hasLink(g, "funcentry1", "gv_value", "addn", "b"), "inside: FuncEntry's input -> the node that read the outside value");
    check(hasLink(g, "st", "then", "funcreturn1", "exec"), "inside: the old exit runs FuncReturn");
    check(hasLink(g, "addn", "result", "funcreturn1", "addn_result"), "inside: the value read after is returned");
    check(hasLink(g, "mul", "result", "addn", "a") && hasLink(g, "addn", "result", "st", "value"),
          "inside wires are untouched");
    check(!hasLink(g, "tick", "exec", "st", "exec") && !hasLink(g, "gv", "value", "addn", "b") &&
          !hasLink(g, "st", "then", "sl", "exec") && !hasLink(g, "addn", "result", "add2", "a"),
          "and no wire still crosses the boundary directly");
    check(ed.selectedNodes().size() == 1 && ed.selectedNodes()[0] == "callfunc1", "the call node is selected afterwards");
    check(ed.dirty(), "the editor is dirty");

    // One undo step.
    ed.undoForTest();
    check(ed.serializeForSave() == before, "one Ctrl+Z restores the original exactly");
    check(ed.graph().functions.empty() && ed.graph().nodes.size() == nodes0, "no function, no extra nodes");
    ed.redoForTest();
    check(ed.graph().functions.size() == 1 && ed.graph().nodes.size() == nodes0 + 3, "and redo brings it back");

    // Saved: the file reparses, the new nodes' pins (which only restate the FUNC record) are not written,
    // and the C# suite gets the real output to compile.
    std::string why;
    check(ed.save(&why), "save succeeds (" + why + ")");
    const std::string saved = readFile(path);
    fmt::OcGraphData reread;
    std::string err;
    check(fmt::parseOcgraph(saved, reread, &err), "the saved file reparses (" + err + ")");
    check(reread.nodes.size() == nodes0 + 3 && reread.functions.size() == 1, "with the function and 13 nodes");
    check(saved.find("PIN funcentry1") == std::string::npos && saved.find("PIN callfunc1") == std::string::npos,
          "the synthesised pins of the new nodes are not written");
    check(saved.find("FUNC ComputeScore") != std::string::npos && saved.find("FUNCIN ComputeScore gv_value float") != std::string::npos &&
          saved.find("FUNCOUT ComputeScore addn_result float") != std::string::npos, "FUNC, FUNCIN and FUNCOUT are written");
    check(saved.find("NODE callfunc1 CallFunc 240 140 call=ComputeScore") != std::string::npos,
          "the call node sits at the selection's centroid");
    writeFile(AVER_COLLAPSE_FIXTURE_OUTPUT, saved);
    AVER_INFO("[GraphCollapseTest] wrote {} for the C# side to compile and run", AVER_COLLAPSE_FIXTURE_OUTPUT);

    // A second collapse in the same graph: the name is made unique, and a data-only selection is PURE.
    ed.selectNodes({"c10", "add2"});
    const CollapseResult r2 = ed.collapseSelectionToFunction("ComputeScore");
    check(r2.ok && r2.pure, "a selection with no exec at all collapses to a pure function (reason '" + r2.reason + "')");
    check(r2.functionName == "ComputeScore2", "an existing name is made unique: " + r2.functionName);
    if (r2.ok && ed.graph().functions.size() == 2) {
        const auto& fn = ed.graph().functions[1];
        check(fn.pure && fn.inputs.size() == 1 && fn.outputs.size() == 1, "one input (the call's output) and one output");
        const fmt::OcGraphNode* entry2 = nodeById(ed.graph(), r2.entryNodeId);
        const fmt::OcGraphNode* call2 = nodeById(ed.graph(), r2.callNodeId);
        bool anyExec = false;
        if (entry2) for (const auto& p : entry2->pins) anyExec |= p.type == "exec";
        if (call2) for (const auto& p : call2->pins) anyExec |= p.type == "exec";
        check(!anyExec, "a pure function's entry and call have no exec pins");
    }
    check(idsUnique(ed.graph()), "ids are still unique after two collapses");
}

// ================================================================================ a pure selection =
static void testPureSelection() {
    AVER_INFO("=== a data-only selection ===");
    const std::string path = scratch("pure.ocgraph", readFile(AVER_COLLAPSE_FIXTURE));
    GraphEditor ed(path);
    ed.selectNodes({"c2", "c3", "mul"});
    const CollapseResult r = ed.collapseSelectionToFunction("Product");
    check(r.ok && r.pure, "collapses to a pure function (reason '" + r.reason + "')");
    if (!r.ok) return;
    const fmt::OcGraphData& g = ed.graph();
    check(g.functions.size() == 1 && g.functions[0].pure && g.functions[0].inputs.empty() &&
          g.functions[0].outputs.size() == 1 && g.functions[0].outputs[0].name == "mul_result",
          "no inputs, one output mul_result");
    check(r.returnNodeId == "funcreturn1" && hasLink(g, "mul", "result", "funcreturn1", "mul_result"),
          "the product returns through FuncReturn");
    check(hasLink(g, "callfunc1", "mul_result", "addn", "a"), "and the call feeds the reader");
    check(!hasLink(g, "mul", "result", "addn", "a"), "the direct wire is gone");
}

// ================================================================================ refusals =
struct Refusal {
    const char* name;
    const char* text;
    std::vector<std::string> select;
    const char* mustContain;
};

static void testRefusals() {
    AVER_INFO("=== every refusal names its reason and changes nothing ===");
    const std::string head = "OCGRAPH 1\nDOMAIN gameplay\nNAME R\n";
    const Refusal cases[] = {
        {"an ENTRY node",
         "NODE tick OnTick 0 0\nNODE p PrintString 100 0 text=a\nLINK tick.exec p.exec\nENTRY tick OnTick\n",
         {"tick", "p"}, "entry point"},
        {"an event with no ENTRY",
         "NODE ev OnTick 0 0\nNODE p PrintString 100 0 text=a\nLINK ev.exec p.exec\n",
         {"ev"}, "is an event"},
        {"two exec exits",
         "NODE tick OnTick 0 0\nNODE br Branch 100 0\nNODE p1 PrintString 200 0 text=a\nNODE p2 PrintString 200 100 text=b\n"
         "LINK tick.exec br.exec\nLINK br.true p1.exec\nLINK br.false p2.exec\nENTRY tick OnTick\n",
         {"br"}, "selection has 2 exec exits; v1 supports one"},
        {"two exec entries",
         "NODE t1 OnTick 0 0\nNODE t2 OnStart 0 100\nNODE a PrintString 100 0 text=a\nNODE b PrintString 100 100 text=b\n"
         "LINK t1.exec a.exec\nLINK t2.exec b.exec\nENTRY t1 OnTick\nENTRY t2 OnStart\n",
         {"a", "b"}, "selection has 2 exec entries; v1 supports one"},
        {"a Param node",
         "PARAM entity int\nNODE pe Param 0 0 param=entity\n",
         {"pe"}, "parameters"},
        {"a loop without its body",
         "NODE tick OnTick 0 0\nNODE w While 100 0\nNODE body PrintString 200 0 text=a\n"
         "LINK tick.exec w.exec\nLINK w.loop body.exec\nENTRY tick OnTick\n",
         {"w"}, "without its whole body"},
        {"a graph OUT",
         "NODE c ConstFloat 0 0 value=1\nOUT c value\n",
         {"c"}, "OUT"},
        {"exec nodes nothing runs",
         "NODE p PrintString 0 0 text=a\n",
         {"p"}, "nothing in or out of the selection runs it"},
        {"a FuncEntry",
         "FUNC F pure\nNODE fe FuncEntry 0 0 func=F\n",
         {"fe"}, "belongs to its own function"},
        {"nothing selected",
         "NODE c ConstFloat 0 0 value=1\n",
         {}, "select the nodes"},
        {"a name with whitespace",
         "NODE c ConstFloat 0 0 value=1\n",
         {"c"}, "whitespace"},
    };

    int n = 0;
    for (const Refusal& c : cases) {
        const std::string path = scratch("refusal" + std::to_string(n++) + ".ocgraph", head + c.text);
        GraphEditor ed(path);
        const std::string before = ed.serializeForSave();
        ed.selectNodes(c.select);
        const bool badName = std::string(c.name) == "a name with whitespace";
        const CollapseResult r = ed.collapseSelectionToFunction(badName ? "two words" : "F2");
        check(!r.ok, std::string(c.name) + ": refused");
        check(r.reason.find(c.mustContain) != std::string::npos,
              std::string(c.name) + ": the reason mentions '" + c.mustContain + "' (got '" + r.reason + "')");
        check(ed.serializeForSave() == before && !ed.dirty(), std::string(c.name) + ": nothing changed");
        ed.undoForTest();
        check(ed.serializeForSave() == before, std::string(c.name) + ": and no undo step was spent");
    }
}

// The body of a loop on its own is an ordinary one-in, none-out selection: the call runs on every pass.
static void testLoopBodyAlone() {
    AVER_INFO("=== a loop body without its loop collapses ===");
    const std::string path = scratch("loopbody.ocgraph",
        "OCGRAPH 1\nDOMAIN gameplay\nNAME L\nNODE tick OnTick 0 0\nNODE w While 100 0\nNODE body PrintString 200 0 text=a\n"
        "LINK tick.exec w.exec\nLINK w.loop body.exec\nENTRY tick OnTick\n");
    GraphEditor ed(path);
    ed.selectNodes({"body"});
    const CollapseResult r = ed.collapseSelectionToFunction("Body");
    check(r.ok && !r.pure, "the body collapses to an impure function (reason '" + r.reason + "')");
    check(hasLink(ed.graph(), "w", "loop", r.callNodeId, "exec"), "and the loop's `loop` pin now runs the call");
    check(!hasLink(ed.graph(), "w", "loop", "body", "exec"), "instead of the node");

    // The loop WITH only part of its body is what is refused.
    GraphEditor two(scratch("loopsplit.ocgraph",
        "OCGRAPH 1\nDOMAIN gameplay\nNAME L2\nNODE tick OnTick 0 0\nNODE w While 100 0\nNODE b1 PrintString 200 0 text=a\n"
        "NODE b2 PrintString 300 0 text=b\nLINK tick.exec w.exec\nLINK w.loop b1.exec\nLINK b1.then b2.exec\nENTRY tick OnTick\n"));
    two.selectNodes({"w", "b1"});
    const CollapseResult s = two.collapseSelectionToFunction("Half");
    check(!s.ok && s.reason.find("without its whole body") != std::string::npos,
          "a loop taken with part of its body is refused: '" + s.reason + "'");
}

int main() {
    AVER_INFO("======== GraphCollapseTest ========");
    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);
    std::filesystem::create_directories(scratchDir());

    testCollapseWithExecAndData();
    testPureSelection();
    testRefusals();
    testLoopBodyAlone();

    std::filesystem::remove_all(scratchDir(), ec);
    if (g_failures == 0) AVER_INFO("======== GraphCollapseTest: all passed ========");
    else AVER_ERROR("======== GraphCollapseTest: {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
