// Biomes as PCG graphs (docs/PCG.md, W4): an .ocbiome plus the .ocfoliage types its entries name compile to one
// `DOMAIN pcg` graph, so a biome runs through the same evaluator, cell partitioner and sinks as a hand-authored graph --
// in the editor (Generate Vegetation) and in a streamed world (GeneratedChunkSource).
//
// Per entry:  world grid at the density's spacing -> jitter -> [Clump] -> ProjectToSurface (align cap, ground offset)
//   -> biome mask (EdgeFalloff) -> Slope -> Height -> LayerWeight -> NoiseMask -> yaw + scale -> tag (type, radius, priority)
// then every entry of every biome meets in one Compete (priority, then radius), and each type gets its own mesh spawner
// carrying the type's cull range, LOD bias and collision flag into the .ocinst group.
#pragma once

#include "aver/formats/OcBiome.hpp"
#include "aver/formats/OcFoliage.hpp"
#include "aver/world/PcgVolumeRunner.hpp"

#include <string>
#include <vector>

namespace aver::world {

struct PcgBiomeSource {
    const fmt::OcBiomeData* biome = nullptr;
    // Parallel to biome->entries: the loaded .ocfoliage for each entry.
    std::vector<fmt::OcFoliageData> types;
};

// Nodes are named b<biome>e<entry>_<step>; changing the order of biomes or entries changes the seeds, as it would for a hand graph.
// False (graph empty) when a biome is invalid or `types` does not match its entries.
bool pcgBuildBiomeGraph(const std::vector<PcgBiomeSource>& sources, fmt::OcGraphData& out, std::string* why = nullptr);

// ---- guardrails (W4.7) ----------------------------------------------------------------------------------------------------

struct PcgGuardrails {
    u32 maxInstancesPerCell = 250000;     // the cell's candidates and its results are both held to this
    usize maxCellBytes = 64u << 20;       // estimated result memory for one cell
};

struct PcgBiomeEstimate {
    f64 candidatesPerCell = 0.0;          // grid points (all entries) before any rule thins them
    f64 instancesPerCellMax = 0.0;        // upper bound after the rules: candidates, as mask and rules only remove points
    f64 bytesPerCellMax = 0.0;            // instancesPerCellMax * (48 B .ocinst row + 48 B of working point data)
    std::vector<std::string> warnings;
    bool overCap = false;                 // a cell would exceed the instance cap or the byte cap
};

// Before generating: how big a cell of `cellCm` can get, and what is wrong with the setup. Warns when an entry has no
// cullEndCm, when its mesh has no LODs (`meshHasLods` returns false; null = unknown, not warned), when a density is so high
// the candidate grid would exceed the cap, and when an entry's layer is not in `knownLayers` (non-empty list only).
PcgBiomeEstimate pcgEstimateBiome(const std::vector<PcgBiomeSource>& sources, f32 cellCm, const PcgGuardrails& caps = {},
                                  bool (*meshHasLods)(const std::string& meshPath, void* user) = nullptr, void* user = nullptr,
                                  const std::vector<std::string>& knownLayers = {});

} // namespace aver::world
