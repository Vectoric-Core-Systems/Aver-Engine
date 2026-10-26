// ItemLoaderTest -- game::ItemLoader, the mechanism half of LevelStreaming (LARGE_WORLD 2.4.1).
//
// The loader was extracted from LevelStreaming without changing what it does, so this pins what a unit is: a batch makes its
// entities under the right parents and files them under their units, a unit evicts as a whole without touching its
// neighbour, and an entity the host made itself can be adopted into a unit and forgotten again. Needs a real scene::World and
// a GameContent with nothing loaded (every mesh is then "not acquirable", which is the headless state): no GPU, no window,
// no project. The residency policy (distance, budgets, pinning, write-back) is LevelStreaming's and is not exercised here.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/game/ItemLoader.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"

#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

#if AVER_MODULE_SCENE

namespace {
// A device with nothing behind it: ItemLoader only hands it to GameContent, which touches it for a lazily loaded mesh.
struct NullDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};
} // namespace

int main() {
    scene::World& world = scene::World::instance();

    fmt::OcWorldData w;
    std::string why;
    check(fmt::parseOcworld("OCWORLD 1\n"
                            "PLACE Meshes/cube.ocmesh 100 0 0 0 0 0 1\n"
                            "BEGIN\n"
                            "  CHILD Meshes/cube.ocmesh 0 50 0 0 0 0 1\n"
                            "END\n"
                            "PLACE Meshes/cube.ocmesh -4000 200 30 0 0 0 1\n", w, &why),
          "the fixture world parses: " + why);
    check(w.placements.size() == 3, "three placements: a root, its child, and a second root");

    game::GameContent content;
    NullDev dev;
    world::InstantiateOptions opt;
    opt.createBodies = false;
    game::ItemLoader loader;
    loader.begin(content, dev, opt);
    check(loader.active(), "begin() makes the loader active");

    // ---- a batch of two units -------------------------------------------------------------------------------------
    // Unit 7 is the root with its child (child's parent is batch slot 0); unit 9 is the second root. Tags are arbitrary
    // caller ids and must come back unchanged.
    std::vector<game::ItemLoader::Entry> entries(3);
    entries[0].rec = &w.placements[0]; entries[0].tag = 100; entries[0].item = 7; entries[0].parent = -1;
    entries[1].rec = &w.placements[1]; entries[1].tag = 101; entries[1].item = 7; entries[1].parent = 0;
    entries[2].rec = &w.placements[2]; entries[2].tag = 102; entries[2].item = 9; entries[2].parent = -1;
    const std::vector<game::ItemLoader::Loaded> loaded = loader.load(entries);
    check(loaded.size() == 3, "a batch of three placements makes three entities");
    if (loaded.size() == 3) {
        check(loaded[0].tag == 100 && loaded[1].tag == 101 && loaded[2].tag == 102, "tags come back in batch order");
        check(loaded[0].item == 7 && loaded[1].item == 7 && loaded[2].item == 9, "each entity names the unit it belongs to");
        check(loaded[0].rec == &w.placements[0] && loaded[2].rec == &w.placements[2], "each entity names its record");
        check(loaded[0].body == -1 && loaded[1].body == -1, "createBodies = false makes no body");
        check(world.parent(loaded[1].entity) == loaded[0].entity, "the child hangs under its root");
        check(world.parent(loaded[0].entity) == scene::kInvalidEntity && world.parent(loaded[2].entity) == scene::kInvalidEntity,
              "roots have no parent");
        check(loader.entitiesOf(7).size() == 2 && loader.entitiesOf(9).size() == 1, "units hold their own entities");
        check(loader.resident(7) && loader.resident(9) && !loader.resident(11), "resident() follows the units");
        loader.applyComponents(loaded);   // no COMPONENT records: must be a quiet no-op
        check(loader.animatedBodies().empty(), "no animated placement, no kinematic body");

        // ---- evicting one unit leaves the other alone ---------------------------------------------------------------
        loader.evict(world, 7);
        world.flush();
        check(!world.valid(loaded[0].entity) && !world.valid(loaded[1].entity), "evicting unit 7 destroys its root and child");
        check(world.valid(loaded[2].entity), "...and leaves unit 9's entity");
        check(loader.entitiesOf(7).empty() && !loader.resident(7), "the evicted unit is forgotten");
        check(loader.entitiesOf(9).size() == 1, "unit 9 is untouched");
    }

    // ---- a unit the host made itself ----------------------------------------------------------------------------------
    {
        const scene::Entity mine = world.create("HostMade", scene::kInvalidEntity, Transform{});
        loader.adopt(11, mine, 0xDEADull);   // no such mesh: the entity is adopted, no reference is taken
        check(loader.entitiesOf(11).size() == 1 && loader.entitiesOf(11)[0] == mine, "adopt() files a host entity under a unit");
        loader.forget(11, mine);
        check(loader.entitiesOf(11).empty(), "forget() drops it again without destroying it");
        check(world.valid(mine), "...the entity itself is the host's");
        world.destroy(mine);
        world.flush();
    }

    // ---- teardown --------------------------------------------------------------------------------------------------------
    loader.evict(world, 9);
    world.flush();
    check(world.count() == 0, "everything the loader made is gone");
    loader.end();

    AVER_INFO("=== {} ===", g_failures == 0 ? "ok" : "FAILED");
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}

#else

int main() {
    AVER_INFO("=== SKIPPED: built without the scene module ===");
    (void)check;
    return 0;
}

#endif
