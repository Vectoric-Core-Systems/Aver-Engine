// .ocgraph format test. Covers round-tripping, unknown records, and deterministic output.
// Also tests malformed input rejection.
#include "aver/formats/OcGraph.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <string>

using namespace aver;

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

// Tests basic parsing and data structure population.
static void testBasicParse() {
    AVER_INFO("=== .ocgraph basic parsing ===");
    using namespace fmt;

    const std::string graph =
        "OCGRAPH 1\n"
        "NAME TestGraph\n"
        "DESCRIPTION A simple test graph\n"
        "NODE add Add 100 50\n"
        "NODE const Constant 0 0\n"
        "PIN add value in float 0.0\n"
        "PIN add result out float\n"
        "PIN const value out float 5.0\n"
        "LINK const.value add.value\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(graph, g, &err), "basic graph parses");
    check(g.version == 1, "version is 1");
    check(g.name == "TestGraph", "name is parsed");
    check(g.description == "A simple test graph", "description is parsed");
    check(g.nodes.size() == 2, "has 2 nodes");
    check(g.links.size() == 1, "has 1 link");

    if (g.nodes.size() >= 1) {
        check(g.nodes[0].id == "add", "first node ID is 'add'");
        check(g.nodes[0].type == "Add", "first node type is 'Add'");
        check(std::fabs(g.nodes[0].x - 100.0) < 1e-6, "first node x position is 100");
        check(std::fabs(g.nodes[0].y - 50.0) < 1e-6, "first node y position is 50");
    }

    if (g.nodes.size() >= 2) {
        check(g.nodes[1].id == "const", "second node ID is 'const'");
        check(g.nodes[1].type == "Constant", "second node type is 'Constant'");
    }

    if (g.nodes.size() >= 1 && g.nodes[0].pins.size() >= 1) {
        const auto& pin = g.nodes[0].pins[0];
        check(pin.name == "value", "pin name is 'value'");
        check(pin.type == "float", "pin type is 'float'");
        check(!pin.isOutput, "value is an input pin");
        check(pin.defaultValue == "0.0", "input default is '0.0'");
    }

    if (g.links.size() >= 1) {
        check(g.links[0].sourceNode == "const", "link source node is 'const'");
        check(g.links[0].sourcePin == "value", "link source pin is 'value'");
        check(g.links[0].destNode == "add", "link dest node is 'add'");
        check(g.links[0].destPin == "value", "link dest pin is 'value'");
    }
}

// Tests that a graph round-trips exactly: parse -> write -> parse -> write must be bit-identical.
static void testRoundTrip() {
    AVER_INFO("=== .ocgraph round-trip ===");
    using namespace fmt;

    OcGraphData g;
    g.version = 1;
    g.name = "RoundTrip";
    g.description = "Test round-trip";

    // Add some nodes.
    OcGraphNode n1;
    n1.id = "node1";
    n1.type = "Add";
    n1.x = 100.5;
    n1.y = 50.25;
    OcGraphPin p1;
    p1.name = "a";
    p1.type = "float";
    p1.isOutput = false;
    p1.defaultValue = "1.0";
    n1.pins.push_back(p1);
    OcGraphPin p2;
    p2.name = "b";
    p2.type = "float";
    p2.isOutput = false;
    p2.defaultValue = "2.0";
    n1.pins.push_back(p2);
    OcGraphPin p3;
    p3.name = "result";
    p3.type = "float";
    p3.isOutput = true;
    n1.pins.push_back(p3);
    g.nodes.push_back(n1);

    OcGraphNode n2;
    n2.id = "node2";
    n2.type = "Multiply";
    n2.x = 250.0;
    n2.y = 75.5;
    OcGraphPin p4;
    p4.name = "factor";
    p4.type = "float";
    p4.isOutput = false;
    p4.defaultValue = "2.0";
    n2.pins.push_back(p4);
    OcGraphPin p5;
    p5.name = "out";
    p5.type = "float";
    p5.isOutput = true;
    n2.pins.push_back(p5);
    g.nodes.push_back(n2);

    // Add a link.
    OcGraphLink l;
    l.sourceNode = "node1";
    l.sourcePin = "result";
    l.destNode = "node2";
    l.destPin = "factor";
    g.links.push_back(l);

    // Write it.
    std::string text1 = writeOcgraph(g);
    check(!text1.empty(), "write produces non-empty text");

    // Parse it back.
    OcGraphData g2;
    std::string err;
    check(parseOcgraph(text1, g2, &err), "parsed output round-trips");

    // Write it again.
    std::string text2 = writeOcgraph(g2);
    check(text1 == text2, "second write reproduces the first byte for byte");

    // Verify structure survived round-trip.
    check(g2.name == "RoundTrip", "name survived round-trip");
    check(g2.nodes.size() == 2, "nodes survived round-trip");
    check(g2.links.size() == 1, "links survived round-trip");
    if (g2.nodes.size() >= 1) {
        check(std::fabs(g2.nodes[0].x - 100.5) < 1e-6, "node position x survived round-trip");
        check(std::fabs(g2.nodes[0].y - 50.25) < 1e-6, "node position y survived round-trip");
    }
}

