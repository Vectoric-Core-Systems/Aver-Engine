// CAnimGraph on an entity: the component registers, a state machine asset resolves through the same
// asset-id path a clip does, parameters written to the component's slots drive it, and the pose that
// reaches the skinning output is the machine's -- not a clip's.
//
// The failure this exists to catch is the recurring one: every layer below passes its own test and the
// pieces are wired together wrongly (a parameter slot nothing reads, a pose source nothing calls).
#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimGraphSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::map<u64, std::string> g_paths;
static std::string g_dir;
static const u64 kSkelId = 0x5EE1AA77ull;

static u64 reg(const std::string& ref, const std::string& file) {
    const u64 id = fnv1a64(std::string_view(ref));
    g_paths[id] = g_dir + "/" + file;
    return id;
}

static std::string resolvePath(u64 id, void*) {
    if (id == kSkelId) return g_dir + "/root.ocskel";
    const auto it = g_paths.find(id);
    return it == g_paths.end() ? std::string{} : it->second;
}

static fmt::OcAnimation slide(f32 to, f32 dur, bool loop) {
    fmt::OcAnimation c;
    c.duration = dur;
    c.flags = loop ? fmt::kOcAnimLoop : 0;
    fmt::OcTrack t;
    t.boneIndex = 0;
    t.channels = fmt::kOcChannelTranslation;
    t.times = {0.0f, dur};
    t.values = {0, 0, 0, to, 0, 0};
    c.tracks.push_back(t);
    return c;
}

static u64 hashOf(const char* s) { return fnv1a64(std::string_view(s)); }

// Writes a parameter the way a script does: find the slot with the hash or the first empty one.
static void setParam(anim::CAnimGraph& c, const char* name, f32 v) {
    const u64 h = hashOf(name);
    u32 slot = anim::kAnimGraphSlots;
    for (u32 i = 0; i < anim::kAnimGraphSlots; ++i)
        if (c.paramHash[i] == h) { slot = i; break; }
    if (slot == anim::kAnimGraphSlots)
        for (u32 i = 0; i < anim::kAnimGraphSlots; ++i)
            if (c.paramHash[i] == 0) { slot = i; break; }
    if (slot == anim::kAnimGraphSlots) return;
    c.paramHash[slot] = h;
    c.paramValue[slot] = v;
}

