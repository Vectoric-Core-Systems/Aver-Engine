// GraphClipboardTest -- the graph editor's shared clipboard (sandbox/src/GraphClipboard.hpp): what a copy
// takes, the AVER_NODE_CLIP text it becomes and how that reads back, and what a paste does to the graph it
// lands in -- missing variables declared, calls to missing functions skipped and named, a refusal across
// domains, and text that is not ours ignored. No ImGui (the OS-clipboard calls are GraphEditor's glue).
#include "GraphClipboard.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static fmt::OcGraphData parse(const std::string& text) {
    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(text, g, &err)) { AVER_ERROR("   FAIL  fixture did not parse: {}", err); ++g_failures; }
    return g;
}

// A source graph with a variable, a function and its call, and a node that lives inside that function.
static const char* kSource = R"(OCGRAPH 1
DOMAIN gameplay
NAME Src
VAR score int 7
FUNC Helper
FUNCIN Helper x float
NODE tick OnTick 0 0
NODE sv SetVar 100 50 var=score
NODE call CallFunc 220 50 call=Helper
NODE keep Add 340 50
NODE fe FuncEntry 0 300 func=Helper
NODE inner Add 340 300 func=Helper
PIN tick exec out exec
PIN sv exec in exec
PIN sv value in int
PIN sv then out exec
PIN call exec in exec
PIN call then out exec
PIN call x in float
PIN keep a in float
PIN keep b in float
PIN keep result out float
PIN fe then out exec
PIN fe x out float
PIN inner a in float
PIN inner b in float
PIN inner result out float
LINK tick.exec sv.exec
LINK sv.then call.exec
ENTRY tick OnTick
)";

static const char* kEmptyTarget = "OCGRAPH 1\nDOMAIN gameplay\nNAME Target\n";

static const fmt::OcGraphNode* nodeById(const fmt::OcGraphData& g, const std::string& id) {
    for (const auto& n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}

static std::string uniqueIdIn(const fmt::OcGraphData& g, std::string type) {
    for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 1;; ++i) {
        const std::string candidate = type + std::to_string(i);
        if (!nodeById(g, candidate)) return candidate;
    }
}

static void testCollect() {
    AVER_INFO("=== a copy takes the nodes, the links wholly inside them, and the declarations they name ===");
    const fmt::OcGraphData g = parse(kSource);
    const ClipPayload p = collectClip(g, {"tick", "sv", "call"});
    check(p.nodes.size() == 3 && p.nodes[0].id == "tick" && p.nodes[2].id == "call", "three nodes, in graph order");
    check(p.links.size() == 2, "both links have both ends inside, got " + std::to_string(p.links.size()));
    check(p.variables.size() == 1 && p.variables[0].name == "score" && p.variables[0].type == "int" &&
          p.variables[0].defaultValue == "7", "the VAR SetVar names comes with its type and default");
    check(p.functions.size() == 1 && p.functions[0].name == "Helper" && p.functions[0].inputs.size() == 1,
          "and the FUNC the call names, with its signature");
    check(p.domain == "gameplay", "and the source's domain");

    check(collectClip(g, {"sv"}).links.empty(), "one end of a link copies no link");
    const ClipPayload plain = collectClip(g, {"keep"});
    check(plain.variables.empty() && plain.functions.empty(), "a node naming nothing carries no declarations");
    check(collectClip(g, {"nope"}).empty(), "an unknown id copies nothing");
}

static void testTextRoundTrip() {
    AVER_INFO("=== the clip text reads back, and reads as .ocgraph ===");
    const fmt::OcGraphData g = parse(kSource);
    const ClipPayload p = collectClip(g, {"tick", "sv", "call"});
    const std::string text = serializeClip(p);
    check(text.rfind("AVER_NODE_CLIP 1\n", 0) == 0, "the text begins with the magic line");
    check(text.find("OCGRAPH 1") != std::string::npos && text.find("NODE sv SetVar") != std::string::npos,
          "and the rest is ordinary .ocgraph records");

    // Pasted into a text editor and saved, it is a graph: everything after the magic parses on its own.
    fmt::OcGraphData asGraph;
    check(fmt::parseOcgraph(text.substr(text.find('\n') + 1), asGraph, nullptr) && asGraph.nodes.size() == 3,
          "everything after the magic line is a valid graph");

    const auto back = parseClip(text);
    check(back.has_value(), "parseClip accepts it");
    if (!back) return;
    check(back->nodes.size() == 3 && back->nodes[1].id == "sv" && back->nodes[1].x == 100.0, "nodes and positions survive");
    bool valuePin = false;
    for (const auto& pin : back->nodes[1].pins) valuePin |= pin.name == "value" && pin.type == "int" && !pin.isOutput;
    check(valuePin, "a node's pins travel with it");
    check(back->links.size() == 2 && back->links[0].sourceNode == "tick", "links survive");
    check(back->variables.size() == 1 && back->variables[0].defaultValue == "7", "the variable declaration survives");
    check(back->functions.size() == 1 && back->functions[0].name == "Helper", "the function declaration survives");
    check(back->domain == "gameplay", "and the domain");

    check(parseClip("   \r\n" + text).has_value(), "leading whitespace before the magic is tolerated");
}

