// BtSystem with blackboards, against a real scene::World: per-entity boards, team boards, observer
// aborts across agents, the scalar relay graph nodes and C# use, perception sync, reload and the
// live-debug view. No physics, no disk except one reload fixture.
#include "aver/synapse/SynapseBt.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
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

static std::map<i32, int> g_work, g_idle;
static synapse::BtStatus workAction(i32 s, const fmt::OcBtNode&, f32, f32&, void*) { ++g_work[s]; return synapse::BtStatus::Running; }
static synapse::BtStatus idleAction(i32 s, const fmt::OcBtNode&, f32, f32&, void*) { ++g_idle[s]; return synapse::BtStatus::Running; }

// 0 Selector -> [1 Sequence (Alert, abort lower priority) -> [2 Work], 3 Idle]
static synapse::BtAsset makeAsset() {
    synapse::BtAsset a;
    synapse::BbKeyDef alert;
    alert.name = "Alert"; alert.type = synapse::BbType::Bool; alert.scope = synapse::BbScope::Shared;
    a.schema.add(alert);
    synapse::BbKeyDef ammo;
    ammo.name = "Ammo"; ammo.type = synapse::BbType::Int;
    a.schema.add(ammo);
    synapse::BbKeyDef see;
    see.name = "CanSeeTarget"; see.type = synapse::BbType::Bool;
    a.schema.add(see);

    synapse::BtAssetNode root;
    root.node.kind = fmt::OcBtNodeKind::Selector;
    root.node.parent = fmt::kOcBtNoParent;
    a.nodes.push_back(root);

    synapse::BtAssetNode seq;
    seq.node.kind = fmt::OcBtNodeKind::Sequence;
    seq.node.parent = 0;
    synapse::BtDecorator d;
    d.key = "Alert";
    d.op = synapse::BbOp::IsSet;
    d.abort = synapse::BtAbortMode::LowerPriority;
    seq.decorators.push_back(d);
    a.nodes.push_back(seq);

    synapse::BtAssetNode work;
    work.node.kind = fmt::OcBtNodeKind::Action;
    work.node.parent = 1;
    work.node.name = "Work";
    a.nodes.push_back(work);

    synapse::BtAssetNode idle;
    idle.node.kind = fmt::OcBtNodeKind::Action;
    idle.node.parent = 0;
    idle.node.name = "Idle";
    a.nodes.push_back(idle);
    return a;
}

