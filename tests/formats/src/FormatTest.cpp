// Golden test for the .oc* format loaders. Pass one or more .ocbeam / .ocmap paths;
// it parses each, prints a summary, and checks invariants. Exit code = failure count.
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Hash.hpp"

#include <cmath>
#include <filesystem>
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

// Loads one .ocbeam, prints what it holds, and checks it is non-empty and identified.
static void testBeam(const std::string& path) {
    AVER_INFO("=== .ocbeam: {} ===", path);
    fmt::OcBeamData b;
    std::string err;
    if (!fmt::loadOcbeam(path, b, &err)) {
        AVER_ERROR("   load failed: {}", err);
        ++g_failures;
        return;
    }
    AVER_INFO("   objectId=0x{:016X}  materials={}  nodes={}  beams={}  panels={}  parts={}",
              b.objectId, b.materials.size(), b.nodes.size(), b.beams.size(), b.panels.size(), b.parts.size());
    AVER_INFO("   importScale={:.3f}  glb={}  rig={}  collision={}  skippedRows={}",
              b.importScale, b.hasEmbeddedGlb, b.hasRig, b.hasCollision, b.skippedRows);
    if (!b.materials.empty()) {
        const auto& m = b.materials.front();
        AVER_INFO("   material[0]: {} axial={} break={}N behavior={}",
                  m.name, m.axialStiffness, m.breakForceN, fmt::beamBehaviorName(m.behavior));
    }
    check(!b.nodes.empty(), "has nodes");
    check(b.objectId != kInvalidObjectId, "objectId assigned");
    check(!b.beams.empty(), "has beams");
}

// Loads one .ocmap, prints its placements, and checks it has a collision source.
static void testMap(const std::string& path) {
    AVER_INFO("=== .ocmap: {} ===", path);
    fmt::OcMapData m;
    std::string err;
    if (!fmt::loadOcmap(path, m, &err)) {
        AVER_ERROR("   load failed: {}", err);
        ++g_failures;
        return;
    }
    AVER_INFO("   name='{}'  id=0x{:016X}  build={}  algo={}  client='{}'",
              m.name, m.contentId, m.build, m.algo, m.clientUmap);
    AVER_INFO("   surfaces={}  placements={} (place={} deform={})  ground={} killZ={:.1f}",
              m.surfaces.size(), m.placements.size(), m.placeCount(), m.deformCount(), m.hasGround, m.killZ);
    for (const auto& p : m.placements) {
        AVER_INFO("   {} '{}' id=0x{:016X} @ ({:.3f},{:.3f},{:.3f}) yaw={:.1f}{}",
                  p.deform ? "DEFORM" : "PLACE ", p.asset, p.objectId, p.x, p.y, p.z, p.yaw,
                  p.deform ? (" mat=" + p.material) : "");
    }
    std::string why;
    const bool serverValid = fmt::ocmapIsServerValid(m, &why);
    AVER_INFO("   server-valid={}{}", serverValid, serverValid ? "" : (" (" + why + ")"));

    check(!m.placements.empty() || m.hasGround, "has a collision source");
    if (m.name == "demoworld") {
        check(m.contentId == 0x376B85BC4D1A03BAull, "demoworld ID == 0x376B85BC4D1A03BA (parsed from file)");
    }
}

// Recomputes fnv1a64 over three known vectors, independent of any test file.
static void checkFnv() {
    AVER_INFO("=== fnv1a64 self-check ===");
    check(fnv1a64("demoworld") == 0x376B85BC4D1A03BAull, "fnv1a64(\"demoworld\") == 0x376B85BC4D1A03BA");
    check(fnv1a64("") == 0xCBF29CE484222325ull, "fnv1a64(\"\") == the offset basis (empty input)");
    check(fnv1a64("Meshes/sphere.ocmesh") == 672114764054563281ull,
          "fnv1a64(\"Meshes/sphere.ocmesh\") agrees with the C# ObjectId");
}


