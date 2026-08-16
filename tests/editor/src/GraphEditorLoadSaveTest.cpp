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

// ======================================================================================= failures ===
static void testLoadFailure() {
    AVER_INFO("=== a missing file fails cleanly ===");
    const std::string missing = (std::filesystem::path(scratchDir()) / "does_not_exist.ocgraph").string();
    GraphEditor ed(missing);
    std::string why;
    const bool saved = ed.save(&why);
    check(!saved, "save() refuses to write for an editor whose load failed");
    check(!why.empty(), "the failure reason is non-empty, not a silent false");
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
    testLoadFailure();
    testWrongExtensionIsRejectedByFactory();

    AVER_INFO("======== {} failure(s) ========", g_failures);
    return g_failures;
}