static int g_enter = 0, g_exit = 0;
static std::string g_lastEvent;
static void onEvent(scene::Entity, const anim::AsmEvent& ev, void*) {
    if (ev.kind == anim::AsmEvent::Kind::Enter) ++g_enter; else ++g_exit;
    if (!ev.name.empty()) g_lastEvent = ev.name;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("AnimGraphSystemTest");

    std::error_code ec;
    g_dir = (std::filesystem::temp_directory_path(ec) / "aver-animgraph-test").string();
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    std::string why;
    fmt::OcSkeleton skel;
    fmt::OcBone root;
    root.name = "root";
    skel.bones = {root};
    skel.rootBone = 0;
    check(fmt::saveOcSkel(g_dir + "/root.ocskel", skel, &why), "the skeleton writes " + why);

    reg("idle.ocanim", "idle.ocanim");
    reg("walk.ocanim", "walk.ocanim");
    reg("kick.ocanim", "kick.ocanim");
    check(fmt::saveOcAnim(g_dir + "/idle.ocanim", slide(100.0f, 1.0f, true), &why), "idle writes " + why);
    check(fmt::saveOcAnim(g_dir + "/walk.ocanim", slide(300.0f, 1.0f, true), &why), "walk writes " + why);
    check(fmt::saveOcAnim(g_dir + "/kick.ocanim", slide(500.0f, 0.5f, false), &why), "kick writes " + why);

    anim::AnimStateMachineAsset asset;
    asset.name = "hero";
    asset.params = {{"speed", anim::AsmParamType::Float, 0.0f}, {"kick", anim::AsmParamType::Trigger, 0.0f}};
    anim::AsmMachine m;
    m.name = "Root";
    auto st = [](const char* n, const char* clip, bool loop) {
        anim::AsmState s;
        s.name = n;
        s.asset = clip;
        s.loop = loop;
        return s;
    };
    m.states = {st("Idle", "idle.ocanim", true), st("Walk", "walk.ocanim", true), st("Kick", "kick.ocanim", false)};
    m.states[1].onEnter = "walk_started";
    auto tr = [](i32 from, i32 to, std::vector<anim::AsmCondition> c) {
        anim::AsmTransition t;
        t.from = from;
        t.to = to;
        t.blendTime = 0.0f;
        t.conditions = std::move(c);
        return t;
    };
    m.transitions.push_back(tr(anim::kAsmAny, 2, {{"kick", anim::AsmOp::Trigger, 0.0f}}));
    m.transitions.push_back(tr(0, 1, {{"speed", anim::AsmOp::Greater, 0.5f}}));
    m.transitions.push_back(tr(1, 0, {{"speed", anim::AsmOp::LessEq, 0.5f}}));
    anim::AsmTransition back = tr(2, 0, {});
    back.hasExitTime = true;
    back.exitTime = 1.0f;
    m.transitions.push_back(back);
    asset.machines = {m};
    check(asset.valid(&why), "the machine is valid " + why);
    const u64 machineId = reg("hero.ocasm", "hero.ocasm");
    check(anim::saveStateMachine(g_dir + "/hero.ocasm", asset, &why), "the machine writes " + why);

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);

    AVER_INFO("=== the component registers ===");
    const u32 type = anim::AnimGraphSystem::registerComponents(w);
    check(type != 0 && w.componentId("CAnimGraph") == type, "CAnimGraph got a type id and answers to its name");
    check(w.componentVerified(type), "its field table covers the struct with no gap");
    check(w.fieldId("CAnimGraph.paramHash15") != 0 && w.fieldId("CAnimGraph.paramValue15") != 0,
          "the last parameter slot is reachable by name");

    anim::AnimGraphSystem graphs;
    graphs.install(sys, w);
    graphs.setEventSink(&onEvent, nullptr);
    check(sys.hasPoseSource(), "installing the system installs the pose source");

    const scene::Entity e = w.create("hero");
    auto* sm = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
    if (sm) sm->skeleton = kSkelId;
    anim::CAnimGraph* g = anim::AnimGraphSystem::attach(w, e, machineId);
    check(g != nullptr && g->playRate == 1.0f, "a graph attaches with playRate 1, not the zero-fill's 0");

    auto step = [&](f32 dt) {
        graphs.tick(w, dt);
        sys.tick(w, dt);
        g = w.component<anim::CAnimGraph>(e, type);
    };
    auto posX = [&]() {
        const anim::Pose* p = sys.pose(e);
        return p && !p->local.empty() ? p->local[0].position.x : -1.0f;
    };

    AVER_INFO("=== idle: the pose is the machine's ===");
    step(0.25f);
    step(0.25f);
    check(g && g->activeState == hashOf("Idle"), "the active state is written back");
    check(std::fabs(posX() - 50.0f) < 0.01f, "the pose is Idle at 0.5 s (got " + std::to_string(posX()) + ")");
    check(g && std::fabs(g->stateTime - 0.5f) < 1e-4f, "the normalised time is written back");

    AVER_INFO("=== a float parameter through the slots ===");
    setParam(*g, "speed", 1.0f);
    step(0.25f);
    check(g->activeState == hashOf("Walk"), "writing speed to a slot moves the machine to Walk");
    check(g_enter >= 2 && g_lastEvent == "walk_started", "and the enter event reached the sink");
    step(0.25f);
    check(std::fabs(posX() - 75.0f) < 0.01f, "the pose is Walk at 0.25 s of a 300 cm slide (got " + std::to_string(posX()) + ")");

    AVER_INFO("=== a trigger through the slots ===");
    setParam(*g, "kick", 1.0f);
    step(0.125f);
    check(g->activeState == hashOf("Kick"), "a trigger slot fires the Any transition");
    u32 kickSlot = anim::kAnimGraphSlots;
    for (u32 i = 0; i < anim::kAnimGraphSlots; ++i)
        if (g->paramHash[i] == hashOf("kick")) kickSlot = i;
    check(kickSlot < anim::kAnimGraphSlots && g->paramValue[kickSlot] == 0.0f, "the trigger slot is zeroed once taken");
    step(0.125f);
    step(0.125f);
    step(0.125f);
    step(0.125f);
    check(g->activeState == hashOf("Idle") || g->activeState == hashOf("Walk"), "the kick leaves through its exit time");

    AVER_INFO("=== playRate and pause ===");
    {
        const f32 before = g->stateTime;
        g->flags |= anim::kAnimGraphPaused;
        step(0.25f);
        check(std::fabs(g->stateTime - before) < 1e-6f, "a paused graph holds its clock");
        g->flags &= ~anim::kAnimGraphPaused;
    }

    AVER_INFO("=== lifetime ===");
    check(graphs.activeInstances() == 1, "one live instance");
    check(graphs.instance(e) != nullptr, "and it is reachable by entity");
    w.removeComponent(e, type);
    graphs.tick(w, 0.016f);
    check(graphs.activeInstances() == 0, "removing the component retires the instance");

    anim::AnimGraphSystem::uninstall(sys);
    check(!sys.hasPoseSource(), "uninstall restores clip-only posing");

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO(g_failures == 0 ? "AnimGraphSystemTest: PASS" : "AnimGraphSystemTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