static void testForeignText() {
    AVER_INFO("=== text that is not ours is ignored ===");
    check(!parseClip("").has_value(), "empty");
    check(!parseClip("hello world").has_value(), "ordinary text");
    check(!parseClip("OCGRAPH 1\nNODE a Add 0 0\n").has_value(), "a bare .ocgraph without the magic line");
    check(!parseClip("AVER_NODE_CLIP 2\nOCGRAPH 1\nNODE a Add 0 0\n").has_value(), "a clip version this build does not know");
    check(!parseClip("AVER_NODE_CLIP 1\nnot a graph at all\n").has_value(), "the magic over a body that does not parse");
    check(!parseClip("AVER_NODE_CLIP 1\nOCGRAPH 1\nNAME Empty\n").has_value(), "a clip with no nodes");
    check(!parseClip("AVER_NODE_CLIP 1").has_value(), "the magic alone");
}

static void testPlan() {
    AVER_INFO("=== planPaste: decided before anything is touched ===");
    const fmt::OcGraphData src = parse(kSource);
    const ClipPayload clip = collectClip(src, {"tick", "sv", "call"});

    // Nothing here: the variable is declared, the call is skipped.
    {
        const fmt::OcGraphData target = parse(kEmptyTarget);
        const PastePlan plan = planPaste(clip, target);
        check(plan.ok, "a gameplay clip pastes into a gameplay graph");
        check(plan.keep.size() == 2 && plan.keep[0] == 0 && plan.keep[1] == 1, "tick and sv are kept");
        check(plan.droppedCalls.size() == 1 && plan.droppedCalls[0] == "call", "the call to a function this graph lacks is dropped and named");
        check(plan.declare.size() == 1 && plan.declare[0].name == "score" && plan.declare[0].type == "int" &&
              plan.declare[0].defaultValue == "7", "the missing VAR is declared with the carried type and default");
        const std::string note = describePaste(plan);
        check(note.find("declared variables score") != std::string::npos && note.find("call") != std::string::npos,
              "and the banner says both: '" + note + "'");
    }
    // The target has the variable under another type: left alone, reported.
    {
        const fmt::OcGraphData target = parse("OCGRAPH 1\nNAME T\nVAR score float 0\nFUNC Helper\nFUNCIN Helper x float\n");
        const PastePlan plan = planPaste(clip, target);
        check(plan.ok && plan.declare.empty(), "an existing variable is not declared again");
        check(plan.typeConflicts.size() == 1 && plan.typeConflicts[0] == "score", "but its different type is reported");
        check(plan.droppedCalls.empty() && plan.keep.size() == 3, "and with the function present the call is kept");
    }
    // Across domains.
    {
        const fmt::OcGraphData material = parse("OCGRAPH 1\nDOMAIN material\nNAME M\n");
        const PastePlan plan = planPaste(clip, material);
        check(!plan.ok && plan.refusal.find("gameplay") != std::string::npos && plan.refusal.find("material") != std::string::npos,
              "gameplay nodes into a material graph are refused, naming both: '" + plan.refusal + "'");
        check(plan.keep.empty(), "and nothing is planned");
        // No DOMAIN record means gameplay.
        check(planPaste(clip, parse("OCGRAPH 1\nNAME NoDomain\n")).ok, "a target with no DOMAIN is gameplay");
    }
    // Nothing pasteable.
    {
        const ClipPayload onlyCall = collectClip(src, {"call"});
        const PastePlan plan = planPaste(onlyCall, parse(kEmptyTarget));
        check(!plan.ok && plan.refusal.find("function") != std::string::npos, "a clip of only an unanswerable call is refused: '" + plan.refusal + "'");
        const ClipPayload entryOnly = collectClip(src, {"fe"});
        const PastePlan p2 = planPaste(entryOnly, parse(kEmptyTarget));
        check(!p2.ok && p2.droppedFunctionNodes.size() == 1, "a FuncEntry is never pasted (a function has exactly one)");
        check(!planPaste(ClipPayload{}, parse(kEmptyTarget)).ok, "an empty clip is refused");
    }
}

