// GraphEditorGeometryTest -- the .ocgraph node editor's headless core: the node descriptor table,
// layout, hit-testing, link rules, cycle detection, and the canvas pan/zoom transform. See
// sandbox/src/GraphEditorGeometry.hpp/.cpp and sandbox/src/GraphNodeDefs.hpp for what is under test.
//
// No ImGui, no Engine, no scene:: -- the whole point of building this core first (per the task brief
// and docs/CHUNKS.md's "headless first slice" precedent) is that it runs with no window and no GPU.
#include "GraphEditorGeometry.hpp"
#include "GraphNodeDefs.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

// Builds a two-node graph (a Const feeding an Add's 'a' input) with real pins, matching the shape of
// the checked-in cross-impl fixture, so layout/hit-test/link-rule tests share one realistic graph.
static fmt::OcGraphData makeSampleGraph() {
    fmt::OcGraphData g;
    fmt::OcGraphNode c1;
    c1.id = "c1"; c1.type = "ConstFloat"; c1.x = 0; c1.y = 0;
    c1.pins.push_back({"value", "float", true, "5"});
    g.nodes.push_back(c1);

    fmt::OcGraphNode sum;
    sum.id = "sum"; sum.type = "Add"; sum.x = 240; sum.y = 20;
    sum.pins.push_back({"a", "float", false, ""});
    sum.pins.push_back({"b", "float", false, ""});
    sum.pins.push_back({"result", "float", true, ""});
    g.nodes.push_back(sum);

    fmt::OcGraphLink link;
    link.sourceNode = "c1"; link.sourcePin = "value";
    link.destNode = "sum"; link.destPin = "a";
    g.links.push_back(link);
    return g;
}

// ======================================================================================= catalog ===
static void testNodeCatalog() {
    AVER_INFO("=== node descriptor table ===");

    const auto& table = graphNodeCatalog();
    check(!table.empty(), "catalog is non-empty");

    // The eight node types named in the task brief.
    const char* baseline[] = {"Add", "Multiply", "Compare", "ConstBool", "ConstFloat", "ConstInt", "GetField", "SetField"};
    for (const char* t : baseline) {
        check(findGraphNodeDesc(t) != nullptr, std::string("catalog has baseline type '") + t + "'");
    }
    // The concurrent workflow's incoming node types -- proving each really was a one-line addition.
    const char* incoming[] = {"Sin", "Cos", "Subtract", "Divide", "Param"};
    for (const char* t : incoming) {
        check(findGraphNodeDesc(t) != nullptr, std::string("catalog has incoming type '") + t + "'");
    }

    // GetFieldVec3/SetFieldVec3: the Vec3 siblings of GetField/SetField -- proving this editor's
    // palette was kept in parity with scripting/csharp/Aver.Graph/OcGraphParser.cs's own
    // "getfieldvec3"/"setfieldvec3" cases rather than left as a compiler-only addition (see
    // GraphNodeDefs.hpp's own header comment on why that parity matters, and its KNOWN GAP note for
    // what is still NOT true of field= -- readable through the GUI -- even once the catalog knows the
    // node type exists).
    const char* vec3Fields[] = {"GetFieldVec3", "SetFieldVec3"};
    for (const char* t : vec3Fields) {
        check(findGraphNodeDesc(t) != nullptr, std::string("catalog has Vec3-field type '") + t + "'");
    }

    // The exec/flow additions: branch, sequence, while, forEach, and the three event-entry trigger
    // types (OnHit is the on-demand-fired one -- see scripting/csharp/Aver.Graph/GraphHost.cs's own
    // PHASE 3 comment for what "on-demand" means; this catalog entry is identically-shaped to
    // OnStart/OnTick because the DIFFERENCE is entirely in how a host drives it, not in the node's own
    // pins). Present in the catalog with the SAME pin shapes OcGraphParser.AddDefaultPins gives them
    // on the C# side -- see GraphNodeDefs.hpp's own header comment on why that parity matters.
    const char* flow[] = {"Branch", "Sequence", "While", "ForEach", "OnStart", "OnTick", "OnHit"};
    for (const char* t : flow) {
        check(findGraphNodeDesc(t) != nullptr, std::string("catalog has flow type '") + t + "'");
    }

    check(findGraphNodeDesc("NoSuchNodeType") == nullptr, "unknown type resolves to nullptr");

    // Case-insensitive lookup, since the fixture file uses "ConstFloat" and GraphCompiler.cs's switch
    // uses "constfloat".
    check(findGraphNodeDesc("constfloat") != nullptr, "lookup is case-insensitive (lowercase)");
    check(findGraphNodeDesc("CONSTFLOAT") != nullptr, "lookup is case-insensitive (uppercase)");

    // Add's shape: two float inputs, one float output.
    const GraphNodeDesc* add = findGraphNodeDesc("Add");
    if (add) {
        int ins = 0, outs = 0;
        for (const auto& p : add->pins) (p.isOutput ? outs : ins)++;
        check(ins == 2, "Add has 2 input pins");
        check(outs == 1, "Add has 1 output pin");
    } else {
        check(false, "Add present (shape check skipped)");
    }

    // A constant's shape: a single output pin, no inputs.
    const GraphNodeDesc* cf = findGraphNodeDesc("ConstFloat");
    if (cf) {
        check(cf->pins.size() == 1 && cf->pins[0].isOutput, "ConstFloat has exactly one output pin");
    } else {
        check(false, "ConstFloat present (shape check skipped)");
    }

    // GetFieldVec3's shape: entity in, x/y/z out, no exec -- matches OcGraphParser.AddDefaultPins's
    // "getfieldvec3" case pin-for-pin, and mirrors GetField's own shape check just above with a wider
    // output.
    const GraphNodeDesc* gv = findGraphNodeDesc("GetFieldVec3");
    if (gv) {
        int ins = 0, outs = 0, execPins = 0;
        for (const auto& p : gv->pins) { (p.isOutput ? outs : ins)++; if (p.type == "exec") ++execPins; }
        check(ins == 1, "GetFieldVec3 has 1 input pin (entity)");
        check(outs == 3, "GetFieldVec3 has 3 output pins (x, y, z)");
        check(execPins == 0, "GetFieldVec3 has no exec pins by default");
    } else {
        check(false, "GetFieldVec3 present (shape check skipped)");
    }

    // SetFieldVec3's shape: entity+x+y+z in, success out, no exec by default -- deliberately, mirroring
    // SetField's own no-exec-by-default shape (see GraphCompiler.EmitSetFieldVec3's comment for why).
    const GraphNodeDesc* sv = findGraphNodeDesc("SetFieldVec3");
    if (sv) {
        int ins = 0, outs = 0, execPins = 0;
        for (const auto& p : sv->pins) { (p.isOutput ? outs : ins)++; if (p.type == "exec") ++execPins; }
        check(ins == 4, "SetFieldVec3 has 4 input pins (entity, x, y, z)");
        check(outs == 1, "SetFieldVec3 has 1 output pin (success)");
        check(execPins == 0, "SetFieldVec3 has no exec pins by default");
    } else {
        check(false, "SetFieldVec3 present (shape check skipped)");
    }

    // Spawn: the visual-scripting slice's Spawn node -- proving this editor's palette was kept in
    // parity with scripting/csharp/Aver.Graph/OcGraphParser.cs's own "spawn" case rather than left
    // compiler-only (see GraphNodeDefs.hpp's own comment on why it gets exec pins by default here,
    // unlike GetField/SetField/GetFieldVec3/SetFieldVec3 above).
    check(findGraphNodeDesc("Spawn") != nullptr, "catalog has 'Spawn'");

    const GraphNodeDesc* spawn = findGraphNodeDesc("Spawn");
    if (spawn) {
        int execIns = 0, execOuts = 0, dataIns = 0, dataOuts = 0;
        for (const auto& p : spawn->pins) {
            if (p.type == "exec") (p.isOutput ? execOuts : execIns)++;
            else (p.isOutput ? dataOuts : dataIns)++;
        }
        check(execIns == 1, "Spawn has exactly one incoming exec pin");
        check(execOuts == 1, "Spawn has exactly one outgoing exec pin ('then')");
        check(dataIns == 3, "Spawn has 3 non-exec input pins (x, y, z)");
        check(dataOuts == 1, "Spawn has 1 non-exec output pin (entity)");
    } else {
        check(false, "Spawn present (shape check skipped)");
    }

    // MouseDelta/MoveAxis: the continuous-input pair -- proving this editor's palette was kept in
    // parity with scripting/csharp/Aver.Graph/OcGraphParser.cs's own "mousedelta"/"moveaxis" cases,
    // exactly like the GetFieldVec3/SetFieldVec3/Spawn checks above. Both get exec pins by default
    // (mirroring Spawn/Raycast, unlike GetFieldVec3/SetFieldVec3) -- see GraphNodeDefs.hpp's own
    // comment on why.
    check(findGraphNodeDesc("MouseDelta") != nullptr, "catalog has 'MouseDelta'");
    const GraphNodeDesc* mouseDelta = findGraphNodeDesc("MouseDelta");
    if (mouseDelta) {
        int execIns = 0, execOuts = 0, dataIns = 0, dataOuts = 0;
        for (const auto& p : mouseDelta->pins) {
            if (p.type == "exec") (p.isOutput ? execOuts : execIns)++;
            else (p.isOutput ? dataOuts : dataIns)++;
        }
        check(execIns == 1, "MouseDelta has exactly one incoming exec pin");
        check(execOuts == 1, "MouseDelta has exactly one outgoing exec pin ('then')");
        check(dataIns == 0, "MouseDelta has no non-exec input pins");
        check(dataOuts == 3, "MouseDelta has 3 non-exec output pins (deltaX, deltaY, wheel)");
    } else {
        check(false, "MouseDelta present (shape check skipped)");
    }

    check(findGraphNodeDesc("MoveAxis") != nullptr, "catalog has 'MoveAxis'");
    const GraphNodeDesc* moveAxis = findGraphNodeDesc("MoveAxis");
    if (moveAxis) {
        int execIns = 0, execOuts = 0, dataIns = 0, dataOuts = 0;
        for (const auto& p : moveAxis->pins) {
            if (p.type == "exec") (p.isOutput ? execOuts : execIns)++;
            else (p.isOutput ? dataOuts : dataIns)++;
        }
        check(execIns == 1, "MoveAxis has exactly one incoming exec pin");
        check(execOuts == 1, "MoveAxis has exactly one outgoing exec pin ('then')");
        check(dataIns == 0, "MoveAxis has no non-exec input pins");
        check(dataOuts == 2, "MoveAxis has 2 non-exec output pins (forward, right) -- no 'z' pin");
    } else {
        check(false, "MoveAxis present (shape check skipped)");
    }

    // Branch's shape: one incoming exec pulse, a bool condition, and two outgoing exec pins -- the
    // "decide" primitive the whole visual-scripting phase exists to add.
    const GraphNodeDesc* branch = findGraphNodeDesc("Branch");
    if (branch) {
        int execIns = 0, execOuts = 0, dataIns = 0;
        for (const auto& p : branch->pins) {
            if (p.type == "exec") (p.isOutput ? execOuts : execIns)++;
            else if (!p.isOutput) dataIns++;
        }
        check(execIns == 1, "Branch has exactly one incoming exec pin");
        check(execOuts == 2, "Branch has exactly two outgoing exec pins (true/false)");
        check(dataIns == 1, "Branch has exactly one non-exec input (the bool condition)");
    } else {
        check(false, "Branch present (shape check skipped)");
    }

    // OnStart/OnTick/OnHit: no inputs at all, one exec output -- the node an ENTRY record points at.
    // OnHit's shape is identical to the other two on purpose (see the "flow" catalog check above) --
    // this loop proves that identity rather than assuming it.
    for (const char* triggerType : {"OnStart", "OnTick", "OnHit"}) {
        const GraphNodeDesc* trigger = findGraphNodeDesc(triggerType);
        if (trigger) {
            bool anyInput = false;
            int execOuts = 0;
            for (const auto& p : trigger->pins) {
                if (!p.isOutput) anyInput = true;
                if (p.isOutput && p.type == "exec") ++execOuts;
            }
            check(!anyInput, std::string(triggerType) + " has no input pins of its own");
            check(execOuts == 1, std::string(triggerType) + " has exactly one exec output pin");
        } else {
            check(false, std::string(triggerType) + " present (shape check skipped)");
        }
    }
}

