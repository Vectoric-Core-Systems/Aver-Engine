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

static AabbD boxD(f64 x, f64 y, f64 h = 50.0) {
    return AabbD{Vec3d{x - h, y - h, 0}, Vec3d{x + h, y + h, 100}};
}

// The same flight in either precision, as the sequence of (toLoad, toEvict) it produced. Marks what was asked, as a host would.
struct Step { std::vector<u32> load, ev; };
static std::vector<Step> flight(bool f64Path) {
    PlacementStreamSettings s;
    s.cellCm = 1000; s.loadCm = 2000; s.evictCm = 3000; s.loadBudget = 2; s.evictBudget = 0;
    const f32 xs[] = {500, 1500, 1800, 10000, 2800};
    PlacementStreamer st;
    if (f64Path) {
        std::vector<AabbD> b;
        for (const f32 x : xs) b.push_back(boxD(x, 0));
        st.build(b, s);
    } else {
        std::vector<Aabb> b;
        for (const f32 x : xs) b.push_back(box(x, 0));
        st.build(b, s);
    }
    const f32 viewers[] = {0, 0, 0, 5000, -50000, -50000, 0, 0};
    std::vector<Step> out;
    for (const f32 vx : viewers) {
        Step step;
        if (f64Path) st.update(std::vector<Vec3d>{Vec3d{vx, 0, 0}}, step.load, step.ev);
        else         st.update(std::vector<Vec3>{Vec3{vx, 0, 0}}, step.load, step.ev);
        for (const u32 i : step.load) st.markLoaded(i);
        for (const u32 i : step.ev) st.markEvicted(i);
        out.push_back(step);
    }
    return out;
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

    // ---- the f64 path (LARGE_WORLD 2.4.1): the same answers in region 0, the exact answer at 100 km ----------------------
    {
        const std::vector<Step> a = flight(false), b = flight(true);
        bool same = a.size() == b.size();
        for (usize i = 0; same && i < a.size(); ++i) same = a[i].load == b[i].load && a[i].ev == b[i].ev;
        check(same, "the f64 overloads produce the f32 path's loads and evictions step for step in region 0");
    }
    {
        // Two boxes east of a viewer at x = 10^8 cm, their near faces 2003 cm and 1990 cm out. An f32 grid there is 8 cm, so
        // the 2003 snaps to 2000 -- inside the 2000 cm load distance -- while the exact answer is outside it.
        PlacementStreamSettings s;
        s.cellCm = 1000; s.loadCm = 2000; s.evictCm = 3000; s.loadBudget = 0; s.evictBudget = 0;
        const f64 vx = 1.0e8;
        const f64 edgeA = vx + 2003.0, edgeB = vx + 1990.0;   // nearest face of each box
        std::vector<AabbD> bd = {AabbD{Vec3d{edgeA, -10, 0}, Vec3d{edgeA + 10, 10, 100}},
                                 AabbD{Vec3d{edgeB, -10, 0}, Vec3d{edgeB + 10, 10, 100}}};

        PlacementStreamer exact;
        exact.build(bd, s);
        check(exact.f64Mode(), "building from AabbD puts the streamer in f64 mode");
        std::vector<u32> load, ev;
        exact.update(std::vector<Vec3d>{Vec3d{vx, 0, 0}}, load, ev);
        check(load.size() == 1 && load[0] == 1, "f64: the item 1990 cm out loads, the one 2003 cm out does not");
        check(exact.distance(0, std::vector<Vec3d>{Vec3d{vx, 0, 0}}) == 2003.0, "f64: the distance to the box is exactly 2003 cm");

        // The same numbers through the f32 API: the box face and the viewer both snap to the 8 cm grid.
        std::vector<Aabb> bf;
        for (const AabbD& b : bd) {
            Aabb a;
            a.min = Vec3{static_cast<f32>(b.min.x), static_cast<f32>(b.min.y), static_cast<f32>(b.min.z)};
            a.max = Vec3{static_cast<f32>(b.max.x), static_cast<f32>(b.max.y), static_cast<f32>(b.max.z)};
            bf.push_back(a);
        }
        PlacementStreamer lossy;
        lossy.build(bf, s);
        check(!lossy.f64Mode(), "building from Aabb keeps f32 mode");
        load.clear(); ev.clear();
        lossy.update({Vec3{static_cast<f32>(vx), 0, 0}}, load, ev);
        bool aLoaded = false;
        for (const u32 i : load) aLoaded = aLoaded || i == 0;
        check(aLoaded, "control: in f32 the item 2003 cm out is loaded -- 10^8 cm quantises to an 8 cm grid and the answer is wrong");

        // add / setBounds in either precision keep both forms, so the f64 streamer follows an f32 add.
        const u32 extra = exact.add(box(static_cast<f32>(vx) + 100.0f, 0, 5.0f));
        load.clear(); ev.clear();
        exact.update(std::vector<Vec3d>{Vec3d{vx + 1.0, 0, 0}}, load, ev);
        bool sawExtra = false;
        for (const u32 i : load) sawExtra = sawExtra || i == extra;
        check(sawExtra, "an f32 add to an f64 streamer is widened and used");
    }

    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