// Tests that unknown records are preserved through a round-trip.
static void testUnknownRecords() {
    AVER_INFO("=== .ocgraph unknown records ===");
    using namespace fmt;

    // Original graph with an unknown record.
    const std::string original =
        "OCGRAPH 1\n"
        "NAME Original\n"
        "NODE a Add 10 20\n"
        "PIN a x in float 0.0\n"
        "# This is a comment, should survive\n"
        "MYSTERY extra data here\n"
        "LINK a.x a.x\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(original, g, &err), "graph with unknown record parses");
    check(g.name == "Original", "known fields are parsed");

    // Rewrite it using the original as context.
    const std::string rewritten = writeOcgraph(g, original);
    check(rewritten.find("MYSTERY extra data here") != std::string::npos,
          "unknown record 'MYSTERY' survives a write");
    check(rewritten.find("# This is a comment") != std::string::npos,
          "comment survives a write");

    // Parse the rewritten version.
    OcGraphData g2;
    check(parseOcgraph(rewritten, g2, &err), "rewritten graph parses");

    // Write again without the original context - unknown records should still be there.
    const std::string again = writeOcgraph(g2, rewritten);
    check(again.find("MYSTERY extra data here") != std::string::npos,
          "unknown records survive a second write");
    check(again == rewritten, "second rewrite of preserved data is bit-identical");
}

// Tests that VAR records (graph-local persistent variables, declared by the C# runtime's own
// OcGraphParser.cs -- see that file's VAR-parsing comment for the full storage/lifetime contract)
// both PARSE INTO REAL STRUCTURED FIELDS and survive a round trip.
//
// THIS USED TO BE A WEAKER CLAIM. VAR was originally invisible to this C++ reader entirely --
// classifyLine had no "Var" branch, so a VAR line fell into OwnedLineKind::Other and rode through a
// write unread, the same path a comment or an unrecognised record like MYSTERY (testUnknownRecords,
// above) takes. That was enough to prove the editor would not silently DESTROY a graph's variables on
// save, but g.variables did not exist at all: nothing in C++ could answer "what variables does this
// graph declare," which is the one question a Variables panel (declare/rename/retype/delete, and a
// var= picker on GetVar/SetVar) cannot avoid asking. OcGraphData::variables, classifyLine's own
// OwnedLineKind::Var, and writeOcgraph's placedVar/varBlock pair (OcGraph.cpp) are what closed that
// gap -- the identical shape ENTRY and OUT already established. This test now asserts BOTH halves:
// the structured fields a Variables panel would read, and the byte-identical round trip a save must
// still guarantee.
static void testVarRecordsSurviveRoundTrip() {
    AVER_INFO("=== .ocgraph VAR records (graph-local persistent variables) ===");
    using namespace fmt;

    const std::string original =
        "OCGRAPH 1\n"
        "NAME VarTest\n"
        "VAR score int 0\n"
        "VAR cooldown float 1.5\n"
        "NODE gv GetVar var=score\n"
        "OUT gv value\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(original, g, &err), "graph with VAR records parses");
    check(g.name == "VarTest", "known fields still parse alongside VAR records");

    // Structural proof: VAR is a real field now, not merely opaque text that happens to survive.
    check(g.variables.size() == 2, "both VAR records were parsed into OcGraphData::variables");
    if (g.variables.size() == 2) {
        check(g.variables[0].name == "score" && g.variables[0].type == "int" && g.variables[0].defaultValue == "0",
              "'score' parsed with name/type/default, in file order");
        check(g.variables[1].name == "cooldown" && g.variables[1].type == "float" && g.variables[1].defaultValue == "1.5",
              "'cooldown' parsed with name/type/default, in file order");
    }

    const std::string rewritten = writeOcgraph(g, original);
    check(rewritten.find("VAR score int 0") != std::string::npos, "VAR 'score' survives a write");
    check(rewritten.find("VAR cooldown float 1.5") != std::string::npos, "VAR 'cooldown' survives a write");
    check(rewritten.find("var=score") != std::string::npos,
          "GetVar's var= attribute survives a write (via the SAME extraTokens mechanism field=/param=/class= use)");
    check(rewritten == original, "a load -> save with no edits reproduces this VAR-bearing file byte for byte");

    OcGraphData g2;
    check(parseOcgraph(rewritten, g2, &err), "rewritten graph (with VAR records) parses");
    check(g2.variables.size() == 2, "the re-parsed graph still has both variables");
    const std::string again = writeOcgraph(g2, rewritten);
    check(again == rewritten, "second rewrite of VAR-bearing data is bit-identical");

    // A VAR with no default token at all -- entirely legal (OcGraphParser.cs falls back to the
    // type's own zero value when the 3rd token is absent); defaultValue must come back empty, not a
    // placeholder, and the written line must not grow a default nobody wrote.
    OcGraphData g3;
    check(parseOcgraph("OCGRAPH 1\nNAME NoDefault\nVAR flag bool\n", g3, &err), "a VAR with no default parses");
    check(g3.variables.size() == 1 && g3.variables[0].defaultValue.empty(),
          "a VAR with no 3rd token has an empty defaultValue, not an invented one");
    const std::string freshWrite = writeOcgraph(g3);
    check(freshWrite.find("VAR flag bool\n") != std::string::npos,
          "a fresh write (no `existing`) of a no-default VAR omits the default entirely, rather than writing a stray trailing space");
}