// ======================================================================================== layout ===
static void testLayout() {
    AVER_INFO("=== layout ===");
    GraphLayoutStyle style;

    fmt::OcGraphNode add;
    add.id = "n1"; add.type = "Add"; add.x = 100; add.y = 50;
    add.pins.push_back({"a", "float", false, ""});
    add.pins.push_back({"b", "float", false, ""});
    add.pins.push_back({"result", "float", true, ""});

    const GraphNodeLayout l1 = computeNodeLayout(add, "Add", style, 1.0f);
    check(l1.nodeId == "n1", "layout carries the node id");
    check(l1.min.x == 100.0f && l1.min.y == 50.0f, "layout box originates at the node's stored position");
    check(l1.max.x > l1.min.x && l1.max.y > l1.min.y, "layout box has positive size");
    check(l1.pins.size() == 3, "layout produced one pin entry per node pin");

    // Two inputs at rows 0/1 on the LEFT edge, one output at row 0 on the RIGHT edge.
    // NOTE: layouts1 must be a named local, not a `{l1}` temporary passed straight into
    // findPinLayout() -- a temporary vector's lifetime ends at the end of the full expression, so
    // pointers into it (pa/pb/pr) would dangle by the time the checks below dereference them.
    const std::vector<GraphNodeLayout> layouts1{l1};
    const GraphPinLayout* pa = findPinLayout(layouts1, "n1", "a");
    const GraphPinLayout* pb = findPinLayout(layouts1, "n1", "b");
    const GraphPinLayout* pr = findPinLayout(layouts1, "n1", "result");
    check(pa && pb && pr, "all three pins are findable by name");
    if (pa && pb && pr) {
        check(pa->pos.x == l1.min.x, "input pin 'a' sits on the node's left edge");
        check(pb->pos.x == l1.min.x, "input pin 'b' sits on the node's left edge");
        check(pr->pos.x == l1.max.x, "output pin 'result' sits on the node's right edge");
        check(pb->pos.y > pa->pos.y, "second input pin is on a lower row than the first");
        check(!pa->isOutput && !pb->isOutput && pr->isOutput, "pin direction is preserved into the layout");
    }

    // A long title should not shrink the box below what the title needs.
    const GraphNodeLayout lShort = computeNodeLayout(add, "Add", style, 1.0f);
    const GraphNodeLayout lLong = computeNodeLayout(add, "A Very Long Node Title Indeed", style, 1.0f);
    check(lLong.max.x - lLong.min.x > lShort.max.x - lShort.min.x, "a longer title widens the box");

    // More pins (taller rows) should grow the height.
    fmt::OcGraphNode add4 = add;
    add4.pins.push_back({"c", "float", false, ""});
    const GraphNodeLayout l4 = computeNodeLayout(add4, "Add", style, 1.0f);
    check(l4.max.y - l4.min.y > l1.max.y - l1.min.y, "an extra input pin makes the box taller");

    // DPI-scale-awareness: doubling scale should not shrink anything, and should grow the box.
    const GraphNodeLayout lScaled = computeNodeLayout(add, "Add", style, 2.0f);
    check(lScaled.max.x - lScaled.min.x > l1.max.x - l1.min.x, "scale=2 widens the box vs scale=1");
    check(lScaled.max.y - lScaled.min.y > l1.max.y - l1.min.y, "scale=2 heightens the box vs scale=1");
}

