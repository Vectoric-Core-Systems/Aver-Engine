// OcWorldOutlinerTest -- the .ocworld OUTFOLDER / OUTITEM records: folders (empty ones too), locks and folder
// membership round trip, a placement index follows the writer's renumbering, and a level without them is unchanged.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"

#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static fmt::OcWorldPlacement place(const std::string& asset, i32 parent = -1) {
    fmt::OcWorldPlacement p;
    p.asset = asset;
    p.parent = parent;
    return p;
}

int main() {
    AVER_INFO("=== OcWorldOutlinerTest ===");
    std::string err;

    {
        fmt::OcWorldData w;
        w.name = "T";
        w.placements = {place("A.ocmesh"), place("B.ocmesh"), place("C.ocmesh")};
        const std::string plain = fmt::writeOcworld(w);
        check(plain.find("OUT") == std::string::npos, "a level without organisation gains no OUT lines");
        w.outlinerFolders = {"Lights", "Lights/Indoor", "My Props #1", "Empty"};
        w.outlinerItems = {{'P', 1, true, "My Props #1"}, {'P', 2, false, "Lights/Indoor"}, {'L', 0, true, ""}};
        const std::string text = fmt::writeOcworld(w);
        fmt::OcWorldData r;
        check(fmt::parseOcworld(text, r, &err), "the level parses: " + err);
        check(r.outlinerFolders == w.outlinerFolders, "folders round trip, empty and awkward names included");
        check(r.outlinerItems == w.outlinerItems, "locks and folder membership round trip");
        check(fmt::writeOcworld(r) == text, "and the text is byte stable");
    }

    {
        // A child listed before its parent is written after it, so a later placement's index moves.
        fmt::OcWorldData w;
        w.name = "T";
        w.placements = {place("Child.ocmesh", 1), place("Parent.ocmesh"), place("Other.ocmesh")};
        w.outlinerItems = {{'P', 2, true, "F"}, {'P', 1, false, "F"}};
        w.outlinerFolders = {"F"};
        fmt::OcWorldData r;
        check(fmt::parseOcworld(fmt::writeOcworld(w), r, &err), "renumbered level parses");
        bool ok = r.placements.size() == 3 && r.outlinerItems.size() == 2;
        for (const auto& it : r.outlinerItems) {
            const std::string& asset = r.placements[static_cast<usize>(it.index)].asset;
            if (it.locked) ok = ok && asset == "Other.ocmesh";
            else           ok = ok && asset == "Parent.ocmesh";
        }
        check(ok, "an item still names its own placement after the writer renumbers");
    }

    {
        fmt::OcWorldData r;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\nOUTITEM P x\nOUTITEM Q 1 lock\nOUTITEM P 3 lock folder A%20B\n", r, &err),
              "malformed items do not fail the parse");
        check(r.outlinerItems.size() == 1 && r.outlinerItems[0].index == 3 && r.outlinerItems[0].folder == "A B",
              "only the well formed item is kept, its folder decoded");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
