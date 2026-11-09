// OcWorldDataLayersTest -- the .ocworld DATALAYER header record, per-placement and per-FOLIAGE `layers=`, STREAM `hlod=`,
// and the .ocstream HLOD record. All additive: absent means the old behaviour, and old text parses unchanged.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcStream.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("=== OcWorldDataLayersTest ===");
    std::string err;

    {   // a level without layers or hlod writes none of the new tokens and is byte-stable
        fmt::OcWorldData w;
        w.name = "T";
        w.stream.enabled = true;
        fmt::OcWorldPlacement p; p.asset = "A.ocmesh"; w.placements.push_back(p);
        w.foliageFiles = {"f.ocinst"};
        const std::string t = fmt::writeOcworld(w);
        check(t.find("DATALAYER") == std::string::npos && t.find("layers=") == std::string::npos && t.find("hlod=") == std::string::npos,
              "no layer, hlod tokens for a plain level");
        fmt::OcWorldData r;
        check(fmt::parseOcworld(t, r, &err), "plain level parses: " + err);
        check(fmt::writeOcworld(r) == t, "plain level round-trips byte-identically");
        check(r.dataLayers.empty() && r.placements[0].layers.empty() && r.stream.hlodCm == 0.0f, "absent fields read as unset");
    }
    {   // full round trip, awkward names included
        fmt::OcWorldData w;
        w.name = "T";
        w.stream.enabled = true;
        w.stream.hlodCm = 90000.0f;
        w.dataLayers = {
            {"Day", fmt::OcDataLayerState::Activated, false, ""},
            {"Boss Room, east #1", fmt::OcDataLayerState::Unloaded, true, "Day"},
            {"Interiors", fmt::OcDataLayerState::Loaded, true, ""},
        };
        fmt::OcWorldPlacement a; a.asset = "A.ocmesh"; a.layers = {"Day", "Boss Room, east #1"}; w.placements.push_back(a);
        fmt::OcWorldPlacement b; b.asset = "B.ocmesh"; b.parent = -1; w.placements.push_back(b);
        w.foliageFiles = {"f0.ocinst", "f1.ocinst"};
        w.foliageLayers = {{}, {"Interiors"}};
        const std::string t = fmt::writeOcworld(w);
        fmt::OcWorldData r;
        check(fmt::parseOcworld(t, r, &err), "layered level parses: " + err);
        check(r.dataLayers == w.dataLayers, "layer definitions round trip (state, runtime, parent, name escapes)");
        check(r.placements.size() == 2 && r.placements[0].layers == a.layers && r.placements[1].layers.empty(), "placement membership round trips");
        check(r.foliageLayers.size() == 2 && r.foliageLayers[0].empty() && r.foliageLayers[1] == std::vector<std::string>{"Interiors"},
              "FOLIAGE layers round trip");
        check(r.foliageFiles == w.foliageFiles, "FOLIAGE paths unchanged by layers=");
        check(r.stream.hlodCm == 90000.0f, "STREAM hlod= round trips");
        check(fmt::writeOcworld(r) == t, "second write is identical");
    }
    {   // hand-written text: defaults, malformed records
        fmt::OcWorldData r;
        const char* text = "OCWORLD 1\nNAME T\nDATALAYER L1\nDATALAYER L2 initial=bogus runtime=0\nDATALAYER\nDATALAYER L3 initial=LOADED runtime=1 parent=L1\n"
                           "PLACE A.ocmesh 0 0 0 0 0 0 1 layers=L1,,L3 \n";
        check(fmt::parseOcworld(text, r, &err), "hand-written parses");
        check(r.dataLayers.size() == 3, "a nameless DATALAYER is skipped");
        check(r.dataLayers[0].initial == fmt::OcDataLayerState::Activated && !r.dataLayers[0].runtime, "initial defaults to activated, runtime to 0");
        check(r.dataLayers[2].initial == fmt::OcDataLayerState::Loaded && r.dataLayers[2].runtime && r.dataLayers[2].parent == "L1", "keys read case-insensitively");
        check(r.placements.size() == 1 && r.placements[0].layers == std::vector<std::string>{"L1", "L3"} && r.placements[0].material.empty(),
              "empty list entries dropped, layers= not read as a material");
    }
    {   // an older reader's view: the new header records and tokens must not break the placements it can read
        fmt::OcWorldData r;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\nSTREAM cell=6400 load=25000 evict=30000 hlod=80000\nDATALAYER X\nPLACE A.ocmesh 1 2 3 0 0 0 1 id=00000000000000aa\n", r, &err),
              "mixed record order parses");
        check(r.placements.size() == 1 && r.placements[0].placementId == 0xaa && r.stream.cellCm == 6400.0f, "the rest of the level reads as before");
    }
    {   // .ocstream HLOD records
        const std::string path = (std::filesystem::temp_directory_path() / "aver_hlod_fmt_test.ocstream").string();
        fmt::OcStreamData d;
        d.sourceHash = 0x1234;
        d.hlod = {{0, 0, 1, "Binaries/Streaming/L/hlod/0_0.ocmesh", 0xdeadbeefcafe}, {-3, 7, 2, "Binaries/Streaming/L/hlod/b_-3_7.ocmesh", 0x1}};
        fmt::OcStreamBounds b; b.id = 5; b.max[0] = 1; d.bounds.push_back(b);
        check(fmt::saveOcStream(path, d, &err), "stream saves: " + err);
        fmt::OcStreamData r;
        check(fmt::loadOcStream(path, r, &err), "stream loads: " + err);
        check(r.hlod == d.hlod && r.bounds.size() == 1 && r.sourceHash == 0x1234, "HLOD records round trip beside bounds");
        fmt::OcStreamData none;
        check(fmt::saveOcStream(path, none, &err) && fmt::loadOcStream(path, r, &err) && r.hlod.empty(), "no HLOD records = none read");
        std::filesystem::remove(path);
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