// Tests that output is deterministic: same data written twice produces identical output.
static void testForwardReferencedEntryAndOut() {
    AVER_INFO("=== ENTRY/OUT may name a node declared later in the file ===");
    using namespace fmt;

    // EVERY REAL GRAPH IN THIS REPO IS WRITTEN THIS WAY -- an ENTRY line reads as a heading over the
    // node it names, so it is written above it. This reader validated the reference inline, against
    // the nodes seen SO FAR, while the C# reader defers until the file is read; the divergence meant
    // the C++ node editor could not open test-content/AN_Playable's graphs, GraphDemo's IdleMotion,
    // or any FirstPerson template graph. It had been written down as a known gap rather than fixed,
    // which is exactly why it lasted.
    const std::string forward =
        "OCGRAPH 1\n"
        "ENTRY tick OnTick\n"
        "NODE tick OnTick\n"
        "OUT k value\n"
        "NODE k ConstFloat value=2.5\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(forward, g, &err), "an ENTRY above its own NODE parses");
    check(g.entryPoints.size() == 1 && g.entryPoints[0].first == "tick", "the forward ENTRY was captured");
    check(g.outputs.size() == 1 && g.outputs[0].first == "k", "the forward OUT was captured");

    // THE CHECK MOVED, IT DID NOT GO AWAY. A node id that never appears anywhere is still refused,
    // and the message still names it -- otherwise this fix would have traded a false rejection for a
    // silent acceptance, which is the worse of the two.
    OcGraphData bad;
    std::string badErr;
    check(!parseOcgraph("OCGRAPH 1\nENTRY ghost OnTick\nNODE real OnTick\n", bad, &badErr),
          "an ENTRY naming a node that never appears is still refused");
    check(badErr.find("ghost") != std::string::npos, "and the error names the missing node");

    OcGraphData bad2;
    std::string badErr2;
    check(!parseOcgraph("OCGRAPH 1\nNODE real OnTick\nOUT ghost value\n", bad2, &badErr2),
          "the same holds for OUT");
}

static void testInterleavedRecordsStayPut() {
    AVER_INFO("=== a save keeps every record where its author put it ===");
    using namespace fmt;

    // A HAND-WRITTEN GRAPH INTERLEAVES ITS RECORDS, in commented sections, each holding the nodes
    // and links that belong together. The merge used to emit a whole KIND at that kind's first line
    // and drop the rest, which hauled every NODE up to the first one -- so opening AN_Playable's
    // Rules.ocgraph and saving it unchanged moved `NODE tick OnTick` six lines up, away from the
    // comment written directly above it. Nothing was lost, which is exactly what made it dangerous:
    // the file still parsed and still ran, and no longer said what its author meant.
    const std::string original =
        "OCGRAPH 1\n"
        "NAME Interleaved\n"
        "\n"
        "# the first thing\n"
        "NODE a ConstFloat value=1.0\n"
        "\n"
        "# the second thing, which the comment above belongs to\n"
        "NODE b ConstFloat value=2.0\n"
        "LINK a.value b.x\n"
        "\n"
        "# and the third\n"
        "NODE c Add\n"
        "OUT c result\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(original, g, &err), "the interleaved graph parses");
    check(writeOcgraph(g, original) == original,
          "an edit-free save is byte-identical -- no record hauled up to its kind's first line");

    // A record REMOVED from the graph loses its line, and nothing else moves.
    OcGraphData minusB = g;
    minusB.nodes.erase(std::remove_if(minusB.nodes.begin(), minusB.nodes.end(),
                                      [](const OcGraphNode& n) { return n.id == "b"; }),
                       minusB.nodes.end());
    const std::string afterDelete = writeOcgraph(minusB, original);
    check(afterDelete.find("NODE b ") == std::string::npos, "a deleted node's line is gone");
    check(afterDelete.find("# the second thing") != std::string::npos,
          "...and the comment that sat above it is untouched, still where the author left it");
    check(afterDelete.find("NODE a ") != std::string::npos && afterDelete.find("NODE c ") != std::string::npos,
          "...and its neighbours did not move");

    // A record ADDED has no line to claim, so it is appended rather than invented into the middle.
    OcGraphData plusD = g;
    OcGraphNode d; d.id = "d"; d.type = "Multiply";
    plusD.nodes.push_back(d);
    const std::string afterAdd = writeOcgraph(plusD, original);
    check(afterAdd.find("NODE d Multiply") != std::string::npos, "a new node is written");
    check(afterAdd.find("NODE a ConstFloat") < afterAdd.find("NODE d Multiply"),
          "...after the records that already had a place, not spliced among them");
}

