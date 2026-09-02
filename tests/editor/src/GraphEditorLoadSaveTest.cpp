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
#include "GraphEditor.hpp"

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

static void testWrongExtensionIsRejectedByFactory() {
    AVER_INFO("=== factory only claims .ocgraph ===");
    check(makeGraphEditor("something.ocmesh") == nullptr, "makeGraphEditor declines a .ocmesh path");
    check(makeGraphEditor("something.OCGRAPH") != nullptr, "makeGraphEditor accepts .ocgraph case-insensitively");
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
    testCommentBoxesRoundTripAndCarryTheirContents();
    testFramingAndAutoLayout();
    testFunctionsInTheEditor();
    testLoadFailure();
    testWrongExtensionIsRejectedByFactory();
    testContentBrowserStarterOpensAndKeepsItsClass();

    AVER_INFO("======== {} failure(s) ========", g_failures);
    return g_failures;
}
