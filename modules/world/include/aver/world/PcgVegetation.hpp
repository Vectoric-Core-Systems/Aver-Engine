// Generate Vegetation, the model (docs/PCG.md, W4): run biomes over a landscape area cell by cell, write a cell-sorted
// `.ocinst`, and keep a record (`.ocveg`) so the run can be repeated. Hand edits made afterwards are kept as an override set
// (brush erasures and additions) that every regenerate re-applies on top of the fresh baseline.
//
// No GPU, no editor: the editor's Generate button, a build tool and the tests all call this. Landscape access is the
// ILandscapeQuery the PCG evaluator already uses.
#pragma once

#include "aver/formats/OcInstances.hpp"
#include "aver/formats/OcVegetation.hpp"
#include "aver/world/PcgBiome.hpp"
#include "aver/world/PcgSinks.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aver::world {

struct VegetationInputs {
    std::vector<PcgBiomeSource> sources;                  // loaded biomes, each with its foliage types
    std::shared_ptr<const ILandscapeQuery> landscape;
    std::map<std::string, std::vector<Vec3>> splines;     // for spline masks
    std::map<std::string, PcgBox> volumes;                // for volume masks
    std::vector<std::string> knownLayers;                 // paint layers the landscape has (guardrail warning only)
    bool (*meshHasLods)(const std::string& meshPath, void* user) = nullptr;   // guardrail warning only
    void* user = nullptr;
};

struct VegetationResult {
    bool ok = false;
    std::string error;                    // set when !ok
    fmt::OcInstanceData table;            // cell-sorted, overrides applied
    std::vector<std::string> warnings;    // guardrails and evaluator warnings
    PcgBiomeEstimate estimate;
    u32 generated = 0;                    // baseline rows before overrides
    u32 erasedByOverride = 0, addedByOverride = 0;
    u32 cellsRun = 0, cellsReused = 0, cellsCapped = 0;
    u64 inputHash = 0;
};

class VegetationGenerator {
public:
    VegetationGenerator(VegetationInputs inputs, fmt::OcVegetationRecord record, PcgGuardrails caps = {});

    // Runs every cell of the record's area. Cells already generated under the same graph and environment are reused, so a
    // repeat run after editing one biome entry regenerates only what changed (every cell, as the graph hash changes) and a
    // repeat after invalidate() regenerates only the marked cells. With the estimate over the guardrail the run is refused
    // (ok = false) unless `ignoreCaps`.
    VegetationResult run(bool ignoreCaps = false);

    // The landscape changed inside `box` (a sculpt or paint stroke): the cells it can reach regenerate on the next run.
    u32 invalidate(const PcgBox& box);

    // Replaces the inputs (an edited biome or foliage type); the next run regenerates the cells the changed graph makes stale.
    void setInputs(VegetationInputs inputs);
    // Landscape, splines or volumes changed everywhere: bump the environment stamp.
    void bumpEnvironment() { ++envStamp_; }

    // ---- the manual override set ----
    void eraseAt(const Vec3& center, f32 radiusCm);
    void addInstance(const std::string& asset, const Transform& xf);
    void clearOverrides() { rec_.added.clear(); rec_.erased.clear(); }

    const fmt::OcVegetationRecord& record() const { return rec_; }
    // The hash a run with these inputs records; compare with a saved record to tell a stale table.
    u64 currentInputHash() const;

private:
    bool buildProgram(std::string* why);

    VegetationInputs in_;
    fmt::OcVegetationRecord rec_;
    PcgGuardrails caps_;
    std::shared_ptr<PcgProgram> program_;
    std::unique_ptr<PcgCellStore> store_;
    u64 envStamp_ = 0;
};

// Convenience for the editor's layer button and the tests: the one-entry biome "this foliage type on this paint layer".
fmt::OcBiomeData pcgLayerBiome(const std::string& layer, const std::string& foliagePath, f32 densityPer100m2, f32 minWeight);

} // namespace aver::world