int main() {
    AVER_INFO("BtBlackboardSystemTest");
    scene::World& w = scene::World::instance();
    synapse::BtSystem& sys = synapse::btSystem();

    check(sys.registerComponents(w) != 0, "CSynapseBehavior registers");
    synapse::perceptionSystem().registerComponents(w);
    synapse::registerBuiltinBehaviors(sys);
    sys.registry().registerAction("Work", &workAction);
    sys.registry().registerAction("Idle", &idleAction);
    clearWorld(w);
    sys.clearBlackboards();

    const synapse::BtAsset asset = makeAsset();
    check(asset.valid() && asset.validate().empty(), "the fixture asset is valid");
    const u64 treeId = sys.registerTree("test://guard", asset);
    check(treeId != 0, "an in-memory tree registers");

    const auto spawn = [&](const char* name, const char* team) {
        const scene::Entity e = w.create(name);
        auto* b = sys.attach(w, e);
        if (b) b->treeAssetId = treeId;
        sys.setTeam(w, e, team);
        return e;
    };
    const scene::Entity A = spawn("A", "red");
    const scene::Entity B = spawn("B", "red");
    const scene::Entity C = spawn("C", "blue");

    sys.tick(w, 0.016f);
    check(sys.blackboard(A) && sys.blackboard(B) && sys.blackboard(C), "each entity got a board on its first tick");
    check(sys.blackboard(A) != sys.blackboard(B), "and they are different boards");
    check(g_idle[static_cast<i32>(A)] == 1 && g_idle[static_cast<i32>(C)] == 1, "all three start idle");

    AVER_INFO("agent scope is private, team scope is shared");
    sys.blackboard(A)->setInt("Ammo", 3);
    check(sys.blackboard(B)->getInt("Ammo") == 0 && sys.blackboard(C)->getInt("Ammo") == 0, "Ammo is per agent");

    sys.blackboard(A)->setBool("Alert", true);
    check(sys.blackboard(B)->getBool("Alert") && !sys.blackboard(C)->getBool("Alert"), "Alert is per team");
    sys.tick(w, 0.016f);
    check(g_work[static_cast<i32>(A)] == 1 && g_work[static_cast<i32>(B)] == 1, "both red agents pre-empt idle");
    check(g_work[static_cast<i32>(C)] == 0, "the blue agent keeps idling");

    AVER_INFO("changing team re-binds the shared keys");
    check(sys.setTeam(w, C, "red"), "C joins the red team");
    sys.tick(w, 0.016f);
    check(g_work[static_cast<i32>(C)] == 1, "and sees the team's Alert at once");

    AVER_INFO("the scalar relay (graph nodes and C#)");
    {
        const i32 a = static_cast<i32>(A);
        f32 fv[3] = {0.75f, 0.0f, 0.0f};
        i64 iv = 0;
        char text[16] = {};
        check(synapse::blackboardRelay(synapse::kBbRelaySet, a, "Fear", 3, nullptr, fv, nullptr, 0, nullptr) == 1,
              "set on an undefined key defines it");
        check(synapse::blackboardRelay(synapse::kBbRelayType, a, "Fear", 0, nullptr, nullptr, nullptr, 0, nullptr) == 3,
              "its type code is Float (1 + BbType)");
        fv[0] = 0.0f;
        check(synapse::blackboardRelay(synapse::kBbRelayGet, a, "Fear", 0, nullptr, fv, nullptr, 0, nullptr) == 1 && fv[0] == 0.75f,
              "get reads it back");
        check(synapse::blackboardRelay(synapse::kBbRelayGet, a, "Ammo", 3, nullptr, fv, nullptr, 0, nullptr) == 1 && fv[0] == 3.0f,
              "get can ask for another numeric type");
        check(synapse::blackboardRelay(synapse::kBbRelayGet, a, "Alert", 0, &iv, nullptr, nullptr, 0, nullptr) == 1 && iv == 1,
              "a Bool comes back in the integer slot");
        iv = 0;
        check(synapse::blackboardRelay(synapse::kBbRelaySet, a, "Label", 5, nullptr, nullptr, const_cast<char*>("guard"), 6, nullptr) == 1,
              "a string key can be set");
        check(synapse::blackboardRelay(synapse::kBbRelayGet, a, "Label", 0, nullptr, nullptr, text, 4, nullptr) == 1 &&
              std::string(text) == "gua", "a string read truncates to the caller's buffer and terminates");
        check(synapse::blackboardRelay(synapse::kBbRelayGet, a, "Nope", 0, &iv, fv, text, 16, nullptr) == 0, "an undefined key reads as absent");
        check(synapse::blackboardRelay(synapse::kBbRelaySet, a, "Ammo", 5, nullptr, nullptr, const_cast<char*>("x"), 2, nullptr) == 0,
              "a String cannot be written to an Int key");
        check(synapse::blackboardRelay(synapse::kBbRelayReset, a, "Ammo", 0, nullptr, nullptr, nullptr, 0, nullptr) == 1 &&
              sys.blackboard(A)->getInt("Ammo") == 0, "reset restores the default");
        check(synapse::blackboardRelay(synapse::kBbRelayGet, 0x7FFFFFF0, "Ammo", 0, &iv, fv, text, 16, nullptr) == 0,
              "a dead entity reads as absent");
        check(synapse::blackboardRelay(synapse::kBbRelaySetTeam, a, "red", 0, nullptr, nullptr, nullptr, 0, nullptr) == 1,
              "the team can be set through the relay");
    }

    AVER_INFO("perception feeds the conventional keys");
    {
        const scene::Entity D = spawn("D", "green");
        auto* p = synapse::perceptionSystem().attach(w, D);
        check(p != nullptr, "perception attaches");
        if (p) { p->canSeeTarget = 1; p->lastKnownTargetEntity = 7; }
        sys.tick(w, 0.016f);
        check(sys.blackboard(D)->getBool("CanSeeTarget"), "CanSeeTarget follows the perception component");
    }

    AVER_INFO("hearing memory is mirrored onto the blackboard");
    {
        synapse::HeardMemory m;
        m.pos = Vec3{100, 200, 0};
        m.level = 0.8f;
        m.tag = 3;
        m.source = 9;
        m.confidence = 1.0f;
        sys.hearingSink().onHeard(static_cast<u32>(B), m);
        synapse::Blackboard* bb = sys.blackboard(B);
        check(bb->getBool("HasHeard") && bb->getVec3("HeardPosition").y == 200.0f && bb->getInt("HeardTag") == 3 &&
              bb->getEntity("HeardSource") == 9, "a heard noise lands on the listener's board");
        check(!sys.blackboard(C)->has("HasHeard"), "and on nobody else's");

        synapse::HeardMemory other = m;
        other.pos = Vec3{5, 5, 5};
        sys.hearingSink().onForgotten(static_cast<u32>(B), other);
        check(bb->getBool("HasHeard"), "forgetting some other memory leaves the mirrored one alone");
        sys.hearingSink().onForgotten(static_cast<u32>(B), m);
        check(!bb->getBool("HasHeard"), "forgetting the mirrored memory clears HasHeard");
    }

    AVER_INFO("live debug view");
    {
        sys.watch(A);
        sys.tick(w, 0.016f);
        synapse::BtDebugView v;
        check(sys.debugView(A, v) && v.tree && v.state && v.board, "the view carries tree, state and board");
        check(v.state && v.state->nodeTrace.size() == 4, "the watched entity records per-node results");
        synapse::BtDebugView other;
        sys.debugView(B, other);
        check(other.state && other.state->nodeTrace.empty(), "an unwatched entity records nothing");
        check(sys.entitiesUsing(w, treeId).size() >= 4, "entities using the tree can be listed");
        sys.watch(scene::kInvalidEntity);
    }

    AVER_INFO("reload re-binds the schema and restarts the tree");
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-bt-bbsys").string();
        const std::string path = dir + "/guard.ocbt";
        synapse::BtAsset v1 = makeAsset();
        std::string why;
        check(synapse::saveBtAsset(path, v1, &why), "the fixture saves: " + why);
        const u64 id = sys.loadTree(path);
        check(id != 0, "and loads");

        const scene::Entity E = w.create("E");
        auto* b = sys.attach(w, E);
        if (b) b->treeAssetId = id;
        sys.tick(w, 0.016f);
        check(!sys.blackboard(E)->has("Panic"), "the first version has no Panic key");

        synapse::BtAsset v2 = v1;
        synapse::BbKeyDef panic;
        panic.name = "Panic"; panic.type = synapse::BbType::Float; panic.defaultValue = synapse::BbValue::ofFloat(0.5f);
        v2.schema.add(panic);
        check(synapse::saveBtAsset(path, v2, &why), "a second version saves: " + why);
        check(sys.reloadTree(path), "reload succeeds");
        sys.tick(w, 0.016f);
        check(sys.blackboard(E)->has("Panic") && sys.blackboard(E)->getFloat("Panic") == 0.5f,
              "the new key appears with its default after the next tick");

        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("boards go away with their entity");
    {
        w.destroy(A);
        w.flush();
        sys.tick(w, 0.016f);
        check(sys.blackboard(A) == nullptr, "a destroyed entity's board is pruned");
        check(sys.blackboard(B) != nullptr, "and a living one's stays");
    }

    clearWorld(w);
    sys.clearBlackboards();
    AVER_INFO(g_failures ? "BtBlackboardSystemTest: {} FAILURES" : "BtBlackboardSystemTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