static void testLineEndingsSurvive() {
    AVER_INFO("=== a CRLF graph is saved as a CRLF graph ===");
    using namespace fmt;

    // Records are regenerated with "\n" while unknown lines are copied verbatim, so a CRLF file used
    // to come back MIXED -- CRLF on the lines the writer did not touch, LF on exactly the records the
    // author had been editing. Every graph in this repo is LF, which is why nobody noticed; a graph
    // written by a Windows editor is not.
    const std::string crlf =
        "OCGRAPH 1\r\n"
        "NAME Windows\r\n"
        "\r\n"
        "# a comment\r\n"
        "NODE k ConstFloat value=3.0\r\n"
        "OUT k value\r\n";

    OcGraphData g;
    std::string err;
    check(parseOcgraph(crlf, g, &err), "a CRLF graph parses");
    const std::string out = writeOcgraph(g, crlf);
    check(out == crlf, "an edit-free save of a CRLF file is byte-identical");
    check(out.find("\n\n") == std::string::npos || out.find("\r\n\r\n") != std::string::npos,
          "no bare LF was introduced among the CRLFs");
}

static void testDeterministic() {
    AVER_INFO("=== .ocgraph deterministic output ===");
    using namespace fmt;

    OcGraphData g1, g2;
    g1.name = "Graph";
    g2.name = "Graph";

    // Build two graphs with identical contents, in the SAME order.
    //
    // THE COMMENT HERE USED TO SAY "in different order", which the loop below has never done and
    // which would be the wrong property to want. Node order is DATA in this format: the writer walks
    // g.nodes in vector order, so two graphs whose nodes were added in different orders are two
    // different graphs and must serialise differently. What determinism means here is the narrower
    // and actually useful thing -- same data in, same bytes out, every time -- which is what makes a
    // content hash meaningful and a diff readable. It holds by construction because nothing in the
    // writer iterates an unordered container; the two unordered_maps in OcGraph.cpp are parser-side
    // validation and never reach the output.
    for (int i = 0; i < 5; ++i) {
        OcGraphNode n;
        n.id = "node" + std::to_string(i);
        n.type = "Op" + std::to_string(i);
        n.x = static_cast<f64>(i * 100);
        n.y = static_cast<f64>(i * 50);
        g1.nodes.push_back(n);
        g2.nodes.push_back(n);
    }

    // Add links in the same order to both.
    for (int i = 0; i < 4; ++i) {
        OcGraphLink l;
        l.sourceNode = "node" + std::to_string(i);
        l.sourcePin = "out";
        l.destNode = "node" + std::to_string(i + 1);
        l.destPin = "in";
        g1.links.push_back(l);
        g2.links.push_back(l);
    }

    // Write both.
    std::string text1 = writeOcgraph(g1);
    std::string text2 = writeOcgraph(g2);
    check(text1 == text2, "identical graphs produce identical output");

    // Ensure the output is stable (writing the same thing twice).
    std::string text1b = writeOcgraph(g1);
    check(text1 == text1b, "graph written twice produces identical output");
}

