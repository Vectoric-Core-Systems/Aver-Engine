// Data layers and the HLOD ring on PlacementStreamer: layer-state eligibility, Loaded vs Activated, interaction with distance
// residency, the DataLayerSet (parents, runtime flag, completion) and proxy residency over camera walks (no gap, no flicker). Pure logic.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/DataLayers.hpp"
#include "aver/world/PlacementStreamer.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static AabbD boxD(f64 x, f64 y, f64 h = 50.0) { return AabbD{Vec3d{x - h, y - h, 0}, Vec3d{x + h, y + h, 100}}; }

// Runs one streamer frame the way a host does: update, apply what it asked for.
static void frame(PlacementStreamer& st, const std::vector<Vec3d>& v, std::vector<u32>* loads = nullptr, std::vector<u32>* evicts = nullptr) {
    std::vector<u32> l, e;
    st.update(v, l, e);
    for (const u32 i : l) st.markLoaded(i);
    for (const u32 i : e) st.markEvicted(i);
    if (loads) *loads = l;
    if (evicts) *evicts = e;
}

static bool has(const std::vector<u32>& v, u32 x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int main() {
    AVER_INFO("=== DataLayersHlodTest ===");
    PlacementStreamSettings s;
    s.cellCm = 1000; s.loadCm = 2000; s.evictCm = 3000; s.loadBudget = 0; s.evictBudget = 0;

    {   // ---- layers on the streamer ----
        PlacementStreamer st;
        st.build(std::vector<AabbD>{boxD(100, 0), boxD(200, 0), boxD(300, 0), boxD(400, 0)}, s);
        st.setItemLayers(1, {0});
        st.setItemLayers(2, {0, 1});
        st.setItemLayers(3, {1});
        st.setLayerState(0, LayerState::Unloaded);
        st.setLayerState(1, LayerState::Loaded);
        const std::vector<Vec3d> at0{Vec3d{0, 0, 0}};
        std::vector<u32> l, e;
        frame(st, at0, &l, &e);
        check(has(l, 0) && !has(l, 1) && !has(l, 2) && has(l, 3), "an item in an unloaded layer does not load; unlayered and Loaded-layer items do");
        check(st.eligible(3) && !st.activated(3) && st.activated(0), "Loaded layer: eligible but not activated; unlayered: both");
        check(!st.eligible(2) && !st.activated(2), "any unloaded layer of an item blocks it");

        st.setLayerState(0, LayerState::Activated);
        frame(st, at0, &l, &e);
        check(has(l, 1) && has(l, 2) && e.empty(), "activating a layer loads its items on the next update with no viewer movement (idle early-out defeated)");
        check(st.activated(1) && !st.activated(2) && st.eligible(2), "item 2 stays hidden while its other layer is only Loaded");

        st.setLayerState(0, LayerState::Unloaded);
        frame(st, at0, &l, &e);
        check(e.size() == 2 && has(e, 1) && has(e, 2) && st.residentCount() == 2, "unloading a layer evicts its resident items even though they are in range");
        check(l.empty(), "and does not reload them");

        st.pin(1, true);
        frame(st, at0, &l, &e);
        check(!has(l, 1) && !st.resident(1), "a layer unload also beats a pin");
        st.setLayerState(0, LayerState::Activated);
        frame(st, at0, &l, &e);
        check(has(l, 1), "the pinned item returns with its layer");

        st.setLayerState(0, LayerState::Loaded);
        frame(st, at0, &l, &e);
        check(l.empty() && e.empty() && st.resident(1), "Activated -> Loaded changes visibility only, residency is untouched");

        // distance still rules: a layer cannot pull in an item that is out of range
        PlacementStreamer far;
        far.build(std::vector<AabbD>{boxD(50000, 0)}, s);
        far.setItemLayers(0, {0});
        far.setLayerState(0, LayerState::Activated);
        frame(far, at0, &l, &e);
        check(l.empty(), "an activated layer does not load an item beyond loadCm");
    }

    {   // ---- DataLayerSet ----
        std::vector<fmt::OcDataLayer> defs = {
            {"World", fmt::OcDataLayerState::Activated, false, ""},
            {"Night", fmt::OcDataLayerState::Unloaded, true, "World"},
            {"Boss", fmt::OcDataLayerState::Loaded, true, "Night"},
        };
        DataLayerSet set;
        set.build(defs);
        check(set.count() == 3 && set.idOf("Boss") == 2 && set.idOf("nope") < 0, "layers by name");
        check(set.effective(2) == LayerState::Unloaded, "a child is never above its parent (Boss Loaded, Night Unloaded)");
        check(set.setState("World", LayerState::Unloaded, true) == DataLayerSet::Result::NotRuntime && set.state(0) == LayerState::Activated,
              "the running game cannot change a runtime=0 layer");
        check(set.setState("World", LayerState::Activated, false) == DataLayerSet::Result::Ok, "the editor can; setting the same state queues nothing");
        check(set.setState("zzz", LayerState::Loaded, false) == DataLayerSet::Result::Unknown, "unknown name refused");
        check(set.pending() == 0, "no pending change yet");
        check(set.setState("Night", LayerState::Loaded, true) == DataLayerSet::Result::Ok && set.effective(1) == LayerState::Loaded && set.pending() == 1,
              "Night -> Loaded queues one change");

        PlacementStreamer st;
        st.build(std::vector<AabbD>{boxD(100, 0), boxD(200, 0)}, s);
        set.bind(st, 0, {"Night", "unknown-layer"});
        set.bind(st, 1, {"Boss"});
        set.apply(st);
        const std::vector<Vec3d> at0{Vec3d{0, 0, 0}};
        frame(st, at0);
        check(st.resident(0) && st.resident(1) && !st.activated(0), "applied: members resident (Loaded effective), not activated");
        std::vector<DataLayerSet::Completed> done = set.settle(st, at0);
        check(done.size() == 1 && done[0].layer == 1 && done[0].state == LayerState::Loaded && set.pending() == 0, "members loaded in range: the change completes");

        set.setState("Night", LayerState::Unloaded, true);
        set.apply(st);
        done = set.settle(st, at0);
        check(done.empty() && set.pending() == 1, "an unload does not complete while a member is still resident");
        frame(st, at0);
        done = set.settle(st, at0);
        check(done.size() == 1 && done[0].layer == 1 && done[0].state == LayerState::Unloaded && !st.resident(0), "it completes once the streamer evicted the member");
        check(!st.resident(1), "Boss (child) went with its parent");
    }

    {   // ---- HLOD ring over a camera walk ----
        // Cells of 1000 cm along +X; two items per cell. loadCm 2000 / evictCm 3000; proxies out to 8000, hidden past 9000.
        PlacementStreamSettings hs = s;
        hs.hlodCm = 8000; hs.hlodEvictCm = 9000;
        std::vector<AabbD> boxes;
        std::vector<PlacementStreamer::HlodCell> cells;
        for (int c = 0; c < 30; ++c) {
            boxes.push_back(boxD(c * 1000 + 250, 500));
            boxes.push_back(boxD(c * 1000 + 750, 500));
            cells.push_back({c, 0});
        }
        PlacementStreamer st;
        st.build(boxes, hs);
        st.buildHlod(cells);

        std::vector<usize> shows(cells.size(), 0), hides(cells.size(), 0);
        bool gap = false, doubleDraw = false;
        const auto step = [&](f64 x) {
            const std::vector<Vec3d> v{Vec3d{x, 500, 0}};
            std::vector<u32> sh, hi;
            frame(st, v);
            st.updateHlod(v, sh, hi);
            for (const u32 i : sh) ++shows[i];
            for (const u32 i : hi) ++hides[i];
            for (u32 i = 0; i < cells.size(); ++i) {
                const f64 d = std::max({cells[i].cx * 1000.0 - x, 0.0, x - (cells[i].cx + 1) * 1000.0});
                const bool ready = st.resident(2 * i) && st.resident(2 * i + 1);
                if (d <= hs.hlodCm && !st.hlodShown(i) && !ready && !st.resident(2 * i) && !st.resident(2 * i + 1)) gap = true;   // in the ring, nothing real, no proxy
                if (ready && st.hlodShown(i)) doubleDraw = true;   // the swap happens in the update the content completes
            }
        };
        for (f64 x = 0; x <= 21000; x += 100) step(x);   // walk east
        check(!gap, "walking east: every cell in the ring has either its proxy or real content");
        check(!doubleDraw, "a proxy is hidden the update its cell's content is complete");
        bool once = true;
        for (usize i = 0; i < cells.size(); ++i) once = once && shows[i] <= 2 && hides[i] <= 2;
        check(once, "no flicker: a proxy shows ahead, hides when its content is complete, and shows once more behind");
        check(shows[10] == 2 && hides[10] == 2 && !st.hlodShown(10), "a cell walked past: ahead, swapped, behind, then out of the ring");
        check(shows[27] == 1 && st.hlodShown(27), "a cell far ahead got its proxy when it entered the ring");

        shows.assign(cells.size(), 0); hides.assign(cells.size(), 0);
        for (f64 x = 21000; x >= 0; x -= 100) step(x);   // and back west
        bool back = true;
        for (usize i = 0; i < cells.size(); ++i) back = back && shows[i] <= 2 && hides[i] <= 2;
        check(back && !gap, "walking back: no strobing and no gap");

        shows.assign(cells.size(), 0); hides.assign(cells.size(), 0);
        for (int k = 0; k < 200; ++k) step(12000 + (k % 2 ? 40.0 : -40.0));   // jitter at a load boundary
        usize flips = 0;
        for (usize i = 0; i < cells.size(); ++i) flips += shows[i] + hides[i];
        check(flips <= 40, "jitter near a boundary does not strobe proxies (" + std::to_string(flips) + " transitions in 200 frames)");
    }

    {   // ---- HLOD with layers; ring off = nothing ----
        PlacementStreamSettings hs = s;
        hs.hlodCm = 8000;
        PlacementStreamer st;
        st.build(std::vector<AabbD>{boxD(500, 500)}, hs);
        st.buildHlod({{0, 0}});
        st.setItemLayers(0, {0});
        st.setLayerState(0, LayerState::Unloaded);
        const std::vector<Vec3d> v{Vec3d{0, 0, 0}};
        std::vector<u32> sh, hi;
        frame(st, v);
        st.updateHlod(v, sh, hi);
        check(sh.size() == 1 && st.hlodShown(0), "content gated by an unloaded layer: the proxy shows even at the cell");
        st.setLayerState(0, LayerState::Activated);
        sh.clear(); hi.clear();
        frame(st, v);
        st.updateHlod(v, sh, hi);
        check(hi.size() == 1 && !st.hlodShown(0), "layer activates, content loads, proxy swaps out");

        PlacementStreamer off;
        off.build(std::vector<AabbD>{boxD(500, 500)}, s);   // hlodCm 0
        off.buildHlod({{0, 0}});
        sh.clear(); hi.clear();
        off.updateHlod(v, sh, hi);
        check(sh.empty() && hi.empty(), "hlodCm 0 = no ring, nothing ever shows");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
