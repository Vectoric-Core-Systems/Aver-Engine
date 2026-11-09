// The editor glue behind Scatter Foliage From Layer / Generate Vegetation: landscape sections as a PCG query, the one-entry
// layer biome run, the .ocinst / .ocveg / .ocbiome it writes, and a regenerate that keeps manual edits. No ImGui, no window.
#include "BiomeListModel.hpp"
#include "VegetationGenerate.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// 65 x 65 samples at 50 cm = 3200 cm square at the origin, flat at z = 40. Layer "Grass" is painted on x < 1600.
static fmt::OcLandData makeSection() {
    fmt::OcLandData d;
    d.sampleCount = 65;
    d.spacingCm = 50.0f;
    d.originCm[0] = d.originCm[1] = 0.0f;
    d.originCm[2] = 40.0f;
    d.heights.assign(65u * 65u, 40.0f);
    d.boundsMin[0] = d.boundsMin[1] = 0.0f;
    d.boundsMax[0] = d.boundsMax[1] = 3200.0f;
    d.boundsMin[2] = d.boundsMax[2] = 40.0f;
    d.layerNames = {"Base", "Grass"};
    d.weights.assign(65u * 65u, 0);
    for (u32 y = 0; y < 65; ++y) for (u32 x = 0; x < 65; ++x) d.weights[y * 65 + x] = (x * 50.0f < 1600.0f) ? 255 : 0;
    return d;
}

