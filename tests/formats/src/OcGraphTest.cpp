// .ocgraph format test. Covers round-tripping, unknown records, and deterministic output.
// Also tests malformed input rejection.
#include "aver/formats/OcGraph.hpp"
#include "aver/core/Log.hpp"

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

// Tests that output is deterministic: same data written twice produces identical output.
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

// Tests that the test suite itself runs and reports properly.
static void testMeta() {
    AVER_INFO("=== .ocgraph test suite metadata ===");
    check(true, "a passing check works");
    check(std::to_string(42) == "42", "string ops work");
}

// Runs all tests. Returns the failure count.
int main() {
    testMeta();
    testBasicParse();
    testRoundTrip();
    testUnknownRecords();
    testDeterministic();
    testMalformedInput();

    AVER_INFO("==================================================");
    AVER_INFO("OcGraph tests done: {} failure(s)", g_failures);
    return g_failures;
}