// ===================================================================================== hit-test ===
static void testHitTest() {
    AVER_INFO("=== hit-testing ===");
    GraphLayoutStyle style;
    const fmt::OcGraphData g = makeSampleGraph();

    std::vector<GraphNodeLayout> layouts;
    for (const auto& n : g.nodes) layouts.push_back(computeNodeLayout(n, n.type, style, 1.0f));

    const GraphPinLayout* c1value = findPinLayout(layouts, "c1", "value");
    check(c1value != nullptr, "sample graph's c1.value pin is laid out");
    if (c1value) {
        // Exactly on the pin.
        GraphHitResult exact = hitTest(g, layouts, c1value->pos, style, 1.0f);
        check(exact.kind == GraphHitKind::Pin && exact.nodeId == "c1" && exact.pinName == "value",
              "a point exactly on a pin hits that pin");

        // A few pixels off, still inside the generous hit radius but outside the visual dot radius
        // (dot radius 5px, hit radius 9px) -- this is the "pins need a generous radius" requirement.
        const Vec2 nearMiss = c1value->pos + Vec2(7.0f, 0.0f);
        GraphHitResult near = hitTest(g, layouts, nearMiss, style, 1.0f);
        check(near.kind == GraphHitKind::Pin && near.pinName == "value",
              "a near-miss within the generous pin hit radius still hits the pin");

        // Outside even the generous radius.
        const Vec2 farMiss = c1value->pos + Vec2(60.0f, 0.0f);
        GraphHitResult far = hitTest(g, layouts, farMiss, style, 1.0f);
        check(far.kind != GraphHitKind::Pin, "a point well outside the hit radius does not hit the pin");
    }

    // A point in the middle of a node's body (not on any pin) hits the node.
    if (!layouts.empty()) {
        const GraphNodeLayout& sumLayout = layouts[1]; // "sum", per makeSampleGraph()
        const Vec2 mid((sumLayout.min.x + sumLayout.max.x) * 0.5f, sumLayout.min.y + 2.0f); // near header, away from pin rows
        GraphHitResult bodyHit = hitTest(g, layouts, mid, style, 1.0f);
        check(bodyHit.kind == GraphHitKind::Node && bodyHit.nodeId == "sum", "a point on a node's header hits the node body");
    }

    // A point on the link's curve (its exact midpoint sample) hits the link.
    const GraphPinLayout* sumA = findPinLayout(layouts, "sum", "a");
    if (c1value && sumA) {
        const GraphBezier b = linkBezier(c1value->pos, sumA->pos);
        std::vector<Vec2> samples;
        sampleCubicBezier(b, 24, samples);
        const Vec2 midSample = samples[samples.size() / 2];
        GraphHitResult linkHit = hitTest(g, layouts, midSample, style, 1.0f);
        check(linkHit.kind == GraphHitKind::Link && linkHit.linkIndex == 0, "a point on the link curve hits the link");
    }

    // A point far from everything hits nothing.
    GraphHitResult none = hitTest(g, layouts, Vec2(-5000.0f, -5000.0f), style, 1.0f);
    check(none.kind == GraphHitKind::None, "a point in empty canvas space hits nothing");
}

// ==================================================================================== link rules ===
static void testLinkRules() {
    AVER_INFO("=== link rules ===");

    // Valid connection.
    {
        fmt::OcGraphData g = makeSampleGraph();
        g.links.clear(); // start with no links so 'a' is free
        GraphLinkCheck r = canConnectPins(g, "c1", "value", "sum", "a");
        check(r.ok && r.reason == GraphLinkReject::None, "output float -> input float is accepted");
    }

    // Output-to-output.
    {
        fmt::OcGraphData g = makeSampleGraph();
        GraphLinkCheck r = canConnectPins(g, "c1", "value", "sum", "result");
        check(!r.ok, "output -> output is rejected");
    }

    // Input-to-input.
    {
        fmt::OcGraphData g = makeSampleGraph();
        GraphLinkCheck r = canConnectPins(g, "sum", "a", "sum", "b");
        check(!r.ok && r.reason == GraphLinkReject::SourceNotOutput, "input -> input is rejected (source not an output)");
    }

    // Self-connection.
    {
        fmt::OcGraphData g = makeSampleGraph();
        g.nodes[1].pins.push_back({"selfIn", "float", false, ""}); // give 'sum' a spare input to target
        GraphLinkCheck r = canConnectPins(g, "sum", "result", "sum", "selfIn");
        check(!r.ok && r.reason == GraphLinkReject::SelfConnection, "a node connecting to itself is rejected");
    }

    // Destination already occupied.
    {
        fmt::OcGraphData g = makeSampleGraph(); // c1.value -> sum.a already exists
        fmt::OcGraphNode c2;
        c2.id = "c2"; c2.type = "ConstFloat"; c2.x = 0; c2.y = 150;
        c2.pins.push_back({"value", "float", true, "1"});
        g.nodes.push_back(c2);
        GraphLinkCheck r = canConnectPins(g, "c2", "value", "sum", "a");
        check(!r.ok && r.reason == GraphLinkReject::DestOccupied, "a second link into an occupied input is rejected");
    }

    // Type mismatch.
    {
        fmt::OcGraphData g = makeSampleGraph();
        g.links.clear();
        fmt::OcGraphNode cb;
        cb.id = "cb"; cb.type = "ConstBool"; cb.x = 0; cb.y = 300;
        cb.pins.push_back({"value", "bool", true, "false"});
        g.nodes.push_back(cb);
        GraphLinkCheck r = canConnectPins(g, "cb", "value", "sum", "a");
        check(!r.ok && r.reason == GraphLinkReject::TypeMismatch, "bool output -> float input is rejected (type mismatch)");
    }

    // Unknown pin.
    {
        fmt::OcGraphData g = makeSampleGraph();
        GraphLinkCheck r = canConnectPins(g, "c1", "nope", "sum", "a");
        check(!r.ok && r.reason == GraphLinkReject::UnknownPin, "a nonexistent source pin is rejected");
    }

    // Exec-to-exec: accepted. "exec" is just another pin type to this function -- see
    // canConnectPins' own comment above the type-equality check -- so this needs no exec-specific
    // code path to pass, and that absence is exactly the point being tested.
    {
        fmt::OcGraphData g;
        fmt::OcGraphNode tick;
        tick.id = "tick"; tick.type = "OnTick"; tick.x = 0; tick.y = 0;
        tick.pins.push_back({"exec", "exec", true, ""});
        g.nodes.push_back(tick);
        fmt::OcGraphNode seq;
        seq.id = "seq"; seq.type = "Sequence"; seq.x = 200; seq.y = 0;
        seq.pins.push_back({"exec", "exec", false, ""});
        seq.pins.push_back({"then0", "exec", true, ""});
        g.nodes.push_back(seq);
        GraphLinkCheck r = canConnectPins(g, "tick", "exec", "seq", "exec");
        check(r.ok, "an exec output connects to an exec input");
    }

    // Exec-to-data (and data-to-exec): rejected as a type mismatch, the SAME check and the SAME
    // GraphLinkReject reason a float-to-bool link gets above -- proving the task's "exec pin only
    // connects to exec pin, never to data" requirement is a free consequence of type equality, not a
    // separately maintained rule that could drift out of sync with it.
    {
        fmt::OcGraphData g;
        fmt::OcGraphNode tick;
        tick.id = "tick"; tick.type = "OnTick"; tick.x = 0; tick.y = 0;
        tick.pins.push_back({"exec", "exec", true, ""});
        g.nodes.push_back(tick);
        fmt::OcGraphNode add;
        add.id = "add"; add.type = "Add"; add.x = 200; add.y = 0;
        add.pins.push_back({"a", "float", false, ""});
        add.pins.push_back({"b", "float", false, ""});
        add.pins.push_back({"result", "float", true, ""});
        g.nodes.push_back(add);
        GraphLinkCheck r = canConnectPins(g, "tick", "exec", "add", "a");
        check(!r.ok && r.reason == GraphLinkReject::TypeMismatch,
              "an exec output cannot connect to a float input (exec-to-data is refused)");

        // Data-to-exec, with both ends correctly an output and an input respectively (so this isolates
        // TYPE as the only reason it fails, not directionality): add.result (float, output) into an
        // exec INPUT pin.
        fmt::OcGraphNode seq;
        seq.id = "seq"; seq.type = "Sequence"; seq.x = 400; seq.y = 0;
        seq.pins.push_back({"exec", "exec", false, ""});
        g.nodes.push_back(seq);
        GraphLinkCheck r2 = canConnectPins(g, "add", "result", "seq", "exec");
        check(!r2.ok && r2.reason == GraphLinkReject::TypeMismatch,
              "a float output cannot connect to an exec input (data-to-exec is refused)");
    }
}