int main() {
    const fmt::OcLandData land = makeSection();
    check(land.valid() && land.layersValid(), "the fixture section is valid");

    editor::LandSectionsQuery q({&land});
    f32 z = 0;
    check(q.heightAt(800, 800, z) && std::fabs(z - 40.0f) < 0.01f, "heightAt answers inside the section");
    check(!q.heightAt(9000, 800, z), "and false outside it");
    check(q.layerWeight("Grass", 400, 800) > 0.9f && q.layerWeight("Grass", 2400, 800) < 0.1f, "layerWeight follows the painted weights");
    check(q.layerWeight("Nope", 400, 800) == 0.0f, "an unknown layer reads 0");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-vegetation-generate-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "Foliage", ec);
    const std::string contentDir = dir.generic_string();

    editor::LayerVegetationRequest req;
    req.layer = "Grass";
    req.foliageRel = "Foliage/Fern.ocfoliage";
    req.type.meshPath = "Meshes/fern.ocmesh";
    req.type.cullEndCm = 5000.0f;
    req.densityPer100m2 = 30.0f;
    req.minWeight = 0.5f;
    req.seed = 11;
    req.ocinstRel = "Foliage/Grass_vegetation.ocinst";
    req.cellCm = 1600.0f;

    const auto a = editor::generateLayerVegetation({&land}, req, contentDir);
    check(a.ok, "generates (" + a.error + ")");
    // 3200 x 3200 cm = 10.24 units of 100 m^2; the painted half holds about 5.12 x 30 instances.
    check(a.instances > 100 && a.instances < 220, "about the painted half's share of the density (" + std::to_string(a.instances) + ")");
    bool onPaint = !a.table.transforms.empty();
    for (usize i = 0; i + 12 <= a.table.transforms.size(); i += 12) onPaint &= a.table.transforms[i + 9] < 1650.0f;
    check(onPaint, "every instance sits on the painted layer");
    check(a.table.cellCm == 1600.0f && !a.table.cells.empty(), "cell-sorted");
    check(std::filesystem::exists(dir / "Foliage/Grass_vegetation.ocinst") && std::filesystem::exists(dir / "Foliage/Grass_vegetation.ocveg") &&
              std::filesystem::exists(dir / "Foliage/Grass_vegetation.ocbiome"),
          "wrote the .ocinst, the .ocveg record and the .ocbiome beside it");

    fmt::OcInstanceData loaded;
    std::string why;
    check(fmt::loadOcInstances((dir / "Foliage/Grass_vegetation.ocinst").string(), loaded, &why) && loaded.transforms == a.table.transforms,
          "the table on disk is the table generated (" + why + ")");
    fmt::OcVegetationRecord rec;
    check(fmt::loadOcVegetation((dir / "Foliage/Grass_vegetation.ocveg").string(), rec, &why) && rec.count == a.instances &&
              rec.seed == 11 && rec.biomes.size() == 1 && rec.biomes[0] == "Foliage/Grass_vegetation.ocbiome",
          "the record counts the rows and names its biome (" + why + ")");
    fmt::OcBiomeData biome;
    check(fmt::loadOcBiome((dir / "Foliage/Grass_vegetation.ocbiome").string(), biome, &why) && biome.entries.size() == 1 &&
              biome.entries[0].layer == "Grass" && biome.entries[0].densityPer100m2 == 30.0f,
          "the biome file is the layer's rule (" + why + ")");

    // A second run regenerates the same table.
    const auto b = editor::generateLayerVegetation({&land}, req, contentDir);
    check(b.ok && b.table.transforms == a.table.transforms && b.kept == 0, "a second run gives the same table, nothing to keep");

    // Hand edits: an erase stamp and an added rock recorded in the .ocveg survive the next regenerate.
    rec.erased.push_back(fmt::OcVegErased{{400.0f, 800.0f, 40.0f}, 300.0f});
    fmt::OcVegAdded rock;
    rock.asset = "Meshes/rock.ocmesh";
    rock.pos[0] = 123; rock.pos[1] = 456; rock.pos[2] = 40;
    rec.added.push_back(rock);
    check(fmt::saveOcVegetation((dir / "Foliage/Grass_vegetation.ocveg").string(), rec, &why), "a record with hand edits saves");
    const auto c = editor::generateLayerVegetation({&land}, req, contentDir);
    check(c.ok && c.kept == 2, "the next run carries both edits (" + std::to_string(c.kept) + ")");
    check(c.instances != a.instances, "and the table reflects them (" + std::to_string(c.instances) + " vs " + std::to_string(a.instances) + ")");
    bool stampClear = true;
    for (usize i = 0; i + 12 <= c.table.transforms.size(); i += 12) {
        const f32 dx = c.table.transforms[i + 9] - 400.0f, dy = c.table.transforms[i + 10] - 800.0f;
        if (dx * dx + dy * dy <= 300.0f * 300.0f) stampClear = false;
    }
    check(stampClear, "nothing generated remains inside the stamp");
    bool hasRock = false;
    for (const auto& g : c.table.groups) hasRock |= g.asset == "Meshes/rock.ocmesh";
    check(hasRock, "the added rock is in the table");

    check(!editor::generateLayerVegetation({}, req, contentDir).ok, "no sections is refused");
    editor::LayerVegetationRequest bad = req;
    bad.densityPer100m2 = 0.0f;
    check(!editor::generateLayerVegetation({&land}, bad, contentDir).ok, "a zero density is refused");

    // ---- the Landscape mode's biome list: several .ocbiome files, one table ----
    {
        std::string bwhy;
        fmt::OcFoliageData fern = req.type;
        fmt::OcFoliageData rock = req.type;
        rock.meshPath = "Meshes/rock.ocmesh";
        check(fmt::saveOcFoliage((dir / "Foliage/Fern.ocfoliage").string(), fern, &bwhy), "the fern type saves");
        check(fmt::saveOcFoliage((dir / "Foliage/Rock.ocfoliage").string(), rock, &bwhy), "the rock type saves");
        fmt::OcBiomeData grassBiome = world::pcgLayerBiome("Grass", "Foliage/Fern.ocfoliage", 20.0f, 0.5f);
        grassBiome.name = "Meadow";
        fmt::OcBiomeData rockBiome = world::pcgLayerBiome("Base", "Foliage/Rock.ocfoliage", 2.0f, 0.5f);
        rockBiome.name = "Rocky";
        std::filesystem::create_directories(dir / "Biomes", ec);
        check(fmt::saveOcBiome((dir / "Biomes/Meadow.ocbiome").string(), grassBiome, &bwhy) && fmt::saveOcBiome((dir / "Biomes/Rocky.ocbiome").string(), rockBiome, &bwhy), "two biomes save");

        editor::BiomeVegetationRequest br;
        br.biomes = {"Biomes/Meadow.ocbiome"};
        br.seed = 5;
        br.cellCm = 1600.0f;
        br.ocinstRel = "Foliage/Biomes_vegetation.ocinst";
        const auto est = editor::estimateBiomeVegetation({&land}, br, contentDir);
        check(est.ok && est.instancesPerCellMax > 0.0 && est.bytesPerCellMax > 0.0 && !est.overCap, "the estimate reads the biome and is under the cap");
        const auto one = editor::generateBiomeVegetation({&land}, br, contentDir);
        check(one.ok && one.instances > 50, "one biome generates (" + one.error + ", " + std::to_string(one.instances) + ")");
        br.biomes = {"Biomes/Meadow.ocbiome", "Biomes/Rocky.ocbiome"};
        const auto two = editor::generateBiomeVegetation({&land}, br, contentDir);
        check(two.ok && two.instances > one.instances, "a second biome adds its rows (" + std::to_string(two.instances) + " vs " + std::to_string(one.instances) + ")");
        bool hasRockGroup = false;
        for (const auto& g : two.table.groups) hasRockGroup |= g.asset == "Meshes/rock.ocmesh";
        check(hasRockGroup, "the second biome's mesh is in the table");
        check(two.groupMaterials.size() == two.table.groups.size(), "a material slot for every group");
        const auto again = editor::generateBiomeVegetation({&land}, br, contentDir);
        check(again.ok && again.table.transforms == two.table.transforms, "a second run is the same table");
        fmt::OcVegetationRecord brec;
        check(fmt::loadOcVegetation((dir / "Foliage/Biomes_vegetation.ocveg").string(), brec, &bwhy) && brec.biomes == br.biomes && brec.count == two.instances,
              "the record names both biomes and counts the rows");
        const auto files = editor::biomes::listFiles(contentDir, ".ocbiome");
        check(files == (std::vector<std::string>{"Biomes/Meadow.ocbiome", "Biomes/Rocky.ocbiome", "Foliage/Grass_vegetation.ocbiome"}), "listFiles finds the biomes, content-relative and sorted");
        check(editor::biomes::listFiles(contentDir, ".ocfoliage").size() == 2 && editor::biomes::listFiles(contentDir, ".nope").empty(), "listFiles filters by extension");
        const fmt::OcBiomeData starter = editor::biomes::starterBiome("Fresh", "Foliage/Fern.ocfoliage");
        check(starter.valid(), "a starter biome is valid");
        check(editor::biomes::freeBiomePath(contentDir, "Meadow") == "Biomes/Meadow2.ocbiome" && editor::biomes::freeBiomePath(contentDir, "Fresh") == "Biomes/Fresh.ocbiome", "freeBiomePath skips a taken name");
        check(editor::biomes::stemOf("Foliage/Fern.ocfoliage") == "Fern", "stemOf");
        check(editor::biomes::estimateLine(1200.0, 115.0 * 1024.0, false) == "Up to 1200 instances and 115 KB per cell" &&
              editor::biomes::estimateLine(5.0, 3.0 * 1024.0 * 1024.0, true).find("3.0 MB") != std::string::npos, "estimateLine scales to MB and flags the cap");
        check(editor::biomes::outcomeLine(1, 0, 0) == "Generated 1 instance" && editor::biomes::outcomeLine(40, 2, 1).find("kept 2 manual edits") != std::string::npos, "outcomeLine");
        br.biomes = {"Biomes/Missing.ocbiome"};
        const auto missing = editor::generateBiomeVegetation({&land}, br, contentDir);
        check(!missing.ok && missing.error.find("Missing") != std::string::npos, "a biome that cannot load is refused, naming it");
        br.biomes.clear();
        check(!editor::generateBiomeVegetation({&land}, br, contentDir).ok && !editor::estimateBiomeVegetation({&land}, br, contentDir).ok, "no biome selected is refused");
        br.biomes = {"Biomes/Meadow.ocbiome"};
        check(!editor::generateBiomeVegetation({}, br, contentDir).ok, "no sections is refused");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO("VegetationGenerateTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
