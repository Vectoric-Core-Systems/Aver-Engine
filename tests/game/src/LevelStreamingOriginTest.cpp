// LevelStreamingOriginTest -- game::LevelStreaming under the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.4.1, 2.6).
//
// The flat LevelStreaming holds a level's placements as ABSOLUTE f64 records with absolute f64 bounds, converts the viewers it is given
// (origin-relative) with the origin each tick, and narrows each load against that origin. So a shift loads and evicts nothing, a level that
// sits regions away from the world's zero keeps the exact numbers its text says, and the untouched-record rule can write back what the
// author wrote. Needs a real scene::World and a GameContent with nothing loaded (every mesh is "ready", the headless state): no GPU, no window.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/game/LevelStreaming.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"
#include "aver/world/WorldOrigin.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

#if AVER_MODULE_SCENE

namespace {

struct NullDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

constexpr f64 kStep = 1638400.0;
constexpr int kCount = 80;            // placements along +X, 40 m apart
constexpr f64 kSpacing = 4000.0;

// Absolute X of placement k: three regions out, on a quarter-centimetre grid an f32 absolute cannot hold (4.9M cm: the grid is 0.5).
f64 absX(int k) { return 3.0 * kStep + 50000.0 + static_cast<f64>(k) * kSpacing + 0.25; }
f64 absY(int k) { return -2.0 * kStep + 1000.0 + static_cast<f64>(k % 3) * 0.75; }

std::string fixtureText() {
    std::string t = "OCWORLD 1\n";
    for (int k = 0; k < kCount; ++k)
        t += "PLACE Meshes/cube.ocmesh " + std::to_string(absX(k)) + " " + std::to_string(absY(k)) + " 40 0 0 0 1\n";
    return t;
}

world::ShiftEvent shiftEvent(world::OriginCm from, world::OriginCm to, u32 generation) {
    world::ShiftEvent ev;
    ev.oldOrigin = from;
    ev.newOrigin = to;
    ev.deltaCm = world::Vec3d{static_cast<f64>(to.x - from.x), static_cast<f64>(to.y - from.y), 0.0};
    ev.deltaF = Vec3{static_cast<f32>(to.x - from.x), static_cast<f32>(to.y - from.y), 0.0f};
    ev.generation = generation;
    ev.urgent = false;
    return ev;
}

f64 absOfEntity(const scene::World& w, scene::Entity e, world::OriginCm o) {
    return static_cast<f64>(w.localTransform(e).position.x) + static_cast<f64>(o.x);
}

} // namespace