// Tests that malformed input is rejected properly.
static void testMalformedInput() {
    AVER_INFO("=== .ocgraph malformed input ===");
    using namespace fmt;

    {
        const std::string noHeader = "NAME Test\nNODE a Add 0 0\n";
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(noHeader, g, &err), "input without OCGRAPH header is rejected");
        check(err.find("OCGRAPH") != std::string::npos, "error message mentions missing header");
    }

    {
        const std::string badNode = "OCGRAPH 1\nNODE a\n"; // Missing type and position.
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(badNode, g, &err), "malformed NODE is rejected");
        check(err.find("NODE") != std::string::npos, "error message mentions NODE");
    }

    {
        const std::string badPin = "OCGRAPH 1\nNODE a Add 0 0\nPIN a\n"; // Missing fields.
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(badPin, g, &err), "malformed PIN is rejected");
        check(err.find("PIN") != std::string::npos, "error message mentions PIN");
    }

    {
        const std::string invalidDirection = "OCGRAPH 1\nNODE a Add 0 0\nPIN a x inout float\n";
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(invalidDirection, g, &err), "invalid PIN direction is rejected");
        check(err.find("direction") != std::string::npos || err.find("in") != std::string::npos,
              "error message mentions direction");
    }

    {
        const std::string linkBadFormat = "OCGRAPH 1\nOCGRAPH 1\nNODE a Add 0 0\nLINK a.x\n"; // Missing dest.
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(linkBadFormat, g, &err), "malformed LINK is rejected");
        check(err.find("LINK") != std::string::npos, "error message mentions LINK");
    }

    {
        const std::string linkBadFormat2 = "OCGRAPH 1\nNODE a Add 0 0\nLINK a_x_b_y\n"; // No dot separator.
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(linkBadFormat2, g, &err), "LINK without dot separator is rejected");
    }

    {
        const std::string linkNonexistentNode = "OCGRAPH 1\nNODE a Add 0 0\nLINK ghost.x a.x\n";
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(linkNonexistentNode, g, &err), "LINK to non-existent node is rejected");
        check(err.find("ghost") != std::string::npos, "error message mentions the bad node ID");
    }

    {
        const std::string duplicateNode = "OCGRAPH 1\nNODE a Add 0 0\nNODE a Multiply 10 10\n";
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(duplicateNode, g, &err), "duplicate node ID is rejected");
        check(err.find("duplicate") != std::string::npos, "error message mentions duplication");
    }

    {
        const std::string pinBadNode = "OCGRAPH 1\nNODE a Add 0 0\nPIN b x in float\n"; // b doesn't exist.
        OcGraphData g;
        std::string err;
        check(!parseOcgraph(pinBadNode, g, &err), "PIN referencing non-existent node is rejected");
        check(err.find("non-existent") != std::string::npos, "error message indicates node doesn't exist");
    }
}

