#include "VegetationGenerate.hpp"

#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/Paint.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <memory>
#include <cmath>
#include <filesystem>

namespace aver::editor {

namespace {
// The table's folder (Content/Foliage) does not exist in a project that never scattered: the save failed with "write failed".
void makeOutputDir(const std::string& contentDir, const std::string& rel) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(contentDir) / std::filesystem::path(rel).parent_path(), ec);
}
}  // namespace

bool LandSectionsQuery::heightAt(f64 x, f64 y, f32& z) const {
    for (const fmt::OcLandData* d : sections_)
        if (d && landscape::surfaceHeightAt(*d, static_cast<f32>(x), static_cast<f32>(y), z)) return true;
    return false;
}

f32 LandSectionsQuery::layerWeight(std::string_view layer, f64 x, f64 y) const {
    for (const fmt::OcLandData* d : sections_) {
        if (!d || !d->hasLayers()) continue;
        f32 z;
        if (!landscape::surfaceHeightAt(*d, static_cast<f32>(x), static_cast<f32>(y), z)) continue;   // not this section
        u32 idx = 0;
        bool found = false;
        for (u32 i = 0; i < d->layerNames.size(); ++i)
            if (d->layerNames[i] == layer) { idx = i; found = true; break; }
        f32 w[8];
        if (!found || !landscape::layerWeightsAt(*d, static_cast<f32>(x), static_cast<f32>(y), w)) return 0.0f;
        return w[idx];
    }
    return 0.0f;
}

LayerVegetationOutcome generateLayerVegetation(const std::vector<const fmt::OcLandData*>& sections, const LayerVegetationRequest& req,
                                               const std::string& contentDir) {
    LayerVegetationOutcome out;
    if (sections.empty()) { out.error = "no landscape section is loaded"; return out; }
    if (req.ocinstRel.empty()) { out.error = "no output path"; return out; }

    fmt::OcVegetationRecord rec;
    const std::string recRel = std::filesystem::path(req.ocinstRel).replace_extension(".ocveg").generic_string();
    fmt::OcVegetationRecord previous;
    const bool hadPrevious = fmt::loadOcVegetation(contentDir + "/" + recRel, previous, nullptr);
    if (hadPrevious) { rec.added = previous.added; rec.erased = previous.erased; }

    f32 lo[2] = {1.0e30f, 1.0e30f}, hi[2] = {-1.0e30f, -1.0e30f};
    for (const fmt::OcLandData* d : sections) {
        if (!d || !d->valid()) continue;
        lo[0] = std::min(lo[0], d->boundsMin[0]); lo[1] = std::min(lo[1], d->boundsMin[1]);
        hi[0] = std::max(hi[0], d->boundsMax[0]); hi[1] = std::max(hi[1], d->boundsMax[1]);
    }
    if (!(lo[0] < hi[0]) || !(lo[1] < hi[1])) { out.error = "the landscape has no extent"; return out; }
    // The layer's single-entry biome is saved beside the table as a real .ocbiome, so the record can be re-run from its own files.
    const std::string biomeRel = std::filesystem::path(req.ocinstRel).replace_extension(".ocbiome").generic_string();
    rec.biomes = {biomeRel};
    rec.seed = req.seed;
    rec.areaMin[0] = lo[0]; rec.areaMin[1] = lo[1]; rec.areaMax[0] = hi[0]; rec.areaMax[1] = hi[1];
    rec.cellCm = static_cast<i32>(req.cellCm);
    rec.output = req.ocinstRel;

    const fmt::OcBiomeData biome = world::pcgLayerBiome(req.layer, req.foliageRel, req.densityPer100m2, req.minWeight);
    world::VegetationInputs in;
    world::PcgBiomeSource src;
    src.biome = &biome;
    src.types = {req.type};
    in.sources = {src};
    in.landscape = std::make_shared<LandSectionsQuery>(sections);
    for (const fmt::OcLandData* d : sections)
        if (d) for (const std::string& n : d->layerNames) if (!n.empty() && std::find(in.knownLayers.begin(), in.knownLayers.end(), n) == in.knownLayers.end()) in.knownLayers.push_back(n);

    world::VegetationGenerator gen(std::move(in), rec);
    world::VegetationResult r = gen.run();
    out.warnings = r.warnings;
    if (!r.ok) { out.error = r.error; return out; }

    std::string why;
    makeOutputDir(contentDir, req.ocinstRel);
    if (!fmt::saveOcBiome(contentDir + "/" + biomeRel, biome, &why)) { out.error = why; return out; }
    if (!fmt::saveOcInstances(contentDir + "/" + req.ocinstRel, r.table, &why)) { out.error = why; return out; }
    if (!fmt::saveOcVegetation(contentDir + "/" + recRel, gen.record(), &why)) { out.error = why; return out; }
    out.ok = true;
    out.instances = static_cast<u32>(r.table.transforms.size() / 12);
    out.kept = static_cast<u32>(rec.added.size() + rec.erased.size());
    out.table = std::move(r.table);
    return out;
}

