// AuthoredStreamingTest -- game::AuthoredStreaming, the shipped path of a cooked level (LARGE_WORLD 2.4.6, tests 5.2).
//
//   CHOICE        resolve(): a fresh set is used and hands back the slim level with streaming switched off; an edited level reads as stale
//                 (never serve stale); a directory with no set and a level that names none say nothing was cooked
//   LOAD          begin() + tick(): the chunks around the viewer become entities through game::ItemLoader with the transform, rotation
//                 (Euler round trip, bit for bit), scale, visibility, animator, ground snap and hierarchy the flat path gives them; the
//                 class placement stays with the level; the afterLoad hook sees the cook's placement ids; owns() tells streamed scenery
//   RESIDENCY     teleporting evicts what was around the old place and loads the new one; nothing is left behind
//   ORIGIN        a floating-origin shift between two ticks (scene roots moved, onOriginShift called) loads and evicts NOTHING
//   TEARDOWN      end() leaves the scene empty
// A real scene::World and a GameContent with nothing loaded (every mesh counts as ready, none as acquirable): no GPU, no window, no project.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

#if AVER_MODULE_SCENE

#  include "aver/game/AuthoredStreaming.hpp"
#  include "aver/game/GameContent.hpp"
#  include "aver/world/ChunkCook.hpp"
#  include "aver/world/LevelTransform.hpp"

