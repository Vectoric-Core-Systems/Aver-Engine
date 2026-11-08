// Clean-room cluster partitioner: recursive spatial bisection of the cluster adjacency graph, then
// Fiduccia-Mattheyses boundary refinement on shared-vertex weight. Same contract as
// meshopt_partitionClusters so the builder can swap one for the other: every output group holds
// kMinGroupSize..kMaxGroupSize clusters (a group may be smaller only when fewer than kMinGroupSize
// clusters exist in total).
#pragma once

#include "aver/core/Types.hpp"

namespace aver::trifactor {

inline constexpr u32 kMinGroupSize = 4;
inline constexpr u32 kMaxGroupSizeBisect = 8;

// clusterIndices holds each cluster's vertex ids back to back (clusterIndexCounts[i] of them);
// two clusters are adjacent with weight = number of vertex ids they share. positions is a
// vertexCount x 3 f32 array (stride bytes apart). Writes partition[i] for each of `clusterCount`
// clusters and returns the group count.
usize partitionClustersBisect(u32* partition, const u32* clusterIndices, const u32* clusterIndexCounts,
                              usize clusterCount, const f32* positions, usize vertexCount,
                              usize positionStrideBytes, usize targetGroupSize);

// Total weight of adjacency edges whose endpoints sit in different groups (the quantity the
// refinement minimises); exposed for tests and the partitioner comparison.
u64 partitionCutWeight(const u32* partition, const u32* clusterIndices, const u32* clusterIndexCounts,
                       usize clusterCount, usize vertexCount);

} // namespace aver::trifactor