// Round-trips a .ocproject manifest: what the writer does not own must survive, and writing twice
// must reproduce the file byte for byte.
static void checkOcproject() {
    using namespace aver::fmt;

    // SkyForge's manifest as it looks on disk, hand-edits and all.
    const std::string original =
        "OCPROJECT 1\n"
        "# Created by the Aver Engine editor. This project lives OUTSIDE the engine tree and\n"
        "# references it; see the engine's docs/PROJECTS.md.\n"
        "NAME SkyForge\n"
        "ENGINE Aver 0.1.0\n"
        "CONTENT Content\n"
        "STARTMAP Maps/Default.ocworld\n"
        "# AUTHOR <your name>\n";

    ProjectDesc d;
    std::string err;
    check(parseOcproject(original, d, &err), "the existing manifest parses");
    check(d.name == "SkyForge", "with its name");
    check(!d.hasRenderSettings(), "and states no render settings at all");
    check(d.giQuality == -1, "an absent RENDER.GI is -1, not 0 -- 0 would mean 'GI off'");

    d.giQuality = 3;
    d.voxelResolution = 256;
    d.giIntensity = 1.25f;
    d.giMaxDistance = 3500.0f;
    const std::string written = writeOcproject(d, original);

    check(written.find("# Created by the Aver Engine editor.") != std::string::npos,
          "the hand-written comment block survives a write");
    check(written.find("# references it; see the engine's docs/PROJECTS.md.") != std::string::npos,
          "including its second line");
    check(written.find("# AUTHOR <your name>") != std::string::npos,
          "and a commented-out key is not resurrected as a real one");
    check(written.rfind("OCPROJECT 1", 0) == 0, "the header is still the first line");

    ProjectDesc back;
    check(parseOcproject(written, back, &err), "what was written parses again");
    check(back.name == d.name && back.startMap == d.startMap && back.contentRoot == d.contentRoot,
          "and the original keys round-trip");
    check(back.giQuality == 3 && back.voxelResolution == 256, "the new integer settings round-trip");
    check(std::fabs(back.giIntensity - 1.25f) < 1.0e-6f, "and the float ones, exactly");
    check(std::fabs(back.giMaxDistance - 3500.0f) < 1.0e-3f, "including the large one");

    const std::string again = writeOcproject(back, written);
    check(again == written, "writing an unchanged manifest reproduces it byte for byte");

    const std::string future = written + "COOKTARGET WindowsClient\n";
    ProjectDesc f;
    check(parseOcproject(future, f, &err), "a manifest with an unknown key still parses");
    const std::string refuture = writeOcproject(f, future);
    check(refuture.find("COOKTARGET WindowsClient") != std::string::npos,
          "and the unknown key survives being written back");

    ProjectDesc n;
    n.name = "Fresh";
    n.engineName = "Aver";
    n.engineMinVersion = "0.1.0";
    n.startMap = "Maps/Default.ocworld";
    const std::string fresh = writeOcproject(n, "");
    ProjectDesc nb;
    check(parseOcproject(fresh, nb, &err), "a manifest written from nothing parses");
    check(nb.name == "Fresh", "and carries its name");
}

// Checks the .ocworld environment records: SUN in both spellings, the new SKY record, and the
// round trip. The reader and the writer had NO coverage at all before this.
static void checkOcworld() {
    AVER_INFO("=== .ocworld environment records ===");
    using namespace fmt;
    std::string err;

    {
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "SUN dir -0.3 -0.4 -0.85 color 1 0.98 0.92 lux 90000\n"
                           "SKY model physical mie 0.008 multiscatter 1.4 steps 24 aerial 6\n"
                           "FOG exp density 0.00014 color 0.7 0.78 0.88\n", w, &err),
              "a world with SUN, SKY and FOG parses");
        check(w.hasSun && w.hasSky && w.hasFog, "and reports all three present");
        check(std::fabs(w.sunDir[2] + 0.85) < 1e-9, "the sun vector is taken verbatim");
        check(w.sunLux == 90000.0, "including its lux");
        check(w.skyPhysical, "the sky model reads as physical");
        check(std::fabs(w.skyMieScatter - 0.008) < 1e-12, "the Mie override survives");
        check(w.skyViewSteps == 24 && w.skyAerialSteps == 6, "and both step counts");
    }
    {
        // Degrees are the form a person authors a time of day in, so they must reach the vector.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSUN elev 30 azim 90\n", w, &err),
              "SUN accepts elevation and azimuth instead of a vector");
        check(std::fabs(w.sunDir[2] - 0.5) < 1e-6, "30 degrees of elevation puts z at sin(30)");
        check(std::fabs(w.sunDir[0]) < 1e-6 && std::fabs(w.sunDir[1] - std::cos(30.0 * 3.14159265358979 / 180.0)) < 1e-6,
              "and a 90-degree bearing puts the rest on +Y");
    }
    {
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSKY model authored\n", w, &err), "an authored sky parses");
        check(!w.skyPhysical, "and selects the two-colour dome");
    }
    {
        // The record is optional and must stay so: every level written before it existed has none.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "a world with no SUN or SKY still parses");
        check(!w.hasSun && !w.hasSky, "and says so rather than inventing defaults as authored values");
    }
    {
        OcWorldData w;
        w.name = "RoundTrip";
        w.hasSun = true;  w.sunDir[0] = -0.5481; w.sunDir[1] = 0.3838; w.sunDir[2] = 0.7431;
        w.hasSky = true;  w.skyPhysical = true; w.skyMieScatter = 0.004; w.skyViewSteps = 32;
        w.hasFog = true;  w.fogDensity = 4e-6;
        const std::string text = writeOcworld(w);
        OcWorldData b;
        check(parseOcworld(text, b, &err), "what the writer produced parses again");
        check(b.hasSun && b.hasSky, "with both records still present");
        for (int i = 0; i < 3; ++i)
            check(std::fabs(b.sunDir[i] - w.sunDir[i]) < 1e-9, "the sun vector round-trips exactly");
        check(b.skyPhysical && b.skyViewSteps == 32, "and so does the sky model");
        // The elevation rides along as a comment for a reader; it must not be read back as data.
        check(text.find("# elev") != std::string::npos, "the written SUN line carries a readable elevation");
        check(writeOcworld(b) == text, "and a second write reproduces the first byte for byte");
    }
}

// Runs the self-checks, then every file named on the command line. Returns the failure count.
int main(int argc, char** argv) {
    checkFnv();
    checkOcproject();
    checkOcworld();
    if (argc < 2) {
        AVER_INFO("usage: FormatTest <file.ocbeam|file.ocmap> [more...]");
        return g_failures;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        switch (assetTypeFromPath(path)) {
            case AssetType::Beam: testBeam(path); break;
            case AssetType::Map:  testMap(path); break;
            default: AVER_WARN("skipping (unknown type): {}", path); break;
        }
    }
    AVER_INFO("==================================================");
    AVER_INFO("Format tests done: {} failure(s)", g_failures);
    return g_failures;
}
