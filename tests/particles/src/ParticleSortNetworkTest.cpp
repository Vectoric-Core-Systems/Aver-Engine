// The bitonic sorting network the GPU tier sorts translucent particles with, run on the CPU: the pass plan against
// std::sort for many sizes, the step-count formula and the pure partner / direction functions. Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleSortNetwork.hpp"

#include <algorithm>
#include <string>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// xorshift: reproducible without <random>.
static u32 rng(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static bool sortsLikeStd(u32 n, u32 seed, bool manyDuplicates) {
    u32 s = seed;
    std::vector<SortEntry> a(n);
    for (u32 i = 0; i < n; ++i) {
        a[i].key = manyDuplicates ? rng(s) % 8u : rng(s);
        a[i].slot = i;   // unique, so (key, slot) is a total order
    }
    std::vector<SortEntry> want = a;
    std::sort(want.begin(), want.end(), [](const SortEntry& x, const SortEntry& y) { return sortKey64(x) < sortKey64(y); });
    sortNetworkReference(a);
    for (u32 i = 0; i < n; ++i)
        if (a[i].key != want[i].key || a[i].slot != want[i].slot) return false;
    return true;
}

int main() {
    AVER_INFO("ParticleSortNetworkTest");

    // ---- pure index math ----
    {
        u32 p;
        check(sortPartner(0, 1, p) && p == 1, "element 0 pairs with 1 at stride 1");
        check(!sortPartner(1, 1, p), "the upper element of a pair does nothing");
        check(sortPartner(3, 4, p) && p == 7, "stride 4: 3 pairs with 7");
        check(sortAscending(5, 2) == ((5u & 2u) == 0), "direction is bit k of the index");
        check(sortAscending(0, 8) && sortAscending(7, 8) && !sortAscending(8, 8) && !sortAscending(15, 8) && sortAscending(16, 8),
              "direction alternates every k elements");
        check(sortNextPow2(1) == 1 && sortNextPow2(2) == 2 && sortNextPow2(3) == 4 && sortNextPow2(5000) == 8192 && sortNextPow2(65536) == 65536,
              "next power of two");
    }

    // ---- step counts and the dispatch plan ----
    {
        check(sortStepCount(2) == 1 && sortStepCount(4) == 3 && sortStepCount(8) == 6 && sortStepCount(65536) == 136, "step count m(m+1)/2");
        std::vector<SortPass> plan;
        buildSortPlan(1024, plan);
        check(plan.size() == 1 && plan[0].kind == SortPassKind::Local && plan[0].k == 1024, "up to one tile: a single Local pass");
        buildSortPlan(2048, plan);
        check(plan.size() == 1, "exactly one tile: a single Local pass");
        buildSortPlan(4096, plan);
        check(plan.size() == 3 && plan[1].kind == SortPassKind::Global && plan[1].j == 2048 && plan[2].kind == SortPassKind::LocalMerge,
              "4096: Local, one Global (j 2048), one LocalMerge");
        buildSortPlan(65536, plan);
        check(plan.size() == 21, "65,536 entries: 21 dispatches");
        check(plan.front().kind == SortPassKind::Local && plan.back().kind == SortPassKind::LocalMerge && plan.back().k == 65536, "the plan starts Local and ends with the full merge");
        u32 globals = 0, merges = 0;
        for (const SortPass& p : plan) { globals += p.kind == SortPassKind::Global; merges += p.kind == SortPassKind::LocalMerge; }
        check(globals == 15 && merges == 5, "15 Global steps and 5 LocalMerge passes");
        buildSortPlan(1, plan);
        check(plan.empty(), "one entry needs no passes");
    }

    // ---- the network sorts ----
    {
        bool all = true;
        u32 seed = 12345;
        for (u32 n = 2; n <= 65536; n <<= 1) all = all && sortsLikeStd(n, seed + n, false);
        check(all, "every power of two from 2 to 65,536 sorts identically to std::sort");
        bool dup = true;
        for (u32 n = 2; n <= 8192; n <<= 1) dup = dup && sortsLikeStd(n, seed * 3 + n, true);
        check(dup, "heavy duplicate keys sort identically (slot breaks ties)");
        bool many = true;
        for (u32 t = 0; t < 1000; ++t) {
            const u32 e = 1 + rng(seed) % 11;   // 2 .. 2048 entries
            many = many && sortsLikeStd(1u << e, rng(seed) | 1u, (t & 1) != 0);
        }
        check(many, "1,000 random arrays of random sizes");
        // Reversed and constant inputs.
        std::vector<SortEntry> a(4096);
        for (u32 i = 0; i < 4096; ++i) a[i] = {4095u - i, i};
        sortNetworkReference(a);
        bool asc = true;
        for (u32 i = 0; i < 4096; ++i) asc = asc && a[i].key == i;
        check(asc, "a reversed array sorts");
        for (u32 i = 0; i < 4096; ++i) a[i] = {7u, i};
        sortNetworkReference(a);
        bool tie = true;
        for (u32 i = 0; i < 4096; ++i) tie = tie && a[i].slot == i;
        check(tie, "a constant-key array stays in slot order");
        // Padding entries (0xFFFFFFFF keys) sink to the end.
        std::vector<SortEntry> padded(1024);
        for (u32 i = 0; i < 1024; ++i) padded[i] = i < 1000 ? SortEntry{(i * 7919u) % 1000u, i} : SortEntry{0xFFFFFFFFu, i};
        sortNetworkReference(padded);
        bool pads = true;
        for (u32 i = 1000; i < 1024; ++i) pads = pads && padded[i].key == 0xFFFFFFFFu;
        check(pads, "0xFFFFFFFF padding keys end up last");
    }

    AVER_INFO("ParticleSortNetworkTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
