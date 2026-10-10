// PlacementStreamer residency: load/evict radii, ordering, budgets, pinning. Exits 0 or 1 (ExitCode).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/PlacementStreamer.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static Aabb box(f32 x, f32 y, f32 h = 50.0f) {
    Aabb b;
    b.min = Vec3{x - h, y - h, 0};
    b.max = Vec3{x + h, y + h, 100};
    return b;
}

int main() {
    PlacementStreamSettings s;
    s.cellCm = 1000; s.loadCm = 2000; s.evictCm = 3000; s.loadBudget = 2; s.evictBudget = 0;
    std::vector<Aabb> items = {box(500, 0), box(1500, 0), box(1800, 0), box(10000, 0), box(2800, 0)};
    PlacementStreamer st;
    st.build(items, s);

    std::vector<u32> load, ev;
    st.update({Vec3{0, 0, 0}}, load, ev);
    check(load.size() == 2 && load[0] == 0 && load[1] == 1, "nearest first, capped by the load budget");
    for (u32 i : load) st.markLoaded(i);

    load.clear(); ev.clear();
    st.update({Vec3{0, 0, 0}}, load, ev);
    check(load.size() == 1 && load[0] == 2, "the rest follows next call");
    st.markLoaded(2);

    load.clear(); ev.clear();
    st.update({Vec3{0, 0, 0}}, load, ev);
    check(load.empty() && ev.empty(), "settled: nothing to do (item 4 is 2750 away, past loadCm)");

    load.clear(); ev.clear();
    st.update({Vec3{5000, 0, 0}}, load, ev);
    check(ev.size() == 3, "all three evicted once beyond evictCm");
    for (u32 i : ev) st.markEvicted(i);
    check(st.residentCount() == 0, "residentCount follows markEvicted");

    st.pin(3, true);
    load.clear(); ev.clear();
    st.update({Vec3{-50000, 0, 0}}, load, ev);
    check(load.size() == 1 && load[0] == 3, "a pinned far item loads regardless of distance");
    st.markLoaded(3);
    load.clear(); ev.clear();
    st.update({Vec3{-50000, 0, 0}}, load, ev);
    check(ev.empty(), "a pinned item is never evicted");

    st.pin(3, false);
    load.clear(); ev.clear();
    st.update({Vec3{-50000, 0, 0}}, load, ev);
    check(ev.size() == 1 && ev[0] == 3, "unpinned, it is evicted");
    st.markEvicted(3);

    st.remove(0);
    load.clear(); ev.clear();
    st.update({Vec3{0, 0, 0}}, load, ev);
    bool sawRemoved = false;
    for (u32 i : load) sawRemoved = sawRemoved || i == 0;
    check(!sawRemoved, "a removed item is never reported");

    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
