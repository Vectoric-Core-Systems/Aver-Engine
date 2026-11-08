// The clean-room cluster partitioner (Aver.Trifactor ClusterPartition): group sizes, full coverage,
// cut quality against a naive baseline, and that a DAG built with it is valid. Pure CPU.
#include "TrifactorFixtures.hpp"
#include "aver/core/Log.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/trifactor/ClusterPartition.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::trifactor;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// A w x h lattice of unit "clusters"; cluster (x,y) owns the four corner vertices of its cell, so
// neighbours share two vertices (edge) and diagonal cells share one.
struct Lattice {
    std::vector<u32> idx, counts;
    std::vector<f32> pos;
    usize n = 0, vcount = 0;
};
static Lattice makeLattice(u32 w, u32 h) {
    Lattice l;
    l.vcount = usize(w + 1) * (h + 1);
    for (u32 y = 0; y <= h; ++y) for (u32 x = 0; x <= w; ++x) l.pos.insert(l.pos.end(), {f32(x), f32(y), 0.0f});
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 v = y * (w + 1) + x;
            l.idx.insert(l.idx.end(), {v, v + 1, v + w + 1, v + w + 2});
            l.counts.push_back(4);
        }
    l.n = usize(w) * h;
    return l;
}

static void sizesOk(const char* name, const Lattice& l, const std::vector<u32>& part, usize groups, bool exactMin) {
    std::vector<u32> sz(groups, 0);
    for (u32 g : part) { if (g >= groups) { check(false, std::string(name) + ": group id in range"); return; } ++sz[g]; }
    u32 mn = ~0u, mx = 0;
    for (u32 s : sz) { mn = std::min(mn, s); mx = std::max(mx, s); }
    check(mx <= kMaxGroupSizeBisect, std::string(name) + ": no group above the max");
    check(!exactMin || mn >= kMinGroupSize, std::string(name) + ": no group below the min");
    check(mn > 0, std::string(name) + ": no empty group");
    (void)l;
}

int main() {
    for (auto [w, h] : {std::pair<u32, u32>{20, 20}, {37, 11}, {5, 5}, {3, 1}, {1, 1}}) {
        const Lattice l = makeLattice(w, h);
        std::vector<u32> part(l.n);
        const usize groups = partitionClustersBisect(part.data(), l.idx.data(), l.counts.data(), l.n, l.pos.data(),
                                                     l.vcount, sizeof(f32) * 3, 6);
        const std::string name = std::to_string(w) + "x" + std::to_string(h);
        sizesOk(name.c_str(), l, part, groups, l.n >= kMinGroupSize);
        const u64 cut = partitionCutWeight(part.data(), l.idx.data(), l.counts.data(), l.n, l.vcount);
        if (l.n >= 100) {
            // naive baseline: consecutive rows of 6 in scanline order
            std::vector<u32> naive(l.n);
            for (usize i = 0; i < l.n; ++i) naive[i] = static_cast<u32>(i / 6);
            const u64 base = partitionCutWeight(naive.data(), l.idx.data(), l.counts.data(), l.n, l.vcount);
            AVER_INFO("  {} cut {} vs scanline {}", name, cut, base);
            check(cut < base, name + ": cut weight beats scanline grouping");
        }
    }

    // determinism
    {
        const Lattice l = makeLattice(30, 30);
        std::vector<u32> a(l.n), b(l.n);
        partitionClustersBisect(a.data(), l.idx.data(), l.counts.data(), l.n, l.pos.data(), l.vcount, 12, 6);
        partitionClustersBisect(b.data(), l.idx.data(), l.counts.data(), l.n, l.pos.data(), l.vcount, 12, 6);
        check(a == b, "same input, same partition");
    }
    check(partitionClustersBisect(nullptr, nullptr, nullptr, 0, nullptr, 0, 12, 6) == 0, "empty input is zero groups");

    // a DAG built with the bisect partitioner is valid
    setGroupPartitioner(GroupPartitioner::Bisect);
    for (int which = 0; which < 3; ++which) {
        fmt::OcMeshData m = which == 0 ? trifix::makeGrid(100) : which == 1 ? trifix::makeSphere(120, 60) : trifix::makeGrid(60);
        if (which == 2) trifix::addQuadFragments(m, 300);
        LodDag dag;
        std::string why;
        const bool built = buildClusters(m, dag, &why) && buildLodHierarchy(m, dag, &why);
        check(built, "builds with the bisect partitioner");
        const ValidationReport vr = validateLodDag(m, dag);
        check(vr.ok && validateClusterHierarchy(dag, &why), "the DAG validates (coverage, limits, monotone error, hierarchy)");
        check(dag.levelCount() >= 3, "the ladder has several levels");
    }
    setGroupPartitioner(GroupPartitioner::Meshopt);

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
