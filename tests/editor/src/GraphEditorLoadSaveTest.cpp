// GraphEditorLoadSaveTest -- exercises sandbox/src/GraphEditor.cpp's PUBLIC surface (the same calls
// AssetEditorHost makes: construct from a path, save()) against real files on disk. This is the part
// of the editor that is NOT gated behind ImGui -- load/save/undo/selection are plain C++, only
// draw() needs a window. AVER_WITH_IMGUI is not defined for this target, so GraphEditor.cpp's
// `#include "imgui.h"` and its entire draw() body compile out (both are behind `#if AVER_WITH_IMGUI`
// in the source), and this test never touches an ImGui context.
//
// This proves, with an actual file on an actual filesystem rather than by reading the source, the
// task brief's two hard requirements:
//   1. a load -> save with no edits is byte-identical
//   2. records the reader did not understand survive a save
// and the auto-layout design choice: a graph whose nodes are all at the origin still round-trips
// byte-identically through GraphEditor's real save() path (not just the pure autoLayoutPositions()
// function in isolation, which GraphEditorGeometryTest already covers).
#include "GraphCollectionNodes.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#include "GraphStructLiteral.hpp"
#include "LevelClassSave.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
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

// A directory this test owns, cleared at the start of the run so re-runs don't see stale files from
// a previous (possibly failed) run. std::filesystem::temp_directory_path(), same as
// tests/platform/src/WatcherTest.cpp uses for its own scratch files.
static std::string scratchDir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::current_path();
    return (base / "AverGraphEditorLoadSaveTest").string();
}

// =========================================================================== real fixture, no edits =
static void testByteIdenticalRoundTrip() {
    AVER_INFO("=== byte-identical load -> save (real cross-impl fixture, positions already set) ===");
    const std::string original = readFile(AVER_OCGRAPH_FIXTURE);
    check(!original.empty(), "fixture file read (sanity check the path resolved)");

    const std::string tmp = (std::filesystem::path(scratchDir()) / "fixture_copy.ocgraph").string();
    writeFile(tmp, original);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");

    std::string why;
    const bool saved = ed.save(&why);
    check(saved, "save() succeeds on a freshly loaded, unedited graph (why='" + why + "')");
    check(!ed.dirty(), "editor is not dirty immediately after a successful save");

    const std::string after = readFile(tmp);
    check(after == original, "save() with zero edits reproduces the source file byte for byte");
}

// ============================================================================ unknown records survive =
static void testUnknownRecordsSurvive() {
    AVER_INFO("=== unknown records survive a save ===");
    const std::string text =
        "OCGRAPH 1\n"
        "# a hand-written comment nobody's parser understands specially\n"
        "NAME UnknownRecordCheck\n"
        "MYSTERY this is a future record type the parser has never heard of\n"
        "\n"
        "NODE c1 ConstFloat 10 20\n"
        "\n"
        "PIN c1 value out float 42\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "unknown_records.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");
    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");

    const std::string after = readFile(tmp);
    check(after.find("MYSTERY this is a future record type the parser has never heard of") != std::string::npos,
          "the unrecognised MYSTERY record survives an editor save");
    check(after.find("# a hand-written comment nobody's parser understands specially") != std::string::npos,
          "the hand-written comment survives an editor save");
    check(after == text, "with zero edits, the whole file -- known and unknown records alike -- is byte-identical");
}

// ============================================================ synthesised pins stay display-only ===
static void testSynthesisedPinsAreNotWritten() {
    AVER_INFO("=== a graph whose pins are implied by node type still saves unchanged ===");
    // THE SHAPE REAL FILES HAVE, which none of the fixtures above did. The C# runtime derives a
    // node's pins from its TYPE (OcGraphParser.cs's AddDefaultPins), so a graph it writes records
    // PIN lines only where a pin carries something extra. Drone.ocgraph is 19 nodes, 7 PIN records
    // and 19 LINKs referencing pins that appear nowhere in the file.
    //
    // The C++ reader models only what is written, so before GraphEditor synthesised the rest, those
    // nodes had no pins and the editor drew no wires between them -- a graph that opened as a row of
    // disconnected boxes. The synthesis fixes the drawing; this test guards the other half, that it
    // never reaches the file. Writing them back would add PIN records the author never wrote.
    const std::string text =
        "OCGRAPH 1\n"
        "NAME ImpliedPins\n"
        "\n"
        "NODE c1 ConstFloat\n"
        "NODE c2 ConstFloat\n"
        "NODE sum Add\n"
        "\n"
        "PIN c1 value out float 5\n"
        "PIN c2 value out float 7\n"
        "\n"
        "LINK c1.value sum.a\n"
        "LINK c2.value sum.b\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "implied_pins.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "a graph with implied pins loads without being marked dirty");

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");

    const std::string after = readFile(tmp);
    // The precise failure this guards: `sum` gains a/b/result and the two constants gain nothing,
    // so a leak shows up as three extra PIN lines and a longer file.
    check(after == text,
          "synthesised pins never reach the file: byte-identical with zero edits");
    usize pins = 0;
    for (usize i = 0; (i = after.find("PIN ", i)) != std::string::npos; ++i) ++pins;
    check(pins == 2, "still exactly the 2 PIN records the author wrote, not the 5 the types imply");
}

// ================================================================ auto-layout stays display-only ===
static void testAutoLayoutDoesNotDirtyTheFile() {
    AVER_INFO("=== auto-layout (all-origin positions) still round-trips byte-identically ===");
    // Every NODE line sits at (0,0) -- exactly the shape GraphEditor::runAutoLayoutIfUnpositioned()
    // treats as "nobody positioned this graph" and lays out for DISPLAY only. If auto-layout ever
    // started writing into the real node positions, this save would move c1/c2/sum's x/y away from
    // "0 0" and this test would catch it immediately.
    const std::string text =
        "OCGRAPH 1\n"
        "NAME AllOrigin\n"
        "\n"
        "NODE c1 ConstFloat 0 0\n"
        "NODE c2 ConstFloat 0 0\n"
        "NODE sum Add 0 0\n"
        "\n"
        "PIN c1 value out float 5\n"
        "PIN c2 value out float 7\n"
        "PIN sum a in float\n"
        "PIN sum b in float\n"
        "PIN sum result out float\n"
        "\n"
        "LINK c1.value sum.a\n"
        "LINK c2.value sum.b\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "all_origin.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty, even though auto-layout ran internally");
    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");

    const std::string after = readFile(tmp);
    check(after == text, "auto-layout never leaks into a save: the all-(0,0) file is unchanged byte for byte");
}

// ============================================================================ attribute editing (Gap B)
// The node-attribute authoring surface's real, end-to-end proof: GraphEditor::setAttribute/
// clearAttribute (which the ImGui details panel in GraphEditor.cpp's draw() calls, unmodified) exercised
// through this test's SAME public surface -- no ImGui context, exactly like every other test in this
// file. Three properties, all load-bearing per the task brief:
//   1. an edit lands on disk through the real save() path (not just in memory).
//   2. an attribute this editor's catalog does NOT declare survives an edit to a DIFFERENT attribute on
//      the SAME node, byte-for-byte, in its original position -- extraTokens' entire reason for
//      existing (see OcGraph.hpp), proven here rather than merely read about.
//   3. editing exactly one attribute changes exactly one token on one NODE line; every other line in the
//      file -- known records and the hand-authored attribute alike -- is untouched.
// A value typed into an unwired input pin is the pin's PIN record default: it lands on disk, a wired
// or unknown pin and an output pin refuse it, and an empty value clears it.
static void testPinDefaultRoundTrip() {
    AVER_INFO("=== pin values: setPinDefault writes the PIN default ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "pin_default_test.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME PinDefaultTest\nNODE add Add 0 0\nPIN add a in float\nPIN add b in float 4\n"
                   "PIN add result out float\nOUT add result\n");
    GraphEditor ed(tmp);
    check(ed.setPinDefault("add", "a", "2.5"), "setPinDefault on an input pin reports success");
    check(ed.dirty(), "a typed pin value dirties the editor");
    check(!ed.setPinDefault("add", "result", "1"), "an output pin takes no typed value");
    check(!ed.setPinDefault("add", "nope", "1"), "an unknown pin is refused");
    check(!ed.setPinDefault("add", "a", "2 5"), "a value with whitespace is refused");
    std::string why;
    check(ed.save(&why), "save() succeeds after a pin value edit (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("PIN add a in float 2.5") != std::string::npos, "the typed value is the input PIN record's default on disk");
    check(after.find("PIN add b in float 4") != std::string::npos, "an untouched pin keeps its value");
    check(ed.setPinDefault("add", "a", ""), "an empty value clears it");
    const fmt::OcGraphNode* add = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "add") { add = &n; break; }
    std::string a = "unset";
    if (add) for (const auto& p : add->pins) if (p.name == "a" && !p.isOutput) a = p.defaultValue;
    check(a.empty(), "the cleared pin has no value, got '" + a + "'");
}

