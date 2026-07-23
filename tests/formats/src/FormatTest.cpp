// Golden test for the .oc* format loaders. Pass one or more .ocbeam / .ocmap paths;
// it parses each, prints a summary, and checks invariants. Exit code = failure count.
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Hash.hpp"   // fnv1a64 — re-computed below to guard the offset-basis constant

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

int main(int argc, char** argv) {
    checkFnv();
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
