// The bitonic sorting network (Batcher 1968) the GPU tier sorts translucent particles with, as pure index math: the
// HLSL (particles_sort.hlsl) uses the same formulas, ParticleSortNetworkTest runs the same network on the CPU.
//
// For a power-of-two N, a step is (k, j): element i is compared with i ^ j (only when that is greater than i), ascending
// when (i & k) == 0, descending otherwise. Steps run k = 2, 4, ..., N and for each k j = k/2, ..., 1. The GPU groups them:
//   Local        every step with k <= tile, inside a groupshared tile (one dispatch)
//   Global       one step with j >= tile (one dispatch per step)
//   LocalMerge   the remaining steps j < tile of one k > tile, inside a tile (one dispatch)
// For 65,536 entries and a 2,048 tile that is 21 dispatches.
#pragma once
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::particles {

inline constexpr u32 kSortTile = 2048;                  // entries per groupshared tile (1024 threads x 2)
inline constexpr u32 kSortMaxEntries = 65536;           // above this an emitter is not sorted

struct SortEntry {
    u32 key;    // ~asuint(viewDepth): ascending key == back to front
    u32 slot;   // particle slot
};
inline u64 sortKey64(const SortEntry& e) { return (static_cast<u64>(e.key) << 32) | e.slot; }

inline u32 sortNextPow2(u32 n) {
    u32 p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Partner of element i at stride j; false when i is the upper element of its pair (it does nothing in this step).
inline bool sortPartner(u32 i, u32 j, u32& partner) {
    partner = i ^ j;
    return partner > i;
}
inline bool sortAscending(u32 i, u32 k) { return (i & k) == 0; }

// Steps in the whole network for n = 2^m entries: m (m + 1) / 2.
inline u32 sortStepCount(u32 pow2) {
    u32 m = 0;
    while ((1u << m) < pow2) ++m;
    return m * (m + 1) / 2;
}

enum class SortPassKind : u8 { Local, Global, LocalMerge };
struct SortPass {
    SortPassKind kind;
    u32 k;   // Local: the largest k handled (min(tile, N)); Global and LocalMerge: the merge size
    u32 j;   // Global: the stride; others: unused
};

// The dispatch plan for a power-of-two entry count.
inline void buildSortPlan(u32 pow2, std::vector<SortPass>& out) {
    out.clear();
    if (pow2 < 2) return;
    const u32 tile = pow2 < kSortTile ? pow2 : kSortTile;
    out.push_back({SortPassKind::Local, tile, 0});
    for (u32 k = tile << 1; k <= pow2; k <<= 1) {
        for (u32 j = k >> 1; j >= tile; j >>= 1) out.push_back({SortPassKind::Global, k, j});
        out.push_back({SortPassKind::LocalMerge, k, 0});
    }
}

// Sorts `a` (size a power of two) ascending by sortKey64 with the network above, executing the plan's passes as plain
// loops. The reference ParticleSortNetworkTest checks against std::sort.
void sortNetworkReference(std::vector<SortEntry>& a);

} // namespace aver::particles