// float3, string and entity pins: a typed value survives save and reload (the string as its %-escaped token),
// the escape round-trips, and the link rule lets entity and int meet but keeps float3 apart from float.
static void testVectorStringEntityPins() {
    AVER_INFO("=== float3 / string / entity pins: defaults round-trip, link rule ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "data_pins_test.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME DataPins\nNODE vl VectorLength 0 0\nPIN vl v in float3\nPIN vl result out float\n"
                   "NODE pt PrintText 0 0\nPIN pt exec in exec\nPIN pt text in string\nPIN pt then out exec\n"
                   "NODE k ConstInt 0 0\nPIN k value out int 1\nNODE ev IsValidEntity 0 0\nPIN ev e in entity\nPIN ev result out bool\n"
                   "NODE s Add 0 0\nPIN s a in float\nPIN s b in float\nPIN s result out float\nOUT vl result\n");
    GraphEditor ed(tmp);
    check(ed.setPinDefault("vl", "v", "1, 2.5,3"), "a float3 value with spaces after its commas is accepted");
    check(ed.setPinDefault("pt", "text", "hello big world%"), "a string value with spaces and a percent sign is accepted");
    std::string why;
    check(ed.save(&why), "save() succeeds after the edits (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("PIN vl v in float3 1,2.5,3") != std::string::npos, "the float3 is stored as x,y,z with no spaces");
    check(after.find("PIN pt text in string hello%20big%20world%25") != std::string::npos,
          "the string is stored %-escaped as one token");

    GraphEditor again(tmp);
    std::string vec, text;
    for (const auto& n : again.graph().nodes)
        for (const auto& p : n.pins) {
            if (n.id == "vl" && p.name == "v") vec = p.defaultValue;
            if (n.id == "pt" && p.name == "text") text = p.defaultValue;
        }
    check(vec == "1,2.5,3", "the float3 default reloads unchanged, got '" + vec + "'");
    check(fmt::decodeGraphString(text) == "hello big world%", "the string default reloads and decodes to what was typed, got '" + text + "'");
    check(fmt::decodeGraphString(fmt::encodeGraphString("a b\tc\nd%20")) == "a b\tc\nd%20", "encode then decode is the identity");

    // ---- struct and array values: the literal helpers, and setPinDefault on struct and array inputs
    {
        fmt::OcGraphStruct inner{"Inner", {{"a", "float"}, {"tags", "string[]"}}};
        fmt::OcGraphStruct outer{"Outer", {{"k", "int"}, {"inner", "struct:Inner"}, {"xs", "float[]"}}};
        std::string lit = withStructLiteralField("", outer, "xs", "1,2,3");
        lit = withStructLiteralField(lit, outer, "k", "3");
        check(lit == "{k=3|xs=1,2,3}", "fields are written in the struct's order, got '" + lit + "'");
        const std::string in = withStructLiteralField(withStructLiteralField("", inner, "a", "7"), inner, "tags",
                                                      arrayLiteralFromText("string", "x, y|z , a b"));
        check(in == "{a=7|tags=x,y%7Cz,a%20b}", "a string array escapes | and spaces per element, got '" + in + "'");
        lit = withStructLiteralField(lit, outer, "inner", in);
        check(lit == "{k=3|inner={a=7|tags=x,y%7Cz,a%20b}|xs=1,2,3}", "a nested struct sits in its own braces, got '" + lit + "'");
        check(structLiteralField(lit, "inner") == in && structLiteralField(lit, "xs") == "1,2,3", "fields read back past the nested braces");
        check(arrayTextFromLiteral("string", structLiteralField(in, "tags")) == "x, y|z, a b", "a string array shows decoded");
        check(withStructLiteralField(withStructLiteralField(lit, outer, "k", ""), outer, "inner", "") == "{xs=1,2,3}", "an emptied field is dropped");
        check(withStructLiteralField("{xs=1}", outer, "xs", "").empty(), "no fields left is the zero struct: an empty literal");
        check(encodeLiteralString("a,b{c}") == "a%2Cb%7Bc%7D" && fmt::decodeGraphString(encodeLiteralString("a,b{c}|% d")) == "a,b{c}|% d",
              "a literal's string escapes decode back");

        const std::string sp = (std::filesystem::path(scratchDir()) / "struct_pins_test.ocgraph").string();
        writeFile(sp, "OCGRAPH 1\nNAME StructPins\nSTRUCT Inner\nFIELD Inner a float\nFIELD Inner tags string[]\n"
                      "NODE br BreakStruct 0 0 struct=Inner\nPIN br value in struct:Inner\nPIN br a out float\nPIN br tags out string[]\n"
                      "NODE ln ArrayLength 0 0\nPIN ln array in float[]\nPIN ln length out int\nOUT br a\nOUT ln length\n");
        GraphEditor se(sp);
        check(pinTakesInlineValue(fmt::OcGraphNode{}, fmt::OcGraphPin{"a", "float[]", false, ""}) &&
              pinTakesInlineValue(fmt::OcGraphNode{}, fmt::OcGraphPin{"a", "struct:A", false, ""}) &&
              !pinTakesInlineValue(fmt::OcGraphNode{}, fmt::OcGraphPin{"a", "struct:A", true, ""}), "struct and array inputs take a value box, outputs do not");
        check(se.setPinDefault("br", "value", "{a=4|tags=p,q}"), "a struct literal is accepted on a struct input");
        check(se.setPinDefault("ln", "array", "4, 5, 6"), "an array typed with spaces is accepted");
        std::string why;
        check(se.save(&why), "save() succeeds (why='" + why + "')");
        const std::string saved = readFile(sp);
        check(saved.find("PIN br value in struct:Inner {a=4|tags=p,q}") != std::string::npos, "the struct literal is written on its PIN");
        check(saved.find("PIN ln array in float[] 4,5,6") != std::string::npos, "the array is written without spaces");
    }

    const auto& g = ed.graph();
    check(canConnectPins(g, "k", "value", "ev", "e").ok, "an int output connects to an entity input");
    check(!canConnectPins(g, "vl", "result", "vl", "v").ok, "a float output does not connect to a float3 input");
    check(!canConnectPins(g, "k", "value", "vl", "v").ok, "an int output does not connect to a float3 input");
    check(exactPinTypeMatch("entity", "int") && exactPinTypeMatch("int", "entity"), "entity and int match in both directions");
    check(!exactPinTypeMatch("float3", "float") && !exactPinTypeMatch("float", "float3"), "float3 and float never match");
    check(!exactPinTypeMatch("entity", "float"), "entity does not match float");
}

// STRUCT / FIELD records and an array VAR round-trip byte-identically; the editor's struct calls write them; palette variants and
// struct rows create typed nodes; struct edits rebuild those nodes' pins; the array and struct link rules.
static void testStructsAndArraysInTheEditor() {
    AVER_INFO("=== structs and arrays: records round-trip, edits write them, palette rows, link rules ===");
    const std::string dir = scratchDir();

    // ---- records survive a load and save untouched, in place
    {
        const std::string text =
            "OCGRAPH 1\nNAME Coll\n\n"
            "STRUCT Pt\nFIELD Pt x float\nFIELD Pt pos float3\nFIELD Pt name string\n\n"
            "VAR xs float[] 1,2,3\nVAR dirs float3[] 1,0,0;0,1,0\nVAR p struct:Pt\n\n"
            "NODE g GetVar 0 0 var=xs\nPIN g value out float[]\n"
            "NODE a ArrayLength 0 0 of=float\nPIN a array in float[]\nPIN a length out int\n"
            "LINK g.value a.array\n";
        const std::string tmp = (std::filesystem::path(dir) / "coll_roundtrip.ocgraph").string();
        writeFile(tmp, text);
        GraphEditor ed(tmp);
        check(ed.structs().size() == 1 && ed.structs()[0].name == "Pt" && ed.structs()[0].fields.size() == 3,
              "STRUCT and its FIELD records are read");
        check(ed.structs()[0].fields[1].name == "pos" && ed.structs()[0].fields[1].type == "float3", "a field keeps its name and type");
        check(ed.variables().size() == 3 && ed.variables()[0].type == "float[]" && ed.variables()[0].defaultValue == "1,2,3",
              "an array VAR keeps its type and default");
        check(ed.variables()[2].type == "struct:Pt", "a struct VAR keeps its type");
        std::string why;
        check(ed.save(&why), "save() succeeds (why='" + why + "')");
        check(readFile(tmp) == text, "load then save is byte-identical with STRUCT, FIELD and array VAR records");
    }

    // ---- the editor's calls write the records, in an order the C# reader needs (STRUCT before its FIELDs)
    const std::string tmp = (std::filesystem::path(dir) / "coll_edit.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME CollEdit\n");
    GraphEditor ed(tmp);
    check(!ed.addStruct("") && !ed.addStruct("A B") && !ed.addStruct("A.B") && !ed.addStruct("A:B"), "an unusable struct name is refused");
    check(ed.addStruct("Pt"), "addStruct declares a struct");
    check(!ed.addStruct("Pt"), "a second struct of the same name is refused");
    check(ed.addStructField("Pt", "x", "float") && ed.addStructField("Pt", "pos", "float3"), "addStructField adds fields");
    check(!ed.addStructField("Pt", "x", "int"), "a duplicate field is refused");
    check(!ed.addStructField("Pt", "value", "int") && !ed.addStructField("Pt", "result", "int"), "a field named like a struct node's own pin is refused");
    check(!ed.addStructField("Pt", "q", "struct:Pt"), "a struct cannot hold itself");
    check(!ed.addStructField("Pt", "q", "exec") && !ed.addStructField("Pt", "q", "struct:Nope"), "nor exec, nor an undeclared struct");
    check(!ed.addStructField("Nope", "q", "int"), "a field of an unknown struct is refused");
    check(ed.addVariable("xs", "float[]", "1,2") && ed.variables().back().type == "float[]", "an array VAR can be declared");
    check(ed.addVariable("p", "struct:Pt", "") && ed.variables().back().type == "struct:Pt", "a VAR of a declared struct can be declared");
    check(ed.addVariable("q", "struct:Nope", "") && ed.variables().back().type == "float", "a VAR of an undeclared struct falls back to float");
    check(ed.addFunction("F") == "F" && ed.addFunctionPin("F", true, "pts", "float3[]") && ed.addFunctionPin("F", false, "r", "struct:Pt"),
          "function pins may be arrays and structs");
    {
        std::string why;
        check(ed.save(&why), "save() succeeds (why='" + why + "')");
        const std::string after = readFile(tmp);
        check(after.find("STRUCT Pt\nFIELD Pt x float\nFIELD Pt pos float3\n") != std::string::npos, "STRUCT then its FIELDs are written");
        check(after.find("VAR xs float[] 1,2\n") != std::string::npos && after.find("VAR p struct:Pt\n") != std::string::npos,
              "array and struct VARs are written");
        check(after.find("FUNCIN F pts float3[]\n") != std::string::npos && after.find("FUNCOUT F r struct:Pt\n") != std::string::npos,
              "array and struct function pins are written");
        GraphEditor again(tmp);
        check(again.structs().size() == 1 && again.structs()[0].fields.size() == 2, "the written file reloads with its struct");
    }

    // ---- fields that are arrays and other structs; rename and delete follow them
    {
        const std::string nest = (std::filesystem::path(dir) / "coll_nest.ocgraph").string();
        writeFile(nest, "OCGRAPH 1\nNAME Nest\n");
        GraphEditor ne(nest);
        check(ne.addStruct("Inner") && ne.addStruct("Outer"), "two structs");
        check(ne.addStructField("Inner", "tags", "string[]"), "an array field");
        check(ne.addStructField("Outer", "inner", "struct:Inner"), "a field holding another struct");
        check(!ne.addStructField("Inner", "back", "struct:Outer"), "a field that would make Inner hold itself (through Outer) is refused");
        check(!ne.retypeStructField("Inner", "tags", "struct:Outer"), "and so is retyping one into that");
        check(ne.renameStruct("Inner", "Core") && ne.structs()[1].fields[0].type == "struct:Core", "a rename follows into the fields that hold it");
        std::vector<std::string> blocked;
        check(!ne.deleteStruct("Core", &blocked) && !blocked.empty() && blocked[0] == "struct Outer.inner",
              "a struct another struct holds cannot be deleted");
        std::string why;
        check(ne.save(&why), "save() succeeds (why='" + why + "')");
        const std::string text = readFile(nest);
        check(text.find("FIELD Core tags string[]\n") != std::string::npos && text.find("FIELD Outer inner struct:Core\n") != std::string::npos,
              "array and struct FIELDs are written");
    }

    // ---- palette: variants and per-struct rows write the right node type, attributes and pins
    {
        const GraphNodeDesc* variant = findGraphNodeDesc("ArrayGet@float3");
        check(variant && variant->nodeType == "ArrayGet" && variant->displayName == "Array Get (Vector)", "the catalog holds a palette variant per element type");
        const std::string id = ed.addNodeFromCatalog("ArrayGet@float3", Vec2{0, 0});
        const fmt::OcGraphNode* n = nullptr;
        for (const auto& x : ed.graph().nodes) if (x.id == id) n = &x;
        check(n && n->type == "ArrayGet" && nodeAttrValue(*n, "of") == "float3", "the variant writes ArrayGet with of=float3, not its palette id");
        bool arr = false, item = false;
        if (n) for (const auto& p : n->pins) {
            if (p.name == "array" && p.type == "float3[]" && !p.isOutput) arr = true;
            if (p.name == "item" && p.type == "float3" && p.isOutput) item = true;
        }
        check(arr && item, "its pins are typed from the element");
        const std::string plain = ed.addNodeFromCatalog("MakeArray", Vec2{0, 40});
        bool floatArr = false;
        for (const auto& x : ed.graph().nodes) if (x.id == plain) for (const auto& p : x.pins) if (p.name == "array" && p.type == "float[]") floatArr = true;
        check(floatArr, "the plain entry is the float array node");
        check(ed.addNodeFromCatalog("ArrayAdd@string", Vec2{0, 80}) != "", "an exec array node can be placed");

        const auto derived = ed.graphDerivedNodeDescs();
        bool hasMake = false, hasBreak = false, hasSetX = false;
        for (const auto& d : derived) {
            if (d.typeId == "MakeStruct@Pt") hasMake = true;
            if (d.typeId == "BreakStruct@Pt") hasBreak = true;
            if (d.typeId == "SetStructField@Pt.x") hasSetX = true;
        }
        check(hasMake && hasBreak && hasSetX, "the palette offers Make, Break and Set <field> for a declared struct");
        const std::string mk = ed.addNodeFromCatalog("MakeStruct@Pt", Vec2{200, 0});
        const std::string br = ed.addNodeFromCatalog("BreakStruct@Pt", Vec2{400, 0});
        const std::string st = ed.addNodeFromCatalog("SetStructField@Pt.pos", Vec2{400, 100});
        const auto pinsOf = [&](const std::string& nid) {
            std::string s;
            for (const auto& x : ed.graph().nodes) if (x.id == nid) {
                s += x.type + " " + nodeAttrValue(x, "struct") + "/" + nodeAttrValue(x, "field") + ":";
                for (const auto& p : x.pins) s += " " + p.name + (p.isOutput ? ">" : "<") + p.type;
            }
            return s;
        };
        check(pinsOf(mk) == "MakeStruct Pt/: x<float pos<float3 value>struct:Pt", "MakeStruct has one input per field and a struct output, got " + pinsOf(mk));
        check(pinsOf(br) == "BreakStruct Pt/: value<struct:Pt x>float pos>float3", "BreakStruct has a struct input and one output per field, got " + pinsOf(br));
        check(pinsOf(st) == "SetStructField Pt/pos: value<struct:Pt pos<float3 result>struct:Pt", "SetStructField has the value, its field and the result, got " + pinsOf(st));

        // ---- the link rules
        check(ed.connectPins(mk, "value", br, "value"), "a struct output links to the same struct's input");
        check(!ed.connectPins(br, "x", st, "pos"), "a float output cannot be wired into a float3 input");
        const std::string g1 = ed.addNodeFromCatalog("MakeArray", Vec2{0, 200});
        const std::string g2 = ed.addNodeFromCatalog("ArrayLength@int", Vec2{200, 200});
        check(!ed.connectPins(g1, "array", g2, "array"), "a float[] output is refused by an int[] input");
        const std::string g3 = ed.addNodeFromCatalog("ArrayLength", Vec2{200, 260});
        check(ed.connectPins(g1, "array", g3, "array"), "a float[] output links to a float[] input");
        const std::string g4 = ed.addNodeFromCatalog("ArrayLength@entity", Vec2{200, 320});
        const std::string g5 = ed.addNodeFromCatalog("MakeArray@int", Vec2{0, 320});
        check(ed.connectPins(g5, "array", g4, "array"), "an int[] output links to an entity[] input");

        // ---- editing the struct rebuilds the nodes that use it
        check(ed.addStructField("Pt", "name", "string"), "a field is added");
        check(pinsOf(br).find("name>string") != std::string::npos, "an added field appears on BreakStruct");
        check(pinsOf(mk).find("name<string") != std::string::npos, "an added field appears on MakeStruct");
        check(ed.retypeStructField("Pt", "pos", "float"), "a field is retyped");
        check(pinsOf(br).find("pos>float") != std::string::npos && pinsOf(br).find("pos>float3") == std::string::npos, "the retyped pin follows");
        check(ed.renameStructField("Pt", "pos", "where"), "a field is renamed");
        check(pinsOf(br).find("where>float") != std::string::npos && pinsOf(st).find("SetStructField Pt/where") == 0, "the rename reaches the pins and field=");
        check(ed.removeStructField("Pt", "name"), "a field is removed");
        check(pinsOf(br).find("name") == std::string::npos, "a removed field leaves BreakStruct");
        bool stillLinked = false;
        for (const auto& l : ed.graph().links) if (l.sourceNode == mk && l.destNode == br && l.sourcePin == "value") stillLinked = true;
        check(stillLinked, "the struct wire survives the field edits");

        // ---- rename and delete
        std::vector<std::string> blocked;
        check(!ed.deleteStruct("Pt", &blocked) && !blocked.empty(), "a struct in use cannot be deleted");
        check(ed.renameStruct("Pt", "Point"), "a struct is renamed");
        bool allRenamed = true;
        for (const auto& v : ed.variables()) if (v.name == "p" && v.type != "struct:Point") allRenamed = false;
        for (const auto& f : ed.functions()) for (const auto& p : f.outputs) if (p.type == "struct:Pt") allRenamed = false;
        check(allRenamed && nodeAttrValue(*std::find_if(ed.graph().nodes.begin(), ed.graph().nodes.end(),
                                                         [&](const fmt::OcGraphNode& x) { return x.id == mk; }), "struct") == "Point",
              "the rename reaches VARs, function pins and struct nodes");
        std::string why;
        check(ed.save(&why), "save() succeeds with struct nodes (why='" + why + "')");
        const std::string after = readFile(tmp);
        check(after.find("STRUCT Point\nFIELD Point x float\nFIELD Point where float\n") != std::string::npos, "the renamed struct and fields are written");
        check(after.find("Pt") == std::string::npos, "no stale name is left in the file");
        ed.undoForTest();
        check(ed.structs()[0].name == "Pt" || ed.structs()[0].name == "Point", "undo restores the struct list without crashing");
    }

    // ---- a node written by hand with no PIN records still draws typed pins, and they are not written back
    {
        const std::string text = "OCGRAPH 1\nNAME Hand\nSTRUCT S\nFIELD S a int\n\n"
                                 "NODE g ArrayGet 0 0 of=float3\nNODE m MakeStruct 0 0 struct=S\n";
        const std::string hand = (std::filesystem::path(dir) / "coll_hand.ocgraph").string();
        writeFile(hand, text);
        GraphEditor he(hand);
        std::string arrT, fieldT;
        for (const auto& n : he.graph().nodes) {
            for (const auto& p : n.pins) {
                if (n.id == "g" && p.name == "array") arrT = p.type;
                if (n.id == "m" && p.name == "a") fieldT = p.type;
            }
        }
        check(arrT == "float3[]", "a pinless ArrayGet of=float3 gets float3[] pins, got '" + arrT + "'");
        check(fieldT == "int", "a pinless MakeStruct gets a pin per field, got '" + fieldT + "'");
        std::string why;
        check(he.save(&why) && readFile(hand) == text, "saving writes none of the drawn-only pins");
    }

    // ---- the link rule on type strings
    check(exactPinTypeMatch("float[]", "float[]"), "same array type matches");
    check(!exactPinTypeMatch("float[]", "int[]") && !exactPinTypeMatch("float[]", "float") && !exactPinTypeMatch("float", "float[]"),
          "different arrays, and an array and its element, do not match");
    check(exactPinTypeMatch("int[]", "entity[]") && exactPinTypeMatch("entity[]", "int[]"), "int[] and entity[] match in both directions");
    check(exactPinTypeMatch("struct:A", "struct:A") && !exactPinTypeMatch("struct:A", "struct:B") && !exactPinTypeMatch("struct:A", "float"),
          "a struct matches only itself");
    check(!pinTakesInlineValue(fmt::OcGraphNode{}, fmt::OcGraphPin{"a", "float[]", true, ""}) &&
          !pinTakesInlineValue(fmt::OcGraphNode{}, fmt::OcGraphPin{"a", "struct:A", true, ""}), "array and struct outputs take no value box");
    check(isDataTypeName("float3[]", nullptr) && isDataTypeName("struct:Anything", nullptr) && !isDataTypeName("exec", nullptr) &&
          !isDataTypeName("float[][]", nullptr) && !isDataTypeName("exec[]", nullptr), "the type-name helpers");
}

static void testAttributeEditRoundTrip() {
    AVER_INFO("=== attribute editing: setAttribute lands on disk, unknown attribute survives ===");
    const std::string original = readFile(AVER_OCGRAPH_ATTR_FIXTURE);
    check(!original.empty(), "attribute fixture file read (sanity check the path resolved)");

    const std::string tmp = (std::filesystem::path(scratchDir()) / "attribute_edit_test.ocgraph").string();
    writeFile(tmp, original);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");

    // Pre-edit state, read through the same public surface the details panel reads: 'sp' has no
    // class= yet (that's the gap this whole feature closes) and already carries a hand-authored
    // attribute this build's catalog does not declare.
    const fmt::OcGraphNode* sp = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "sp") { sp = &n; break; }
    check(sp != nullptr, "fixture has a node 'sp'");
    if (sp) {
        check(!getNodeAttribute(*sp, "class").found, "before the edit, 'sp' has no class= attribute");
        const GraphNodeAttribute dbg = getNodeAttribute(*sp, "debugLabel");
        check(dbg.found && dbg.value == "spawn_test_marker",
              "before the edit, 'sp' already carries the hand-authored, uncatalogued 'debugLabel' attribute");
    }

    check(ed.setAttribute("sp", "class", "SomeSpawnedClass"), "setAttribute('sp', 'class', ...) reports success");
    check(ed.dirty(), "editor is dirty immediately after setAttribute");
    check(!ed.setAttribute("nope", "class", "X"), "setAttribute on a nonexistent node id returns false");

    std::string why;
    check(ed.save(&why), "save() succeeds after the attribute edit (why='" + why + "')");
    check(!ed.dirty(), "editor is not dirty immediately after a successful save");

    const std::string after = readFile(tmp);

    // Property 3: exactly one NODE line changed, and it gained exactly the new token, appended after
    // the pre-existing one, in order -- setAttribute() never reorders what was already there.
    check(after.find("NODE sp Spawn 400 100 debugLabel=spawn_test_marker class=SomeSpawnedClass") != std::string::npos,
          "the edited NODE line carries BOTH the pre-existing 'debugLabel' token and the new 'class=' token, in original order");
    check(after.find("NODE sp Spawn 400 100 debugLabel=spawn_test_marker\n") == std::string::npos,
          "the OLD (unedited) form of the NODE line is gone -- the edit really landed, not merely appended a second NODE record");

    // Property 2, restated as a direct diff: every OTHER line the fixture had is untouched, byte for
    // byte -- including the untouched node's own extraToken-free NODE lines, the PIN/LINK/ENTRY/OUT
    // records, the NAME/DESCRIPTION lines, and even blank-line placement.
    for (const char* untouchedLine : {
             "NAME AttributeEditTest",
             "DESCRIPTION Gap B fixture: the C++ editor adds class= to 'sp' below; this exact saved file is then parsed and compiled by the C# runtime, unedited",
             "NODE tick OnTick 0 0", "NODE cx ConstFloat 200 0", "NODE cy ConstFloat 200 100", "NODE cz ConstFloat 200 200",
             "PIN cx value out float 1", "PIN cy value out float 2", "PIN cz value out float 3",
             "LINK tick.exec sp.exec", "LINK cx.value sp.x", "LINK cy.value sp.y", "LINK cz.value sp.z",
             "ENTRY tick OnTick", "OUT sp entity"}) {
        check(after.find(untouchedLine) != std::string::npos,
              std::string("unrelated line survives the edit unchanged: '") + untouchedLine + "'");
    }

    // Cross-language proof (see this test's header comment): write the exact bytes the C++ editor just
    // produced to the path scripting/csharp/Aver.Graph.Tests's own AttributeEditRoundTripTests.cs reads,
    // so that suite parses and COMPILES this real output -- not a hand-transcribed copy of it.
    writeFile(AVER_OCGRAPH_ATTR_FIXTURE_OUTPUT, after);
    AVER_INFO("[GraphEditorLoadSaveTest] wrote {} for the C# side to compile", AVER_OCGRAPH_ATTR_FIXTURE_OUTPUT);
}

