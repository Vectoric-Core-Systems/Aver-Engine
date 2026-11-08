// Cluster paging, CPU half (Aver.Trifactor ClusterPages): page plan structure, projected-error
// demand, and the residency planner's invariants. Pure CPU, meshes synthesised in memory.
#include "TrifactorFixtures.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterAdapt.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/trifactor/ClusterPages.hpp"

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

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return u32(s >> 33); }
    f32 unit() { return f32(next() & 0xFFFFFF) / f32(0x1000000); }
    f32 range(f32 a, f32 b) { return a + (b - a) * unit(); }
};

static bool cook(fmt::OcMeshData& m) {
    LodDag dag;
    std::string why;
    return buildClusters(m, dag, &why) && buildLodHierarchy(m, dag, &why) && validateLodDag(m, dag).ok &&
           packLodDag(dag, m, &why);
}

// Every resident page has all its parent pages resident, and roots are resident.
static bool closed(const PagePlan& plan, const ResidencyPlan& r) {
    for (const ClusterPage& p : plan.pages) {
        if (p.isRoot() && !r.resident[p.id]) return false;
        if (!r.resident[p.id]) continue;
        for (u32 pp : p.parentPages) if (!r.resident[pp]) return false;
    }
    return true;
}

static u64 residentBytes(const PagePlan& plan, const ResidencyPlan& r) {
    u64 b = 0;
    for (const ClusterPage& p : plan.pages) if (r.resident[p.id]) b += p.bytes;
    return b;
}

static void runMesh(const char* name, fmt::OcMeshData m) {
    AVER_INFO("--- {}", name);
    check(cook(m), "cooks");
    std::vector<ClusterView> views;
    std::string why;
    check(buildClusterDagViews(m, views, &why), "DAG views build");
    const std::vector<u32> sizes = clusterRecordSizes(m);
    check(sizes.size() == views.size(), "one record size per cluster");

    PagePlan plan;
    const u32 pageBytes = 6000;
    check(buildPagePlan(views, sizes, pageBytes, plan, &why), "page plan builds");
    check(validatePagePlan(plan, views, sizes, &why), "page plan validates: " + why);
    check(plan.pages.size() > 4, "several pages at a small budget");
    u64 sum = 0;
    for (u32 s : sizes) sum += s;
    check(plan.totalBytes == sum, "page bytes add up to the cluster bytes");
    check(plan.rootBytes > 0 && plan.rootBytes < plan.totalBytes, "root pages are a strict part of the total");
    for (const ClusterPage& p : plan.pages) if (p.isRoot()) {
        check(p.level == views.back().level, "root pages sit at the coarsest level");
        break;
    }

    // a corrupted plan is caught
    PagePlan bad = plan;
    bad.pageOfCluster[0] = (bad.pageOfCluster[0] + 1) % static_cast<u32>(bad.pages.size());
    check(!validatePagePlan(bad, views, sizes), "a wrong pageOfCluster is rejected");
    bad = plan;
    bad.pages[0].bytes += 1;
    check(!validatePagePlan(bad, views, sizes), "a wrong page size is rejected");

    // a single big page is the degenerate plan
    PagePlan one;
    check(buildPagePlan(views, sizes, 1u << 30, one, &why) && validatePagePlan(one, views, sizes) &&
          one.pages.size() <= (views.back().level + 1), "an unbounded budget gives one page per level");

    // residency planner over random cameras and budgets
    Rng rng;
    const Vec3 c = (Vec3{m.boundsMin.x, m.boundsMin.y, m.boundsMin.z} + Vec3{m.boundsMax.x, m.boundsMax.y, m.boundsMax.z}) * 0.5f;
    u32 closureBreaks = 0, overBudget = 0, demandMiss = 0;
    f32 nearLoaded = 0, farLoaded = 0;
    for (int i = 0; i < 60; ++i) {
        View v;
        const f32 dist = i % 2 ? rng.range(300.0f, 900.0f) : rng.range(20000.0f, 60000.0f);
        v.eye = c + Vec3{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)} * dist;
        v.viewportHeightPx = 1080.0f;
        const std::vector<f32> demand = computePageDemand(plan, views, 1.0f, v);
        const u64 budget = plan.rootBytes + u64(rng.unit() * f32(plan.totalBytes - plan.rootBytes));
        const ResidencyPlan r = planResidency(plan, demand, budget, {});
        if (!closed(plan, r)) ++closureBreaks;
        if (residentBytes(plan, r) > budget || residentBytes(plan, r) != r.residentBytes) ++overBudget;

        // with everything allowed, every demanded page is resident
        const ResidencyPlan full = planResidency(plan, demand, plan.totalBytes, {});
        for (const ClusterPage& p : plan.pages) if (demand[p.id] > 0.0f && !full.resident[p.id]) ++demandMiss;
        if (!closed(plan, full)) ++closureBreaks;
        (i % 2 ? nearLoaded : farLoaded) += static_cast<f32>(full.residentBytes);
    }
    check(closureBreaks == 0, "every resident page has its parent pages resident (60 views)");
    check(overBudget == 0, "resident bytes never exceed the budget and match the report");
    check(demandMiss == 0, "with room, every demanded page is resident");
    check(nearLoaded > farLoaded, "a near camera needs more resident data than a far one");

    // budget below the roots: roots stay, flagged
    {
        View v; v.eye = c + Vec3{300, 0, 0};
        const ResidencyPlan r = planResidency(plan, computePageDemand(plan, views, 1.0f, v), plan.rootBytes / 2, {});
        check(!r.rootsFit && r.residentBytes == plan.rootBytes && closed(plan, r), "roots stay resident when the budget cannot hold them");
    }

    // hysteresis: with one page of slack, a resident page beats an equal-demand newcomer
    {
        View v; v.eye = c + Vec3{500, 0, 0};
        const std::vector<f32> demand = computePageDemand(plan, views, 1.0f, v);
        const u64 budget = plan.rootBytes + (plan.totalBytes - plan.rootBytes) / 3;
        const ResidencyPlan a = planResidency(plan, demand, budget, {});
        const ResidencyPlan again = planResidency(plan, demand, budget, a.resident);
        check(again.load.empty() && again.evict.empty() && again.resident == a.resident,
              "an unchanged view with the same budget loads and evicts nothing");
    }
}

int main() {
    runMesh("sphere", trifix::makeSphere(120, 60));
    {
        auto g = trifix::makeGrid(120);
        const u32 n = 119;
        trifix::splitByMaterial(g, 3, [&](u32 t) { return (t / 2 % n) * 3 / n; });
        runMesh("grid 3 materials", std::move(g));
    }
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
