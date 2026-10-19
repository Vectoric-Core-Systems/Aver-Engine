// The CPU execution of the bitonic network: the pass plan run as loops, so the test can check the plan itself.
#include "aver/particles/ParticleSortNetwork.hpp"

#include <algorithm>

namespace aver::particles {
namespace {

void compareSwap(std::vector<SortEntry>& a, u32 i, u32 partner, bool ascending) {
    const bool outOfOrder = ascending ? sortKey64(a[i]) > sortKey64(a[partner]) : sortKey64(a[i]) < sortKey64(a[partner]);
    if (outOfOrder) std::swap(a[i], a[partner]);
}

void step(std::vector<SortEntry>& a, u32 k, u32 j, u32 lo, u32 hi) {
    for (u32 i = lo; i < hi; ++i) {
        u32 p;
        if (sortPartner(i, j, p)) compareSwap(a, i, p, sortAscending(i, k));
    }
}

} // namespace

void sortNetworkReference(std::vector<SortEntry>& a) {
    const u32 n = static_cast<u32>(a.size());
    if (n < 2) return;
    std::vector<SortPass> plan;
    buildSortPlan(n, plan);
    const u32 tile = n < kSortTile ? n : kSortTile;
    for (const SortPass& p : plan) {
        switch (p.kind) {
        case SortPassKind::Local:
            for (u32 t = 0; t < n; t += tile)
                for (u32 k = 2; k <= p.k; k <<= 1)
                    for (u32 j = k >> 1; j > 0; j >>= 1) step(a, k, j, t, t + tile);
            break;
        case SortPassKind::Global:
            step(a, p.k, p.j, 0, n);
            break;
        case SortPassKind::LocalMerge:
            for (u32 t = 0; t < n; t += tile)
                for (u32 j = tile >> 1; j > 0; j >>= 1) step(a, p.k, j, t, t + tile);
            break;
        }
    }
}

} // namespace aver::particles