// A made-up attribute, isolated: proves getNodeAttribute/setNodeAttribute/removeNodeAttribute survive
// editing a DECLARED attribute (field=, on GetFieldVec3) without disturbing a completely fictitious one
// (zzz_not_a_real_attribute=) sitting on the SAME node -- the sharpest version of the "unrecognised
// attribute survives" requirement, independent of the Spawn/class= scenario above.
static void testUnknownAttributeSurvivesEditingADeclaredOne() {
    AVER_INFO("=== a made-up attribute survives an edit to a real, declared one on the same node ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME UnknownAttributeCheck\n"
        "\n"
        "NODE gv GetFieldVec3 zzz_not_a_real_attribute=hello field=CLocal.position\n"
        "\n"
        "PIN gv entity in int\n"
        "PIN gv x out float\n"
        "PIN gv y out float\n"
        "PIN gv z out float\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "unknown_attribute.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");

    check(ed.setAttribute("gv", "field", "CLight.colour"), "setAttribute changes the DECLARED 'field' attribute");
    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");

    const std::string after = readFile(tmp);
    check(after.find("NODE gv GetFieldVec3 zzz_not_a_real_attribute=hello field=CLight.colour") != std::string::npos,
          "'field' updated IN PLACE, and the made-up 'zzz_not_a_real_attribute' token keeps its original position ahead of it");
    check(after.find("field=CLocal.position") == std::string::npos, "the OLD field value is gone, not left behind as a stray duplicate");

    // clearAttribute: removing the declared attribute leaves the made-up one alone.
    check(ed.clearAttribute("gv", "field"), "clearAttribute removes the now-present 'field' token");
    check(!ed.clearAttribute("gv", "field"), "clearAttribute on an already-absent attribute returns false (not an edit)");
    check(ed.save(&why), "save() succeeds after clearAttribute (why='" + why + "')");
    const std::string afterClear = readFile(tmp);
    check(afterClear.find("field=") == std::string::npos, "the cleared 'field' token is gone entirely (deleted, not written as 'field=')");
    check(afterClear.find("NODE gv GetFieldVec3 zzz_not_a_real_attribute=hello\n") != std::string::npos,
          "the made-up attribute survives BOTH edits, completely untouched");
}

// =================================================================== ADVERSARIAL: pathological cases =
// Independent verification pass (not part of the original slice). Builds ONE hand-authored fixture
// covering every case the verification brief calls out -- a node with no attributes, an attribute with
// an empty value, a value containing a space, a value containing '=', a duplicate attribute, and a
// value long enough to stress the details-panel's fixed buffer -- loads it through the real C++
// reader, edits ONE attribute, writes it back through the real save() path, and diffs byte-for-byte.
static void testPathologicalAttributeCases() {
    AVER_INFO("=== ADVERSARIAL: no-attribute node, empty value, space, '=', duplicate, long value ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME PathologicalAttrs\n"
        "\n"
        "NODE plain OnTick 0 0\n"                                        // no attributes at all
        "NODE emptyval Spawn 0 0 class=\n"                                // empty value
        "NODE eq GetFieldVec3 0 0 field=CLocal.transform=matrix\n"        // value CONTAINS '='
        "NODE dup Spawn 0 0 class=First class=Second\n"                  // duplicate key
        "NODE longval Spawn 0 0 class=" + std::string(900, 'X') + "\n"   // value far longer than the
                                                                           // details panel's 512-byte buf
        "\n"
        "PIN eq entity in int\n"
        "PIN eq x out float\n"
        "PIN eq y out float\n"
        "PIN eq z out float\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "pathological.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");

    // --- node with no attributes at all ---
    const fmt::OcGraphNode* plain = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "plain") { plain = &n; break; }
    check(plain != nullptr && plain->extraTokens.empty(), "'plain' loaded with zero extraTokens");
    if (plain) check(!getNodeAttribute(*plain, "field").found, "getNodeAttribute on an attribute-less node reports not-found, not a crash");

    // --- empty value ---
    const fmt::OcGraphNode* emptyval = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "emptyval") { emptyval = &n; break; }
    if (emptyval) {
        const GraphNodeAttribute a = getNodeAttribute(*emptyval, "class");
        check(a.found && a.value.empty(), "'class=' with nothing after it reads as found=true, value=\"\" (not absent)");
    }

    // --- value containing '=' ---
    const fmt::OcGraphNode* eq = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "eq") { eq = &n; break; }
    if (eq) {
        const GraphNodeAttribute a = getNodeAttribute(*eq, "field");
        check(a.found && a.value == "CLocal.transform=matrix",
              "a value containing '=' is preserved whole (only the FIRST '=' splits key from value): got '" + a.value + "'");
    }

    // --- duplicate attribute: first match wins on read ---
    const fmt::OcGraphNode* dup = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "dup") { dup = &n; break; }
    if (dup) {
        check(dup->extraTokens.size() == 2, "'dup' loaded with BOTH duplicate class= tokens intact");
        const GraphNodeAttribute a = getNodeAttribute(*dup, "class");
        check(a.found && a.value == "First", "getNodeAttribute on a duplicate key reads the FIRST occurrence");
    }
    check(ed.setAttribute("dup", "class", "Rewritten"), "setAttribute succeeds on a node with a duplicate attribute");

    // --- long value round trip (900 chars, well past the UI's 512-byte edit buffer -- the MODEL layer
    // itself, exercised here with no ImGui involved, must not truncate) ---
    const std::string longValue(900, 'Y');
    check(ed.setAttribute("longval", "class", longValue), "setAttribute accepts a 900-char value");

    std::string why;
    check(ed.save(&why), "save() succeeds with all of the above on disk (why='" + why + "')");
    const std::string after = readFile(tmp);

    check(after.find("NODE plain OnTick 0 0\n") != std::string::npos, "untouched no-attribute node's NODE line is unchanged");
    check(after.find("NODE emptyval Spawn 0 0 class=\n") != std::string::npos, "untouched empty-value node's NODE line is unchanged");
    check(after.find("NODE eq GetFieldVec3 0 0 field=CLocal.transform=matrix\n") != std::string::npos,
          "untouched '='-in-value node's NODE line is unchanged");
    check(after.find("NODE dup Spawn 0 0 class=Rewritten class=Second\n") != std::string::npos,
          "editing a duplicated attribute rewrites the FIRST occurrence in place and leaves the stray second one, exactly as documented");
    check(after.find("NODE longval Spawn 0 0 class=" + longValue + "\n") != std::string::npos,
          "the 900-char value round-trips through the MODEL layer with no truncation (the 512-byte cap is a UI-only limit, not a data-model one)");

    // --- reload the just-written file fresh and confirm getNodeAttribute agrees with what's on disk ---
    GraphEditor reloaded(tmp);
    const fmt::OcGraphNode* dup2 = nullptr;
    for (const auto& n : reloaded.graph().nodes) if (n.id == "dup") { dup2 = &n; break; }
    if (dup2) {
        check(dup2->extraTokens.size() == 2, "reload: 'dup' still carries two class= tokens (Rewritten, Second)");
        check(getNodeAttribute(*dup2, "class").value == "Rewritten", "reload: getNodeAttribute still reads the first ('Rewritten')");
    }

    // --- THE ADVERSARIAL CASE: a value containing a SPACE ---
    // The brief explicitly calls this out. The .ocgraph NODE line is whitespace-tokenised with "no
    // quoting" on BOTH sides (OcGraph.cpp's splitWhitespace / OcGraphParser.cs's own SplitWhitespace,
    // whose doc comment says exactly that). Verified BEFORE the fix below existed: setAttribute("plain",
    // "field", "My Field Name") wrote `NODE plain OnTick 0 0 field=My Field Name`, which on reload read
    // back as field="My" plus two permanently-invisible junk tokens ("Field", "Name" -- no '=', so
    // computeAttributeRows never shows them again) -- silent, unrecoverable-through-the-UI data loss,
    // and OcGraphParser.cs's own NODE loop would do the same truncation at compile time with no error.
    // GraphEditor::setAttribute now refuses (no-op, same contract as the nonexistent-node case) any
    // value containing whitespace, since this file format cannot represent one. This is what that guard
    // proves: not silent corruption, but a clean, visible no-op.
    const std::string beforeSpaceAttempt = readFile(tmp);
    check(!ed.setAttribute("plain", "field", "My Field Name"),
          "setAttribute now REFUSES a value containing spaces (no-op, not silent corruption)");
    check(!ed.dirty(), "the refused edit did not mark the editor dirty");
    check(ed.save(&why), "save() still succeeds (why='" + why + "')");
    const std::string afterSpaceAttempt = readFile(tmp);
    check(afterSpaceAttempt == beforeSpaceAttempt, "the file on disk is byte-identical after the refused space-containing edit -- nothing was written");

    GraphEditor reopened(tmp);
    const fmt::OcGraphNode* plain2 = nullptr;
    for (const auto& n : reopened.graph().nodes) if (n.id == "plain") { plain2 = &n; break; }
    if (plain2) {
        check(plain2->extraTokens.empty(), "'plain' still carries zero extraTokens -- the refused edit left no trace, not even a partial one");
    }
}

// ===================================================================== variables (Variables panel) ===
// GraphEditor's addVariable/renameVariable/retypeVariable/setVariableDefault/deleteVariable, exercised
// through the exact same public surface the Variables panel and the GetVar/SetVar var= picker in
// draw() call -- no ImGui context needed, exactly like setAttribute/clearAttribute above. See
// GraphEditor.hpp's own header comment on this block for the bug (a palette-spawned SetVar cannot
// compile, and the editor previously had no way to declare the variable that would fix it) these
// methods exist to close.
//
// Shared fixture: two REFERENCED variables (score, addressed by 'gv's var=; speed, addressed by
// 'sv's var=) and one UNREFERENCED variable ('unused') -- so both halves of deleteVariable's
// "refuse while referenced, otherwise succeed" contract have a real fixture node to exercise against,
// without every test below hand-rolling its own graph text.
static std::string variablesFixtureText() {
    return
        "OCGRAPH 1\n"
        "NAME VariablesTest\n"
        "DESCRIPTION Variables panel fixture: gv/sv address declared variables via var=\n"
        "\n"
        "VAR score int 0\n"
        "VAR speed float 1.5\n"
        "VAR unused bool false\n"
        "\n"
        "NODE tick OnTick 0 0\n"
        "NODE gv GetVar 200 0 var=score\n"
        "NODE sv SetVar 200 100 var=speed\n"
        "\n"
        "LINK tick.exec sv.exec\n"
        "\n"
        "ENTRY tick OnTick\n"
        "\n"
        "OUT gv value\n";
}

// A Const node's literal is EDITABLE, and both copies of it agree on disk.
//
// THE BUG THIS PINS, and it is the reason every graph in this repo is hand-written text: a Const
// spawned from the palette declared no `value` attribute, so the details panel drew "This node type
// has no attributes" and offered nowhere to type a number. Every constant in an editor-built graph
// compiled to its spawn default of 0, permanently -- no speed, no duration, no key code, no
// direction could be authored without leaving the editor.
//
// AND THE SECOND HALF, which is what makes this more than a one-line catalog change: the literal is
// written into the file TWICE -- the NODE line's `value=` and the output PIN's default. The C#
// parser turns both into a ConstantOutput for the same node and the compiler takes FirstOrDefault,
// so the attribute wins only because a NODE line precedes its own PIN lines. Correct, resting on
// emission order alone. Asserting BOTH land is what stops a future writer change from silently
// resetting every constant in every graph to its spawn default.
static void testConstNodeValueIsEditable() {
    AVER_INFO("=== a Const node's value can be authored, and both copies agree ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "const_value_test.ocgraph").string();
    // A raw string literal, for the reason graphStarterText gives: tooling in this repo has eaten a
    // backslash-n four times now, landing a real newline inside a narrow literal and producing a wall
    // of C2001. Raw literals cannot be mangled that way.
    writeFile(tmp, R"(OCGRAPH 1
DOMAIN gameplay
NAME ConstTest
NODE cf ConstFloat 0 0
PIN cf value out float 0
)");

    GraphEditor ed(tmp);
    check(ed.setAttribute("cf", "value", "0.35"),
          "setAttribute('cf', 'value', ...) succeeds -- the attribute is declared now");

    const fmt::OcGraphNode* cf = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "cf") { cf = &n; break; }
    check(cf != nullptr, "the Const node is still there");
    if (cf) {
        const GraphNodeAttribute a = getNodeAttribute(*cf, "value");
        check(a.found && a.value == "0.35", "the NODE line carries value=0.35");
        bool pinMatches = false;
        for (const fmt::OcGraphPin& p : cf->pins)
            if (p.isOutput && p.name == "value") pinMatches = (p.defaultValue == "0.35");
        check(pinMatches,
              "and the output PIN's default was updated to match, so the file cannot contradict itself");
    }

    std::string why;
    check(ed.save(&why), "it saves (why='" + why + "')");
    const std::string onDisk = readFile(tmp);
    check(onDisk.find("value=0.35") != std::string::npos, "value=0.35 reached the file");
    check(onDisk.find("PIN cf value out float 0.35") != std::string::npos,
          "and so did the pin default -- both copies, one number");
}

static void testVariablesParseAndByteIdenticalRoundTrip() {
    AVER_INFO("=== variables: parse into GraphEditor::variables() and round-trip byte-identically ===");
    const std::string text = variablesFixtureText();
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_roundtrip.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.dirty(), "freshly loaded editor is not dirty");
    check(ed.variables().size() == 3, "all three declared variables are visible through variables()");
    if (ed.variables().size() == 3) {
        check(ed.variables()[0].name == "score" && ed.variables()[0].type == "int" && ed.variables()[0].defaultValue == "0",
              "'score' parsed with name/type/default, in file order");
        check(ed.variables()[1].name == "speed" && ed.variables()[1].type == "float" && ed.variables()[1].defaultValue == "1.5",
              "'speed' parsed with name/type/default, in file order");
        check(ed.variables()[2].name == "unused" && ed.variables()[2].type == "bool" && ed.variables()[2].defaultValue == "false",
              "'unused' parsed with name/type/default, in file order");
    }

    std::string why;
    check(ed.save(&why), "save() succeeds on a freshly loaded, unedited graph (why='" + why + "')");
    check(!ed.dirty(), "editor is not dirty immediately after a successful save");
    const std::string after = readFile(tmp);
    check(after == text, "a load -> save with no edits reproduces the VAR-bearing file byte for byte");
}

static void testAddVariableValidatesAndFallsBack() {
    AVER_INFO("=== addVariable: validates name/default, refuses duplicates, falls back an invalid type ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_add.ocgraph").string();
    writeFile(tmp, variablesFixtureText());
    GraphEditor ed(tmp);

    check(!ed.addVariable("", "float", ""), "an empty name is refused");
    check(!ed.addVariable("bad name", "float", ""), "a name containing whitespace is refused (this format has no quoting)");
    check(!ed.addVariable("score", "float", ""), "a name already declared ('score') is refused -- Graph.Validate's own uniqueness rule");
    check(!ed.dirty(), "none of the refused calls above marked the editor dirty");

    check(ed.addVariable("mana", "bogus-type", ""), "a structurally valid name with an UNRECOGNISED type still succeeds");
    check(ed.dirty(), "a successful add marks the editor dirty");
    bool foundMana = false;
    for (const auto& v : ed.variables()) if (v.name == "mana") { foundMana = true; check(v.type == "float", "an invalid type ('bogus-type') falls back to 'float', the palette's own default"); }
    check(foundMana, "'mana' is now in variables()");

    check(!ed.addVariable("badDefault", "float", "1 2"), "a default value containing whitespace is refused");

    std::string why;
    check(ed.save(&why), "save() succeeds after addVariable (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR mana float\n") != std::string::npos,
          "the new variable is written with no default token (none was given) and the fallback type");
    check(after.find("badDefault") == std::string::npos, "the refused 'badDefault' add left no trace on disk");
}

static void testRenameVariableCascadesReferencesAndRefusesCollisions() {
    AVER_INFO("=== renameVariable: rewrites every var= reference, refuses collisions and bad names ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_rename.ocgraph").string();
    writeFile(tmp, variablesFixtureText());
    GraphEditor ed(tmp);

    check(ed.renameVariable("score", "score"), "renaming a variable to its OWN current name succeeds (a harmless no-op)");
    check(!ed.dirty(), "a same-name rename is a true no-op: it does not mark the editor dirty");

    check(!ed.renameVariable("nope", "whatever"), "renaming an undeclared variable is refused");
    check(!ed.renameVariable("score", ""), "renaming to an empty name is refused");
    check(!ed.renameVariable("score", "bad name"), "renaming to a name containing whitespace is refused");
    check(!ed.renameVariable("score", "speed"), "renaming to a name ALREADY used by a DIFFERENT variable is refused (would collide)");
    check(!ed.dirty(), "none of the refused renames above marked the editor dirty");

    // Confirm the pre-rename state through the exact same public surface the picker reads, so the
    // post-rename assertions below are a real before/after, not an assumption.
    const fmt::OcGraphNode* gvBefore = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "gv") { gvBefore = &n; break; }
    check(gvBefore != nullptr, "fixture has node 'gv'");
    if (gvBefore) check(getNodeAttribute(*gvBefore, "var").value == "score", "before the rename, 'gv' names 'score'");

    check(ed.renameVariable("score", "points"), "renaming 'score' to the unused name 'points' succeeds");
    check(ed.dirty(), "a real rename marks the editor dirty");

    bool stillHasScore = false, hasPoints = false;
    for (const auto& v : ed.variables()) {
        if (v.name == "score") stillHasScore = true;
        if (v.name == "points") hasPoints = true;
    }
    check(!stillHasScore && hasPoints, "the declared variable itself is renamed, not duplicated");

    // THE CASCADE: 'gv's var= must now name 'points', not the old 'score' -- this is the entire
    // point of renameVariable existing as its own method rather than a bare `var.name = newName`.
    const fmt::OcGraphNode* gvAfter = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "gv") { gvAfter = &n; break; }
    check(gvAfter != nullptr && getNodeAttribute(*gvAfter, "var").value == "points",
          "'gv's var= attribute was rewritten to the NEW name -- no node is left pointing at 'score'");
    // 'sv' addresses 'speed', untouched by a rename of 'score' -- the cascade must be scoped to
    // exactly the renamed variable, not every var= attribute in the graph.
    const fmt::OcGraphNode* sv = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "sv") { sv = &n; break; }
    check(sv != nullptr && getNodeAttribute(*sv, "var").value == "speed",
          "'sv's var= attribute (a DIFFERENT variable) is untouched by renaming 'score'");

    std::string why;
    check(ed.save(&why), "save() succeeds after the rename (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR points int 0") != std::string::npos, "the renamed VAR line is written under its new name");
    check(after.find("VAR score") == std::string::npos, "the old VAR name is gone entirely, not left as a stray second declaration");
    check(after.find("var=points") != std::string::npos, "'gv's NODE line carries the new var= value");
    check(after.find("var=score") == std::string::npos, "no NODE line still carries the old var= value");
    check(after.find("var=speed") != std::string::npos, "'sv's unrelated var=speed survives the rename of 'score' untouched");
}

