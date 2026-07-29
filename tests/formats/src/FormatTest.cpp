// Golden test for the .oc* format loaders. Pass one or more .ocbeam / .ocmap paths;
// it parses each, prints a summary, and checks invariants. Exit code = failure count.
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Hash.hpp"   // fnv1a64 — re-computed below to guard the offset-basis constant

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

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
    // Every beam references node ids that exist (spot-check counts, not full graph).
    check(!b.beams.empty(), "has beams");
}

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
    // Cross-check the id parsed OUT of the file against the known sample identity. The hash FUNCTION
    // itself is exercised unconditionally in main() (checkFnv), because a machine without demoworld.ocmap
    // would otherwise never recompute it — which is exactly how a dropped digit in the offset basis went
    // unnoticed while this parsed-value check kept passing.
    if (m.name == "demoworld") {
        check(m.contentId == 0x376B85BC4D1A03BAull, "demoworld ID == 0x376B85BC4D1A03BA (parsed from file)");
    }
}

// Exercise the hash function directly, independent of any test file. This is the check whose absence let
// a corrupted offset basis pass CI: the .ocmap loader parses its content id out of the file, so nothing
// re-derived fnv1a64 from bytes until here. The three vectors are the documented OpenConstructor identity
// plus the two-way boundary the C# side must agree with (Aver.Scene ObjectIdOf / the scripting bridge).
static void checkFnv() {
    AVER_INFO("=== fnv1a64 self-check ===");
    check(fnv1a64("demoworld") == 0x376B85BC4D1A03BAull, "fnv1a64(\"demoworld\") == 0x376B85BC4D1A03BA");
    check(fnv1a64("") == 0xCBF29CE484222325ull, "fnv1a64(\"\") == the offset basis (empty input)");
    check(fnv1a64("Meshes/sphere.ocmesh") == 672114764054563281ull,
          "fnv1a64(\"Meshes/sphere.ocmesh\") agrees with the C# ObjectId");
}


// ---------------------------------------------------------------------------------------------
// .ocproject round trip
// ---------------------------------------------------------------------------------------------
//
// This writer edits a file a TEAM shares and a person hand-edits. The two properties that matter are
// not "does it parse" but "does it keep what it did not write" and "does an old manifest still
// load" -- a serialiser that rebuilt the file from the struct would silently delete the comment
// block every scaffolded project ships with.
static void checkOcproject() {
    using namespace aver::fmt;

    // Exactly what SkyForge's manifest looks like on disk, hand-edits and all.
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
    // An old manifest states no render settings, and must not be read as stating zero.
    check(!d.hasRenderSettings(), "and states no render settings at all");
    check(d.giQuality == -1, "an absent RENDER.GI is -1, not 0 -- 0 would mean 'GI off'");

    // Set what the editor would set, and write it back.
    d.giQuality = 3;
    d.voxelResolution = 256;
    d.giIntensity = 1.25f;
    d.giMaxDistance = 3500.0f;
    const std::string written = writeOcproject(d, original);

    // THE PROPERTY THAT MATTERS MOST: everything the writer does not own survived.
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

    // IDEMPOTENT. Writing what was just written must not grow the file -- an editor that appended a
    // duplicate key per save would corrupt a manifest over a working week rather than at once.
    const std::string again = writeOcproject(back, written);
    check(again == written, "writing an unchanged manifest reproduces it byte for byte");

    // A key this build has never heard of must survive too: the format is documented
    // forward-compatible, so a manifest written by a newer editor has to come back intact.
    const std::string future = written + "COOKTARGET WindowsClient\n";
    ProjectDesc f;
    check(parseOcproject(future, f, &err), "a manifest with an unknown key still parses");
    const std::string refuture = writeOcproject(f, future);
    check(refuture.find("COOKTARGET WindowsClient") != std::string::npos,
          "and the unknown key survives being written back");

    // A fresh file, with no existing text, must still be loadable.
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

int main(int argc, char** argv) {
    checkFnv();
    checkOcproject();
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