// Tests the exec/flow addition: a link can be an EXEC link (control flow) rather than a data link --
// purely by both pins it connects being type "exec", requiring no new field on OcGraphLink -- and a
// node can be marked as an event ENTRY point. Covers the round trip (write -> parse -> write must be
// byte-identical, matching testRoundTrip's own standard above) and the backward-compatibility claim:
// an OLD graph with no exec pins and no ENTRY records parses and writes exactly as it did before this
// feature existed, because entryPoints stays empty and nothing about parsing a plain float/int/bool
// pin changed one bit.
static void testExecLinksAndEntryPoints() {
    AVER_INFO("=== .ocgraph exec links and ENTRY points ===");
    using namespace fmt;

    OcGraphData g;
    g.version = 1;
    g.name = "FlowRoundTrip";

    OcGraphNode onTick;
    onTick.id = "tick"; onTick.type = "OnTick"; onTick.x = 0; onTick.y = 0;
    onTick.pins.push_back({"exec", "exec", true, ""});
    g.nodes.push_back(onTick);

    OcGraphNode branch;
    branch.id = "b"; branch.type = "Branch"; branch.x = 200; branch.y = 0;
    branch.pins.push_back({"exec", "exec", false, ""});
    branch.pins.push_back({"cond", "bool", false, ""});
    branch.pins.push_back({"true", "exec", true, ""});
    branch.pins.push_back({"false", "exec", true, ""});
    g.nodes.push_back(branch);

    OcGraphNode setOnTrue;
    setOnTrue.id = "st"; setOnTrue.type = "SetField"; setOnTrue.x = 400; setOnTrue.y = -40;
    setOnTrue.pins.push_back({"exec", "exec", false, ""});
    setOnTrue.pins.push_back({"entity", "int", false, ""});
    setOnTrue.pins.push_back({"value", "float", false, ""});
    setOnTrue.pins.push_back({"then", "exec", true, ""});
    g.nodes.push_back(setOnTrue);

    // The exec link -- output pin "exec" (type exec) on `tick`, feeding input pin "exec" (type exec)
    // on `b`. Nothing on OcGraphLink itself says "this one is exec"; it IS one purely because both
    // pins it names are exec-typed. A DATA link (below) has exactly the same shape in the struct.
    g.links.push_back({"tick", "exec", "b", "exec"});
    g.links.push_back({"b", "true", "st", "exec"});

    OcGraphNode cnd;
    cnd.id = "c"; cnd.type = "ConstBool"; cnd.x = 200; cnd.y = 100;
    cnd.pins.push_back({"value", "bool", true, "true"});
    g.nodes.push_back(cnd);
    g.links.push_back({"c", "value", "b", "cond"}); // an ordinary DATA link, same struct, different pin types

    g.entryPoints.emplace_back("tick", "OnTick");

    const std::string text1 = writeOcgraph(g);
    check(!text1.empty(), "write produces non-empty text for a graph with exec links and an ENTRY record");
    check(text1.find("ENTRY tick OnTick") != std::string::npos, "the ENTRY record is written");
    check(text1.find("PIN tick exec out exec") != std::string::npos, "an exec-typed PIN line is written exactly like any other typed pin");
    check(text1.find("LINK tick.exec b.exec") != std::string::npos, "the exec LINK is written exactly like a data LINK -- same record, same grammar");

    OcGraphData g2;
    std::string err;
    check(parseOcgraph(text1, g2, &err), "a graph with exec pins and an ENTRY record parses: " + err);
    check(g2.entryPoints.size() == 1, "one ENTRY point round-tripped");
    if (g2.entryPoints.size() == 1) {
        check(g2.entryPoints[0].first == "tick" && g2.entryPoints[0].second == "OnTick",
              "the ENTRY point names the right node and event");
    }

    const std::string text2 = writeOcgraph(g2);
    check(text1 == text2, "second write of the parsed exec/entry graph reproduces the first byte for byte");

    // ENTRY validation: a record naming a node that does not exist is refused, the same way OUT
    // already is (modules/formats/src/OcGraph.cpp's OUT branch).
    {
        const std::string badEntry = "OCGRAPH 1\nNODE a Add 0 0\nENTRY ghost OnStart\n";
        OcGraphData bad;
        std::string badErr;
        check(!parseOcgraph(badEntry, bad, &badErr), "ENTRY referencing a non-existent node is rejected");
        check(badErr.find("ghost") != std::string::npos, "the ENTRY error names the bad node id");
    }

    // BACKWARD COMPATIBILITY, THE REGRESSION TEST THIS FEATURE MOST NEEDS. A graph that predates exec
    // support entirely -- no exec pins, no ENTRY records, exactly the shape testRoundTrip above
    // already builds -- must come out of the parser with an EMPTY entryPoints, and must write back
    // byte-identically. If adding ENTRY/exec ever made an old file's bytes move even one byte, every
    // .ocgraph already authored (Drone.ocgraph, cross_impl_test.ocgraph) would silently reformat the
    // moment it was next saved.
    {
        const std::string oldStyle =
            "OCGRAPH 1\n"
            "NAME OldGraph\n"
            "\n"
            "NODE c1 ConstFloat 0 0\n"
            "NODE sum Add 200 0\n"
            "\n"
            "PIN c1 value out float 5\n"
            "PIN sum a in float\n"
            "PIN sum result out float\n"
            "\n"
            "LINK c1.value sum.a\n"
            "\n"
            "OUT sum result\n";
        OcGraphData old;
        std::string oldErr;
        check(parseOcgraph(oldStyle, old, &oldErr), "a pre-exec graph still parses: " + oldErr);
        check(old.entryPoints.empty(), "a pre-exec graph has zero entry points -- nothing invented one");
        const std::string oldRewritten = writeOcgraph(old, oldStyle);
        check(oldRewritten == oldStyle, "a pre-exec graph round-trips byte-identically -- ENTRY never appears for a file that never had it");
    }
}

// Tests that the test suite itself runs and reports properly.
static void testMeta() {
    AVER_INFO("=== .ocgraph test suite metadata ===");
    check(true, "a passing check works");
    check(std::to_string(42) == "42", "string ops work");
}

