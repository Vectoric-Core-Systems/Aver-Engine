// Builder time and cluster/triangle counts per fixture mesh, for before/after comparison of cook
// changes. Prints one table; asserts nothing. Not a ctest target (the name has no "Test" suffix).
#include "TrifactorFixtures.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/trifactor/MeshReport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

struct Totals { u64 locked = 0, coarsest = 0, groups = 0; double ms = 0; u32 bad = 0; };

static void report(const char* name, fmt::OcMeshData m, trifactor::GroupPartitioner which, Totals* tot = nullptr) {
    trifactor::setGroupPartitioner(which);
    trifactor::LodDag dag;
    std::string why;
    const auto t0 = std::chrono::steady_clock::now();
    const bool a = trifactor::buildClusters(m, dag, &why);
    const bool b = a && trifactor::buildLodHierarchy(m, dag, &why);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!b) { std::printf("%-28s FAILED: %s\n", name, why.c_str()); return; }
    const trifactor::ValidationReport vr = trifactor::validateLodDag(m, dag);
    const trifactor::LodReport lr = trifactor::measureLodDag(dag, m);
    u64 tris0 = 0;
    for (u32 id : dag.levels.front()) tris0 += dag.clusters[id].triangleCount();
    std::printf("%-28s %-7s tris %7llu | clusters L0 %5zu total %6zu | levels %2u | coarsest %6llu | groups %5zu | locked %8llu | build %8.1f ms | valid %s\n",
                name, which == trifactor::GroupPartitioner::Bisect ? "bisect" : "meshopt", (unsigned long long)tris0,
                dag.levels.front().size(), dag.clusters.size(), dag.levelCount(), (unsigned long long)lr.coarsestTriangles,
                dag.groupNodes.size(), (unsigned long long)lr.lockedEdgesTotal, ms, vr.ok ? "yes" : "NO");
    if (tot) {
        tot->locked += lr.lockedEdgesTotal; tot->coarsest += lr.coarsestTriangles;
        tot->groups += dag.groupNodes.size(); tot->ms += ms; tot->bad += vr.ok ? 0 : 1;
    }
}

static void both(const char* name, const fmt::OcMeshData& m, Totals* tm = nullptr, Totals* tb = nullptr) {
    report(name, m, trifactor::GroupPartitioner::Meshopt, tm);
    report(name, m, trifactor::GroupPartitioner::Bisect, tb);
}

int main(int argc, char** argv) {
    // Corpus mode: BuildBench <dir-or-file>... compares both partitioners on real cooked meshes
    // (LOD 0 only, as RelodTool rebuilds it). Never writes.
    if (argc > 1) {
        Totals tm, tb;
        std::vector<std::filesystem::path> files;
        for (int i = 1; i < argc; ++i) {
            std::error_code ec;
            if (std::filesystem::is_directory(argv[i], ec)) {
                for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i], ec))
                    if (e.is_regular_file() && e.path().extension() == ".ocmesh") files.push_back(e.path());
            } else files.push_back(argv[i]);
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            fmt::OcMeshData md, src;
            std::string why;
            if (!fmt::loadOcMesh(f.string(), md, &why)) continue;
            if (md.hasSkin() || md.indices.size() < 3 * 2000) continue;   // virtualization candidates only
            src.positions = md.positions; src.normals = md.normals; src.uvs = md.uvs; src.indices = md.indices;
            src.boundsMin = md.boundsMin; src.boundsMax = md.boundsMax; src.flags = md.flags;
            src.submeshes = md.submeshes; src.materialSlots = md.materialSlots;
            both(f.filename().string().c_str(), src, &tm, &tb);
        }
        std::printf("TOTAL meshopt: locked %llu coarsest %llu groups %llu build %.0f ms invalid %u\n",
                    (unsigned long long)tm.locked, (unsigned long long)tm.coarsest, (unsigned long long)tm.groups, tm.ms, tm.bad);
        std::printf("TOTAL bisect : locked %llu coarsest %llu groups %llu build %.0f ms invalid %u\n",
                    (unsigned long long)tb.locked, (unsigned long long)tb.coarsest, (unsigned long long)tb.groups, tb.ms, tb.bad);
        return 0;
    }
    both("grid150 1 material", trifix::makeGrid(150));
    {
        auto m = trifix::makeGrid(150);
        const u32 n = 149;
        trifix::splitByMaterial(m, 4, [&](u32 t) { return (t / 2 % n) * 4 / n; });
        both("grid150 4 stripes", std::move(m));
    }
    both("sphere 1 material", trifix::makeSphere(200, 100));
    {
        auto m = trifix::makeSphere(200, 100);
        trifix::splitByMaterial(m, 3, [](u32 t) { return t * 3 / 40000; });
        both("sphere 3 bands", std::move(m));
    }
    {
        auto m = trifix::makeGrid(60);
        trifix::addQuadFragments(m, 600);
        both("grid60+600 cards 1 mat", m);
        const u32 base = 59 * 59 * 2;
        trifix::splitByMaterial(m, 2, [&](u32 t) { return t < base ? 0u : 1u; });
        both("grid60+600 cards 2 mat", std::move(m));
    }
    return 0;
}