// ===================================================================== pluggable pin type checks ===
// canConnectPins' type check takes a PinTypeCompat predicate with a default (exactPinTypeMatch) that
// reproduces the exact-equality rule testLinkRules above already exercises end to end. These tests
// are about the PLUGGABILITY itself: that passing materialPinTypeMatch changes exactly the cases
// widen() (modules/render.pbr/src/MaterialGraphHlsl.cpp) changes and none of the ones it doesn't,
// and that exec-to-data stays refused no matter which predicate is in play.
static void testPluggablePinTypeChecks() {
    AVER_INFO("=== pluggable pin type checks (exactPinTypeMatch / materialPinTypeMatch) ===");

    // A one-node-per-type-string graph: every source/dest pair below is built from these, so a test
    // only has to name which two pins it wants to try connecting.
    auto makeTypedGraph = [](const char* outType, const char* inType) {
        fmt::OcGraphData g;
        fmt::OcGraphNode src; src.id = "src"; src.type = "Src"; src.x = 0; src.y = 0;
        src.pins.push_back({"out", outType, true, ""});
        g.nodes.push_back(src);
        fmt::OcGraphNode dst; dst.id = "dst"; dst.type = "Dst"; dst.x = 200; dst.y = 0;
        dst.pins.push_back({"in", inType, false, ""});
        g.nodes.push_back(dst);
        return g;
    };

    // exec -> data (and the reverse) is refused under BOTH predicates -- the property the task brief
    // calls out by name: nothing about making the type check pluggable may let control flow reach a
    // data pin. materialPinTypeMatch only ever promotes between float/float2/float3/float4, so an
    // "exec" pin string falls through to that predicate's own exact-equality branch, same refusal.
    {
        fmt::OcGraphData g = makeTypedGraph("exec", "float3");
        GraphLinkCheck viaDefault = canConnectPins(g, "src", "out", "dst", "in");
        check(!viaDefault.ok && viaDefault.reason == GraphLinkReject::TypeMismatch,
              "exec -> float3 refused under the default (exact-match) predicate");
        GraphLinkCheck viaMaterial = canConnectPins(g, "src", "out", "dst", "in", materialPinTypeMatch);
        check(!viaMaterial.ok && viaMaterial.reason == GraphLinkReject::TypeMismatch,
              "exec -> float3 STILL refused under materialPinTypeMatch");

        fmt::OcGraphData g2 = makeTypedGraph("float3", "exec");
        GraphLinkCheck reverseDefault = canConnectPins(g2, "src", "out", "dst", "in");
        check(!reverseDefault.ok && reverseDefault.reason == GraphLinkReject::TypeMismatch,
              "float3 -> exec refused under the default predicate");
        GraphLinkCheck reverseMaterial = canConnectPins(g2, "src", "out", "dst", "in", materialPinTypeMatch);
        check(!reverseMaterial.ok && reverseMaterial.reason == GraphLinkReject::TypeMismatch,
              "float3 -> exec STILL refused under materialPinTypeMatch");
    }

    // float -> float3: refused by the default predicate (today's exact-equality behaviour, unchanged
    // by this whole feature existing), ALLOWED by materialPinTypeMatch (a scalar splats), matching
    // widen()'s "((float3)(x))" cast exactly.
    {
        fmt::OcGraphData g = makeTypedGraph("float", "float3");
        GraphLinkCheck viaDefault = canConnectPins(g, "src", "out", "dst", "in");
        check(!viaDefault.ok && viaDefault.reason == GraphLinkReject::TypeMismatch,
              "float -> float3 refused by the default predicate");
        GraphLinkCheck viaMaterial = canConnectPins(g, "src", "out", "dst", "in", materialPinTypeMatch);
        check(viaMaterial.ok, "float -> float3 ALLOWED by materialPinTypeMatch (scalar splat)");
    }

    // float4 -> float3: allowed by materialPinTypeMatch (a wider vector truncates), matching widen()'s
    // ".xyz" swizzle. Also refused by the default predicate, for the same reason as float -> float3
    // above -- exact equality does not know about width at all.
    {
        fmt::OcGraphData g = makeTypedGraph("float4", "float3");
        GraphLinkCheck viaDefault = canConnectPins(g, "src", "out", "dst", "in");
        check(!viaDefault.ok && viaDefault.reason == GraphLinkReject::TypeMismatch,
              "float4 -> float3 refused by the default predicate");
        GraphLinkCheck viaMaterial = canConnectPins(g, "src", "out", "dst", "in", materialPinTypeMatch);
        check(viaMaterial.ok, "float4 -> float3 ALLOWED by materialPinTypeMatch (vector truncation)");
    }

    // float2 -> float3: refused by BOTH predicates. This is widen()'s own deliberate asymmetry --
    // inventing the third component is the compiler guessing, and it would guess zero -- so the
    // editor must keep refusing it even once the material predicate is in play, not just the default.
    {
        fmt::OcGraphData g = makeTypedGraph("float2", "float3");
        GraphLinkCheck viaDefault = canConnectPins(g, "src", "out", "dst", "in");
        check(!viaDefault.ok && viaDefault.reason == GraphLinkReject::TypeMismatch,
              "float2 -> float3 refused by the default predicate");
        GraphLinkCheck viaMaterial = canConnectPins(g, "src", "out", "dst", "in", materialPinTypeMatch);
        check(!viaMaterial.ok && viaMaterial.reason == GraphLinkReject::TypeMismatch,
              "float2 -> float3 STILL refused by materialPinTypeMatch (widening a float2 is refused)");
    }

    // The rejection message still names the two pin types, regardless of which predicate refused the
    // link -- a caller reporting the rejection to a user should not need to know which predicate ran.
    {
        fmt::OcGraphData g = makeTypedGraph("float2", "float3");
        GraphLinkCheck r = canConnectPins(g, "src", "out", "dst", "in", materialPinTypeMatch);
        check(r.message.find("float2") != std::string::npos && r.message.find("float3") != std::string::npos,
              "the rejection message names both pin types ('float2' and 'float3')");
    }

    // A same-width connection (float3 -> float3) is accepted under both predicates -- the ordinary
    // case is unaffected either way, exact equality having already been satisfied before either
    // predicate reaches its promotion rules.
    {
        fmt::OcGraphData g = makeTypedGraph("float3", "float3");
        GraphLinkCheck viaDefault = canConnectPins(g, "src", "out", "dst", "in");
        check(viaDefault.ok, "float3 -> float3 accepted by the default predicate");
        fmt::OcGraphData g2 = makeTypedGraph("float3", "float3");
        GraphLinkCheck viaMaterial = canConnectPins(g2, "src", "out", "dst", "in", materialPinTypeMatch);
        check(viaMaterial.ok, "float3 -> float3 accepted by materialPinTypeMatch too");
    }
}