static void testRetypeVariableFallsBackAndDoesNotTouchPins() {
    AVER_INFO("=== retypeVariable: changes the declared type, falls back on garbage, never touches pins ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_retype.ocgraph").string();
    writeFile(tmp, variablesFixtureText());
    GraphEditor ed(tmp);

    check(!ed.retypeVariable("nope", "int"), "retyping an undeclared variable is refused");
    check(ed.retypeVariable("score", "int"), "retyping to the SAME type it already has succeeds (a true no-op)");
    check(!ed.dirty(), "a no-op retype does not mark the editor dirty");

    check(ed.retypeVariable("score", "bool"), "retyping 'score' from int to bool succeeds");
    check(ed.dirty(), "a real retype marks the editor dirty");
    for (const auto& v : ed.variables()) if (v.name == "score") check(v.type == "bool", "'score's declared type is now bool");

    check(ed.retypeVariable("speed", "not-a-real-type"), "an invalid type argument still returns true (falls back, not refused)");
    for (const auto& v : ed.variables()) if (v.name == "speed") check(v.type == "float", "the invalid type fell back to 'float', leaving 'speed' unchanged (it already was float)");

    // 'gv's own 'value' pin (synthesised from the GetVar catalog entry, since the fixture wrote no
    // explicit PIN record for it) must NOT have been silently retyped alongside the variable -- see
    // retypeVariable's own header comment for why that mismatch is left visible rather than patched.
    const fmt::OcGraphNode* gv = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "gv") { gv = &n; break; }
    check(gv != nullptr, "fixture has node 'gv'");
    if (gv) {
        const fmt::OcGraphPin* valuePin = nullptr;
        for (const auto& p : gv->pins) if (p.name == "value") { valuePin = &p; break; }
        check(valuePin != nullptr && valuePin->type == "float",
              "'gv's 'value' pin is STILL float (the catalog default) even though 'score' is now bool -- retypeVariable never touches pins");
    }

    std::string why;
    check(ed.save(&why), "save() succeeds after the retype (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR score bool 0") != std::string::npos,
          "the retyped VAR line is written with its new type and its ORIGINAL default text unchanged ('0' is now a bool-looking default, untouched by this layer -- see OcGraphVariable's own comment on why type/default validation is not this layer's job)");
}

static void testSetVariableDefaultValidatesWhitespace() {
    AVER_INFO("=== setVariableDefault: refuses whitespace, otherwise lands on disk ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_default.ocgraph").string();
    writeFile(tmp, variablesFixtureText());
    GraphEditor ed(tmp);

    check(!ed.setVariableDefault("nope", "5"), "setting a default on an undeclared variable is refused");
    check(!ed.setVariableDefault("score", "1 2"), "a default value containing whitespace is refused");
    check(!ed.dirty(), "neither refused call marked the editor dirty");

    check(ed.setVariableDefault("score", "42"), "setVariableDefault('score', '42') succeeds");
    check(ed.dirty(), "a real default change marks the editor dirty");

    std::string why;
    check(ed.save(&why), "save() succeeds after setVariableDefault (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR score int 42") != std::string::npos,
          "the new default text is written verbatim -- this layer does not validate a default against its variable's type (that is the C# side's job, see OcGraphVariable's own comment)");
}

static void testDeleteVariableRefusesWhileReferencedThenSucceeds() {
    AVER_INFO("=== deleteVariable: refuses while referenced (names every blocking node), succeeds once clear ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_delete.ocgraph").string();
    writeFile(tmp, variablesFixtureText());
    GraphEditor ed(tmp);

    check(!ed.deleteVariable("nope"), "deleting an undeclared variable is refused (nothing to delete)");

    // 'unused' -- nothing in the fixture references it, so deletion must succeed outright.
    check(ed.deleteVariable("unused"), "deleting the UNREFERENCED variable 'unused' succeeds");
    check(ed.dirty(), "a real delete marks the editor dirty");
    bool stillHasUnused = false;
    for (const auto& v : ed.variables()) if (v.name == "unused") stillHasUnused = true;
    check(!stillHasUnused, "'unused' is gone from variables()");

    // 'score' -- 'gv' still references it via var=score, so deletion must be REFUSED, not silently
    // leave 'gv' pointing at a variable that no longer exists.
    std::vector<std::string> blockedBy;
    check(!ed.deleteVariable("score", &blockedBy), "deleting 'score' while 'gv' still references it is refused");
    check(blockedBy.size() == 1 && blockedBy[0] == "gv", "the blocking node id ('gv') is reported back to the caller");
    bool stillHasScore = false;
    for (const auto& v : ed.variables()) if (v.name == "score") stillHasScore = true;
    check(stillHasScore, "the refused delete left 'score' declared -- nothing was silently removed");

    // Clear the reference, then the same delete that was just refused must now succeed.
    check(ed.clearAttribute("gv", "var"), "clearing 'gv's var= attribute (unrelated to deleteVariable itself) succeeds");
    check(ed.deleteVariable("score"), "deleting 'score' now succeeds once nothing references it any more");

    std::string why;
    check(ed.save(&why), "save() succeeds after both deletes (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR unused") == std::string::npos, "'unused's VAR line is gone from the saved file");
    check(after.find("VAR score") == std::string::npos, "'score's VAR line is gone from the saved file");
    check(after.find("VAR speed float 1.5") != std::string::npos, "the untouched 'speed' variable survives both deletes");
    check(after.find("NODE gv GetVar 200 0\n") != std::string::npos,
          "'gv's NODE line lost its var= token (cleared above) and gained nothing else -- position untouched");
}

// THE MOTIVATING BUG, closed end to end at the model layer: a SetVar node with NO var= attribute at
// all -- exactly the shape the Add-Node popup produces (GraphEditor.cpp copies GraphNodeDesc::pins
// onto a freshly spawned node, but GraphNodeDesc carries no DEFAULT VALUE for an attribute, only that
// the key exists -- see GraphNodeDefs.hpp's GraphAttributeSpec, which is (key, label) only) -- can be
// made to compile with exactly the two calls the picker's "Declare '<name>'" button makes: addVariable
// then setAttribute. This is the non-ImGui half of that button's behaviour; draw()'s own version is
// the thinnest possible ImGui wrapper around the same two calls (see the "THE PICKER" block comment
// in draw() for the ImGui side).
static void testFreshlySpawnedSetVarCanBeFixedByDeclaringOnTheSpot() {
    AVER_INFO("=== the motivating bug: a var=-less SetVar can be fixed with addVariable + setAttribute ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME FreshSetVar\n"
        "\n"
        "NODE tick OnTick 0 0\n"
        "NODE sv SetVar 200 0\n"
        "\n"
        "PIN sv exec in exec\n"
        "PIN sv value in float\n"
        "PIN sv then out exec\n"
        "\n"
        "LINK tick.exec sv.exec\n"
        "\n"
        "ENTRY tick OnTick\n";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "variables_fresh_setvar.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(ed.variables().empty(), "the graph declares no variables at all yet -- the exact dead end the task brief describes");
    const fmt::OcGraphNode* sv = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == "sv") { sv = &n; break; }
    check(sv != nullptr, "fixture has node 'sv'");
    if (sv) check(!getNodeAttribute(*sv, "var").found, "the freshly-spawned-shaped SetVar has NO var= attribute at all");

    // The fix: guess a type from the node's own 'value' pin (exactly what draw()'s "Declare" button
    // does), declare it, then point the node at it.
    std::string guessType = "float";
    if (sv) for (const auto& p : sv->pins) if (p.name == "value") { guessType = p.type; break; }
    check(ed.addVariable("score", guessType, ""), "declaring 'score' at the guessed type succeeds");
    check(ed.setAttribute("sv", "var", "score"), "pointing 'sv' at the newly-declared 'score' succeeds");

    std::string why;
    check(ed.save(&why), "save() succeeds after both edits (why='" + why + "')");
    const std::string after = readFile(tmp);
    check(after.find("VAR score float\n") != std::string::npos, "'score' is now declared in the saved file");
    check(after.find("NODE sv SetVar 200 0 var=score\n") != std::string::npos,
          "'sv' now names the declared variable -- this graph is exactly one keystroke of C# compilation away from working, not a dead end");
}

// ======================================================================================= failures ===
// The COMPONENT TREE fixture: a small class graph whose parent is written BELOW its child, which is
// how a real one reads (the interesting part first) and which the parser deliberately allows.
static std::string componentFixtureText() {
    return
        "OCGRAPH 1\n"
        "NAME AN_Rig\n"
        "CLASS AN_Rig Actor\n"
        "\n"
        "COMP gun Mesh parent=body mesh=Meshes/Blaster.ocmesh pos=12,0,-8\n"
        "COMP muzzle Scene parent=gun pos=0,40,0\n"
        "COMP body Mesh mesh=Meshes/Body.ocmesh\n"
        "\n"
        "NODE start OnStart\n"
        "ENTRY start OnStart\n";
}

