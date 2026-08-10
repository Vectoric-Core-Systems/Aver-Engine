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
#include <limits>
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

// Checks the SCATTER record: every field, round-trip byte-identity, an unbounded density band's
// deliberate omission from the written text, and that a line this parser does not understand (a
// stand-in for a future record) does not disturb SCATTER or anything else already parsed.
static void checkOcworldScatter() {
    AVER_INFO("=== .ocworld SCATTER record ===");
    using namespace fmt;
    std::string err;

    {
        // Two species, every field given explicitly, so nothing here is a struct default in disguise.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "SCATTER mesh Meshes/island_tree_03.ocmesh material M_Bark weight 0.6 scale 0.9 1.6 "
            "density 0.8 1.0 collide 170\n"
            "SCATTER mesh Meshes/grass_medium_01.ocmesh material M_Foliage weight 6 scale 0.8 1.4 "
            "collide 0 noyaw\n", w, &err),
            "a world with two SCATTER records parses");
        check(w.scatterSpecies.size() == 2, "and keeps both");

        const OcScatterSpecies& a = w.scatterSpecies[0];
        check(a.meshPath == "Meshes/island_tree_03.ocmesh", "species 0: mesh path");
        check(a.material == "M_Bark", "species 0: material");
        check(std::fabs(a.weight - 0.6) < 1e-12, "species 0: weight");
        check(std::fabs(a.scaleMin - 0.9) < 1e-12 && std::fabs(a.scaleMax - 1.6) < 1e-12,
              "species 0: scale range");
        check(std::fabs(a.densityMin - 0.8) < 1e-12 && std::fabs(a.densityMax - 1.0) < 1e-12,
              "species 0: density band");
        check(std::fabs(a.collisionRadiusCm - 170.0) < 1e-9, "species 0: collision radius");
        check(a.randomizeYaw, "species 0: yaw randomises by default (no `noyaw` token)");

        const OcScatterSpecies& b = w.scatterSpecies[1];
        check(b.meshPath == "Meshes/grass_medium_01.ocmesh", "species 1: mesh path");
        check(std::fabs(b.weight - 6.0) < 1e-12, "species 1: weight");
        check(b.densityMin <= -std::numeric_limits<f64>::max() / 2.0 &&
              b.densityMax >=  std::numeric_limits<f64>::max() / 2.0,
              "species 1: no `density` clause leaves the unbounded default");
        check(!b.randomizeYaw, "species 1: `noyaw` turns off yaw randomisation");

        // ---- round trip --------------------------------------------------------------------------
        const std::string text = writeOcworld(w);
        check(text.find("SCATTER mesh Meshes/island_tree_03.ocmesh") != std::string::npos,
              "the written text carries the first species");
        check(text.find("density 0.8 1") != std::string::npos,
              "...with its explicit density band");
        check(text.find("noyaw") != std::string::npos, "and the second species' `noyaw`");
        // The UNBOUNDED case is deliberately not printed as ~1.79769e+308 -- see writeOcworld's own
        // comment. Checked by absence: nothing in the grass line's output should carry a density
        // clause the input never gave it.
        const usize grassPos = text.find("Meshes/grass_medium_01.ocmesh");
        const usize grassLineEnd = text.find('\n', grassPos);
        check(grassPos != std::string::npos &&
              text.substr(grassPos, grassLineEnd - grassPos).find("density") == std::string::npos,
              "an unbounded species' line has no `density` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.scatterSpecies.size() == 2, "with both species still present");
        check(back.scatterSpecies[0].meshPath == a.meshPath &&
              std::fabs(back.scatterSpecies[0].weight - a.weight) < 1e-9 &&
              std::fabs(back.scatterSpecies[0].collisionRadiusCm - a.collisionRadiusCm) < 1e-6,
              "and the first species' fields round-trip");
        check(!back.scatterSpecies[1].randomizeYaw, "...including the second species' `noyaw`");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // A line no branch of the parser understands must not disturb a SCATTER record next to it --
        // the reader's own contract ("unknown records are skipped, not failed") exercised with SCATTER
        // specifically, since it is the newest record in the chain of if/else-if branches.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "FUTURERECORD something nobody has written yet\n"
            "SCATTER mesh Meshes/rock.ocmesh weight 1\n", w, &err),
            "a world with an unknown record before SCATTER still parses");
        check(w.scatterSpecies.size() == 1 && w.scatterSpecies[0].meshPath == "Meshes/rock.ocmesh",
              "and SCATTER is unaffected by the record it did not understand");
    }
    {
        // A malformed SCATTER line (missing the value a keyword expects) is simply the token that
        // never matches any branch -- the same tolerance PCGVOLUME already has for a short `bounds`.
        // What must NOT happen is the whole record disappearing or the parse failing outright.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSCATTER mesh Meshes/x.ocmesh weight\n", w, &err),
              "a SCATTER line with a dangling keyword still parses the file");
        check(w.scatterSpecies.size() == 1 && std::fabs(w.scatterSpecies[0].weight - 1.0) < 1e-12,
              "and the species keeps weight's own default rather than reading garbage");
    }
    {
        // GAMEMODE: the level's own override of the project's default, by CLASS NAME -- what a World
        // Settings window edits. By name and not by handle because framework class handles come from
        // aver_fw_class_declare at runtime and are process-local; a number in a file would mean
        // something different next launch.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nGAMEMODE ForestGameMode\n", w, &err),
              "a GAMEMODE record parses");
        check(w.gameMode == "ForestGameMode", "and keeps the class name verbatim");

        // The REST OF THE LINE, so a class name with spaces survives -- nothing forbids one, and
        // silently truncating at the first space would bind the wrong class or none.
        OcWorldData sp;
        check(parseOcworld("OCWORLD 1\nGAMEMODE My Game Mode\n", sp, nullptr), "a spaced name parses");
        check(sp.gameMode == "My Game Mode", "...and is not truncated at the first space");

        OcWorldData none;
        check(parseOcworld("OCWORLD 1\nNAME T\n", none, nullptr), "a level with no GAMEMODE parses");
        check(none.gameMode.empty(), "...and states no override, rather than a class named nothing");

        const std::string text = writeOcworld(w);
        check(text.find("GAMEMODE ForestGameMode") != std::string::npos, "the writer emits it");
        check(writeOcworld(none).find("GAMEMODE") == std::string::npos,
              "a level with no override writes no GAMEMODE line at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.gameMode == w.gameMode, "and it round-trips");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // SEVERAL DENSITY FIELDS, and species bound to them by name. The tokens that make a level
        // able to have a foreground and a background: PCGVOLUME `radius` (how far this field
        // streams, in chunks) and SCATTER `volume` (which field places this species).
        //
        // One field forces one streaming radius for everything, and one radius cannot be right for
        // two things at once -- ground cover wants to be dense and near, a canopy wants to be sparse
        // and far. Both defaults are the unset sentinel, so a level naming neither behaves exactly
        // as every level did before these existed, which is the property checked last here.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "PCGVOLUME name Canopy seed 7 cell 3200 octaves 2 floor 0.7 bias 1 samples 3 radius 10 infinite\n"
            "PCGVOLUME name Floor seed 8 cell 1600 octaves 3 floor 0.3 bias 1 samples 12 infinite\n"
            "SCATTER mesh Meshes/pine.ocmesh material M_pine volume Canopy weight 2.5 scale 0.9 1.4 collide 120\n"
            "SCATTER mesh Meshes/fern.ocmesh material M_fern weight 16 scale 0.8 1.6 collide 0\n", w, &err),
            "a world with two PCGVOLUMEs and a volume-bound species parses");
        check(w.pcgVolumes.size() == 2 && w.scatterSpecies.size() == 2, "both volumes and both species survive");
        check(w.pcgVolumes[0].radiusChunks == 10, "the canopy volume keeps its streaming radius");
        check(w.pcgVolumes[1].radiusChunks == 0,
              "a volume with no `radius` clause keeps the unset-zero sentinel, not a defaulted number");
        check(w.scatterSpecies[0].volume == "Canopy", "the species names its volume verbatim");
        check(w.scatterSpecies[1].volume.empty(),
              "a species with no `volume` clause stays empty -- the first-non-Sky behaviour it always had");
        // `radius` must not disturb the six numbers `bounds` consumes, nor the tokens after `samples`.
        check(w.pcgVolumes[0].infinite && std::fabs(w.pcgVolumes[0].cellSizeCm - 3200.0) < 1e-9 &&
              w.pcgVolumes[0].samplesPerAxis == 3,
              "`radius` does not shift the tokens around it");

        const std::string text = writeOcworld(w);
        check(text.find("radius 10") != std::string::npos, "the written text carries the radius");
        check(text.find("volume Canopy") != std::string::npos, "...and the species' volume binding");
        // Both unset forms must be ABSENT, not written as `radius 0` / `volume `.
        const usize floorPos = text.find("name Floor");
        const usize floorEnd = text.find('\n', floorPos);
        check(floorPos != std::string::npos &&
              text.substr(floorPos, floorEnd - floorPos).find("radius") == std::string::npos,
              "a volume with no declared radius has no `radius` token at all");
        const usize fernPos = text.find("Meshes/fern.ocmesh");
        const usize fernEnd = text.find('\n', fernPos);
        check(fernPos != std::string::npos &&
              text.substr(fernPos, fernEnd - fernPos).find("volume") == std::string::npos,
              "a species with no declared volume has no `volume` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "the written text parses again");
        check(back.pcgVolumes.size() == 2 && back.pcgVolumes[0].radiusChunks == 10 &&
              back.pcgVolumes[1].radiusChunks == 0 &&
              back.scatterSpecies[0].volume == "Canopy" && back.scatterSpecies[1].volume.empty(),
              "radius and volume round-trip, present and absent alike");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
}