// Runs all tests. Returns the failure count.
// The graph the CROSS-IMPLEMENTATION fixture holds: (5 + 7) * 3 == 36.
//
// Chained on purpose, so the C# side has to walk more than one node to get the answer -- a single
// constant would pass on a runtime that ignored links entirely.
static fmt::OcGraphData crossFixtureGraph() {
    using namespace fmt;
    OcGraphData g;
    g.name = "CrossImplementationTest";
    g.description = "Written by the C++ writer, parsed and executed by the C# runtime";

    const auto node = [&](const char* id, const char* type, f64 x, f64 y) {
        OcGraphNode n; n.id = id; n.type = type; n.x = x; n.y = y; g.nodes.push_back(n);
    };
    const auto pin = [&](const char* nodeId, const char* name, bool isOut, const char* def) {
        for (OcGraphNode& n : g.nodes) if (n.id == nodeId) {
            OcGraphPin p; p.name = name; p.isOutput = isOut; p.type = "float";
            if (def) p.defaultValue = def;
            n.pins.push_back(p);
        }
    };
    const auto link = [&](const char* sn, const char* sp, const char* dn, const char* dp) {
        OcGraphLink l; l.sourceNode = sn; l.sourcePin = sp; l.destNode = dn; l.destPin = dp;
        g.links.push_back(l);
    };

    node("c1", "ConstFloat", 0, 0);
    node("c2", "ConstFloat", 0, 100);
    node("c3", "ConstFloat", 0, 200);
    node("sum", "Add", 200, 50);
    node("prod", "Multiply", 400, 100);
    pin("c1", "value", true, "5");
    pin("c2", "value", true, "7");
    pin("c3", "value", true, "3");
    pin("sum", "a", false, nullptr);
    pin("sum", "b", false, nullptr);
    pin("sum", "result", true, nullptr);
    pin("prod", "a", false, nullptr);
    pin("prod", "b", false, nullptr);
    pin("prod", "result", true, nullptr);
    link("c1", "value", "sum", "a");
    link("c2", "value", "sum", "b");
    link("sum", "result", "prod", "a");
    link("c3", "value", "prod", "b");
    g.outputs.emplace_back("prod", "result");

    // AND AN EXEC CHAIN, because a fixture that does not contain the newest feature cannot catch the
    // newest divergence. This file exists for exactly one reason -- OcGraph.hpp records that the C++
    // and C# readers once "agreed on nodes and links and silently disagreed about what a graph
    // RETURNS" -- and when exec pins and ENTRY records were added, this fixture kept testing only the
    // float dataflow that already worked. Every test of the new records was single-implementation:
    // C++ wrote and C++ read it back, C# parsed text a human typed. The one thing neither proved is
    // the thing this fixture is for.
    //
    // Sequence rather than Branch on purpose: Branch needs a Bool `cond`, and wiring one in would
    // make this fixture also a test of default-value handling on an unconnected data pin, which is a
    // different question and would muddy what a failure here means. Sequence has exec pins only.
    //
    // The dataflow above is untouched, so the C# side still evaluates this graph to 36 through the
    // old Compile() path -- the exec chain rides alongside it and proves the two readers agree on the
    // new records byte for byte, which is all it is here to do.
    {
        OcGraphNode tick; tick.id = "tick"; tick.type = "OnTick"; tick.x = 0;   tick.y = 400;
        OcGraphPin  tickOut; tickOut.name = "exec"; tickOut.isOutput = true; tickOut.type = "exec";
        tick.pins.push_back(tickOut);
        g.nodes.push_back(tick);

        OcGraphNode seq; seq.id = "seq"; seq.type = "Sequence"; seq.x = 200; seq.y = 400;
        OcGraphPin  seqIn;  seqIn.name  = "exec"; seqIn.isOutput  = false; seqIn.type = "exec";
        OcGraphPin  seqOut; seqOut.name = "then"; seqOut.isOutput = true;  seqOut.type = "exec";
        seq.pins.push_back(seqIn);
        seq.pins.push_back(seqOut);
        g.nodes.push_back(seq);

        link("tick", "exec", "seq", "exec");
        g.entryPoints.emplace_back("tick", "OnTick");
    }
    return g;
}

// Asserts the checked-in cross-implementation fixture is still exactly what the writer produces.
//
// WHY THIS TEST EXISTS. The C# runtime is tested against a fixture file, and a fixture hand-written
// to satisfy the C# PARSER proves nothing about whether C# can read what C++ WRITES -- which is the
// only property that makes this one format rather than two with the same name. The first attempt at
// that fixture was hand-written and diverged in two ways at once: it had none of the blank lines the
// writer emits between sections, and it ended with a record the writer did not then produce at all.
//
// So the fixture is GENERATED (`OcGraphTest --write-fixture <path>`) and this test keeps it honest:
// change the writer without regenerating, and this fails naming the file, rather than the C# suite
// quietly continuing to pass against a stale approximation.
static void testCrossFixtureCurrent(const std::string& fixturePath) {
    AVER_INFO("=== .ocgraph cross-implementation fixture ===");
    std::string onDisk;
    if (!readFileText(fixturePath, onDisk)) {
        check(false, "the cross-implementation fixture is readable at " + fixturePath);
        return;
    }
    const std::string fresh = fmt::writeOcgraph(crossFixtureGraph());
    check(fresh == onDisk,
          "the checked-in fixture is byte-identical to what writeOcgraph produces today "
          "(regenerate with --write-fixture if the writer changed on purpose)");
}