// =================================================================================== cycle rules ===
static void testCycleDetection() {
    AVER_INFO("=== cycle detection ===");

    // Chain: a -> b -> c (a.result feeds b.a, b.result feeds c.a).
    fmt::OcGraphData g;
    for (const char* id : {"a", "b", "c"}) {
        fmt::OcGraphNode n;
        n.id = id; n.type = "Add"; n.x = 0; n.y = 0;
        n.pins.push_back({"a", "float", false, ""});
        n.pins.push_back({"b", "float", false, ""});
        n.pins.push_back({"result", "float", true, ""});
        g.nodes.push_back(n);
    }
    g.links.push_back({"a", "result", "b", "a"});
    g.links.push_back({"b", "result", "c", "a"});

    // Direct checks on wouldCreateCycle() itself, ahead of the pin-level check that wraps it.
    // a -> c directly: "c" (the would-be dest) has no outgoing edges yet, so it cannot reach "a"
    // (the would-be source) -- not a cycle.
    check(!wouldCreateCycle(g, "a", "c"), "wouldCreateCycle(a, c) is false: c cannot yet reach a");
    // c -> a: "a" (the would-be dest) CAN already reach "c" (the would-be source) via a -> b -> c --
    // adding c -> a would close that loop.
    check(wouldCreateCycle(g, "c", "a"), "wouldCreateCycle(c, a) is true: a already reaches c via a->b->c");

    // c.result -> a.b would close the loop a -> b -> c -> a: refused via the pin-level check too.
    GraphLinkCheck closeLoop = canConnectPins(g, "c", "result", "a", "b");
    check(!closeLoop.ok && closeLoop.reason == GraphLinkReject::WouldCreateCycle, "closing a -> b -> c -> a is refused as a cycle");

    // A link that does NOT close a cycle (e.g. a second independent edge a -> c) is accepted.
    GraphLinkCheck noLoop = canConnectPins(g, "a", "result", "c", "b");
    check(noLoop.ok, "a non-cyclic extra link (a -> c) is accepted");

    // tryAddLink actually mutates the graph on success and leaves it untouched on rejection.
    fmt::OcGraphData g2 = g;
    const usize before = g2.links.size();
    GraphLinkCheck rejected = tryAddLink(g2, "c", "result", "a", "b");
    check(!rejected.ok && g2.links.size() == before, "tryAddLink leaves the graph unchanged when the link is refused");
    GraphLinkCheck accepted = tryAddLink(g2, "a", "result", "b", "b");
    check(accepted.ok && g2.links.size() == before + 1, "tryAddLink appends the link when it is accepted");
}

// ================================================================================= canvas transform =
static void testCanvasTransform() {
    AVER_INFO("=== canvas transform (pan/zoom) ===");

    const f32 zooms[] = {0.25f, 0.5f, 1.0f, 1.75f, 4.0f};
    const Vec2 pans[] = {Vec2(0, 0), Vec2(120, -80), Vec2(-500, 40)};
    const Vec2 points[] = {Vec2(0, 0), Vec2(100, 50), Vec2(-300, 275.5f), Vec2(12345, -6789)};

    for (f32 zoom : zooms) {
        for (Vec2 pan : pans) {
            CanvasTransform t;
            t.zoom = zoom;
            t.panPx = pan;
            for (Vec2 p : points) {
                const Vec2 screen = canvasToScreen(t, p);
                const Vec2 back = screenToCanvas(t, screen);
                const f32 err = std::sqrt((back.x - p.x) * (back.x - p.x) + (back.y - p.y) * (back.y - p.y));
                check(err < 0.01f,
                      "round-trip canvas->screen->canvas stable at zoom=" + std::to_string(zoom)
                      + " pan=(" + std::to_string(pan.x) + "," + std::to_string(pan.y) + ")"
                      + " point=(" + std::to_string(p.x) + "," + std::to_string(p.y) + ") err=" + std::to_string(err));
            }
        }
    }

    // Zoom-to-cursor: the canvas point under a fixed screen pivot must stay exactly under it after
    // the zoom changes -- the classic "zoom drifts" bug this function exists to prevent.
    CanvasTransform t;
    t.zoom = 1.0f;
    t.panPx = Vec2(50, 30);
    const Vec2 pivot(400.0f, 300.0f);
    const Vec2 canvasBefore = screenToCanvas(t, pivot);
    for (f32 newZoom : {0.3f, 0.7f, 1.0f, 2.5f, 6.0f}) {
        CanvasTransform t2 = zoomAroundScreenPoint(t, newZoom, pivot);
        const Vec2 canvasAfter = screenToCanvas(t2, pivot);
        const f32 err = std::sqrt((canvasAfter.x - canvasBefore.x) * (canvasAfter.x - canvasBefore.x)
                                   + (canvasAfter.y - canvasBefore.y) * (canvasAfter.y - canvasBefore.y));
        check(err < 0.01f, "zoomAroundScreenPoint keeps the pivot's canvas point fixed at newZoom=" + std::to_string(newZoom));
    }
}

