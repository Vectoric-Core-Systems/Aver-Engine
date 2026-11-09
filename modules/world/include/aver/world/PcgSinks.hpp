// Where PCG results go (docs/PCG.md): a baked instance table, a streaming chunk payload, or actor spawns.
#pragma once

#include "aver/formats/OcInstances.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/PcgGraph.hpp"

#include <string>
#include <vector>

namespace aver::world {

// (a) `.ocinst` rows, one group per (mesh, material) in first-seen order. cellCm > 0 sorts them into XY cells
// (fmt::cellOcInstances) with groupRadiusCm[group] growing each cell's bounds; 0 leaves one always-resident table.
bool pcgToInstanceData(const PcgResult& r, fmt::OcInstanceData& out, f32 cellCm = 0.0f,
                       const std::vector<f32>& groupRadiusCm = {}, std::string* why = nullptr);

// (b) The entities of chunk `c` (XY half-open, the same ownership rule the cell generator uses). Entities are sorted by
// (seed, group, position) and named from the seed, never from a counter, so the payload bytes are a pure function of
// the graph, the seed and the coordinate -- which is what lets persistChunk compare a chunk against its regeneration.
ChunkPayload pcgToChunkPayload(const PcgResult& r, const ChunkCoord& c, i32 chunkSizeCm);

// (c) Actor spawns owned by the XY box (half-open), canonical order. The host spawns them (Aver.Pcg.Apply on the C# side).
std::vector<PcgActorSpawn> pcgActorsIn(const PcgResult& r, const PcgBox& box);

} // namespace aver::world
