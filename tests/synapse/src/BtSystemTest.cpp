// CSynapseBehavior + BtSystem::tick, against a real scene::World -- loading a real .ocbt file
// through the real built-in "Wait" action, and the half BtTest.cpp (which uses raw opaque i32
// subjects, no real entities) cannot cover at all: that a RECYCLED entity handle does not inherit
// the previous occupant's Running state. See AnimSystem::posed_'s own header comment
// (modules/anim.scene/include/aver/anim/AnimSystem.hpp) for the exact bug shape this guards
// against -- it was keyed by entityIndex() alone once, and a fresh spawn reusing a destroyed
// entity's index came up wearing a stranger's state.
#include "aver/synapse/SynapseBt.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

int main() {
    AVER_INFO("BtSystemTest");
    scene::World& w = scene::World::instance();

    const u32 type = synapse::btSystem().registerComponents(w);
    check(type != 0, "CSynapseBehavior registers");
    synapse::registerBuiltinBehaviors(synapse::btSystem());

    // A one-node-deep tree: Sequence -> Action "Wait" (1.0s) -- the built-in itself, exercised for
    // real, not a test-local stand-in the way BtTest.cpp's TestWait was.
    fmt::OcBtData tree;
    tree.nodes.push_back({fmt::OcBtNodeKind::Sequence, fmt::kOcBtNoParent, "", {}});
    fmt::OcBtNode wait;
    wait.kind = fmt::OcBtNodeKind::Action;
    wait.parent = 0;
    wait.name = "Wait";
    wait.params[0] = 1.0f;
    tree.nodes.push_back(wait);
    check(tree.valid(), "the fixture tree is valid");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-bt-system").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/wait.ocbt";
    std::string why;
    check(fmt::saveOcBt(path, tree, &why), "the fixture writes to a real file: " + why);

    const u64 treeId = synapse::btSystem().loadTree(path);
    check(treeId != 0, "and BtSystem::loadTree reads it back");

    AVER_INFO("a real entity runs the real built-in Wait action through BtSystem::tick");
    scene::Entity a = scene::kInvalidEntity;
    {
        clearWorld(w);
        a = w.create("Behaver");
        auto* ba = synapse::btSystem().attach(w, a);
        check(ba != nullptr, "the entity takes CSynapseBehavior");
        if (ba) ba->treeAssetId = treeId;

        synapse::btSystem().tick(w, 0.5f);
        const auto* after = w.component<synapse::CSynapseBehavior>(a, type);
        check(after && after->lastStatus == static_cast<i32>(synapse::BtStatus::Running),
              "0.5s of dt: Wait has not resolved yet, the tree reports Running");
        check(synapse::btSystem().runningCount() == 1, "and the running-state table holds one entry");
    }

    AVER_INFO("destroying that entity, WITHOUT ticking again first, leaves a stale entry behind");
    {
        w.destroy(a);
        w.flush();
        // Deliberately NOT calling btSystem().tick() here -- prune() only runs as part of tick(),
        // so this is the exact window a real frame could hit: an entity destroyed mid-frame, one
        // tick before the system that owned its state would have cleaned it up.
        check(synapse::btSystem().runningCount() == 1,
              "the entry is still there -- proves the NEXT check exercises a genuinely stale entry, "
              "not one prune() already removed for us");
    }

    AVER_INFO("a NEW entity reusing the destroyed one's index does not inherit its Running progress");
    {
        const scene::Entity b = w.create("SecondBehaver");
        check(scene::entityIndex(b) == scene::entityIndex(a),
              "the new entity reused the destroyed one's index (the scenario this test exists for)");
        check(b != a, "but is a DIFFERENT handle -- the generation moved on");

        auto* bb = synapse::btSystem().attach(w, b);
        check(bb != nullptr, "it takes CSynapseBehavior");
        if (bb) bb->treeAssetId = treeId;

        // 0.55s: if this entity's Wait incorrectly inherited entity A's 0.5s head start (the
        // index-keyed bug), 0.5 + 0.55 = 1.05s would cross the 1.0s duration and report Success.
        // Correctly starting fresh, 0.55s alone is still short of it.
        synapse::btSystem().tick(w, 0.55f);
        const auto* afterB = w.component<synapse::CSynapseBehavior>(b, type);
        check(afterB && afterB->lastStatus == static_cast<i32>(synapse::BtStatus::Running),
              "STILL RUNNING at 0.55s of its OWN dt -- entity A's stale 0.5s never leaked into it");

        // And entity A's own stale entry is now gone -- this same tick's own prune() pass swept it,
        // since A no longer carries CSynapseBehavior at all.
        check(synapse::btSystem().runningCount() == 1,
              "AND the side table holds exactly one entry now -- A's stale one was pruned, not merely ignored");
    }

    AVER_INFO(g_failures ? "BtSystemTest: {} FAILURES" : "BtSystemTest: all checks passed ({})", g_failures);
    std::filesystem::remove_all(dir, ec);
    return g_failures ? 1 : 0;
}