// Checks the LANDSCAPE record: every field, round-trip byte-identity, the unset-extent field's
// deliberate omission from the written text, and that a line this parser does not understand does not
// disturb LANDSCAPE or anything else already parsed -- the same three properties checkOcworldScatter
// already proves for SCATTER, exercised against the newest record in the chain of if/else-if branches.
static void checkOcworldLandscape() {
    AVER_INFO("=== .ocworld LANDSCAPE record ===");
    using namespace fmt;
    std::string err;

    {
        // Two sections, every field given explicitly on the first, so nothing here is a struct
        // default in disguise. The second omits `extent` and `name`, which is the common case: a
        // level with one terrain section rarely bothers naming it or restating its own footprint.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "LANDSCAPE name Valley section Terrain/valley_00.ocland at 1000 -500 120 extent 25600\n"
            "LANDSCAPE section Terrain/valley_01.ocland at 26600 -500 80\n", w, &err),
            "a world with two LANDSCAPE records parses");
        check(w.landscapes.size() == 2, "and keeps both");

        const OcLandscapePlacement& a = w.landscapes[0];
        check(a.name == "Valley", "section 0: name");
        check(a.section == "Terrain/valley_00.ocland", "section 0: asset reference");
        check(std::fabs(a.x - 1000.0) < 1e-9 && std::fabs(a.y + 500.0) < 1e-9 && std::fabs(a.z - 120.0) < 1e-9,
              "section 0: world placement");
        check(std::fabs(a.extentCm - 25600.0) < 1e-6, "section 0: declared extent");

        const OcLandscapePlacement& b = w.landscapes[1];
        check(b.name.empty(), "section 1: no `name` clause leaves the name empty");
        check(b.section == "Terrain/valley_01.ocland", "section 1: asset reference");
        check(std::fabs(b.x - 26600.0) < 1e-9, "section 1: world placement, x");
        check(b.extentCm == 0.0, "section 1: no `extent` clause leaves the unset-zero default");

        // ---- round trip --------------------------------------------------------------------------
        const std::string text = writeOcworld(w);
        check(text.find("LANDSCAPE name Valley section Terrain/valley_00.ocland") != std::string::npos,
              "the written text carries the first section");
        check(text.find("extent 25600") != std::string::npos, "...with its explicit extent");
        // UNSET extent is deliberately not printed as `extent 0` -- see writeOcworld's own comment.
        // Checked by absence: the second section's line must carry no `extent` token at all, and its
        // name must come back as the writer's own "unnamed" placeholder rather than an empty token a
        // reader could not parse a second time.
        check(text.find("LANDSCAPE name unnamed section Terrain/valley_01.ocland") != std::string::npos,
              "an unnamed section is written with the same 'unnamed' placeholder PCGVOLUME uses");
        const usize sec1Pos = text.find("Terrain/valley_01.ocland");
        const usize sec1LineEnd = text.find('\n', sec1Pos);
        check(sec1Pos != std::string::npos &&
              text.substr(sec1Pos, sec1LineEnd - sec1Pos).find("extent") == std::string::npos,
              "a section with no declared extent has no `extent` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.landscapes.size() == 2, "with both sections still present");
        check(back.landscapes[0].name == a.name && back.landscapes[0].section == a.section &&
              std::fabs(back.landscapes[0].extentCm - a.extentCm) < 1e-6,
              "and the first section's fields round-trip");
        // NOT empty here -- "unnamed" is a WRITTEN placeholder, same as PCGVOLUME's own, and the
        // parser has no way to tell "the file says unnamed" from "the file says the word unnamed" on
        // a second read. That collision is already accepted for PCGVOLUME; b.name.empty() above is
        // the check that actually matters (an un-round-tripped level reads back empty, not "unnamed").
        check(back.landscapes[1].name == "unnamed",
              "...with the second section now carrying the literal placeholder the first write chose "
              "-- idempotent from here on, which the byte-for-byte check just below confirms");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // `material` on a LANDSCAPE line: the token that decides what terrain is SHADED with, and
        // the reason it exists is that there was no way to say it. LandscapeRenderer::setSurface
        // held a hardcoded olive and had no call site in the tree, so every level's terrain rendered
        // the same flat colour whatever it contained.
        //
        // Checked BOTH WAYS on purpose. Stating it must survive a round trip, and NOT stating it
        // must not invent one -- an empty material means "the renderer keeps its own default", so a
        // level that never mentioned terrain shading must not come back from a save claiming a
        // material it does not have. That is the same unset-is-not-a-value rule `extent` follows
        // just above, and the writer omits both for the same reason.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "LANDSCAPE name Floor section Terrain/floor.ocland material M_forest_leaves_02 at 0 0 0\n"
            "LANDSCAPE name Bare section Terrain/bare.ocland at 100 0 0\n", w, &err),
            "a LANDSCAPE record with a `material` clause parses");
        check(w.landscapes.size() == 2, "and both sections survive");
        check(w.landscapes[0].material == "M_forest_leaves_02", "the material name is kept verbatim");
        check(w.landscapes[1].material.empty(),
              "a section with no `material` clause leaves it empty, not defaulted to a name");
        // Placement still parses correctly with `material` sitting between `section` and `at` --
        // the token order is free, and a new key must not shift the ones after it.
        check(std::fabs(w.landscapes[1].x - 100.0) < 1e-9,
              "a later key still parses with `material` present on the sibling record");

        const std::string text = writeOcworld(w);
        check(text.find("material M_forest_leaves_02") != std::string::npos,
              "the written text carries the material");
        const usize barePos = text.find("Terrain/bare.ocland");
        const usize bareEnd = text.find('\n', barePos);
        check(barePos != std::string::npos &&
              text.substr(barePos, bareEnd - barePos).find("material") == std::string::npos,
              "a section with no material has no `material` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "the written text parses again");
        check(back.landscapes.size() == 2 &&
              back.landscapes[0].material == "M_forest_leaves_02" &&
              back.landscapes[1].material.empty(),
              "and material round-trips, present and absent alike");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // Unknown-record tolerance, both directions: a line no branch understands must not disturb a
        // LANDSCAPE record next to it, whichever side it sits on.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "FUTURERECORD something nobody has written yet\n"
            "LANDSCAPE section Terrain/mesa.ocland at 0 0 0\n"
            "ANOTHERUNKNOWNRECORD 1 2 3\n", w, &err),
            "a world with unknown records around LANDSCAPE still parses");
        check(w.landscapes.size() == 1 && w.landscapes[0].section == "Terrain/mesa.ocland",
              "and LANDSCAPE is unaffected by records it did not understand, before or after it");
    }
    {
        // A malformed LANDSCAPE line (a dangling keyword with no value) is simply the token that never
        // matches any branch -- the same tolerance SCATTER and PCGVOLUME already have.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nLANDSCAPE section Terrain/x.ocland extent\n", w, &err),
              "a LANDSCAPE line with a dangling keyword still parses the file");
        check(w.landscapes.size() == 1 && w.landscapes[0].extentCm == 0.0,
              "and the section keeps extent's own unset default rather than reading garbage");
    }
    {
        // The record is optional and must stay so: every level written before it existed has none.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "a world with no LANDSCAPE still parses");
        check(w.landscapes.empty(), "and reports none, rather than inventing a section from nothing");
    }
}

// Runs the self-checks, then every file named on the command line. Returns the failure count.
int main(int argc, char** argv) {
    checkFnv();
    checkOcproject();
    checkOcworld();
    checkOcworldScatter();
    checkOcworldLandscape();
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