namespace {

// A device with nothing behind it (ItemLoaderTest's): ItemLoader only hands it to GameContent.
struct NullDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

fmt::OcWorldPlacement place(const std::string& asset, f64 x, f64 y, f64 z, u64 id) {
    fmt::OcWorldPlacement p;
    p.asset = asset;
    p.objectId = fnv1a64(std::string_view(asset));
    p.x = x; p.y = y; p.z = z;
    p.placementId = id;
    return p;
}

fmt::OcWorldData makeLevel() {
    fmt::OcWorldData w;
    w.name = "Authored";
    w.stream.cellCm = 6400.0f;
    w.stream.loadCm = 20000.0f;
    w.stream.evictCm = 30000.0f;
    {
        fmt::OcWorldPlacement p = place("Meshes/T0.ocmesh", 1000.25, 2000.5, 0.0, 0xB0);   // T0: rotated and scaled
        p.yaw = 30.0; p.pitch = 10.0; p.roll = -5.5;
        p.sx = 2.0; p.sy = 0.5; p.sz = 1.5;
        w.placements.push_back(p);
    }
    w.placements.push_back(place("Meshes/T1.ocmesh", -1600.0, 0.0, 50.0, 0xB1));          // T1: a root with a child (T1c)
    {
        fmt::OcWorldPlacement c = place("Meshes/T1c.ocmesh", 40.25, 30.0, 8.0, 0xB2);
        c.parent = 1;
        c.yaw = 45.0;
        w.placements.push_back(c);
    }
    {
        fmt::OcWorldPlacement p = place("Meshes/T2.ocmesh", 3000.0, 3000.0, 5.0, 0xB3);   // T2: snaps to the ground
        p.snapToGround = true;
        w.placements.push_back(p);
    }
    {
        fmt::OcWorldPlacement p = place("Meshes/T3.ocmesh", 500.0, -900.0, 20.0, 0xB4);   // T3: animated
        p.animClip = "Anims/Spin.ocanim"; p.animSpeed = 0.5f; p.animTime = 1.25f; p.animOnce = true;
        w.placements.push_back(p);
    }
    {
        fmt::OcWorldPlacement p = place("Meshes/T4.ocmesh", -800.0, 800.0, 10.0, 0xB5);   // T4: hidden, no collision
        p.visible = false; p.collide = false;
        w.placements.push_back(p);
    }
    w.placements.push_back(place("Meshes/T5.ocmesh", 400000.0, 0.0, 0.0, 0xB6));          // T5: 250 chunks away
    {
        fmt::OcWorldPlacement p = place("none", 100.0, 100.0, 0.0, 0xB7);                 // a class: level-global
        p.className = "AN_Thing";
        w.placements.push_back(p);
    }
    return w;
}

world::CookInput cookInput(world::LevelAssetLibrary* lib, const std::string& projectDir) {
    world::CookInput in;
    in.levelAsset = "Maps/Authored.ocworld";
    in.levelStem = "Authored";
    in.projectDir = projectDir;
    in.library = lib;
    in.meshBounds = [](const std::string& asset, Vec3& lo, Vec3& hi) {
        if (asset.rfind("Meshes/T", 0) != 0) return false;
        lo = Vec3{-100, -100, -100};
        hi = Vec3{100, 100, 100};
        return true;
    };
    return in;
}

bool bitEqual(const Quat& a, const Quat& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

} // namespace

int main() {
    namespace fs = std::filesystem;
    scene::World& world = scene::World::instance();
    const fs::path root = fs::temp_directory_path() / "aver-authoredstreaming-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    // ---- cook a level --------------------------------------------------------------------------------------------------------
    const fmt::OcWorldData level = makeLevel();
    world::LevelAssetLibrary lib;
    lib.setLoader([](const std::string&, fmt::OcWorldData&, std::string* err) { if (err) *err = "no children"; return false; });
    const fs::path setDir = root / "Binaries" / "Chunks" / "Authored";
    world::CookOptions copt;
    copt.outDir = setDir.string();
    world::CookReport rep;
    std::string why;
    const bool cooked = world::cookAuthoredLevel(level, cookInput(&lib, root.string()), copt, rep, &why);
    check(cooked, "the test level cooks: " + why);
    check(rep.chunkOwned == 7 && rep.globalClass == 1 && rep.levelGlobal == 1,
          "seven placements are chunk-owned (T0 to T5 and T1's child), the class stays with the level");

    // ---- the choice -------------------------------------------------------------------------------------------------------------
    {
        const std::string chunks = "Binaries/Chunks/Authored";   // project-relative, as a STREAM record carries it
        game::AuthoredStreaming::Choice c = game::AuthoredStreaming::resolve(root.string(), "", chunks, "Maps/Authored.ocworld", level);
        check(c.use && !c.stale && c.why.empty(), "a fresh set is used");
        check(!c.slim.stream.enabled && c.slim.placements.size() == 1 && c.slim.placements[0].className == "AN_Thing",
              "...the slim level it hands back has streaming off and only the class placement");

        fmt::OcWorldData edited = level;
        edited.placements[0].x += 1.0;
        const game::AuthoredStreaming::Choice s = game::AuthoredStreaming::resolve(root.string(), "", chunks, "Maps/Authored.ocworld", edited);
        check(!s.use && s.stale && !s.why.empty(), "an edited level reads the set as stale and is not served from it: " + s.why);

        const game::AuthoredStreaming::Choice none = game::AuthoredStreaming::resolve(root.string(), "", "Binaries/Chunks/Nowhere", "Maps/X.ocworld", level);
        check(!none.use && !none.stale && !none.why.empty(), "a missing set says so");
        const game::AuthoredStreaming::Choice nothing = game::AuthoredStreaming::resolve(root.string(), "", "", "Maps/X.ocworld", level);
        check(!nothing.use && !nothing.stale && nothing.why.empty(), "a level that names no set says nothing");
    }

    // ---- stream it ----------------------------------------------------------------------------------------------------------------
    game::GameContent content;
    NullDev dev;
    world::InstantiateOptions opt;
    opt.createBodies = false;
    opt.groundHeightAt = [](f64, f64, f64& z) { z = 100.0; return true; };   // the ground is 100 cm up everywhere

    fmt::OcStreamSettings stream;
    stream.loadCm = 20000.0f;
    stream.evictCm = 30000.0f;
    std::set<u64> hookIds;
    game::AuthoredStreaming::Hooks hooks;
    hooks.afterLoad = [&](const std::vector<scene::Entity>&, const std::vector<u64>& ids, const std::vector<i32>&) {
        for (const u64 id : ids) hookIds.insert(id);
    };

    game::AuthoredStreaming as;
    check(!as.active(), "inactive before begin()");
    check(!as.begin((root / "nothing").string(), content, dev, opt, stream, hooks, &why), "begin() refuses a directory with no set");
    check(as.begin(setDir.string(), content, dev, opt, stream, hooks, &why), "begin() opens the cooked set: " + why);
    check(as.active() && as.wideItems() == 0, "active, no wide item in this level");

    const auto settle = [&](const Vec3& viewer, int maxTicks) {
        for (int i = 0; i < maxTicks; ++i) {
            as.tick(world, {viewer});
            if (i > 0 && as.stats().pendingLoads == 0 && as.waitingChunks() == 0) return;
        }
    };
    settle(Vec3{0, 0, 0}, 300);
    check(as.residentEntities() == 6, "six entities are resident around the origin (T0, T1, T1c, T2, T3, T4; T5 is 250 chunks out)");

    const auto at = [&](const char* name) { return world.find(name); };
    const scene::Entity t0 = at("Meshes/T0.ocmesh"), t1 = at("Meshes/T1.ocmesh"), t1c = at("Meshes/T1c.ocmesh"), t2 = at("Meshes/T2.ocmesh"),
                        t3 = at("Meshes/T3.ocmesh"), t4 = at("Meshes/T4.ocmesh");
    check(t0 != scene::kInvalidEntity && t1 != scene::kInvalidEntity && t1c != scene::kInvalidEntity && t2 != scene::kInvalidEntity &&
              t3 != scene::kInvalidEntity && t4 != scene::kInvalidEntity,
          "every placement near the origin is an entity, named by its asset");
    check(at("Meshes/T5.ocmesh") == scene::kInvalidEntity, "the placement 250 chunks away is not");
    check(at("none") == scene::kInvalidEntity, "the class placement is not a raw entity (the host spawns it from the slim level)");

    if (t0 != scene::kInvalidEntity) {
        const Transform& x = world.localTransform(t0);
        check(x.position.x == 1000.25f && x.position.y == 2000.5f && x.position.z == 0.0f, "T0 is exactly where the level put it");
        check(bitEqual(x.rotation, world::quatFromEulerDeg(Vec3{-5.5f, 10.0f, 30.0f})), "...with the rotation the flat path computes, bit for bit");
        check(x.scale.x == 2.0f && x.scale.y == 0.5f && x.scale.z == 1.5f, "...and its scale");
    }
    if (t1 != scene::kInvalidEntity && t1c != scene::kInvalidEntity) {
        check(world.parent(t1c) == t1 && world.parent(t1) == scene::kInvalidEntity, "T1c hangs under T1");
        const Transform& c = world.localTransform(t1c);
        check(c.position.x == 40.25f && c.position.y == 30.0f && c.position.z == 8.0f, "...at its parent-relative position");
        check(world.localTransform(t1).position.x == -1600.0f, "T1 sits exactly on a chunk boundary at a negative coordinate");
    }
    if (t2 != scene::kInvalidEntity)
        check(world.localTransform(t2).position.z == 105.0f, "T2's snap is resolved against the host's ground: 100 + the authored offset 5");
    if (t3 != scene::kInvalidEntity) {
        const auto* an = world.component<scene::CAnimator>(t3, scene::kComponentAnimator);
        check(an && an->clip == fnv1a64(std::string_view("Anims/Spin.ocanim")) && an->speed == 0.5f && an->time == 1.25f &&
                  (an->flags & scene::kAnimatorOnce) != 0,
              "T3 carries its animator (clip id, speed, start time, once)");
    }
    if (t4 != scene::kInvalidEntity) {
        const auto* mr = world.component<scene::CMeshRenderer>(t4, scene::kComponentMeshRenderer);
        check(mr && (mr->flags & scene::kMeshRendererVisible) == 0, "T4 is hidden");
    }
    {
        const std::set<u64> expected = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
        check(hookIds == expected, "the afterLoad hook saw the cook's placement ids, and not the far placement's");
    }
    check(as.owns(t0) && as.owns(t1c) && !as.owns(world.create("HostMade")), "owns() tells streamed scenery from what the host made");

    // ---- the floating origin: a shift loads and evicts nothing -------------------------------------------------------------
    {
        const usize before = as.residentEntities();
        const world::OriginCm o1{world::kOriginStepCm, 0};
        world::ShiftEvent ev;
        ev.oldOrigin = world::OriginCm{};
        ev.newOrigin = o1;
        ev.deltaCm = world::Vec3d{static_cast<f64>(world::kOriginStepCm), 0.0, 0.0};
        ev.deltaF = Vec3{static_cast<f32>(world::kOriginStepCm), 0.0f, 0.0f};
        ev.generation = 1;
        ev.urgent = false;
        std::vector<scene::Entity> roots;
        for (u32 i = 0; i < world.count(); ++i) {
            const scene::Entity e = world.at(i);
            if (world.parent(e) == scene::kInvalidEntity) roots.push_back(e);
        }
        for (const scene::Entity e : roots) {
            const Vec3 p = world.localTransform(e).position;
            world.setLocalPosition(e, Vec3{p.x - ev.deltaF.x, p.y - ev.deltaF.y, p.z - ev.deltaF.z});
        }
        as.onOriginShift(ev);
        as.tick(world, {Vec3{-ev.deltaF.x, 0.0f, 0.0f}});
        as.tick(world, {Vec3{-ev.deltaF.x, 0.0f, 0.0f}});
        check(as.residentEntities() == before && as.stats().loadedThisUpdate == 0 && as.stats().evictedThisUpdate == 0,
              "a rebase between two ticks loads and evicts nothing");
        if (t0 != scene::kInvalidEntity) {
            const f64 absX = static_cast<f64>(world.localTransform(t0).position.x) + static_cast<f64>(o1.x);
            check(absX == 1000.25, "...and T0's absolute position survived the shift exactly");
        }
        // Put the scene back so the rest of the test runs at origin zero.
        for (const scene::Entity e : roots) {
            const Vec3 p = world.localTransform(e).position;
            world.setLocalPosition(e, Vec3{p.x + ev.deltaF.x, p.y + ev.deltaF.y, p.z + ev.deltaF.z});
        }
        as.onOriginShift(world::ShiftEvent{o1, world::OriginCm{}, world::Vec3d{-ev.deltaCm.x, 0.0, 0.0},
                                           Vec3{-ev.deltaF.x, 0.0f, 0.0f}, 2, false});
        as.tick(world, {Vec3{0, 0, 0}});
        as.tick(world, {Vec3{0, 0, 0}});
        check(as.residentEntities() == before && as.stats().loadedThisUpdate == 0 && as.stats().evictedThisUpdate == 0,
              "...and the shift back changes nothing either");
    }

    // ---- teleport: the old place goes, the new one comes ------------------------------------------------------------------
    settle(Vec3{400000, 0, 0}, 600);
    check(world.find("Meshes/T0.ocmesh") == scene::kInvalidEntity && world.find("Meshes/T5.ocmesh") != scene::kInvalidEntity,
          "flying 4 km away evicts the first place and loads T5");
    check(as.residentEntities() == 1, "...one entity resident");

    // ---- teardown ---------------------------------------------------------------------------------------------------------------
    as.end(world);
    world.flush();
    for (u32 i = 0; i < world.count(); ++i) {
        const scene::Entity e = world.at(i);
        if (std::string(world.name(e)) == "HostMade") world.destroy(e);
    }
    world.flush();
    check(!as.active() && world.count() == 0, "end() leaves the scene empty");

    fs::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}

#else

int main() {
    AVER_INFO("=== SKIPPED: built without the scene module ===");
    return exitCode(ExitCode::Ok);
}

#endif