// ======================================================================================= framing ===
static void testFrameTransform() {
    AVER_INFO("=== frameTransform (fit content into a viewport) ===");

    const Vec2 viewport(1000.0f, 600.0f);
    const f32 pad = 40.0f;

    // ---- the ordinary case: content larger than the viewport, so the zoom is the real fit.
    {
        const Vec2 lo(0.0f, 0.0f), hi(4000.0f, 1200.0f);
        const CanvasTransform t = frameTransform(lo, hi, viewport, pad, 0.15f, 4.0f);
        const Vec2 sLo = canvasToScreen(t, lo);
        const Vec2 sHi = canvasToScreen(t, hi);
        check(sLo.x >= pad - 0.5f && sLo.y >= pad - 0.5f,
              "the top-left of the content lands inside the padding");
        check(sHi.x <= viewport.x - pad + 0.5f && sHi.y <= viewport.y - pad + 0.5f,
              "the bottom-right of the content lands inside the padding");
        // Centred, which is the property a naive pan gets wrong on the axis that is NOT the
        // limiting one -- here the content is much wider than tall, so y has slack to get wrong.
        const f32 cx = (sLo.x + sHi.x) * 0.5f, cy = (sLo.y + sHi.y) * 0.5f;
        check(std::abs(cx - viewport.x * 0.5f) < 0.5f, "content is horizontally centred");
        check(std::abs(cy - viewport.y * 0.5f) < 0.5f, "content is vertically centred on the slack axis");
    }

    // ---- THE CLAMP CASE, and the reason this function exists. Three small nodes want a zoom far
    // above the ceiling; the zoom must clamp AND the content must still be centred. A naive
    // implementation that centres from the unclamped zoom leaves the content in a corner.
    {
        const Vec2 lo(0.0f, 0.0f), hi(60.0f, 40.0f);
        const CanvasTransform t = frameTransform(lo, hi, viewport, pad, 0.15f, 4.0f);
        check(std::abs(t.zoom - 4.0f) < 0.001f, "a tiny graph clamps to the maximum zoom, not past it");
        const Vec2 sLo = canvasToScreen(t, lo), sHi = canvasToScreen(t, hi);
        const f32 cx = (sLo.x + sHi.x) * 0.5f, cy = (sLo.y + sHi.y) * 0.5f;
        check(std::abs(cx - viewport.x * 0.5f) < 0.5f && std::abs(cy - viewport.y * 0.5f) < 0.5f,
              "...and the content is STILL centred after the clamp bit");
    }
    {
        // The other end: content so large the fit falls under the floor. Same requirement.
        const Vec2 lo(-50000.0f, -50000.0f), hi(50000.0f, 50000.0f);
        const CanvasTransform t = frameTransform(lo, hi, viewport, pad, 0.15f, 4.0f);
        check(std::abs(t.zoom - 0.15f) < 0.001f, "a vast graph clamps to the minimum zoom");
        const Vec2 sLo = canvasToScreen(t, lo), sHi = canvasToScreen(t, hi);
        const f32 cx = (sLo.x + sHi.x) * 0.5f, cy = (sLo.y + sHi.y) * 0.5f;
        check(std::abs(cx - viewport.x * 0.5f) < 0.5f && std::abs(cy - viewport.y * 0.5f) < 0.5f,
              "...and it is centred rather than pinned to a corner");
    }

    // ---- negative coordinates. A comment box drawn up and to the left of the nodes puts the
    // content bounds in negative canvas space, which is ordinary and must not need a special case.
    {
        const Vec2 lo(-900.0f, -400.0f), hi(-100.0f, -50.0f);
        const CanvasTransform t = frameTransform(lo, hi, viewport, pad, 0.15f, 4.0f);
        const Vec2 sLo = canvasToScreen(t, lo), sHi = canvasToScreen(t, hi);
        check(sLo.x >= pad - 0.5f && sHi.x <= viewport.x - pad + 0.5f,
              "content entirely in negative canvas space still frames on screen");
    }

    // ---- DEGENERATE INPUTS. Each axis falls back independently; the failure being guarded against
    // is a NaN pan, which does not throw -- it silently blanks the canvas and looks like data loss.
    {
        const CanvasTransform flat = frameTransform(Vec2(10.0f, 10.0f), Vec2(410.0f, 10.0f), viewport, pad, 0.15f, 4.0f);
        check(std::isfinite(flat.panPx.x) && std::isfinite(flat.panPx.y) && std::isfinite(flat.zoom),
              "a zero-HEIGHT content box produces a finite transform");
        const CanvasTransform point = frameTransform(Vec2(7.0f, 7.0f), Vec2(7.0f, 7.0f), viewport, pad, 0.15f, 4.0f);
        check(std::isfinite(point.panPx.x) && std::isfinite(point.panPx.y) && std::isfinite(point.zoom),
              "a single-POINT content box produces a finite transform");
        const Vec2 sp = canvasToScreen(point, Vec2(7.0f, 7.0f));
        check(std::abs(sp.x - viewport.x * 0.5f) < 0.5f && std::abs(sp.y - viewport.y * 0.5f) < 0.5f,
              "...and puts that point in the middle of the screen, which is the only sane answer");
        const CanvasTransform tiny = frameTransform(Vec2(0.0f, 0.0f), Vec2(400.0f, 300.0f), Vec2(4.0f, 4.0f), pad, 0.15f, 4.0f);
        check(std::isfinite(tiny.zoom) && tiny.zoom >= 0.15f,
              "a viewport smaller than its own padding does not divide by zero");
    }
}

// ==================================================================================== auto-layout ===
static void testAutoLayout() {
    AVER_INFO("=== auto-layout ===");

    // A chain: c1, c2 (no incoming links) both feed sum -- mirrors the shape of the checked-in
    // cross-impl fixture (scripting/csharp/Aver.Graph.Tests/cross_impl_test.ocgraph), but with every
    // position left at the origin -- the shape a hand-written file has when nobody set x/y.
    fmt::OcGraphData g;
    auto addConst = [&](const char* id) {
        fmt::OcGraphNode n; n.id = id; n.type = "ConstFloat"; n.x = 0; n.y = 0;
        n.pins.push_back({"value", "float", true, "1"});
        g.nodes.push_back(n);
    };
    addConst("c1");
    addConst("c2");
    fmt::OcGraphNode sum;
    sum.id = "sum"; sum.type = "Add"; sum.x = 0; sum.y = 0;
    sum.pins.push_back({"a", "float", false, ""});
    sum.pins.push_back({"b", "float", false, ""});
    sum.pins.push_back({"result", "float", true, ""});
    g.nodes.push_back(sum);
    g.links.push_back({"c1", "value", "sum", "a"});
    g.links.push_back({"c2", "value", "sum", "b"});

    const fmt::OcGraphData before = g; // deep copy, to check autoLayoutPositions mutates nothing
    GraphLayoutStyle style;
    const auto pos = autoLayoutPositions(g, style, 1.0f);

    check(pos.size() == g.nodes.size(), "auto-layout places every node exactly once");
    check(pos.count("c1") != 0 && pos.count("c2") != 0 && pos.count("sum") != 0, "every node id is present in the result");
    if (pos.count("c1") && pos.count("sum")) {
        check(pos.at("sum").x > pos.at("c1").x, "a downstream node lands in a later column than its source");
    }
    if (pos.count("c1") && pos.count("c2")) {
        check(pos.at("c1").x == pos.at("c2").x, "two nodes with no incoming links share the first column");
        check(pos.at("c1").y != pos.at("c2").y, "two nodes in the same column get distinct rows");
    }

    // MUST NOT MUTATE the graph it's given. This is the load-bearing property GraphEditor.cpp relies
    // on: it feeds auto-layout results into a DISPLAY-only position map, never into the OcGraphData
    // itself, specifically so a load with no further edits followed immediately by a save stays
    // byte-identical even when auto-layout ran (see GraphEditorGeometry.hpp's doc comment on
    // autoLayoutPositions for the full reasoning). If this function ever starts writing into node.x/
    // node.y, that guarantee silently breaks -- so it's checked here, not just asserted in a comment.
    check(g.nodes.size() == before.nodes.size(), "autoLayoutPositions does not add or remove nodes");
    for (usize i = 0; i < g.nodes.size() && i < before.nodes.size(); ++i) {
        check(g.nodes[i].x == before.nodes[i].x && g.nodes[i].y == before.nodes[i].y,
              "autoLayoutPositions leaves node '" + g.nodes[i].id + "'s stored x/y untouched");
    }
    check(g.links.size() == before.links.size(), "autoLayoutPositions does not add or remove links");

    // A cycle must not hang the layering pass. The editor's own connect rules (canConnectPins /
    // wouldCreateCycle, tested above) refuse to CREATE one, but nothing stops a hand-edited file from
    // already containing one, and auto-layout runs on load, before any connect rule gets a say.
    fmt::OcGraphData cyc;
    for (const char* id : {"a", "b"}) {
        fmt::OcGraphNode n; n.id = id; n.type = "Add"; n.x = 0; n.y = 0;
        n.pins.push_back({"a", "float", false, ""});
        n.pins.push_back({"result", "float", true, ""});
        cyc.nodes.push_back(n);
    }
    cyc.links.push_back({"a", "result", "b", "a"});
    cyc.links.push_back({"b", "result", "a", "a"});
    const auto cycPos = autoLayoutPositions(cyc, style, 1.0f);
    check(cycPos.size() == 2, "a 2-node cycle still gets both nodes placed (layering terminates, does not hang)");

    // Empty graph: no crash, empty result.
    fmt::OcGraphData empty;
    const auto emptyPos = autoLayoutPositions(empty, style, 1.0f);
    check(emptyPos.empty(), "an empty graph produces an empty layout with no crash");
}

