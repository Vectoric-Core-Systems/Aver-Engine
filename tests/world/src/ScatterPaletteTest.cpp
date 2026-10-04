// aver::world::buildScatterPalette -- the format -> runtime conversion for a level's SCATTER
// palette, and the validation that stops a bad species from silently scattering nothing.
// Exit code = failure count.
//
// TWO HALVES. The first parses a real .ocworld-shaped level (two species, every field given) and
// checks the conversion narrows every one of them correctly. The second is entirely about
// VALIDATION: a level author gets no editor and no compiler, so a naming mistake, a weight of zero,
// or a swapped min/max has to be caught here, named by species, or it is indistinguishable from the
// feature simply not working -- exactly what this task's brief calls out.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ScatterPalette.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// True if `errors` has an entry mentioning both `needle` substrings -- used to check a validation
// message names the right species without pinning the exact wording.
static bool anyErrorContains(const std::vector<std::string>& errors, const std::string& a,
                              const std::string& b = "") {
    for (const std::string& e : errors)
        if (e.find(a) != std::string::npos && (b.empty() || e.find(b) != std::string::npos)) return true;
    return false;
}

int main() {
    // ---- a real level's worth of SCATTER records, every field given -------------------------------
    {
        fmt::OcWorldData w;
        std::string err;
        check(fmt::parseOcworld(
            "OCWORLD 1\nNAME ScatterFixture\n"
            "PCGVOLUME name Terrain seed 3 cell 1600 octaves 4 floor 0.45 bias 1.6 infinite\n"
            "SCATTER mesh Meshes/island_tree_03.ocmesh material M_Bark weight 0.6 scale 0.9 1.6 "
            "density 0.8 1.0 collide 170\n"
            "SCATTER mesh Meshes/grass_medium_01.ocmesh material M_Foliage weight 6 scale 0.8 1.4 "
            "collide 0 noyaw\n", w, &err),
            "the fixture level parses");
        check(w.scatterSpecies.size() == 2, "with two SCATTER records");

        std::vector<ScatterSpecies> pal;
        std::vector<std::string> errors;
        // No content root: this fixture names no real mesh on disk, and the existence check is what
        // the SECOND half of this test exercises deliberately -- here the point is field narrowing.
        const bool ok = buildScatterPalette(w.scatterSpecies, "", pal, errors);
        check(ok, "a level with no malformed species reports no errors");
        check(errors.empty(), "...and the errors vector is actually empty, not just 'ok'");
        check(pal.size() == 2, "both species convert");

        const ScatterSpecies& t = pal[0];
        check(t.meshPath == "Meshes/island_tree_03.ocmesh", "species 0: mesh path carries over");
        check(t.material == "M_Bark", "species 0: material carries over");
        check(std::fabs(t.weight - 0.6f) < 1e-5f, "species 0: weight narrows to f32");
        check(std::fabs(t.scaleMin - 0.9f) < 1e-5f && std::fabs(t.scaleMax - 1.6f) < 1e-5f,
              "species 0: scale range narrows");
        check(std::fabs(t.densityMin - 0.8f) < 1e-5f && std::fabs(t.densityMax - 1.0f) < 1e-5f,
              "species 0: density band narrows");
        check(std::fabs(t.collisionRadiusCm - 170.0f) < 1e-3f, "species 0: collision radius narrows");
        check(t.randomizeYaw, "species 0: yaw randomises (no `noyaw` in the record)");

        const ScatterSpecies& g = pal[1];
        check(g.meshPath == "Meshes/grass_medium_01.ocmesh", "species 1: mesh path");
        check(!g.randomizeYaw, "species 1: `noyaw` narrows to randomizeYaw == false");
        check(g.densityMin < -1.0e30f && g.densityMax > 1.0e30f,
              "species 1: the unbounded default lands on ScatterSpecies's OWN sentinel, not f64's");
    }

    // ---- a mesh that exists, and a content root to check it against -------------------------------
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "aver-scatterpalette-test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "Meshes", ec);
        writeFileText((dir / "Meshes" / "real.ocmesh").string(), "not a real mesh, just a stat target");

        fmt::OcScatterSpecies present;
        present.meshPath = "Meshes/real.ocmesh";
        fmt::OcScatterSpecies missing;
        missing.meshPath = "Meshes/does_not_exist.ocmesh";

        std::vector<ScatterSpecies> pal;
        std::vector<std::string> errors;
        const bool ok = buildScatterPalette({present, missing}, dir.string(), pal, errors);
        check(!ok, "a palette naming a mesh absent from the content root fails validation");
        check(pal.size() == 1 && pal[0].meshPath == "Meshes/real.ocmesh",
              "the species whose mesh exists still converts");
        check(anyErrorContains(errors, "species 1", "does_not_exist.ocmesh"),
              "the error names species 1 by index and by its mesh path");
        check(!anyErrorContains(errors, "species 0"), "and species 0 -- which is fine -- is not blamed");

        std::filesystem::remove_all(dir, ec);
    }

    // ---- every other validation rule, and that several bad species are ALL reported ---------------
    {
        fmt::OcScatterSpecies zeroWeight;
        zeroWeight.meshPath = "Meshes/a.ocmesh";
        zeroWeight.weight = 0.0;

        fmt::OcScatterSpecies invertedDensity;
        invertedDensity.meshPath = "Meshes/b.ocmesh";
        invertedDensity.densityMin = 0.9;
        invertedDensity.densityMax = 0.5;

        fmt::OcScatterSpecies invertedScale;
        invertedScale.meshPath = "Meshes/c.ocmesh";
        invertedScale.scaleMin = 2.0;
        invertedScale.scaleMax = 0.5;

        fmt::OcScatterSpecies noMesh;
        // meshPath left empty on purpose.

        fmt::OcScatterSpecies fine;
        fine.meshPath = "Meshes/d.ocmesh";

        std::vector<ScatterSpecies> pal;
        std::vector<std::string> errors;
        // No content root -- these four are broken for reasons that have nothing to do with the
        // filesystem, and the mesh-existence check is skipped entirely when contentDir is empty.
        const bool ok = buildScatterPalette(
            {zeroWeight, invertedDensity, invertedScale, noMesh, fine}, "", pal, errors);

        check(!ok, "a palette with four malformed species fails validation");
        check(errors.size() == 4, "...and reports exactly one error per malformed species, not fewer");
        check(pal.size() == 1 && pal[0].meshPath == "Meshes/d.ocmesh",
              "the one valid species among them still converts");

        check(anyErrorContains(errors, "species 0", "weight"), "species 0 (weight 0) is named and blamed for its weight");
        check(anyErrorContains(errors, "species 1", "densityMin"),
              "species 1 (inverted density) is named and blamed for densityMin above densityMax");
        check(anyErrorContains(errors, "species 2", "scaleMin"),
              "species 2 (inverted scale) is named and blamed for scaleMin above scaleMax");
        check(anyErrorContains(errors, "species 3", "no mesh"), "species 3 (no mesh at all) is named and blamed");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