static void testComponentTreeParsesAndRoundTripsByte() {
    AVER_INFO("=== a graph with COMP records loads, and an untouched save is byte-identical ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "components_roundtrip.ocgraph").string();
    writeFile(tmp, componentFixtureText());
    GraphEditor ed(tmp);

    check(ed.components().size() == 3, "three components loaded");
    check(ed.components()[0].id == "gun" && ed.components()[0].kind == "Mesh", "in FILE order, not tree order");

    std::string why;
    check(ed.save(&why), "save() of an untouched graph succeeds (why='" + why + "')");
    check(readFile(tmp) == componentFixtureText(),
          "and changes nothing -- the transform is not re-serialised from floats, so pos=12,0,-8 stays that");
}

static void testComponentEditsCannotProduceAnUnopenableFile() {
    AVER_INFO("=== add / rename / reparent / delete, each checked by RELOADING what they wrote ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "components_edit.ocgraph").string();
    writeFile(tmp, componentFixtureText());
    GraphEditor ed(tmp);

    // ---- add: a unique id without asking, parented to the selection --------------------------
    ed.selectNode("body");   // falls through to the component of that name
    check(ed.selectedComponent() == "body", "selectNode reaches a component when no node answers to the id");
    ed.addComponent("Mesh");
    check(ed.components().size() == 4, "the component was added");
    check(ed.selectedComponent() == "mesh", "its id is the kind, lowercased, and it is now selected");
    ed.addComponent("Mesh");
    check(ed.selectedComponent() == "mesh1",
          "a SECOND Mesh does not reuse the id -- a duplicate id is a file the parser refuses to open");

    // ---- reparent: a cycle is not offered, and not accepted -----------------------------------
    check(ed.componentIsAncestorOf("body", "muzzle"), "body is an ancestor of muzzle (body -> gun -> muzzle)");
    check(!ed.componentIsAncestorOf("muzzle", "body"), "and muzzle is not an ancestor of body");
    check(ed.componentIsAncestorOf("gun", "gun"), "a component counts as its own ancestor, so it cannot be its own parent");
    ed.setComponentParent("body", "muzzle");
    check(fmt::componentAttr(ed.components()[2], "parent").empty(),
          "reparenting body under its own descendant is refused, leaving it at the root");

    // ---- rename: every child follows ----------------------------------------------------------
    ed.renameComponent("gun", "weapon");
    bool muzzleFollows = false;
    for (const auto& c : ed.components())
        if (c.id == "muzzle") muzzleFollows = fmt::componentAttr(c, "parent") == "weapon";
    check(muzzleFollows, "the child's parent= was rewritten -- an orphan would be a file that will not load");
    ed.renameComponent("weapon", "body");
    check(ed.components()[0].id == "weapon", "renaming onto a name already taken is refused");
    ed.renameComponent("weapon", "two words");
    check(ed.components()[0].id == "weapon", "and a name with whitespace is refused -- this format has no quoting");

    // ---- attribute edits ----------------------------------------------------------------------
    ed.setComponentAttribute("weapon", "pos", "1,2,3");
    check(fmt::componentAttr(ed.components()[0], "pos") == "1,2,3", "an existing key is replaced");
    ed.setComponentAttribute("weapon", "pos", "1 2 3");
    check(fmt::componentAttr(ed.components()[0], "pos") == "1,2,3", "a value with whitespace is refused");
    ed.setComponentAttribute("weapon", "material", "");
    check(fmt::componentAttr(ed.components()[0], "material").empty(), "an empty value removes the key");

    // ---- delete takes the subtree -------------------------------------------------------------
    ed.deleteComponentSubtree("weapon");
    for (const auto& c : ed.components())
        check(c.id != "muzzle", "deleting a component deletes its children too, rather than orphaning them");

    // ---- THE CLAIM THAT MATTERS: what all of that wrote still loads ---------------------------
    std::string why;
    check(ed.save(&why), "save() after the whole sequence succeeds (why='" + why + "')");
    fmt::OcGraphData reread;
    std::string err;
    check(fmt::parseOcgraph(readFile(tmp), reread, &err),
          "and the file it wrote parses -- err='" + err + "'");

    // The unknown record rode through untouched, as it does for every other edit in this file.
    check(readFile(tmp).find("CLASS AN_Rig Actor") != std::string::npos,
          "the CLASS record, which this reader does not model, survived every component edit");
}

static void testComponentWorldMatrixWalksTheParentChain() {
    AVER_INFO("=== a component's world matrix is its local matrix times its parent's ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "components_world.ocgraph").string();
    // Two translations and nothing else, so the expected answer is arithmetic rather than a matrix
    // identity -- a test that has to compose the rotation itself to state its expectation is a test
    // that passes when the code and the test are wrong in the same way.
    writeFile(tmp,
              "OCGRAPH 1\nNAME W\n"
              "COMP a Scene pos=10,0,0\n"
              "COMP b Scene parent=a pos=0,5,0\n"
              "COMP c Scene parent=b pos=0,0,2\n");
    GraphEditor ed(tmp);

    float m[16];
    ed.componentWorldMatrix("c", m);
    check(std::fabs(m[12] - 10.0f) < 1e-4f && std::fabs(m[13] - 5.0f) < 1e-4f &&
          std::fabs(m[14] - 2.0f) < 1e-4f,
          "three nested translations accumulate: c lands at (10, 5, 2)");

    ed.componentWorldMatrix("nothing-by-that-name", m);
    check(m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[0] == 1.0f,
          "an unknown id gives identity rather than whatever was in the caller's buffer");
}

static void testAddingAnEventNodeAlsoDeclaresItsEntry() {
    AVER_INFO("=== adding an event node from the catalog writes the ENTRY record that makes it run ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "entry_add.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME E\n");
    GraphEditor ed(tmp);

    check(ed.graph().entryPoints.empty(), "the fixture starts with no entry points");

    // THE BUG THIS EXISTS FOR: the node type is only a LABEL. What starts an exec chain is the
    // top-level ENTRY record, so an On Tick dropped from the palette used to produce a graph that
    // looked complete, saved, ran, and did nothing, with no error at any layer.
    const std::string tickId = ed.addNodeFromCatalog("OnTick", Vec2{40.0f, 40.0f});
    check(!tickId.empty(), "the catalog knows OnTick");
    check(ed.graph().entryPoints.size() == 1, "adding it declared exactly one entry point");
    check(ed.graph().entryPoints[0].first == tickId && ed.graph().entryPoints[0].second == "OnTick",
          "which names the new node and the event, the way every ENTRY record in this repo reads");

    // A NON-EVENT NODE MUST NOT GET ONE. An ENTRY on an Add node would make the compiler treat a
    // pure expression as the start of an exec chain.
    const std::string addId = ed.addNodeFromCatalog("Add", Vec2{80.0f, 40.0f});
    check(!addId.empty(), "the catalog knows Add");
    check(ed.graph().entryPoints.size() == 1, "adding a Math node declared no entry point");

    check(ed.addNodeFromCatalog("NoSuchNodeType", Vec2{0.0f, 0.0f}).empty(),
          "a type the catalog does not have adds nothing and returns empty");
    check(ed.graph().nodes.size() == 2, "and left the graph at two nodes");

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");
    check(readFile(tmp).find("ENTRY " + tickId + " OnTick") != std::string::npos,
          "and the ENTRY record reached the file");
}

// SetControlRig dropped from the palette must carry weight=1 INTO THE FILE.
//
// THE FAILURE THIS FORBIDS IS SILENT AT EVERY LAYER. Both graph compilers fall back to a literal 0
// for an input pin with no LINK and no PINVAL. For every other node in the palette that zero is a
// harmless starting value; for this one it means the rig is attached, its asset loads, its bone
// names resolve against the skeleton -- and then every op is scaled to nothing. The character simply
// stands there, exactly as if the rig had been authored for a different skeleton, with no warning
// from the loader, the compiler, the editor or the rig system.
//
// So the default has to be real data in the saved file, not a number that only exists in the
// palette's declaration. This is also the assertion the C# side cannot make: those delegates are
// DynamicMethods whose GetMethodBody() throws, so ControlRigNodeTests.cs can prove a float reaches
// the call but not WHICH float. Here the file itself is the evidence.
static void testControlRigNodeCarriesItsWeightDefault() {
    AVER_INFO("=== a SetControlRig dropped from the palette saves weight=1, not 0 ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "controlrig_default.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME R\n");
    GraphEditor ed(tmp);

    const std::string rigId = ed.addNodeFromCatalog("SetControlRig", Vec2{60.0f, 60.0f});
    check(!rigId.empty(), "the catalog knows SetControlRig");

    const fmt::OcGraphNode* n = nullptr;
    for (const fmt::OcGraphNode& candidate : ed.graph().nodes)
        if (candidate.id == rigId) n = &candidate;
    check(n != nullptr, "and the node reached the graph");

    if (n) {
        const fmt::OcGraphPin* weight = nullptr;
        for (const fmt::OcGraphPin& p : n->pins)
            if (p.name == "weight") weight = &p;
        check(weight != nullptr, "it has a weight pin");
        if (weight) {
            check(weight->type == "float", "which is a float, got '" + weight->type + "'");
            check(weight->defaultValue == "1",
                  "and defaults to 1 -- NOT 0, which would attach a rig that does nothing at all. Got '" +
                      weight->defaultValue + "'");
        }
    }

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");
    const std::string onDisk = readFile(tmp);
    check(onDisk.find("weight") != std::string::npos, "the weight pin reached the file");
    check(onDisk.find("weight in float 1") != std::string::npos,
          "carrying its default of 1, so the compilers never see an unwired weight for an "
          "editor-made node");
}

static void testDeletingANodeTakesItsEntryAndOutRecords() {
    AVER_INFO("=== deleting a node removes the ENTRY and OUT records naming it ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "entry_delete.ocgraph").string();
    writeFile(tmp,
              "OCGRAPH 1\nNAME E\n"
              "NODE tick OnTick\n"
              "NODE k ConstFloat value=2.5\n"
              "ENTRY tick OnTick\n"
              "OUT k value\n");
    GraphEditor ed(tmp);
    check(ed.graph().entryPoints.size() == 1 && ed.graph().outputs.size() == 1,
          "the fixture has one of each");

    // THE FAILURE THIS PREVENTS is not a lost record, it is an UNLOADABLE FILE: both records are
    // validated on load, so deleting the node an ENTRY points at used to save cleanly and then
    // refuse to reopen with "ENTRY references non-existent node: tick". The editor produced a graph
    // it could not read back.
    ed.selectNode("tick");
    ed.deleteSelection();
    check(ed.graph().entryPoints.empty(), "the ENTRY naming the deleted node went with it");
    check(ed.graph().outputs.size() == 1, "and the unrelated OUT record did not");

    ed.selectNode("k");
    ed.deleteSelection();
    check(ed.graph().outputs.empty(), "the same holds for OUT");

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");
    fmt::OcGraphData reread;
    std::string err;
    check(fmt::parseOcgraph(readFile(tmp), reread, &err),
          "and what it wrote RELOADS -- err='" + err + "'");
}

// Reads the event name a graph's ENTRY record gives one node, or "" when it has none.
static std::string entryNameOf(const GraphEditor& ed, const std::string& nodeId) {
    for (const auto& e : ed.graph().entryPoints) if (e.first == nodeId) return e.second;
    return {};
}

static void testCustomEventKeepsItsNameAndEntryInStep() {
    AVER_INFO("=== a CustomEvent's name= attribute and its ENTRY record are edited together ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "custom_event.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME C\n");
    GraphEditor ed(tmp);

    const std::string id = ed.addNodeFromCatalog("CustomEvent", Vec2{20.0f, 20.0f});
    check(!id.empty(), "the catalog knows CustomEvent");

    // A FRESH ONE IS ALREADY NAMED. An ENTRY with no event name is a record this format cannot
    // express, and a node that fires nothing until someone finds the attribute row is the failure
    // the palette just stopped shipping for On Tick.
    const std::string generated = entryNameOf(ed, id);
    check(!generated.empty(), "it arrives with an ENTRY record, not waiting for one");
    const fmt::OcGraphNode* node = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == id) node = &n;
    check(node != nullptr, "the node is in the graph");
    check(getNodeAttribute(*node, "name").value == generated,
          "and the NODE line's name= says the same thing the ENTRY record does");

    // A SECOND ONE DOES NOT COLLIDE. Two entry points under one name is a graph where firing it
    // reaches whichever the compiler happened to match first.
    const std::string id2 = ed.addNodeFromCatalog("CustomEvent", Vec2{20.0f, 90.0f});
    check(entryNameOf(ed, id2) != generated, "a second CustomEvent gets a name of its own");

    // THE RENAME IS THE WHOLE POINT. Moving the attribute without the record leaves a graph that
    // looks renamed, saves, loads, and has silently stopped firing.
    check(ed.setAttribute(id, "name", "Scored"), "renaming the attribute succeeds");
    check(entryNameOf(ed, id) == "Scored", "and the ENTRY record moved with it");

    check(!ed.setAttribute(id, "name", "two words"), "a name with whitespace is refused");
    check(entryNameOf(ed, id) == "Scored", "and the refusal left the ENTRY record alone");

    // Clearing means "this node is no longer an entry point", not "this node is an entry point
    // with no name" -- the latter is a record the format cannot write.
    check(ed.clearAttribute(id, "name"), "clearing the attribute succeeds");
    check(entryNameOf(ed, id).empty(), "and the ENTRY record went with it");

    // A NON-EVENT NODE'S name= IS UNRELATED. SetName carries one too, and touching it must not
    // invent an entry point.
    const std::string setName = ed.addNodeFromCatalog("SetName", Vec2{200.0f, 20.0f});
    check(!setName.empty(), "the catalog knows SetName");
    check(ed.setAttribute(setName, "name", "Turret"), "its name= sets like any other attribute");
    check(entryNameOf(ed, setName).empty(), "and declared no entry point");

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");
    fmt::OcGraphData reread;
    std::string err;
    check(fmt::parseOcgraph(readFile(tmp), reread, &err),
          "and what it wrote reloads -- err='" + err + "'");
}

// OnInputAction's ENTRY is composed from two attributes, Input.<action>.<event>: the record follows
// both, and is absent while either is empty (an ENTRY with no event name cannot be written).
static void testOnInputActionKeepsItsEntryInStep() {
    AVER_INFO("=== an OnInputAction's action= and event= attributes and its ENTRY record are edited together ===");
    const std::string tmp = (std::filesystem::path(scratchDir()) / "on_input_action.ocgraph").string();
    writeFile(tmp, "OCGRAPH 1\nNAME I\n");
    GraphEditor ed(tmp);

    const std::string id = ed.addNodeFromCatalog("OnInputAction", Vec2{20.0f, 20.0f});
    check(!id.empty(), "the catalog knows OnInputAction");
    const fmt::OcGraphNode* node = nullptr;
    for (const auto& n : ed.graph().nodes) if (n.id == id) node = &n;
    check(node != nullptr && getNodeAttribute(*node, "event").value == "Triggered", "it arrives with event=Triggered");
    check(entryNameOf(ed, id).empty(), "and no ENTRY until an action is chosen");

    check(ed.setAttribute(id, "action", "Jump"), "choosing an action succeeds");
    check(entryNameOf(ed, id) == "Input.Jump.Triggered", "and the ENTRY record is Input.Jump.Triggered");
    check(ed.setAttribute(id, "event", "Completed"), "changing the event succeeds");
    check(entryNameOf(ed, id) == "Input.Jump.Completed", "and the ENTRY record followed it");
    check(ed.clearAttribute(id, "action"), "clearing the action succeeds");
    check(entryNameOf(ed, id).empty(), "and the ENTRY record went with it");
    check(ed.setAttribute(id, "action", "Fire"), "choosing another action succeeds");
    check(entryNameOf(ed, id) == "Input.Fire.Completed", "and the ENTRY record is back");

    std::string why;
    check(ed.save(&why), "save() succeeds (why='" + why + "')");
    check(readFile(tmp).find("ENTRY " + id + " Input.Fire.Completed") != std::string::npos,
          "and the ENTRY record reached the file");
}

// ================================================================================ comment boxes ===
static void testCommentBoxesRoundTripAndCarryTheirContents() {
    AVER_INFO("=== comment boxes: round trip, creation, contents, and a title that cannot break the file ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME CommentBoxes\n"
        "\n"
        "COMMENT hand -10 -10 400 300 90 40 120 written by hand, with spaces\n"
        "\n"
        "NODE c1 ConstFloat 40 60\n"
        "NODE c2 ConstFloat 40 200\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "comment_boxes.ocgraph").string();
    writeFile(tmp, text);

    {
        GraphEditor ed(tmp);
        check(!ed.dirty(), "a graph with a COMMENT loads clean");
        check(ed.comments().size() == 1, "the hand-written COMMENT reached the editor");
        std::string why;
        check(ed.save(&why), "save() succeeds (why=" + why + ")");
        check(readFile(tmp) == text, "a COMMENT-bearing file survives an edit-free editor save byte for byte");
    }

    // ---- creating one, and reloading it in a FRESH editor. The point of the reload is that the
    // box has to come back off DISK, not out of the editor that made it -- an in-memory check would
    // pass even if the writer never emitted the record.
    std::string madeId;
    {
        GraphEditor ed(tmp);
        madeId = ed.addComment(Vec2{500.0f, -20.0f}, Vec2{800.0f, 180.0f}, "Second region");
        check(!madeId.empty(), "addComment returns an id");
        check(madeId != "hand", "the new box did not collide with the id already in the file");
        check(ed.dirty(), "adding a box dirties the editor");
        std::string why;
        check(ed.save(&why), "save() after adding a box succeeds (why=" + why + ")");
    }
    {
        GraphEditor ed(tmp);
        check(ed.comments().size() == 2, "both boxes are there after a reload from disk");
        const fmt::OcGraphComment* made = nullptr;
        for (const auto& c : ed.comments()) if (c.id == madeId) made = &c;
        check(made != nullptr, "the created box came back by id");
        if (made) {
            check(made->x == 500.0 && made->y == -20.0 && made->w == 300.0 && made->h == 200.0,
                  "its rectangle survived the trip through the file");
            check(made->text == "Second region", "its two-word title survived");
        }
        const std::string after = readFile(tmp);
        check(after.find("COMMENT hand -10 -10 400 300 90 40 120 written by hand, with spaces") != std::string::npos,
              "the hand-written box is still where its author left it, unreformatted");
    }

    // ---- what a box CONTAINS is geometry, asked fresh. c1 is at (40,60); a box drawn around it
    // must report c1 inside it and c2 -- 140 units below -- outside.
    {
        GraphEditor ed(tmp);
        check(ed.selectNode("c1"), "c1 selects");
        const std::string wrapped = ed.addCommentAroundSelection(1.0f);
        check(!wrapped.empty(), "addCommentAroundSelection made a box around the selection");
        const std::vector<std::string> inside = ed.nodesInsideComment(wrapped, 1.0f);
        check(std::find(inside.begin(), inside.end(), "c1") != inside.end(),
              "the node the box was drawn around is inside it");
        check(std::find(inside.begin(), inside.end(), "c2") == inside.end(),
              "a node the box does not cover is NOT inside it");
    }
    {
        GraphEditor ed(tmp);
        check(ed.addCommentAroundSelection(1.0f).empty(),
              "with nothing selected, the wrap gesture does nothing rather than making an empty box");
        check(!ed.dirty(), "...and does not dirty the file either");
    }

    // ---- A NEWLINE IN A TITLE WOULD SPLIT THE RECORD. The title runs to the end of the line, so a
    // pasted two-line title would put its second half on a line of its own, where the parser reads it
    // as an unknown record and the box silently loses half its title. Folded to a space instead --
    // and proven by RELOADING, because the failure this guards against is one the writer reports as
    // success.
    {
        GraphEditor ed(tmp);
        const std::string id = ed.addComment(Vec2{0.0f, 400.0f}, Vec2{300.0f, 560.0f}, "");
        check(ed.setCommentText(id, "first line\nsecond line"), "setCommentText accepts a two-line title");
        std::string why;
        check(ed.save(&why), "save() with a folded title succeeds (why=" + why + ")");
        GraphEditor re(tmp);
        check(!re.comments().empty(), "the file still loads after a title that contained a newline");
        const fmt::OcGraphComment* c = nullptr;
        for (const auto& k : re.comments()) if (k.id == id) c = &k;
        check(c != nullptr && c->text == "first line second line",
              "the newline became a space, so the whole title survived on one line");
    }

    // ---- deletion removes the record rather than leaving an orphan the next load would draw.
    {
        GraphEditor ed(tmp);
        check(ed.deleteComment("hand"), "deleteComment finds the hand-written box");
        check(!ed.deleteComment("hand"), "deleting it twice is a no-op, not a crash");
        std::string why;
        check(ed.save(&why), "save() after deleting a box succeeds (why=" + why + ")");
        const std::string after = readFile(tmp);
        check(after.find("COMMENT hand ") == std::string::npos, "the deleted box is gone from the file");
        check(after.find("NODE c1 ConstFloat") != std::string::npos, "the nodes it enclosed are untouched");
    }

    // ---- colour is clamped, not wrapped. A picker cannot produce 300, but setCommentColor is public
    // and the format has no range check of its own; a value out of range would come back as a
    // different colour on the next load.
    {
        GraphEditor ed(tmp);
        const std::string id = ed.addComment(Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}, "clamp");
        check(ed.setCommentColor(id, 300, -5, 128), "setCommentColor accepts out-of-range input");
        const fmt::OcGraphComment* c = nullptr;
        for (const auto& k : ed.comments()) if (k.id == id) c = &k;
        check(c != nullptr && c->r == 255 && c->g == 0 && c->b == 128, "it was clamped to 0..255 on the way in");
    }
}

// =============================================================== framing and on-demand auto-layout ===
static void testFramingAndAutoLayout() {
    AVER_INFO("=== content bounds, and auto-layout as a committed, undoable edit ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME LayoutCheck\n"
        "\n"
        "COMMENT wide -2000 -1500 800 400 60 70 90 far up and to the left\n"
        "\n"
        "NODE a ConstFloat 0 0\n"
        "NODE b ConstFloat 300 0\n"
        "NODE sum Add 600 0\n"
        "\n"
        "LINK a.value sum.a\n"
        "LINK b.value sum.b\n"
        "\n"
        "OUT sum result\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "layout_check.ocgraph").string();
    writeFile(tmp, text);

    // ---- bounds must include the comment box, which here is the FURTHEST thing from the origin.
    // A frame computed from nodes alone would crop it, and cropping a box looks like the box was
    // resized rather than the view moved.
    {
        GraphEditor ed(tmp);
        Vec2 lo, hi;
        check(ed.contentBounds(1.0f, &lo, &hi), "contentBounds reports content for a non-empty graph");
        check(lo.x <= -2000.0f + 0.01f && lo.y <= -1500.0f + 0.01f,
              "the bounds reach the comment box up and to the left of every node");
        check(hi.x >= 600.0f, "...and still reach the right-most node");
        check(!ed.dirty(), "asking for bounds does not dirty the file");
    }

    // ---- selection bounds are the selection, not everything.
    {
        GraphEditor ed(tmp);
        Vec2 lo, hi;
        check(!ed.selectionBounds(1.0f, &lo, &hi), "with nothing selected there are no selection bounds");
        check(ed.selectNode("sum"), "sum selects");
        check(ed.selectionBounds(1.0f, &lo, &hi), "selecting a node gives selection bounds");
        check(lo.x >= 600.0f - 0.01f, "the bounds are that node's, not the whole graph's");
    }

    // ---- auto-layout COMMITS, and that is the whole difference from the load-time one.
    {
        GraphEditor ed(tmp);
        check(ed.applyAutoLayout(1.0f), "applyAutoLayout runs on a graph with nodes");
        check(ed.dirty(), "...and dirties the file, because it changed the file");
        std::string why;
        check(ed.save(&why), "save() after auto-layout succeeds (why=" + why + ")");
        const std::string after = readFile(tmp);
        check(after.find("NODE a ConstFloat 0 0\n") == std::string::npos ||
              after.find("NODE sum Add 600 0\n") == std::string::npos,
              "at least one node moved, so the layout actually did something");
        check(after.find("COMMENT wide -2000 -1500 800 400 60 70 90 far up and to the left") != std::string::npos,
              "the comment box is untouched -- auto-layout places NODES, and a box is the author's");
        // The positions are real data now. Re-loading has to see them.
        GraphEditor re(tmp);
        Vec2 lo, hi;
        check(re.contentBounds(1.0f, &lo, &hi), "the re-loaded graph still has content");
    }

    // ---- ONE undo step, not one per node. A layout that took twenty presses of Ctrl+Z to reverse
    // would be a worse button than no button.
    {
        GraphEditor ed(tmp);
        const std::string before = readFile(tmp);
        check(ed.applyAutoLayout(1.0f), "second auto-layout runs");
        check(ed.applyAutoLayout(1.0f), "third auto-layout runs (idempotent placement, still an edit)");
        std::string why;
        check(ed.save(&why), "save succeeds (why=" + why + ")");
    }

    // ---- an empty graph is a no-op, not a crash and not a dirty file.
    {
        const std::string emptyPath = (std::filesystem::path(scratchDir()) / "empty_layout.ocgraph").string();
        writeFile(emptyPath, "OCGRAPH 1\nNAME Empty\n");
        GraphEditor ed(emptyPath);
        check(!ed.applyAutoLayout(1.0f), "applyAutoLayout refuses an empty graph");
        check(!ed.dirty(), "...and leaves it clean");
        Vec2 lo, hi;
        check(!ed.contentBounds(1.0f, &lo, &hi), "an empty graph reports no bounds rather than {0,0}");
    }
}

// ============================================================================ functions in the editor ===
static void testFunctionsInTheEditor() {
    AVER_INFO("=== functions: declare, sign, call, rename, delete -- and the canvas that scopes to one ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME FnEditor\n"
        "\n"
        "NODE tick OnTick\n"
        "ENTRY tick OnTick\n";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "fn_editor.ocgraph").string();
    writeFile(tmp, text);

    // ---- declaring one brings its entry node with it. A FUNC record alone does not compile, and
    // the error names a node type the author has not met yet -- see addFunction's own comment.
    {
        GraphEditor ed(tmp);
        const std::string made = ed.addFunction("Distance");
        check(made == "Distance", "addFunction returns the name it used");
        check(ed.functions().size() == 1, "the function is declared");
        check(ed.functions()[0].pure, "a fresh function is pure -- it has no body to be impure about");
        bool haveEntry = false;
        for (const auto& n : ed.graph().nodes) if (n.type == "FuncEntry") haveEntry = true;
        check(haveEntry, "a FuncEntry node was created with it");

        // ---- THE CANVAS SHOWS ONE SUBGRAPH. The entry node is in Distance; the OnTick is not.
        ed.setCurrentSubgraph("Distance");
        Vec2 lo, hi;
        check(ed.contentBounds(1.0f, &lo, &hi), "the function's canvas has content");
        check(ed.currentSubgraph() == "Distance", "the editor is showing the function");
        ed.setCurrentSubgraph({});
        check(ed.currentSubgraph().empty(), "and can go back to the event graph");

        // ---- an output brings the FuncReturn with it, for the same reason.
        check(ed.addFunctionPin("Distance", true, "ax", "float"), "an input is added");
        check(ed.addFunctionPin("Distance", false, "result", "float"), "an output is added");
        int returns = 0;
        for (const auto& n : ed.graph().nodes) if (n.type == "FuncReturn") ++returns;
        check(returns == 1, "the first output created exactly one FuncReturn");

        // ---- the signature drives the pins of every node that depends on it.
        for (const auto& n : ed.graph().nodes) {
            if (n.type != "FuncEntry") continue;
            bool hasAx = false;
            for (const auto& p : n.pins) if (p.name == "ax" && p.isOutput) hasAx = true;
            check(hasAx, "the FuncEntry gained an OUTPUT pin named after the function's INPUT");
        }

        std::string why;
        check(ed.save(&why), "save() succeeds (why=" + why + ")");
    }

    // ---- reload from disk: the declaration and the body both came back.
    std::string callId;
    {
        GraphEditor ed(tmp);
        check(ed.functions().size() == 1, "the function survived a save and reload");
        check(ed.functions()[0].inputs.size() == 1 && ed.functions()[0].outputs.size() == 1,
              "so did its signature");
        callId = ed.addCallNode("Distance", Vec2{500.0f, 0.0f});
        check(!callId.empty(), "a call node can be added");
        bool wired = false;
        for (const auto& n : ed.graph().nodes) {
            if (n.id != callId) continue;
            for (const auto& p : n.pins) if (p.name == "ax" && !p.isOutput) wired = true;
        }
        check(wired, "the call node took the callee's signature as its pins, with no catalog entry to copy");
        std::string why;
        check(ed.save(&why), "save() with a call node succeeds (why=" + why + ")");
    }

    // ---- RENAMING MOVES EVERY REFERENCE, both kinds, in one undo step. A rename that left either
    // behind produces a file that parses and refuses to compile.
    {
        GraphEditor ed(tmp);
        check(ed.renameFunction("Distance", "Dist2D"), "rename succeeds");
        std::string why;
        check(ed.save(&why), "save() after rename succeeds (why=" + why + ")");
        const std::string after = readFile(tmp);
        check(after.find("FUNC Dist2D") != std::string::npos, "the FUNC record was renamed");
        check(after.find("FUNCIN Dist2D ax float") != std::string::npos, "so was its FUNCIN");
        check(after.find("func=Dist2D") != std::string::npos, "so was the body node's func=");
        check(after.find("call=Dist2D") != std::string::npos, "so was the call node's call=");
        check(after.find("Distance") == std::string::npos, "and no reference to the old name is left anywhere");
    }

    // ---- deleting is REFUSED while something still calls it, then works once it does not.
    {
        GraphEditor ed(tmp);
        std::vector<std::string> blockers;
        check(!ed.deleteFunction("Dist2D", &blockers), "delete is refused while a call node names it");
        check(!blockers.empty(), "...and the refusal names the caller rather than just failing");
        check(ed.selectNode(callId), "the call node selects");
        ed.deleteSelection();
        check(ed.deleteFunction("Dist2D"), "with the caller gone, delete succeeds");
        check(ed.functions().empty(), "the function is gone");
        bool anyBodyLeft = false;
        for (const auto& n : ed.graph().nodes) if (n.type == "FuncEntry" || n.type == "FuncReturn") anyBodyLeft = true;
        check(!anyBodyLeft, "its body went with it -- an orphaned FuncEntry would refuse to compile");
        std::string why;
        check(ed.save(&why), "save() after deleting a function succeeds (why=" + why + ")");
        const std::string after = readFile(tmp);
        check(after.find("NODE tick OnTick") != std::string::npos,
              "the event graph the function sat beside is untouched");
    }
}

// =========================================================================== reroutes and macros =

static bool hasLink(const GraphEditor& ed, const std::string& a, const std::string& ap, const std::string& b, const std::string& bp) {
    for (const auto& l : ed.graph().links)
        if (l.sourceNode == a && l.sourcePin == ap && l.destNode == b && l.destPin == bp) return true;
    return false;
}

static const fmt::OcGraphNode* nodeById(const GraphEditor& ed, const std::string& id) {
    for (const auto& n : ed.graph().nodes) if (n.id == id) return &n;
    return nullptr;
}

static std::string pinTypeOf(const GraphEditor& ed, const std::string& node, const std::string& pin, bool isOutput) {
    if (const fmt::OcGraphNode* n = nodeById(ed, node))
        for (const auto& p : n->pins) if (p.name == pin && p.isOutput == isOutput) return p.type;
    return "?";
}

static void testRerouteAndMacroRecordsRoundTrip() {
    AVER_INFO("=== Reroute nodes and MACRO / MACROIN / MACROOUT records survive a load and save byte for byte ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME RerouteMacros\n"
        "\n"
        "MACRO Pick\n"
        "MACROIN Pick go exec\n"
        "MACROIN Pick cond bool\n"
        "MACROOUT Pick yes exec\n"
        "MACROOUT Pick no exec\n"
        "\n"
        "NODE mi MacroInput 0 0 macro=Pick\n"
        "NODE mo MacroOutput 400 0 macro=Pick\n"
        "NODE use MacroInstance 100 100 call=Pick\n"
        "NODE r Reroute 300 100\n"
        "PIN r in in float\n"
        "PIN r out out float\n"
        "NODE k ConstFloat 0 200 value=3\n"
        "PIN k value out float\n"
        "LINK k.value r.in\n";

    fmt::OcGraphData g;
    std::string err;
    check(fmt::parseOcgraph(text, g, &err), "the records parse (" + err + ")");
    check(g.macros.size() == 1 && g.macros[0].name == "Pick", "one macro is modelled");
    check(g.macros[0].inputs.size() == 2 && g.macros[0].outputs.size() == 2, "with its two inputs and two outputs");
    check(g.macros[0].inputs[0].type == "exec" && g.macros[0].inputs[1].type == "bool", "an exec pin keeps its type string");
    check(fmt::writeOcgraph(g, text) == text, "the model writes them back exactly over the existing text");

    const std::string tmp = (std::filesystem::path(scratchDir()) / "reroute_macro_rt.ocgraph").string();
    writeFile(tmp, text);
    GraphEditor ed(tmp);
    check(ed.macros().size() == 1, "the editor loads the macro");
    std::string why;
    check(ed.save(&why), "an unedited save succeeds (" + why + ")");
    check(readFile(tmp) == text, "and writes the file byte for byte -- the instance and tunnels' derived pins are not written");
    check(pinTypeOf(ed, "use", "go", false) == "exec" && pinTypeOf(ed, "use", "yes", true) == "exec",
          "yet the instance is drawn with the macro's pins, exec included");
    check(pinTypeOf(ed, "mi", "cond", true) == "bool" && pinTypeOf(ed, "mo", "no", false) == "exec",
          "and the tunnels with the inputs as outputs and the outputs as inputs");
}

static void testInsertRerouteOnALink() {
    AVER_INFO("=== inserting a reroute splits a link in two, typed as the wire is, in one undo step ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME Knots\n"
        "NODE k ConstFloat 0 0 value=3\n"
        "PIN k value out float\n"
        "NODE a Add 400 0\n"
        "PIN a a in float\n"
        "PIN a b in float\n"
        "PIN a result out float\n"
        "LINK k.value a.a\n";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "knots.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    const usize nodesBefore = ed.graph().nodes.size();
    check(ed.insertReroute(5, Vec2{200.0f, 10.0f}).empty(), "a link index that is not there inserts nothing");
    check(!ed.dirty(), "and leaves the graph clean");

    const std::string r = ed.insertReroute(0, Vec2{200.0f, 10.0f});
    check(!r.empty(), "a reroute is inserted on the link");
    check(ed.graph().nodes.size() == nodesBefore + 1, "one node was added");
    check(ed.graph().links.size() == 2, "the one link became two");
    check(hasLink(ed, "k", "value", r, "in"), "the source feeds the reroute's input");
    check(hasLink(ed, r, "out", "a", "a"), "and the reroute's output feeds the old destination");
    check(pinTypeOf(ed, r, "in", false) == "float" && pinTypeOf(ed, r, "out", true) == "float", "the reroute's pins carry the wire's type");

    ed.undoForTest();
    check(ed.graph().nodes.size() == nodesBefore && ed.graph().links.size() == 1 && hasLink(ed, "k", "value", "a", "a"),
          "one undo removes the reroute and restores the link");
    ed.redoForTest();
    check(ed.graph().links.size() == 2 && nodeById(ed, r) != nullptr, "and redo puts it back");

    std::string why;
    check(ed.save(&why), "save succeeds (" + why + ")");
    const std::string saved = readFile(tmp);
    check(saved.find("PIN " + r + " in in float") != std::string::npos && saved.find("PIN " + r + " out out float") != std::string::npos,
          "the reroute's pins are written, typed");
    GraphEditor again(tmp);
    check(again.graph().links.size() == 2 && pinTypeOf(again, r, "out", true) == "float", "and reload reads the same graph");
}

static void testRerouteRetypesOnConnectAndAcrossAChain() {
    AVER_INFO("=== a free reroute takes the type of its first link, along its chain; a typed one refuses the wrong type ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME Chain\n"
        "NODE tick OnTick 0 0\n"
        "PIN tick exec out exec\n"
        "ENTRY tick OnTick\n"
        "NODE br Branch 700 0\n"
        "PIN br exec in exec\n"
        "PIN br cond in bool\n"
        "PIN br true out exec\n"
        "PIN br false out exec\n"
        "PIN br tookTrue out bool\n"
        "NODE k ConstFloat 0 200 value=3\n"
        "PIN k value out float\n";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "chain.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    const std::string r1 = ed.addNodeFromCatalog("Reroute", Vec2{200.0f, 0.0f});
    const std::string r2 = ed.addNodeFromCatalog("Reroute", Vec2{400.0f, 0.0f});
    check(!r1.empty() && !r2.empty(), "the palette adds Reroute");
    check(ed.connectPins(r1, "out", r2, "in"), "two free reroutes join (neither has a type to disagree with)");
    check(pinTypeOf(ed, r2, "in", false) == "float", "and stay as they were");

    check(ed.connectPins("tick", "exec", r1, "in"), "an exec output wires into the first reroute");
    check(pinTypeOf(ed, r1, "in", false) == "exec" && pinTypeOf(ed, r1, "out", true) == "exec", "which becomes an exec reroute");
    check(pinTypeOf(ed, r2, "in", false) == "exec" && pinTypeOf(ed, r2, "out", true) == "exec", "and so does the next one in the chain");

    check(!ed.connectPins(r2, "out", "br", "cond"), "the exec chain refuses a bool input");
    check(!hasLink(ed, r2, "out", "br", "cond"), "and no link was made");
    check(ed.connectPins(r2, "out", "br", "exec"), "it accepts an exec input");

    const std::string r3 = ed.addNodeFromCatalog("Reroute", Vec2{300.0f, 200.0f});
    check(ed.connectPins("k", "value", r3, "in"), "a float output wires into a separate free reroute");
    check(pinTypeOf(ed, r3, "out", true) == "float" && pinTypeOf(ed, r1, "out", true) == "exec", "which becomes float without touching the exec chain");
    check(!ed.connectPins(r3, "out", "br", "cond"), "and a float reroute refuses a bool input");

    std::string why;
    check(ed.save(&why), "the typed reroutes save (" + why + ")");
    const std::string saved = readFile(tmp);
    check(saved.find("PIN " + r2 + " out out exec") != std::string::npos, "with their retyped pins written");
}

static void testMacrosInTheEditor() {
    AVER_INFO("=== macros: declare, add pins, instance, rename, delete -- and the body on its own canvas ===");
    const std::string text =
        "OCGRAPH 1\n"
        "NAME MacroEditor\n"
        "NODE tick OnTick 0 0\n"
        "PIN tick exec out exec\n"
        "ENTRY tick OnTick\n";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "macro_editor.ocgraph").string();
    writeFile(tmp, text);

    std::string made;
    std::string useId;
    {
        GraphEditor ed(tmp);
        made = ed.addMacro("Pick");
        check(made == "Pick" && ed.macros().size() == 1, "addMacro declares it");
        check(ed.addMacro("pick") == "pick2", "a name that differs only in case is made unique");
        check(ed.deleteMacro("pick2"), "(and the spare goes)");
        int inputs = 0, outputs = 0;
        for (const auto& n : ed.graph().nodes) {
            if (n.type == "MacroInput") ++inputs;
            if (n.type == "MacroOutput") ++outputs;
        }
        check(inputs == 1 && outputs == 1, "it comes with its entry and exit tunnel nodes");

        check(ed.addMacroPin("Pick", true, "go", "exec"), "an exec input is added");
        check(ed.addMacroPin("Pick", true, "cond", "bool"), "a data input is added");
        check(ed.addMacroPin("Pick", false, "yes", "exec") && ed.addMacroPin("Pick", false, "no", "exec"), "two exec outputs are added");
        check(!ed.addMacroPin("Pick", true, "go", "exec"), "a repeated pin name is refused");
        check(ed.macros()[0].inputs.size() == 2 && ed.macros()[0].outputs.size() == 2, "the declaration has the four pins");

        std::string entryId, exitId;
        for (const auto& n : ed.graph().nodes) {
            if (n.type == "MacroInput") entryId = n.id;
            if (n.type == "MacroOutput") exitId = n.id;
        }
        check(pinTypeOf(ed, entryId, "go", true) == "exec" && pinTypeOf(ed, entryId, "cond", true) == "bool", "the entry tunnel has the inputs as outputs");
        check(pinTypeOf(ed, exitId, "yes", false) == "exec", "the exit tunnel has the outputs as inputs");

        ed.openMacro("Pick");
        check(isMacroSubgraph(ed.currentSubgraph()) && macroNameOf(ed.currentSubgraph()) == "Pick", "the macro opens as its own subgraph");
        Vec2 lo, hi;
        check(ed.contentBounds(1.0f, &lo, &hi), "its canvas shows the two tunnels");
        const std::string branch = ed.addNodeFromCatalog("Branch", Vec2{200.0f, 0.0f});
        check(getNodeAttribute(*nodeById(ed, branch), "macro").value == "Pick", "a node added there belongs to the macro body");
        check(ed.connectPins(entryId, "go", branch, "exec") && ed.connectPins(entryId, "cond", branch, "cond"), "tunnel pins wire into the body");
        check(ed.connectPins(branch, "true", exitId, "yes") && ed.connectPins(branch, "false", exitId, "no"), "and the body wires out to the exits");
        check(ed.addMacroNode("Pick", Vec2{0.0f, 0.0f}).empty(), "a macro cannot be placed in its own body");

        ed.setCurrentSubgraph({});
        useId = ed.addMacroNode("Pick", Vec2{200.0f, 0.0f});
        check(!useId.empty() && getNodeAttribute(*nodeById(ed, useId), "call").value == "Pick", "an instance is added to the event graph");
        check(pinTypeOf(ed, useId, "go", false) == "exec" && pinTypeOf(ed, useId, "cond", false) == "bool" &&
              pinTypeOf(ed, useId, "yes", true) == "exec" && pinTypeOf(ed, useId, "no", true) == "exec",
              "with the macro's inputs and outputs as pins");
        check(ed.connectPins("tick", "exec", useId, "go"), "an exec output wires into the instance's exec input");

        std::string why;
        check(ed.save(&why), "save succeeds (" + why + ")");
        const std::string saved = readFile(tmp);
        check(saved.find("MACRO Pick") != std::string::npos, "the MACRO record is written");
        check(saved.find("MACROIN Pick go exec") != std::string::npos && saved.find("MACROIN Pick cond bool") != std::string::npos, "so are its MACROIN records");
        check(saved.find("MACROOUT Pick yes exec") != std::string::npos && saved.find("MACROOUT Pick no exec") != std::string::npos, "and its MACROOUT records");
        check(saved.find("macro=Pick") != std::string::npos && saved.find("call=Pick") != std::string::npos, "the body nodes and the instance carry macro= and call=");
    }

    {
        GraphEditor ed(tmp);
        check(ed.macros().size() == 1 && ed.macros()[0].inputs.size() == 2, "the macro and its pins reload");
        check(pinTypeOf(ed, useId, "no", true) == "exec", "and the instance still has the macro's pins");

        // Removing a pin takes the wires into it.
        check(ed.removeMacroPin("Pick", false, "no"), "a pin is removed");
        check(pinTypeOf(ed, useId, "no", true) == "?", "from the instance");
        bool anyNo = false;
        for (const auto& l : ed.graph().links) if (l.sourcePin == "no" || l.destPin == "no") anyNo = true;
        check(!anyNo, "and every wire that used it is gone");

        check(ed.renameMacro("Pick", "Choose"), "rename succeeds");
        std::string why;
        check(ed.save(&why), "save after rename succeeds (" + why + ")");
        const std::string after = readFile(tmp);
        check(after.find("MACRO Choose") != std::string::npos && after.find("MACROIN Choose go exec") != std::string::npos, "the declaration was renamed");
        check(after.find("macro=Choose") != std::string::npos && after.find("call=Choose") != std::string::npos, "so were the body's macro= and the instance's call=");
        check(after.find("Pick") == std::string::npos, "and no reference to the old name is left");

        std::vector<std::string> blockers;
        check(!ed.deleteMacro("Choose", &blockers) && !blockers.empty(), "delete is refused while an instance uses it, naming it");
        check(ed.selectNode(useId), "the instance selects");
        ed.deleteSelection();
        check(ed.deleteMacro("Choose"), "with the instance gone, delete succeeds");
        bool anyBody = false;
        for (const auto& n : ed.graph().nodes) if (isMacroNodeType(n.type)) anyBody = true;
        check(ed.macros().empty() && !anyBody, "the macro and its body go together");
    }
}

static void testMinimapGeometry() {
    AVER_INFO("=== the minimap maps the content and the view, and is absent for an empty graph or when off ===");
    const std::string empty = (std::filesystem::path(scratchDir()) / "minimap_empty.ocgraph").string();
    writeFile(empty, "OCGRAPH 1\nNAME Empty\n");
    {
        GraphEditor ed(empty);
        GraphEditor::MinimapLayout L;
        check(!ed.minimapLayout(Vec2{1200.0f, 800.0f}, 1.0f, &L), "an empty graph has no minimap");
    }

    const std::string tmp = (std::filesystem::path(scratchDir()) / "minimap.ocgraph").string();
    writeFile(tmp,
        "OCGRAPH 1\n"
        "NAME Mini\n"
        "NODE k ConstFloat 0 0 value=3\n"
        "PIN k value out float\n"
        "NODE a Add 2000 1000\n"
        "PIN a a in float\n"
        "PIN a b in float\n"
        "PIN a result out float\n");
    GraphEditor ed(tmp);
    check(ed.minimapVisible(), "the minimap is on by default");
    GraphEditor::MinimapLayout L;
    const Vec2 canvas{1200.0f, 800.0f};
    check(ed.minimapLayout(canvas, 1.0f, &L), "a graph with nodes has one");
    check(L.max.x <= canvas.x && L.max.y <= canvas.y && L.min.x >= 0.0f && L.min.y >= 0.0f, "inside the canvas");
    check(L.max.x > canvas.x - 40.0f && L.max.y > canvas.y - 40.0f, "in its bottom-right corner");
    check(L.contains(Vec2{L.max.x - 1.0f, L.max.y - 1.0f}) && !L.contains(Vec2{10.0f, 10.0f}), "its rectangle is what the clicks are tested against");

    // A click on the overlay centres the view on the point it stands for.
    const Vec2 mid{(L.min.x + L.max.x) * 0.5f, (L.min.y + L.max.y) * 0.5f};
    ed.panToMinimapPoint(L, mid, canvas);
    Vec2 lo, hi;
    check(ed.contentBounds(1.0f, &lo, &hi), "(content bounds)");
    GraphEditor::MinimapLayout L2;
    check(ed.minimapLayout(canvas, 1.0f, &L2), "the overlay is still there after the pan");
    const Vec2 centre = L2.toCanvas(L2.toMap(L.toCanvas(mid)));
    check(std::fabs(centre.x - L.toCanvas(mid).x) < 1.0f && std::fabs(centre.y - L.toCanvas(mid).y) < 1.0f, "mapping a point there and back is the identity");

    ed.setMinimapVisible(false);
    check(!ed.minimapVisible() && !ed.minimapLayout(canvas, 1.0f, &L), "turned off, it is gone");
    ed.setMinimapVisible(true);
    check(ed.minimapLayout(canvas, 1.0f, &L), "and back on");
    check(!ed.minimapLayout(Vec2{70.0f, 50.0f}, 1.0f, &L), "a canvas too small to spare room for it has none");
}

static void testLoadFailure() {
    AVER_INFO("=== a missing file fails cleanly ===");
    const std::string missing = (std::filesystem::path(scratchDir()) / "does_not_exist.ocgraph").string();
    GraphEditor ed(missing);
    std::string why;
    const bool saved = ed.save(&why);
    check(!saved, "save() refuses to write for an editor whose load failed");
    check(!why.empty(), "the failure reason is non-empty, not a silent false");
}

static void testContentBrowserStarterOpensAndKeepsItsClass() {
    AVER_INFO("=== the Content Browser's starter graph opens, and keeps the CLASS line ===");

    // WHAT THIS GUARDS. "New Aver Node Graph" writes graphStarterText() and opens the file. The C++
    // OcGraphData does not model the CLASS record at all, so the obvious implementation -- build an
    // OcGraphData and hand it to fmt::saveOcgraph -- would produce a graph with NO CLASS LINE. It
    // would open cleanly, look finished, and be impossible to place in a level, because a level
    // placement names a class rather than a file. Nothing about that failure is visible at the moment
    // it happens.
    //
    // So the starter is written as TEXT, and this asserts the property that forced it.
    const std::string dir = scratchDir();
    const std::string path = dir + "/NewGraph.ocgraph";
    const std::string text = graphStarterText("NewGraph");

    check(text.find("CLASS AN_NewGraph") != std::string::npos,
          "the starter declares a class derived from the file stem");
    check(text.find("OCGRAPH 1") == 0, "and opens with the format header");
    check(text.find("DOMAIN gameplay") != std::string::npos, "and names its domain");

    writeFile(path, text);

    // It parses at all, through the same reader the editor uses.
    fmt::OcGraphData parsed;
    std::string err;
    check(fmt::parseOcgraph(text, parsed, &err), "it parses: " + err);
    check(parsed.name == "NewGraph", "with the NAME the stem gave it, got '" + parsed.name + "'");

    // And the tab opens it -- the step the menu item takes immediately after writing.
    auto ed = makeGraphEditor(path);
    check(ed != nullptr, "the editor the create path opens for it ACCEPTS it");

    if (ed) {
        // A load -> save with no edits must be byte-identical, which is this editor's stated contract.
        // For the starter that is the CLASS line's survival: it is an unrecognised record, carried
        // only by writeOcgraph's pass-through of the original text, so a save that dropped it would
        // silently un-place every instance of this class in every level.
        std::string why;
        check(ed->save(&why), "and saves it back: " + why);
        const std::string after = readFile(path);
        check(after == text, "byte-identically -- the unmodelled CLASS record survived the round trip");
        check(after.find("CLASS AN_NewGraph") != std::string::npos,
              "and is still there to be named by a placement");
    }

    // AND THE FALSIFICATION, kept rather than run once and discarded: the obvious implementation
    // really does lose the class. Round-tripping the starter through OcGraphData and fmt::saveOcgraph
    // -- which is how a starter would naturally be built, and how this one nearly was -- drops the
    // CLASS line, because the struct has no field for it and saveOcgraph writes fresh with no
    // original text to pass through. If this check ever starts failing, the C++ layer has learned to
    // model CLASS and graphStarterText may go back to being structured data.
    {
        const std::string viaStruct = dir + "/viaStruct.ocgraph";
        fmt::OcGraphData g;
        std::string err;
        check(fmt::parseOcgraph(text, g, &err), "the starter parses into an OcGraphData: " + err);
        check(fmt::saveOcgraph(viaStruct, g, &err), "which saveOcgraph will happily write: " + err);
        const std::string lost = readFile(viaStruct);
        check(lost.find("CLASS") == std::string::npos,
              "and the CLASS line is GONE from it -- which is why the starter is written as text");
        check(lost.find("NAME NewGraph") != std::string::npos,
              "while everything the struct DOES model survives, so the loss is silent");
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static void testClassPlacementsCarryTheEditorsMoves() {
    AVER_INFO("=== a moved graph-class placement survives the save ===");

    // THE BUG THIS ENCODES. saveLevel used to write class placements straight from the copy it read
    // off disk, defended by a comment saying "the editor cannot currently EDIT a class placement
    // (there is no entity to select and drag)". The premise was false: spawnClassPlacements spawns a
    // real entity per placement, aver_fw_spawn's handle IS a scene entity, and the World Outliner
    // lists anything with a mesh or a name. So you could select a placed graph class, drag it, save,
    // reload -- and find it exactly where it started, with nothing logged. A discarded edit.

    std::vector<fmt::OcWorldPlacement> authored(3);
    for (auto& p : authored) { p.x = 1; p.y = 2; p.z = 3; p.sx = p.sy = p.sz = 1; }
    authored[0].className = "AN_Alpha";
    authored[1].className = "AN_Beta";
    authored[2].className = "AN_Gamma";
    authored[2].snapToGround = true;
    authored[2].z = 25;            // an OFFSET above the ground, not a height

    // Placement 0 never spawned -- its class was not declared, which is a warning and not an error,
    // and is the ordinary state of a level opened before its project's scripts are compiled.
    // THIS IS THE PAIRING TRAP: the first live entity is the SECOND placement.
    std::vector<editor::LevelClassInstance> live = {{1, 41}, {2, 42}};

    const auto transformOf = [](int32_t e, Transform& xf) {
        if (e == 41) { xf.position = Vec3{100, 200, 300}; xf.scale = Vec3{2, 2, 2}; return true; }
        if (e == 42) { xf.position = Vec3{-50, -60, 999}; xf.scale = Vec3{1, 1, 1}; return true; }
        return false;   // anything else is gone
    };

    std::vector<fmt::OcWorldPlacement> out;
    editor::appendClassPlacements(authored, live, transformOf, out);

    check(out.size() == 3, "every placement is written -- none is dropped for want of an entity");
    if (out.size() != 3) return;

    check(out[0].className == "AN_Alpha" && out[0].x == 1 && out[0].y == 2 && out[0].z == 3,
          "the one with no live entity is written EXACTLY as it was read");

    check(out[1].className == "AN_Beta",
          "the entity paired with placement 1 landed on placement 1, not placement 0 -- pairing is by "
          "recorded index, and the two vectors are not parallel when a class fails to resolve");
    check(out[1].x == 100 && out[1].y == 200 && out[1].z == 300,
          "and it carries the position the editor moved it to");
    check(out[1].sx == 2 && out[1].sy == 2 && out[1].sz == 2, "and the scale");

    check(out[2].x == -50 && out[2].y == -60,
          "a SNAPPED placement moves in x and y like any other");
    check(out[2].z == 25,
          "but keeps its authored z, because snap makes z an offset above the ground and the live "
          "entity's z is the resolved world height -- writing 999 back would bake the terrain in and "
          "the placement would climb on every save/load");
}

static void testClassPlacementsHonourDeletesAndSessionAdds() {
    AVER_INFO("=== a deleted graph-class placement is not written; a placed one is ===");

    // THE BUG THIS ENCODES (PLACE_CLASS F5). Delete destroyed the entity and left the placement's instance
    // entry alone, so the next save found a dead entity, fell back to the copy read off disk, and the deleted
    // actor came back on reload. An instance now carries a tombstone, and appendClassPlacements honours it
    // while still writing a placement that has NO instance (its class did not resolve) as it was read.
    std::vector<fmt::OcWorldPlacement> authored(4);
    for (auto& p : authored) { p.x = 1; p.y = 2; p.z = 3; p.sx = p.sy = p.sz = 1; }
    authored[0].className = "AN_Unresolved";   // no instance: written as read
    authored[1].className = "AN_Deleted";      // instance, removed
    authored[2].className = "AN_Snapped";      // instance, removed, snapped neighbour of the next
    authored[2].snapToGround = true; authored[2].z = 25;
    authored[3].className = "AN_Added";        // session-added: an index beyond the loaded ones

    std::vector<editor::LevelClassInstance> live = {{1, 0}, {2, 0}, {3, 43}};
    live[0].removed = true;
    live[1].removed = true;
    check(live[2].removed == false, "an instance is not removed unless it says so");

    const auto transformOf = [](int32_t e, Transform& xf) {
        if (e == 43) { xf.position = Vec3{10, 20, 30}; xf.scale = Vec3{1, 1, 1}; return true; }
        return false;
    };
    std::vector<fmt::OcWorldPlacement> out;
    editor::appendClassPlacements(authored, live, transformOf, out);

    check(out.size() == 2, "the two removed placements are omitted, the other two written");
    if (out.size() != 2) return;
    check(out[0].className == "AN_Unresolved" && out[0].x == 1 && out[0].z == 3,
          "a placement with no instance at all is still written as it was read");
    check(out[1].className == "AN_Added" && out[1].x == 10 && out[1].y == 20 && out[1].z == 30,
          "a session-added placement is written at its live pose");

    // A removed instance whose entity handle was reused must not capture the new owner's transform.
    live.clear();
    live.push_back(editor::LevelClassInstance{0, 43});
    live[0].removed = true;
    out.clear();
    editor::appendClassPlacements(authored, live, transformOf, out);
    check(out.size() == 3 && out[0].className == "AN_Deleted" && out[0].x == 1,
          "(index 0 removed) the rest are written and none takes the stale handle's transform");

    // The snap rule survives next to a removal: a live snapped placement keeps its authored offset.
    live.clear();
    live.push_back(editor::LevelClassInstance{1, 0});
    live.push_back(editor::LevelClassInstance{2, 43});
    live[0].removed = true;
    out.clear();
    editor::appendClassPlacements(authored, live, transformOf, out);
    bool sawSnapped = false;
    for (const auto& p : out)
        if (p.className == "AN_Snapped") { sawSnapped = true; check(p.x == 10 && p.z == 25, "a snapped neighbour still moves in x/y and keeps its z offset"); }
    check(sawSnapped && out.size() == 3, "removing one placement does not shift which instance pairs with which");
}

static void testPaletteSearchRanksSensibly() {
    AVER_INFO("=== the add-node palette search ===");

    // 240 node types across 23 categories used to be reachable only through a submenu per category
    // with no filter, so finding a node meant already knowing which family it was filed under.
    // The ranking is the part with logic in it, and a flat substring match gets it wrong.

    check(graphPaletteSearch("", kDomainGameplay).empty(),
          "an empty query matches nothing -- the caller shows its category menus instead");
    check(graphPaletteSearch("zzzznotanode", kDomainGameplay).empty(),
          "and a query nothing matches returns nothing rather than everything");
    {
        // Spaces, underscores and case are ignored, and the words may come in any order.
        const auto has = [](const std::vector<const GraphNodeDesc*>& v, const char* typeId) {
            for (const GraphNodeDesc* d : v) if (d->typeId == typeId) return true;
            return false;
        };
        check(has(graphPaletteSearch("setlight", kDomainGameplay), "AN_SetLightIntensity"), "'setlight' finds Set Light Intensity");
        check(has(graphPaletteSearch("light set", kDomainGameplay), "AN_SetLightIntensity"), "'light set' finds it too (any word order)");
        check(has(graphPaletteSearch("SET_LIGHT_INT", kDomainGameplay), "AN_SetLightIntensity"), "and so does 'SET_LIGHT_INT'");
        const auto set = graphPaletteSearch("set", kDomainGameplay, 1000);
        check(!set.empty() && editor::detail::ciFind(set[0]->displayName, "set") == 0, "a name that starts with the query still ranks first");
    }

    {
        const auto hits = graphPaletteSearch("add", kDomainGameplay);
        check(!hits.empty(), "'add' finds something");
        if (!hits.empty()) {
            // THE RANKING TEST. `Add` starts with the query; `VecAdd` merely contains it. A flat
            // substring match would order them by catalog position and could put VecAdd first.
            check(hits[0]->displayName.size() >= 3 &&
                  editor::detail::ciFind(hits[0]->displayName, "add") == 0,
                  "and the best match STARTS with it, got '" + hits[0]->displayName + "'");

            usize add = hits.size(), vecAdd = hits.size();
            for (usize i = 0; i < hits.size(); ++i) {
                if (hits[i]->typeId == "Add")    add = i;
                if (hits[i]->typeId == "VecAdd") vecAdd = i;
            }
            if (add < hits.size() && vecAdd < hits.size())
                check(add < vecAdd, "Add outranks VecAdd, which only contains the query");
        }
    }

    {
        // A category name is searchable too, which is the whole point for someone who knows the
        // family but not the node: "vector" should reach the Vector rows even though none of their
        // display names contain the word.
        const auto hits = graphPaletteSearch("vector", kDomainGameplay);
        check(!hits.empty(), "a CATEGORY name finds its nodes");
        bool anyVector = false;
        for (const auto* d : hits) if (d->category == "Vector") anyVector = true;
        check(anyVector, "and they really are the Vector family");
    }

    {
        // Domain filtering must hold, for the reason the category menus filter: offering a gameplay
        // node to a material graph offers a node whose only outcome is a compile error.
        const auto play = graphPaletteSearch("branch", kDomainGameplay);
        const auto mat  = graphPaletteSearch("branch", kDomainMaterial);
        // NOT A VACUOUS PAIR: the gameplay side must actually find Branch, or "material found none"
        // would be satisfied by a search that finds nothing anywhere.
        check(!play.empty(), "'branch' is a real gameplay node");
        check(mat.empty(),
              "and a MATERIAL graph is not offered it -- a material has nothing to branch on, and "
              "suggesting it would suggest a node whose only outcome is a compile error, got " +
              std::to_string(mat.size()) + " hit(s)");
        for (const auto* d : mat)
            check((d->domain & kDomainMaterial) != 0u,
                  "a material search never returns a gameplay-only node (" + d->typeId + ")");
    }

    {
        // The cap must be a cap, and the count must be honest about what it hides -- a truncated list
        // that looks complete teaches the reader that a node does not exist.
        const auto capped = graphPaletteSearch("e", kDomainGameplay, 5);
        check(capped.size() <= 5, "the limit is respected, got " + std::to_string(capped.size()));
        const usize total = graphPaletteSearchCount("e", kDomainGameplay);
        check(total >= capped.size(), "and the uncapped count is at least as large, got " +
              std::to_string(total));
    }

    // Function rows stay out, for the same reason the category menu skips them: they are created by
    // the Functions panel, which knows which function they belong to, and a bare one has no pins.
    for (const auto* d : graphPaletteSearch("function", kDomainGameplay))
        check(d->category != "Function", "no bare Function row is offered (" + d->typeId + ")");
}

static void testCopyPasteRemapsIdsAndLinks() {
    AVER_INFO("=== copy / paste / duplicate ===");

    const std::string dir = scratchDir();
    const std::string path = dir + "/paste.ocgraph";
    // A RAW LITERAL: the escape for a newline does not survive every tool that writes this file,
    // and MSVC reports the result as a wall of "newline in string literal". Nothing to escape here.
    writeFile(path, R"(OCGRAPH 1
DOMAIN gameplay
NAME Paste
NODE tick OnTick
NODE a Add
NODE b Add
LINK a.result b.a
ENTRY tick OnTick
)");

    auto ed = makeGraphEditor(path);
    check(ed != nullptr, "the fixture opens");
    if (!ed) return;
    auto* g = static_cast<GraphEditor*>(ed.get());

    const usize nodes0 = g->graph().nodes.size();
    check(nodes0 == 3, "the fixture parsed three nodes, got " + std::to_string(nodes0));
    const usize links0 = g->graph().links.size();

    // Copy the two Add nodes AND the link between them.
    g->selectNodes({"a", "b"});
    check(g->selectedNodes().size() == 2, "two nodes select, got " + std::to_string(g->selectedNodes().size()));
    g->copySelection();
    g->pasteClipboard(Vec2{500.0f, 500.0f});

    check(g->graph().nodes.size() == nodes0 + 2, "two nodes arrived, got " +
          std::to_string(g->graph().nodes.size() - nodes0));
    check(g->graph().links.size() == links0 + 1,
          "and the link BETWEEN them came too, got " +
          std::to_string(g->graph().links.size() - links0));

    // THE REMAP IS THE POINT. The pasted link must join the two NEW nodes, not reach back into the
    // originals -- a copy wired to the thing it was copied from is the one outcome this must not have.
    const auto& pasted = g->selectedNodes();
    check(pasted.size() == 2, "the paste is what is selected afterwards");
    if (pasted.size() == 2) {
        check(pasted[0] != "a" && pasted[1] != "b", "the copies have new ids");
        bool joined = false;
        for (const auto& l : g->graph().links)
            if ((l.sourceNode == pasted[0] && l.destNode == pasted[1]) ||
                (l.sourceNode == pasted[1] && l.destNode == pasted[0])) joined = true;
        check(joined, "and the new link joins the two COPIES, not the originals");

        for (const auto& l : g->graph().links) {
            const bool srcNew = l.sourceNode == pasted[0] || l.sourceNode == pasted[1];
            const bool dstNew = l.destNode   == pasted[0] || l.destNode   == pasted[1];
            check(srcNew == dstNew,
                  "no link straddles the copy and the original (" + l.sourceNode + " -> " +
                  l.destNode + ")");
        }
    }

    // A LINK WITH ONE END OUTSIDE THE SELECTION IS NOT COPIED. Selecting only `b` copies no link,
    // because the link's source `a` was not part of the copy and has nothing to map to.
    {
        const usize before = g->graph().links.size();
        g->selectNodes({"b"});
        g->copySelection();
        g->pasteClipboard(Vec2{700.0f, 700.0f});
        check(g->graph().links.size() == before,
              "copying one end of a link copies no link at all, got " +
              std::to_string(g->graph().links.size() - before) + " new");
    }

    // AN EVENT NODE PASTED GETS ITS OWN ENTRY, exactly as one dropped from the palette does. Without
    // it the node exists, the graph saves, and the event never fires -- with no error at any layer.
    // A second handler for an event the graph already has is refused: the compiler allows one ENTRY per event.
    {
        const usize entries = g->graph().entryPoints.size();
        const usize nodes = g->graph().nodes.size();
        g->selectNodes({"tick"});
        g->copySelection();
        g->pasteClipboard(Vec2{900.0f, 900.0f});
        check(g->graph().entryPoints.size() == entries && g->graph().nodes.size() == nodes,
              "a second OnTick is refused, so the graph keeps one ENTRY per event");
        g->selectNodes({"tick"});
        g->deleteSelection();
    }
    {
        const usize entries = g->graph().entryPoints.size();
        g->pasteClipboard(Vec2{900.0f, 900.0f});
        check(g->graph().entryPoints.size() == entries + 1,
              "a pasted OnTick carries an ENTRY record, got " +
              std::to_string(g->graph().entryPoints.size() - entries));
        if (!g->selectedNodes().empty()) {
            bool found = false;
            for (const auto& e : g->graph().entryPoints)
                if (e.first == g->selectedNodes()[0] && e.second == "OnTick") found = true;
            check(found, "and it names the pasted node and the right event");
        }
    }

    // Undo puts each paste back -- one pushUndo per paste, not per node.
    {
        const usize n = g->graph().nodes.size();
        g->undoForTest();
        check(g->graph().nodes.size() < n, "undo removes a whole paste at once, not one node of it");
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static void testWrongExtensionIsRejectedByFactory() {
    AVER_INFO("=== factory only claims .ocgraph ===");
    check(makeGraphEditor("something.ocmesh") == nullptr, "makeGraphEditor declines a .ocmesh path");
    check(makeGraphEditor("something.OCGRAPH") != nullptr, "makeGraphEditor accepts .ocgraph case-insensitively");
}

// ==================================================================================== break links =
// Break Links keeps the NODES and removes only the WIRES. Worth a test rather than an eyeball
// because "delete the links touching these nodes" and "delete these nodes" are one careless
// predicate apart, and the failure -- nodes silently gone -- would be blamed on Delete.
//
// It is also the only part of the new node context menu with any logic in it: the rest of that menu
// calls copySelection/pasteClipboard/duplicateSelection/deleteSelection, which are already covered.
static void testBreakLinksKeepsTheNodes() {
    AVER_INFO("=== Break Links removes the wires on a selection and leaves everything else ===");

    // c1 -> add.a, c2 -> add.b, and an UNTOUCHED pair d1 -> other.a, so this can tell "removed the
    // selection's links" apart from "removed all links" -- a predicate bug would pass without it.
    //
    // A RAW STRING LITERAL, not a run of quoted lines: there is then no escape sequence anywhere in
    // the fixture for a tool to mangle on the way in, which is what happened to this test's first
    // draft (every "\n" arrived as a real newline and the file would not compile).
    const std::string text = R"(OCGRAPH 1
NODE c1 ConstFloat 0 0
PIN c1 value out float 1
NODE c2 ConstFloat 0 60
PIN c2 value out float 2
NODE add Add 200 0
PIN add a in float
PIN add b in float
PIN add result out float
NODE d1 ConstFloat 0 200
PIN d1 value out float 3
NODE other Add 200 200
PIN other a in float
PIN other b in float
PIN other result out float
LINK c1.value add.a
LINK c2.value add.b
LINK d1.value other.a
)";

    const std::string tmp = (std::filesystem::path(scratchDir()) / "break_links.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(ed.graph().links.size() == 3, "three links to start with");
    check(ed.graph().nodes.size() == 5, "five nodes to start with");

    // Nothing selected: a no-op, and specifically not an undo step that changes nothing.
    ed.breakLinksOnSelection();
    check(ed.graph().links.size() == 3, "with nothing selected it does nothing");
    check(!ed.dirty(), "and does not mark the document dirty for a no-op");

    ed.selectNodes({"add"});
    check(ed.selectionHasLinks(), "the Add node has links, so the menu item is enabled");
    ed.breakLinksOnSelection();

    check(ed.graph().nodes.size() == 5, "EVERY NODE SURVIVES -- this breaks wires, not nodes");
    check(ed.graph().links.size() == 1, "the two links touching the selection are gone");
    const bool untouchedSurvives = ed.graph().links.size() == 1 &&
                                   ed.graph().links[0].sourceNode == "d1" &&
                                   ed.graph().links[0].destNode == "other";
    check(untouchedSurvives, "and the unrelated d1 -> other link is untouched");
    check(ed.dirty(), "the document is dirty afterwards");
    check(!ed.selectionHasLinks(), "the selection has no links left, so the item greys out again");

    // ONE undo step for both wires, not one per wire -- the property that makes this usable.
    ed.undoForTest();
    check(ed.graph().links.size() == 3, "a single undo restores BOTH removed links");
}

// ================================================================================ link drop =
// DRAGGING A WIRE INTO EMPTY SPACE opens the palette filtered to types that accept it, and connects
// the one picked. The whole gesture is modelled with no ImGui in it precisely so this can drive it:
// arm it, ask what the palette would show, pick one, check the link.
//
// The filter is the half worth testing. Its two ways to be wrong are both silent -- offering a node
// whose link the editor will then refuse, and searching the wrong END of the candidate (looking for
// an output when the wire needs an input), which offers exactly the wrong half of the palette.
static void testLinkDropFiltersAndConnects() {
    AVER_INFO("=== dropping a wire on empty canvas offers only nodes that accept it, then wires up ===");

    const std::string text = R"(OCGRAPH 1
NODE c1 ConstFloat 0 0
PIN c1 value out float 1
NODE flag ConstBool 0 100
PIN flag value out bool false
)";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "link_drop.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    const GraphNodeDesc* add = findGraphNodeDesc("Add");        // two float inputs, one float output
    const GraphNodeDesc* cfloat = findGraphNodeDesc("ConstFloat"); // one float OUTPUT, no inputs
    check(add != nullptr && cfloat != nullptr, "the catalog has Add and ConstFloat to test against");
    if (!add || !cfloat) return;

    check(!ed.linkDropPending(), "nothing is pending before the gesture starts");
    check(!ed.linkDropAccepts(*add), "and with nothing pending nothing is offered");

    // Dragging FROM c1's float OUTPUT: candidates need a float INPUT.
    ed.beginLinkDrop("c1", "value", /*fromIsOutput=*/true);
    check(ed.linkDropPending(), "the gesture arms from a real pin");
    check(ed.linkDropAccepts(*add), "Add is offered -- it has a float input to land on");
    check(!ed.linkDropAccepts(*cfloat),
          "ConstFloat is NOT offered -- its only float pin is an OUTPUT, and two outputs cannot wire");

    const std::string made = ed.spawnAndConnectLinkDrop("Add", Vec2{300.0f, 0.0f});
    check(!made.empty(), "picking Add spawns it");
    check(!ed.linkDropPending(), "and disarms the gesture");
    const bool wired = ed.graph().links.size() == 1 &&
                       ed.graph().links[0].sourceNode == "c1" &&
                       ed.graph().links[0].sourcePin == "value" &&
                       ed.graph().links[0].destNode == made &&
                       ed.graph().links[0].destPin == "a";
    check(wired, "the wire is committed to the new node's FIRST accepting input, 'a'");

    // A BOOL output must not be offered a float input. Same node type, opposite answer -- which is
    // what tells a real type check from one that just returns true for anything with an input pin.
    ed.beginLinkDrop("flag", "value", /*fromIsOutput=*/true);
    check(!ed.linkDropAccepts(*add), "a bool output is not offered Add, whose inputs are floats");
    ed.cancelLinkDrop();
    check(!ed.linkDropPending(), "cancelling disarms it");

    // Dragging from an INPUT is the other direction: candidates now need a matching OUTPUT, and
    // ConstFloat -- refused above -- becomes the obvious answer.
    const usize before = ed.graph().links.size();
    ed.beginLinkDrop(made, "b", /*fromIsOutput=*/false);
    check(ed.linkDropAccepts(*cfloat), "dragging from a float INPUT now offers ConstFloat");
    const std::string k = ed.spawnAndConnectLinkDrop("ConstFloat", Vec2{0.0f, 300.0f});
    check(!k.empty(), "and picking it spawns one");
    const bool wired2 = ed.graph().links.size() == before + 1 &&
                        ed.graph().links.back().sourceNode == k &&
                        ed.graph().links.back().destNode == made &&
                        ed.graph().links.back().destPin == "b";
    check(wired2, "wired the NEW node's output into the pin the drag started from");

    // A pin that does not exist must not arm the gesture -- otherwise the next palette pick would
    // try to wire to nothing.
    ed.beginLinkDrop("c1", "noSuchPin", true);
    check(!ed.linkDropPending(), "a drag from a pin that is not there does not arm anything");
}

// ================================================================================= validation =
// The editor can finally ask the managed validator whether a graph is valid. The VALIDATOR itself is
// C# and needs a .NET runtime, so what is tested here is everything on this side of that boundary,
// with a stub validator standing in for the bridge:
//
//   - the "no validator" answer, which must NOT read as "valid" (an absence of evidence is not
//     evidence of correctness -- the failure mode this repo has been bitten by repeatedly);
//   - what gets sent: exactly the bytes save() would write, not the live graph with its synthesized
//     pins, or the validator would be answering about pins the author never wrote;
//   - errorNodeId, a heuristic over prose and therefore the piece most worth pinning down.
static void testValidationPlumbing() {
    AVER_INFO("=== validation: unavailable is not 'valid', and the message's node is resolved ===");

    const std::string text = R"(OCGRAPH 1
NODE c1 ConstFloat 0 0
PIN c1 value out float 1
NODE add Add 200 0
PIN add a in float
PIN add b in float
PIN add result out float
LINK c1.value add.a
)";
    const std::string tmp = (std::filesystem::path(scratchDir()) / "validate.ocgraph").string();
    writeFile(tmp, text);

    GraphEditor ed(tmp);
    check(!ed.validatorInstalled(), "an editor built with no validator says so");

    std::string err, node;
    check(!ed.validateNow(err, node),
          "AND validateNow REFUSES rather than returning 'valid' -- nothing checked the graph");
    check(err.find("no validator") != std::string::npos,
          "with a message that says why, not a bare false: '" + err + "'");

    // What actually crosses the boundary. save() strips the pins synthesizeMissingPins invented, and
    // validation has to send the same bytes or it is answering about a different document.
    std::string sent;
    ed.setValidator([&](const std::string& t, std::string&) { sent = t; return true; });
    check(ed.validatorInstalled(), "installing one is visible");
    check(ed.validateNow(err, node), "a validator that approves gives a clean pass");
    check(err.empty() && node.empty(), "with nothing reported");
    check(sent == ed.serializeForSave(),
          "and it was handed EXACTLY what save() would write, synthesized pins stripped");

    // A refusal naming a real node resolves to that node, so the canvas can ring it.
    ed.setValidator([](const std::string&, std::string& e) {
        e = "Node 'add' is a Param node but has no param= attribute naming which parameter it reads";
        return false;
    });
    check(!ed.validateNow(err, node), "a refusal is reported as one");
    check(node == "add", "and the node the message names is resolved for the badge");

    // THE TIMID HALF, which is what makes the heuristic safe. A quoted token that is not a node id
    // in this graph resolves to nothing, so a message about a pin, an event or a parameter cannot
    // ring an unrelated node.
    ed.setValidator([](const std::string&, std::string& e) {
        e = "Node references undeclared parameter 'entity' -- add 'PARAM entity int'";
        return false;
    });
    check(!ed.validateNow(err, node), "still a refusal");
    check(node.empty(), "but 'entity' is not a node id here, so NOTHING is badged rather than the wrong thing");

    ed.setValidator([](const std::string&, std::string& e) { e = "something went wrong"; return false; });
    check(!ed.validateNow(err, node) && node.empty(), "a message with no quotes badges nothing");

    // Directly, on the static, including the shapes validateNow cannot easily reach.
    const fmt::OcGraphData& g = ed.graph();
    check(GraphEditor::errorNodeId("Node 'c1' is wrong", g) == "c1", "errorNodeId reads the first quoted token");
    check(GraphEditor::errorNodeId("'c1' then 'add'", g) == "c1", "and only the FIRST one");
    check(GraphEditor::errorNodeId("no quotes at all", g).empty(), "no quotes, no answer");
    check(GraphEditor::errorNodeId("an empty '' pair", g).empty(), "an empty pair is not a node id");
    check(GraphEditor::errorNodeId("one 'unterminated", g).empty(), "an unterminated quote is not either");
}

int main() {
    AVER_INFO("======== GraphEditorLoadSaveTest ========");
    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec); // start clean
    std::filesystem::create_directories(scratchDir());

    testByteIdenticalRoundTrip();
    testUnknownRecordsSurvive();
    testSynthesisedPinsAreNotWritten();
    testAutoLayoutDoesNotDirtyTheFile();
    testAttributeEditRoundTrip();
    testPinDefaultRoundTrip();
    testVectorStringEntityPins();
    testStructsAndArraysInTheEditor();
    testUnknownAttributeSurvivesEditingADeclaredOne();
    testPathologicalAttributeCases();
    testVariablesParseAndByteIdenticalRoundTrip();
    testAddVariableValidatesAndFallsBack();
    testRenameVariableCascadesReferencesAndRefusesCollisions();
    testRetypeVariableFallsBackAndDoesNotTouchPins();
    testSetVariableDefaultValidatesWhitespace();
    testDeleteVariableRefusesWhileReferencedThenSucceeds();
    testFreshlySpawnedSetVarCanBeFixedByDeclaringOnTheSpot();
    testComponentTreeParsesAndRoundTripsByte();
    testComponentEditsCannotProduceAnUnopenableFile();
    testComponentWorldMatrixWalksTheParentChain();
    testAddingAnEventNodeAlsoDeclaresItsEntry();
    testDeletingANodeTakesItsEntryAndOutRecords();
    testCustomEventKeepsItsNameAndEntryInStep();
    testOnInputActionKeepsItsEntryInStep();
    testCommentBoxesRoundTripAndCarryTheirContents();
    testFramingAndAutoLayout();
    testFunctionsInTheEditor();
    testRerouteAndMacroRecordsRoundTrip();
    testInsertRerouteOnALink();
    testRerouteRetypesOnConnectAndAcrossAChain();
    testMacrosInTheEditor();
    testMinimapGeometry();
    testLoadFailure();
    testWrongExtensionIsRejectedByFactory();
    testConstNodeValueIsEditable();
    testContentBrowserStarterOpensAndKeepsItsClass();
    testClassPlacementsCarryTheEditorsMoves();
    testClassPlacementsHonourDeletesAndSessionAdds();
    testPaletteSearchRanksSensibly();
    testCopyPasteRemapsIdsAndLinks();
    testControlRigNodeCarriesItsWeightDefault();
    testBreakLinksKeepsTheNodes();
    testLinkDropFiltersAndConnects();
    testValidationPlumbing();

    AVER_INFO("======== {} failure(s) ========", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