namespace {

struct LoadedBiomes {
    std::vector<std::unique_ptr<world::PcgLoadedBiome>> list;   // addresses stay put: the sources point into them
    std::vector<world::PcgBiomeSource> sources;
};

bool loadBiomes(const std::vector<std::string>& rels, const std::string& contentDir, LoadedBiomes& out, std::string& error) {
    if (rels.empty()) { error = "no biome is selected"; return false; }
    for (const std::string& rel : rels) {
        auto b = std::make_unique<world::PcgLoadedBiome>();
        std::string why;
        if (!world::pcgLoadBiome(contentDir, rel, *b, &why)) { error = rel + ": " + why; return false; }
        out.list.push_back(std::move(b));
        out.sources.push_back(out.list.back()->source());
    }
    return true;
}

std::vector<std::string> knownLayersOf(const std::vector<const fmt::OcLandData*>& sections) {
    std::vector<std::string> out;
    for (const fmt::OcLandData* d : sections)
        if (d) for (const std::string& n : d->layerNames)
            if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    return out;
}

} // namespace

BiomeEstimate estimateBiomeVegetation(const std::vector<const fmt::OcLandData*>& sections, const BiomeVegetationRequest& req, const std::string& contentDir) {
    BiomeEstimate out;
    LoadedBiomes lb;
    if (!loadBiomes(req.biomes, contentDir, lb, out.error)) return out;
    const world::PcgBiomeEstimate e = world::pcgEstimateBiome(lb.sources, req.cellCm, {}, nullptr, nullptr, knownLayersOf(sections));
    out.ok = true;
    out.instancesPerCellMax = e.instancesPerCellMax;
    out.bytesPerCellMax = e.bytesPerCellMax;
    out.overCap = e.overCap;
    out.warnings = e.warnings;
    return out;
}

BiomeVegetationOutcome generateBiomeVegetation(const std::vector<const fmt::OcLandData*>& sections, const BiomeVegetationRequest& req,
                                               const std::string& contentDir) {
    BiomeVegetationOutcome out;
    if (sections.empty()) { out.error = "no landscape section is loaded"; return out; }
    if (req.ocinstRel.empty()) { out.error = "no output path"; return out; }
    LoadedBiomes lb;
    if (!loadBiomes(req.biomes, contentDir, lb, out.error)) return out;

    f32 lo[2] = {1.0e30f, 1.0e30f}, hi[2] = {-1.0e30f, -1.0e30f};
    for (const fmt::OcLandData* d : sections) {
        if (!d || !d->valid()) continue;
        lo[0] = std::min(lo[0], d->boundsMin[0]); lo[1] = std::min(lo[1], d->boundsMin[1]);
        hi[0] = std::max(hi[0], d->boundsMax[0]); hi[1] = std::max(hi[1], d->boundsMax[1]);
    }
    if (!(lo[0] < hi[0]) || !(lo[1] < hi[1])) { out.error = "the landscape has no extent"; return out; }

    const std::string recRel = std::filesystem::path(req.ocinstRel).replace_extension(".ocveg").generic_string();
    fmt::OcVegetationRecord rec, previous;
    if (fmt::loadOcVegetation(contentDir + "/" + recRel, previous, nullptr)) { rec.added = previous.added; rec.erased = previous.erased; }
    rec.biomes = req.biomes;
    rec.seed = req.seed;
    rec.areaMin[0] = lo[0]; rec.areaMin[1] = lo[1]; rec.areaMax[0] = hi[0]; rec.areaMax[1] = hi[1];
    rec.cellCm = static_cast<i32>(req.cellCm);
    rec.output = req.ocinstRel;

    world::VegetationInputs in;
    in.sources = lb.sources;
    in.landscape = std::make_shared<LandSectionsQuery>(sections);
    in.knownLayers = knownLayersOf(sections);
    world::VegetationGenerator gen(std::move(in), rec);
    world::VegetationResult r = gen.run();
    out.warnings = r.warnings;
    out.bytesPerCellMax = r.estimate.bytesPerCellMax;
    if (!r.ok) { out.error = r.error; return out; }

    std::string why;
    makeOutputDir(contentDir, req.ocinstRel);
    if (!fmt::saveOcInstances(contentDir + "/" + req.ocinstRel, r.table, &why)) { out.error = why; return out; }
    if (!fmt::saveOcVegetation(contentDir + "/" + recRel, gen.record(), &why)) { out.error = why; return out; }
    out.ok = true;
    out.instances = static_cast<u32>(r.table.transforms.size() / 12);
    out.kept = static_cast<u32>(rec.added.size() + rec.erased.size());
    out.cellsRun = r.cellsRun;
    out.cellsCapped = r.cellsCapped;
    for (const auto& g : r.table.groups) {
        std::string mat;
        for (const world::PcgBiomeSource& s : lb.sources)
            for (const fmt::OcFoliageData& t : s.types) if (mat.empty() && t.meshPath == g.asset) mat = t.material;
        out.groupMaterials.push_back(mat);
    }
    out.table = std::move(r.table);
    return out;
}

} // namespace aver::editor