int main(int argc, char** argv) {
    // Regeneration mode, used by a human after a deliberate format change -- never by the suite.
    if (argc > 2 && std::string(argv[1]) == "--write-fixture") {
        std::string err;
        if (!fmt::saveOcgraph(argv[2], crossFixtureGraph(), &err)) {
            AVER_ERROR("could not write the fixture: {}", err);
            return 1;
        }
        AVER_INFO("fixture written to {}", argv[2]);
        return 0;
    }

    // --roundtrip <path>: parse a REAL .ocgraph off disk, write it back, and say whether the bytes
    // survived. Not part of the suite -- a diagnostic, for the question the suite structurally
    // cannot ask, because every graph it tests is one it built itself.
    //
    // That blind spot had already cost something. The C# writer emits `NODE radius ConstFloat` with
    // no coordinates and `NODE time Param param=time` with an attribute; this parser demanded x/y
    // and its writer regenerated the NODE line from the fields it modelled. So a real C#-authored
    // graph would not parse, and one that did would come back out with its attributes stripped --
    // silently, since a rewrite reports success. Point this at a file the other implementation
    // wrote and the answer is a byte count, not an opinion.
    if (argc > 2 && std::string(argv[1]) == "--roundtrip") {
        std::string text, err;
        if (!readFileText(argv[2], text)) { AVER_ERROR("could not read {}", argv[2]); return 1; }
        fmt::OcGraphData g;
        if (!fmt::parseOcgraph(text, g, &err)) { AVER_ERROR("parse: {}", err); return 1; }
        const std::string out = fmt::writeOcgraph(g, text);
        AVER_INFO("parsed {} node(s), {} link(s), {} output(s)", g.nodes.size(), g.links.size(), g.outputs.size());
        for (const fmt::OcGraphNode& n : g.nodes)
            if (!n.extraTokens.empty()) {
                std::string ex;
                for (const std::string& e : n.extraTokens) ex += (ex.empty() ? "" : " ") + e;
                AVER_INFO("  node '{}' carries un-modelled tokens: {}", n.id, ex);
            }
        // Optional third argument: where to put the rewritten file, so a second --roundtrip over the
        // output answers the question that matters when the first pass reformats -- whether the
        // churn settles after one save or keeps moving on every save.
        if (argc > 3) {
            std::string werr;
            if (!fmt::saveOcgraph(argv[3], g, &werr)) { AVER_ERROR("write: {}", werr); return 1; }
            AVER_INFO("rewrote to {}", argv[3]);
        }
        if (out == text) { AVER_INFO("ROUND TRIP EXACT: {} bytes in, {} bytes out", text.size(), out.size()); return 0; }
        AVER_ERROR("ROUND TRIP CHANGED THE FILE: {} bytes in, {} bytes out", text.size(), out.size());
        // The first differing line, both sides. A byte offset alone sends the reader counting
        // characters; the two lines side by side usually name the bug outright.
        {
            usize i = 0, line = 1;
            while (i < text.size() && i < out.size() && text[i] == out[i]) { if (text[i] == '\n') ++line; ++i; }
            const auto lineAt = [](const std::string& s, usize pos) {
                const usize b = s.rfind('\n', pos == 0 ? 0 : pos - 1);
                const usize e = s.find('\n', pos);
                const usize from = (b == std::string::npos) ? 0 : b + 1;
                return s.substr(from, (e == std::string::npos ? s.size() : e) - from);
            };
            AVER_ERROR("  first difference at line {} (byte {})", line, i);
            AVER_ERROR("    in : '{}'", lineAt(text, i));
            AVER_ERROR("    out: '{}'", lineAt(out, i));
        }
        return 1;
    }

    testMeta();
    testBasicParse();
    testRoundTrip();
    testUnknownRecords();
    testVarRecordsSurviveRoundTrip();
    testForwardReferencedEntryAndOut();
    testInterleavedRecordsStayPut();
    testLineEndingsSurvive();
    testDeterministic();
    testMalformedInput();
    testExecLinksAndEntryPoints();
    // The path is passed in by CMake, so the test does not have to guess the repo layout.
    testCrossFixtureCurrent(AVER_OCGRAPH_FIXTURE);

    AVER_INFO("==================================================");
    AVER_INFO("OcGraph tests done: {} failure(s)", g_failures);
    return g_failures;
}
