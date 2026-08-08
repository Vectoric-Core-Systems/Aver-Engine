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
    testLoadFailure();
    testWrongExtensionIsRejectedByFactory();

    AVER_INFO("======== {} failure(s) ========", g_failures);
    return g_failures;
}