// ============================================================================= node attributes (Gap B) =
// The pure, headless half of the node-attribute authoring surface: reading/writing a NODE line's
// key=value extraTokens, and composing the rows a details panel would show. GraphEditorLoadSaveTest
// covers the same functions again end to end (through GraphEditor's real save() path, to a real file on
// disk); these tests isolate the model itself, one property at a time, with no file I/O at all.
static void testNodeAttributeCatalog() {
    AVER_INFO("=== node attribute catalog (GraphNodeDesc::attributes) ===");

    // The six types an earlier task brief named (param=/field=/field=/field=/field=/class=), plus
    // GetVar/SetVar's own var= -- graph-local persistent variables, added later.
    struct Expect { const char* type; const char* key; };
    const Expect expected[] = {
        {"Param", "param"}, {"GetField", "field"}, {"SetField", "field"},
        {"GetFieldVec3", "field"}, {"SetFieldVec3", "field"}, {"Spawn", "class"},
        {"GetVar", "var"}, {"SetVar", "var"},
    };
    for (const auto& e : expected) {
        const GraphNodeDesc* d = findGraphNodeDesc(e.type);
        if (!d) { check(false, std::string(e.type) + " present (attribute check skipped)"); continue; }
        check(d->attributes.size() == 1 && d->attributes[0].key == e.key,
              std::string(e.type) + " declares exactly one attribute, key='" + e.key + "'");
        check(!d->attributes[0].label.empty(), std::string(e.type) + "'s attribute has a non-empty display label");
    }

    // A type with no attributes at all declares an empty list, not a list with an empty entry --
    // computeAttributeRows below relies on this to draw nothing extra for a plain Add node.
    //
    // ConstFloat WAS ON THIS LIST, and it was pinning the defect rather than the design. Declaring no
    // attributes is exactly what made a spawned Const uneditable: the details panel renders a row per
    // declared attribute, so it showed "This node type has no attributes" and there was nowhere to
    // type a number. Every constant in an editor-built graph compiled to 0 forever. The Const types
    // are asserted the other way now, immediately below.
    const char* noAttrs[] = {"Add", "Branch", "OnTick", "MouseDelta", "MoveAxis"};
    for (const char* t : noAttrs) {
        const GraphNodeDesc* d = findGraphNodeDesc(t);
        if (d) check(d->attributes.empty(), std::string(t) + " declares no attributes");
        else check(false, std::string(t) + " present (attribute check skipped)");
    }

    // ...and every Const type MUST declare `value`, or its literal is unauthorable from the editor.
    // Named `value` exactly, because that is the key OcGraphParser.cs reads and parses BY THE NODE'S
    // DECLARED TYPE -- a different spelling would write an attribute nothing reads, which looks like
    // it works right up until the graph runs.
    for (const char* t : {"ConstFloat", "ConstInt", "ConstBool"}) {
        const GraphNodeDesc* d = findGraphNodeDesc(t);
        if (!d) { check(false, std::string(t) + " present"); continue; }
        bool hasValue = false;
        for (const GraphAttributeSpec& a : d->attributes) if (a.key == "value") hasValue = true;
        check(hasValue, std::string(t) + " declares a 'value' attribute so its literal can be typed");
    }
}

static void testNodeAttributeReadWrite() {
    AVER_INFO("=== getNodeAttribute / setNodeAttribute / removeNodeAttribute ===");

    // Absent key: found == false, value == "".
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        const GraphNodeAttribute a = getNodeAttribute(n, "class");
        check(!a.found && a.value.empty(), "getNodeAttribute on a node with no extraTokens: not found, empty value");
    }

    // Present key, found and value read back correctly.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"class=Widget"};
        const GraphNodeAttribute a = getNodeAttribute(n, "class");
        check(a.found && a.value == "Widget", "getNodeAttribute reads a present key=value token");
    }

    // A value containing '=' is preserved WHOLE -- only the FIRST '=' delimits key from value, matching
    // OcGraphParser.cs's own `token.Split('=', 2)` contract exactly.
    {
        fmt::OcGraphNode n; n.id = "gv"; n.type = "GetFieldVec3";
        n.extraTokens = {"field=Some.Weird==Path"};
        const GraphNodeAttribute a = getNodeAttribute(n, "field");
        check(a.found && a.value == "Some.Weird==Path",
              "a value containing '=' round-trips whole: only the first '=' in the token is the delimiter");
    }

    // setNodeAttribute: new key is APPENDED, existing tokens keep their exact order.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"debugLabel=marker"};
        setNodeAttribute(n, "class", "Widget");
        check(n.extraTokens.size() == 2 && n.extraTokens[0] == "debugLabel=marker" && n.extraTokens[1] == "class=Widget",
              "setNodeAttribute appends a new key after every existing token, none reordered");
    }

    // setNodeAttribute: EXISTING key is updated IN PLACE, neighbours untouched -- the core "an edit to
    // one attribute must not disturb another" guarantee.
    {
        fmt::OcGraphNode n; n.id = "gv"; n.type = "GetFieldVec3";
        n.extraTokens = {"zzz_unknown=hello", "field=CLocal.position", "another_unknown=42"};
        setNodeAttribute(n, "field", "CLight.colour");
        check(n.extraTokens.size() == 3, "setNodeAttribute on an existing key does not change the token COUNT");
        check(n.extraTokens[0] == "zzz_unknown=hello", "the token BEFORE the edited one is untouched");
        check(n.extraTokens[1] == "field=CLight.colour", "the edited token is updated in place, same position");
        check(n.extraTokens[2] == "another_unknown=42", "the token AFTER the edited one is untouched");
    }

    // A no-'=' token (a bare flag, not a key=value attribute at all) is never a match and is never
    // touched by setNodeAttribute -- it isn't even representable as an attribute, so nothing here can
    // corrupt it.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"someBareFlag", "class=Old"};
        setNodeAttribute(n, "class", "New");
        check(n.extraTokens.size() == 2 && n.extraTokens[0] == "someBareFlag" && n.extraTokens[1] == "class=New",
              "a bare (no '=') token is left exactly where it was");
        check(!getNodeAttribute(n, "someBareFlag").found, "a bare token is never readable as an attribute (it has no value half)");
    }

    // removeNodeAttribute: deletes exactly the matching token, leaves everything else, no-op if absent.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"debugLabel=marker", "class=Widget"};
        removeNodeAttribute(n, "class");
        check(n.extraTokens.size() == 1 && n.extraTokens[0] == "debugLabel=marker",
              "removeNodeAttribute deletes exactly the matching token, nothing else");
        removeNodeAttribute(n, "class"); // already absent
        check(n.extraTokens.size() == 1, "removeNodeAttribute on an absent key is a no-op, not a crash or a wrong deletion");
    }
}

