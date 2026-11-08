// Builder time and cluster/triangle counts per fixture mesh, for before/after comparison of cook
// changes. Prints one table; asserts nothing. Not a ctest target (the name has no "Test" suffix).
#include "TrifactorFixtures.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <cstdio>

using namespace aver;

static void report(const char* name, fmt::OcMeshData m) {
    trifactor::LodDag dag;
    std::string why;
    const auto t0 = std::chrono::steady_clock::now();
    const bool a = trifactor::buildClusters(m, dag, &why);
    const bool b = a && trifactor::buildLodHierarchy(m, dag, &why);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!b) { std::printf("%-28s FAILED: %s\n", name, why.c_str()); return; }
    const trifactor::ValidationReport vr = trifactor::validateLodDag(m, dag);
    u64 tris0 = 0, trisTop = 0;
    for (u32 id : dag.levels.front()) tris0 += dag.clusters[id].triangleCount();
    for (u32 id : dag.levels.back()) trisTop += dag.clusters[id].triangleCount();
    std::printf("%-28s tris %7llu | clusters L0 %5zu total %6zu | levels %2u | coarsest tris %6llu | groups %5zu | build %8.1f ms | valid %s\n",
                name, (unsigned long long)tris0, dag.levels.front().size(), dag.clusters.size(), dag.levelCount(),
                (unsigned long long)trisTop, dag.groupNodes.size(), ms, vr.ok ? "yes" : "NO");
}

int main() {
    report("grid150 1 material", trifix::makeGrid(150));
    {
        auto m = trifix::makeGrid(150);
        const u32 n = 149;
        trifix::splitByMaterial(m, 4, [&](u32 t) { return (t / 2 % n) * 4 / n; });
        report("grid150 4 stripes", std::move(m));
    }
    report("sphere 1 material", trifix::makeSphere(200, 100));
    {
        auto m = trifix::makeSphere(200, 100);
        trifix::splitByMaterial(m, 3, [](u32 t) { return t * 3 / 40000; });
        report("sphere 3 bands", std::move(m));
    }
    {
        auto m = trifix::makeGrid(60);
        trifix::addQuadFragments(m, 600);
        report("grid60+600 cards 1 mat", m);
        const u32 base = 59 * 59 * 2;
        trifix::splitByMaterial(m, 2, [&](u32 t) { return t < base ? 0u : 1u; });
        report("grid60+600 cards 2 mat", std::move(m));
    }
    return 0;
}