int main() {
    scene::World& w = scene::World::instance();
    {
        std::vector<scene::Entity> all;
        for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
        for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
        w.flush();
    }

    // The fixture text carries f64 numbers at 8 digits ("%f" of a double is 6 decimals), which the parser reads back exactly for a quarter grid.
    fmt::OcWorldData lw;
    std::string why;
    check(fmt::parseOcworld(fixtureText(), lw, &why), "the fixture parses: " + why);
    check(lw.placements.size() == static_cast<usize>(kCount), "80 placements, three regions out");
    check(lw.placements[5].x == absX(5) && lw.placements[5].y == absY(5), "and the parser kept their f64 numbers exactly");
    // Streaming bounds, as the generated .ocstream would give them: a 4 m box around each placement, absolute (f32 holds the quarter grid at 0.5).
    for (fmt::OcWorldPlacement& p : lw.placements) {
        p.hasBounds = true;
        p.boundsMin[0] = static_cast<f32>(p.x - 200.0); p.boundsMin[1] = static_cast<f32>(p.y - 200.0); p.boundsMin[2] = 0.0f;
        p.boundsMax[0] = static_cast<f32>(p.x + 200.0); p.boundsMax[1] = static_cast<f32>(p.y + 200.0); p.boundsMax[2] = 100.0f;
    }
    lw.stream.enabled = true;
    lw.stream.cellCm = 5000.0f;
    lw.stream.loadCm = 10000.0f;
    lw.stream.evictCm = 15000.0f;

    game::GameContent content;
    NullDev dev;
    world::InstantiateOptions opt;
    opt.createBodies = false;

    const world::OriginCm o3{3 * static_cast<i64>(kStep), -2 * static_cast<i64>(kStep)};
    game::LevelStreaming ls;
    ls.setOrigin(o3);
    ls.begin(lw, content, dev, opt, {});
    check(ls.active() && ls.origin() == o3, "begin() at an origin three regions out");

    // A viewer standing at placement 40, in the scene frame (origin-relative).
    const auto viewerAt = [&](int k, world::OriginCm o) {
        return std::vector<Vec3>{Vec3{static_cast<f32>(absX(k) - static_cast<f64>(o.x)), static_cast<f32>(absY(k) - static_cast<f64>(o.y)), 100.0f}};
    };
    std::vector<Vec3> view = viewerAt(40, o3);
    for (int i = 0; i < 12; ++i) ls.tick(w, view);
    w.flush();

    // The expected resident set: every placement whose box is within the load distance of the viewer, judged in f64 on the absolute numbers.
    const auto expectedWithin = [&](int viewerK, f64 radius) {
        std::set<int> s;
        for (int k = 0; k < kCount; ++k) {
            const f64 dx = std::max(0.0, std::fabs(absX(k) - absX(viewerK)) - 200.0);
            const f64 dy = std::max(0.0, std::fabs(absY(k) - absY(viewerK)) - 200.0);
            if (std::sqrt(dx * dx + dy * dy) <= radius) s.insert(k);
        }
        return s;
    };
    const auto residentSet = [&]() {
        std::set<int> s;
        for (int k = 0; k < kCount; ++k) if (ls.entityOf(static_cast<u32>(k)) != scene::kInvalidEntity) s.insert(k);
        return s;
    };
    const std::set<int> wantNear = expectedWithin(40, 10000.0);
    const std::set<int> got = residentSet();
    check(!got.empty() && got.size() < static_cast<usize>(kCount) && wantNear == got,
          "the placements within 100 m of the viewer are resident, judged on the absolute f64 numbers (" + std::to_string(got.size()) + " of 80)");

    // Their entities sit at f32(absolute - origin): exact where the record's number is on the f32 grid of the local frame.
    bool exact = true;
    for (const int k : got) {
        const scene::Entity e = ls.entityOf(static_cast<u32>(k));
        const Vec3 p = w.localTransform(e).position;
        if (p.x != static_cast<f32>(absX(k) - static_cast<f64>(o3.x)) || p.y != static_cast<f32>(absY(k) - static_cast<f64>(o3.y))) exact = false;
    }
    check(exact, "each entity was narrowed against the origin in f64 (the local numbers are 50,000.25 and 1,000.75, not 0.5-grid roundings)");

    // ---- the untouched-record rule ----
    {
        const scene::Entity e = ls.entityOf(40);
        world::Vec3d rec;
        check(ls.recordPositionOf(e, w.localTransform(e).position, rec) && rec.x == absX(40) && rec.y == absY(40) && rec.z == 40.0,
              "an untouched entity writes back the record authored f64 bit for bit");
        Vec3 moved = w.localTransform(e).position;
        moved.x += 3.5f;
        check(ls.recordPositionOf(e, moved, rec) && rec.x == absX(40) + 3.5 && rec.y == absY(40) && rec.z == 40.0,
              "one moved axis is origin + local and the others keep the record (x " + std::to_string(rec.x) + ")");
    }

    // ---- a shift: the scene frame moves by a region, the streamer is told ----
    const std::set<int> beforeShift = residentSet();
    std::vector<scene::Entity> handlesBefore;
    for (const int k : beforeShift) handlesBefore.push_back(ls.entityOf(static_cast<u32>(k)));
    std::vector<f64> absBefore;
    for (const scene::Entity e : handlesBefore) absBefore.push_back(absOfEntity(w, e, o3));

    const world::OriginCm o4{o3.x + static_cast<i64>(kStep), o3.y};
    const world::ShiftEvent ev = shiftEvent(o3, o4, 1);
    w.shiftRoots(ev.deltaF);
    ls.onOriginShift(ev);
    view[0].x -= ev.deltaF.x;                       // the host members moved with the scene
    for (int i = 0; i < 4; ++i) ls.tick(w, view);
    w.flush();
    check(ls.lastLoaded() == 0 && ls.lastEvicted() == 0 && residentSet() == beforeShift,
          "the shift loaded and evicted nothing: the same placements are resident");
    bool sameHandles = true, samePlace = true;
    usize idx = 0;
    for (const int k : beforeShift) {
        const scene::Entity e = ls.entityOf(static_cast<u32>(k));
        if (e != handlesBefore[idx]) sameHandles = false;
        if (std::fabs(absOfEntity(w, e, o4) - absBefore[idx]) > 1e-6) samePlace = false;
        ++idx;
    }
    check(sameHandles && samePlace, "with the same entities, each at the same absolute place");
    check(ls.origin() == o4, "and the streamer holds the new origin");
    {
        const scene::Entity e = ls.entityOf(40);
        world::Vec3d rec;
        check(ls.recordPositionOf(e, w.localTransform(e).position, rec) && rec.x == absX(40) && rec.y == absY(40),
              "the baseline followed the shift: an entity nobody moved still writes back the authored numbers");
        Vec3 moved = w.localTransform(e).position;
        moved.y -= 2.0f;
        check(ls.recordPositionOf(e, moved, rec) && rec.x == absX(40) && rec.y == absY(40) - 2.0, "and a move after the shift is measured against the new origin");
    }

    // ---- the viewer travels on: new placements load by their absolute position under the new origin ----
    view = viewerAt(70, o4);
    for (int i = 0; i < 20; ++i) ls.tick(w, view);
    w.flush();
    check(residentSet() == expectedWithin(70, 10000.0) || residentSet() == expectedWithin(70, 15000.0),
          "walking on, the placements near the new viewer load and the old ones go (judged on the absolute numbers)");
    {
        bool loadedExact = true;
        for (const int k : residentSet()) {
            const Vec3 p = w.localTransform(ls.entityOf(static_cast<u32>(k))).position;
            if (std::fabs(static_cast<f64>(p.x) + static_cast<f64>(o4.x) - absX(k)) > 1e-6) loadedExact = false;
        }
        check(loadedExact, "and each is narrowed against the shifted origin");
    }

    // ---- control: tell the scene but not the streamer, and it streams itself out under the shifted camera ----
    {
        const world::OriginCm o5{o4.x + static_cast<i64>(kStep), o4.y};
        const world::ShiftEvent ev2 = shiftEvent(o4, o5, 2);
        w.shiftRoots(ev2.deltaF);
        view[0].x -= ev2.deltaF.x;
        const std::set<int> before = residentSet();
        for (int i = 0; i < 6; ++i) ls.tick(w, view);
        check(ls.lastEvicted() > 0 || residentSet() != before, "(control) without onOriginShift the viewer is a region away from every placement and the level streams out");
        ls.onOriginShift(ev2);   // and telling it afterwards restores the frame
        for (int i = 0; i < 40; ++i) ls.tick(w, view);
        check(residentSet() == expectedWithin(70, 10000.0) || residentSet() == expectedWithin(70, 15000.0), "(control) told late, it streams back in");
    }

    // ---- a level that sits at the origin is the same as it was ----
    ls.end(w);
    w.flush();
    check(!ls.active(), "end() releases the level");
    {
        game::LevelStreaming plain;
        fmt::OcWorldData pw;
        check(fmt::parseOcworld("OCWORLD 1\nPLACE Meshes/cube.ocmesh 1234.5 -678.25 40 0 0 0 1\nPLACE Meshes/cube.ocmesh 900000 0 0 0 0 0 1\n", pw, &why), "the origin-zero fixture parses");
        for (fmt::OcWorldPlacement& p : pw.placements) {
            p.hasBounds = true;
            p.boundsMin[0] = static_cast<f32>(p.x - 100.0); p.boundsMin[1] = static_cast<f32>(p.y - 100.0); p.boundsMin[2] = 0.0f;
            p.boundsMax[0] = static_cast<f32>(p.x + 100.0); p.boundsMax[1] = static_cast<f32>(p.y + 100.0); p.boundsMax[2] = 100.0f;
        }
        pw.stream.enabled = true;
        pw.stream.cellCm = 5000.0f; pw.stream.loadCm = 10000.0f; pw.stream.evictCm = 15000.0f;
        plain.begin(pw, content, dev, opt, {});
        std::vector<Vec3> v{Vec3{1000.0f, -500.0f, 100.0f}};
        for (int i = 0; i < 6; ++i) plain.tick(w, v);
        const scene::Entity e = plain.entityOf(0);
        check(e != scene::kInvalidEntity && plain.entityOf(1) == scene::kInvalidEntity, "at origin zero only the near placement loads");
        check(e != scene::kInvalidEntity && w.localTransform(e).position.x == 1234.5f && w.localTransform(e).position.y == -678.25f,
              "and it sits exactly where it always did (subtracting a zero origin changes nothing)");
        plain.end(w);
        w.flush();
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}

#else

int main() { return 0; }

#endif