static void testComputeAttributeRows() {
    AVER_INFO("=== computeAttributeRows ===");

    // Declared-but-absent: the row still appears, present == false, value == "" -- so an author sees
    // 'class' is expected on a freshly-spawned Spawn node before typing anything into it.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        const auto rows = computeAttributeRows(n, {{"class", "Class"}});
        check(rows.size() == 1, "one declared attribute produces exactly one row, even when absent");
        if (!rows.empty()) {
            check(rows[0].key == "class" && rows[0].label == "Class" && !rows[0].present && rows[0].value.empty() && rows[0].declared,
                  "the absent declared row: key/label set, present=false, value empty, declared=true");
        }
    }

    // Declared-and-present: value/present reflect the real token.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"class=Widget"};
        const auto rows = computeAttributeRows(n, {{"class", "Class"}});
        check(rows.size() == 1 && rows[0].present && rows[0].value == "Widget", "a present declared attribute reports its real value");
    }

    // A leftover key=value token the declared list doesn't name still produces a row (declared=false),
    // in file order, AFTER the declared rows -- so it stays visible and editable, not opaque.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"class=Widget", "debugLabel=marker"};
        const auto rows = computeAttributeRows(n, {{"class", "Class"}});
        check(rows.size() == 2, "one declared row plus one leftover row");
        if (rows.size() == 2) {
            check(rows[0].key == "class" && rows[0].declared, "declared row comes first");
            check(rows[1].key == "debugLabel" && rows[1].value == "marker" && !rows[1].declared && rows[1].present,
                  "the leftover row carries the uncatalogued key, its value, and declared=false");
        }
    }

    // A bare (no '=') token produces NO row at all -- it cannot be labelled, and (per
    // testNodeAttributeReadWrite above) is never touched by an edit through these rows either.
    {
        fmt::OcGraphNode n; n.id = "s"; n.type = "Spawn";
        n.extraTokens = {"someBareFlag"};
        const auto rows = computeAttributeRows(n, {});
        check(rows.empty(), "a bare token with no declared attributes produces zero rows");
    }

    // An unrecognised node type (no declared attributes at all, the caller passes an empty list) still
    // surfaces its own leftover tokens as generic rows -- computeAttributeRows never needs to know
    // whether the TYPE was known, only what the node's own extraTokens say.
    {
        fmt::OcGraphNode n; n.id = "weird"; n.type = "SomeFutureNodeType";
        n.extraTokens = {"future_attr=123"};
        const auto rows = computeAttributeRows(n, {});
        check(rows.size() == 1 && rows[0].key == "future_attr" && !rows[0].declared,
              "an unrecognised node type's own key=value extraTokens still show up as generic rows");
    }
}

// THE PALETTE AND THE COMPILER MUST KNOW THE SAME NODES, and until this test existed nothing checked
// it. GraphNodeDefs.hpp is a deliberate hand-maintained copy of the vocabulary that
// scripting/csharp/Aver.Graph/OcGraphParser.cs owns -- deliberate because a C++ editor build must not
// depend on a C# file (see GraphNodeDefs.hpp's own header). The cost of that choice is that adding a
// node type to the compiler and forgetting the palette produces a node the runtime fully supports and
// the Add-Node menu has never heard of, authorable only by hand-editing .ocgraph text. That is not
// hypothetical: Select, InputKey and Raycast landed in 1425b67 and were missing from the palette for
// two further slices, noticed only when MouseDelta and MoveAxis were added beside them and the Input
// category would have shown the mouse but not the keyboard.
//
// So this reads the .cs as TEXT and extracts its `case "name":` labels. That is a blunt instrument --
// it would miss a node the parser handles some other way -- but it is the only thing available that
// can fail when someone updates one side and not the other, and a blunt check that fires beats an
// elegant one that does not exist.
static void testNodeCatalogCoversTheCompiler() {
    AVER_INFO("=== palette vs compiler vocabulary ===");
    const std::string parser = std::string(AVER_REPO_ROOT) + "/scripting/csharp/Aver.Graph/OcGraphParser.cs";
    std::ifstream in(parser);
    if (!in) {
        // Not silently skipped: a test that cannot find its input must say so, or it passes forever.
        check(false, "could not open OcGraphParser.cs -- this parity test cannot run");
        return;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // The node types AddDefaultPins switches on. Its `case "x":` labels are all lowercase.
    std::vector<std::string> fromCompiler;
    for (usize i = 0; (i = text.find("case \"", i)) != std::string::npos; ) {
        const usize b = i + 6;
        const usize e = text.find('"', b);
        if (e == std::string::npos) break;
        const std::string name = text.substr(b, e - b);
        i = e;
        if (name.empty()) continue;
        bool lower = true;
        for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) lower = false;
        if (lower) fromCompiler.push_back(name);
    }
    check(fromCompiler.size() > 15,
          "extracted a plausible node vocabulary from OcGraphParser.cs (guards against the scrape "
          "silently matching nothing and the test passing vacuously)");

    const std::vector<aver::editor::GraphNodeDesc> cat = aver::editor::graphNodeCatalog();
    for (const std::string& want : fromCompiler) {
        // Aliases the parser accepts for one palette entry, and the two records that are not node
        // types at all. Listed rather than pattern-matched so adding one is a deliberate act.
        if (want == "sub" || want == "div" || want == "getparam") continue;
        bool found = false;
        for (const aver::editor::GraphNodeDesc& d : cat) {
            std::string lower;
            for (char c : d.typeId) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            if (lower == want) { found = true; break; }
        }
        check(found, "the Add-Node palette offers '" + want + "', which the compiler supports");
    }
}

int main() {
    AVER_INFO("======== GraphEditorGeometryTest ========");
    testNodeCatalog();
    testLayout();
    testHitTest();
    testLinkRules();
    testPluggablePinTypeChecks();
    testCycleDetection();
    testCanvasTransform();
    testFrameTransform();
    testAutoLayout();
    testNodeAttributeCatalog();
    testNodeAttributeReadWrite();
    testComputeAttributeRows();
    testNodeCatalogCoversTheCompiler();
    AVER_INFO("======== {} failure(s) ========", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