static void testApply() {
    AVER_INFO("=== applyPaste: ids, links, positions, the declared variable ===");
    const fmt::OcGraphData src = parse(kSource);
    const ClipPayload clip = collectClip(src, {"tick", "sv", "call"});

    fmt::OcGraphData target = parse("OCGRAPH 1\nDOMAIN gameplay\nNAME Target\nNODE ontick1 OnTick 0 0\n");
    const PasteResult r = applyPaste(target, clip, 500.0, 500.0, "",
                                     [&](const std::string& type) { return uniqueIdIn(target, type); });
    check(r.plan.ok && r.newIds.size() == 2, "two nodes arrived");
    check(target.nodes.size() == 3, "the target holds its own node plus the two");
    check(r.newIds.size() == 2 && r.newIds[0] == "ontick2" && r.newIds[1] == "setvar1",
          "fresh ids that cannot collide with what was there");
    check(r.oldToNew.count("tick") && r.oldToNew.at("tick") == "ontick2", "the old-to-new map is kept");
    check(target.links.size() == 1 && target.links[0].sourceNode == "ontick2" && target.links[0].destNode == "setvar1",
          "the one internal link whose ends both came is remapped; the link to the skipped call is not made");
    check(target.variables.size() == 1 && target.variables[0].name == "score" && target.variables[0].type == "int",
          "the variable was declared in the same step");
    const fmt::OcGraphNode* first = nodeById(target, "ontick2");
    const fmt::OcGraphNode* second = nodeById(target, "setvar1");
    check(first && second && first->x == 500.0 && first->y == 500.0 && second->x == 600.0 && second->y == 550.0,
          "the copied set's top-left lands on the paste point, keeping its relative layout");

    // func= follows where it lands.
    const ClipPayload inFunction = collectClip(src, {"inner"});
    fmt::OcGraphData t2 = parse(kEmptyTarget);
    applyPaste(t2, inFunction, 0.0, 0.0, "", [&](const std::string& t) { return uniqueIdIn(t2, t); });
    check(t2.nodes.size() == 1 && !getNodeAttribute(t2.nodes[0], "func").found, "pasted into the event graph, a node loses its func=");
    fmt::OcGraphData t3 = parse(kEmptyTarget);
    applyPaste(t3, collectClip(src, {"keep"}), 0.0, 0.0, "Other", [&](const std::string& t) { return uniqueIdIn(t3, t); });
    check(t3.nodes.size() == 1 && getNodeAttribute(t3.nodes[0], "func").value == "Other",
          "pasted onto a function's canvas, a node gains that function's func=");

    // A refused paste changes nothing.
    fmt::OcGraphData material = parse("OCGRAPH 1\nDOMAIN material\nNAME M\n");
    const PasteResult refused = applyPaste(material, clip, 0.0, 0.0, "", [&](const std::string& t) { return uniqueIdIn(material, t); });
    check(!refused.plan.ok && material.nodes.empty() && material.variables.empty(), "a refused paste touches nothing");
}

static void testSharedStore() {
    AVER_INFO("=== one store, many graphs: copy once, paste into two different tabs ===");
    const fmt::OcGraphData src = parse(kSource);
    clipStore().text = serializeClip(collectClip(src, {"keep"}));

    // "Tab A closes": nothing of the source is needed any more.
    fmt::OcGraphData a = parse(kEmptyTarget);
    fmt::OcGraphData b = parse("OCGRAPH 1\nNAME OtherTab\n");
    for (fmt::OcGraphData* target : {&a, &b}) {
        const auto clip = parseClip(clipStore().text);
        check(clip.has_value(), "the store still parses");
        if (!clip) continue;
        const PasteResult r = applyPaste(*target, *clip, 10.0, 20.0, "", [&](const std::string& t) { return uniqueIdIn(*target, t); });
        check(r.plan.ok && target->nodes.size() == 1 && target->nodes[0].type == "Add", "pasted into " + target->name);
    }
    clipStore().text.clear();
    check(!parseClip(clipStore().text).has_value(), "an emptied store holds nothing");
}

int main() {
    AVER_INFO("======== GraphClipboardTest ========");
    testCollect();
    testTextRoundTrip();
    testForeignText();
    testPlan();
    testApply();
    testSharedStore();
    if (g_failures == 0) AVER_INFO("======== GraphClipboardTest: all passed ========");
    else AVER_ERROR("======== GraphClipboardTest: {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
