#pragma once
// Editor glue for Generate Vegetation (docs/PCG.md, W4): the landscape sections as a PCG landscape query, and the one-call
// "this foliage type on this paint layer" run behind the Landscape tab's Scatter Foliage From Layer button. The model itself
// is aver::world::VegetationGenerator; nothing here touches ImGui, the scene or a window, so a headless test drives it.
#include "aver/formats/OcFoliage.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/formats/OcLand.hpp"
#include "aver/world/PcgVegetation.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// Heights and paint weights across landscape sections (absolute world cm). Where sections overlap the first one wins.
class LandSectionsQuery final : public world::ILandscapeQuery {
public:
    explicit LandSectionsQuery(std::vector<const fmt::OcLandData*> sections) : sections_(std::move(sections)) {}
    bool heightAt(f64 x, f64 y, f32& z) const override;
    f32 layerWeight(std::string_view layer, f64 x, f64 y) const override;
private:
    std::vector<const fmt::OcLandData*> sections_;
};

struct LayerVegetationRequest {
    std::string layer;              // paint layer name
    std::string foliageRel;         // content-relative .ocfoliage
    fmt::OcFoliageData type;
    f32 densityPer100m2 = 10.0f;
    f32 minWeight = 0.5f;
    i32 seed = 0;
    std::string ocinstRel;          // content-relative output table; the record is the same path with .ocveg
    f32 cellCm = 6400.0f;
};

struct LayerVegetationOutcome {
    bool ok = false;
    std::string error;
    u32 instances = 0;
    u32 kept = 0;                   // manual overrides carried over from the previous record (erase stamps + additions)
    std::vector<std::string> warnings;
    fmt::OcInstanceData table;      // what was written
};

// Runs the layer's one-entry biome over the union of the sections, writes <contentDir>/<ocinstRel> (cell-sorted) and the
// .ocveg record beside it. An existing record at that path supplies the manual override set, so a regenerate keeps hand edits.
LayerVegetationOutcome generateLayerVegetation(const std::vector<const fmt::OcLandData*>& sections, const LayerVegetationRequest& req,
                                               const std::string& contentDir);

// The Landscape mode's biome list: several `.ocbiome` files run together over the landscape (priority decides overlaps), into one `.ocinst`
// and its `.ocveg` record. Manual edits recorded in an existing record are kept, as for a layer run.
struct BiomeVegetationRequest {
    std::vector<std::string> biomes;   // content-relative .ocbiome paths, in the order they are listed (order is the seed order)
    i32 seed = 0;
    f32 cellCm = 6400.0f;
    std::string ocinstRel;             // content-relative output; the record is the same path with .ocveg
};

struct BiomeVegetationOutcome {
    bool ok = false;
    std::string error;
    u32 instances = 0;
    u32 kept = 0;
    u32 cellsRun = 0, cellsCapped = 0;
    f64 bytesPerCellMax = 0.0;         // the estimate that gated the run
    std::vector<std::string> warnings;
    fmt::OcInstanceData table;
    std::vector<std::string> groupMaterials;   // parallel to table.groups: the material of the foliage type whose mesh the group is
};

// What a run would cost, without running it: the guardrail estimate for the listed biomes, or why they cannot load.
struct BiomeEstimate {
    bool ok = false;
    std::string error;
    f64 instancesPerCellMax = 0.0, bytesPerCellMax = 0.0;
    bool overCap = false;
    std::vector<std::string> warnings;
};
BiomeEstimate estimateBiomeVegetation(const std::vector<const fmt::OcLandData*>& sections, const BiomeVegetationRequest& req, const std::string& contentDir);

BiomeVegetationOutcome generateBiomeVegetation(const std::vector<const fmt::OcLandData*>& sections, const BiomeVegetationRequest& req,
                                               const std::string& contentDir);

} // namespace aver::editor
