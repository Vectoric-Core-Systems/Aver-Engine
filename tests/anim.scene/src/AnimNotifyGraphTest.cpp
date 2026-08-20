// THE WHOLE NOTIFY WIRE, END TO END, with a real .NET runtime in the process.
//
// Every other test of this feature stops at a boundary. AnimTest checks the interval arithmetic with
// no clip on disk; AnimSystemTest checks the system delivers to a sink with no scripting host;
// Aver.Graph.Tests checks a graph runs when Fire() is called with no animation anywhere. Each of
// those is worth having and NONE of them would notice if the two halves never met -- if the export
// were bound under the wrong name, if the UTF-8 pointer arrived mangled, if the entity id the sink
// passes were not the id the graph is keyed by.
//
// That gap is the exact shape of this codebase's recurring failure: a mechanism that marshals,
// stores and registers perfectly and that nothing ever reaches. So this test asserts the ONE thing
// no unit test can: a marker in a file on disk made a graph run, and the number it wrote is the
// number of times the clip passed it.
//
// IT IS ALLOWED TO BE UNAVAILABLE. A machine with no .NET runtime (or a build staged without the
// bridge) cannot run this and is not failing it -- ScriptHost::init declines, and this reports
// "unavailable" and exits 0, exactly as the engine itself boots without scripting.
#include "aver/anim/AnimSystem.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/scripting/ScriptHost.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string g_dir;
static constexpr u64 kClipId = 0xC11B0BEAull;

static std::string resolvePath(u64 id, void*) {
    return id == kClipId ? g_dir + "/beat.ocanim" : std::string();
}

// The sink under test: the same two lines SandboxApp and GameApp each install, with the return value
// kept so the test can tell "the graph ran" from "the notify was delivered and landed nowhere".
static scripting::ScriptHost* g_host = nullptr;
static int g_delivered = 0;   // times the sink was called at all
static int g_handled = 0;     // times a graph actually ran one

static void fireIntoGraph(scene::Entity e, const char* name, void*) {
    ++g_delivered;
    if (g_host && g_host->graphFire(static_cast<i32>(e), name)) ++g_handled;
}

// Counts every OnBeat it receives and writes the running total into the entity's own X position,
// where a native reader can see it. A VAR alone would prove the graph ran but leave the count inside
// managed memory this test cannot look at; CLocal.position is the cheapest thing that is visible
// from both sides of the boundary being tested.
static const char* kGraphText =
    "OCGRAPH 1\n"
    "NAME AnimNotifyProbe\n"
    "DESCRIPTION Counts OnBeat and writes the total into its own X, so a native test can read it.\n"
    "\n"
    // PARAM, then a Param NODE that reads it: a Param node only grows its `value` pin when a
    // matching PARAM record declares the type -- the record is the declaration, the node is the
    // read, and a graph with only the node compiles to "no such pin".
    "PARAM entity int\n"
    "NODE ent Param param=entity\n"
    "VAR beats float 0\n"
    "\n"
    "ENTRY beat OnBeat\n"
    "NODE beat CustomEvent name=OnBeat\n"
    "\n"
    "NODE one ConstFloat value=1.0\n"
    "NODE prev GetVar var=beats\n"
    "NODE next Add\n"
    "LINK prev.value next.a\n"
    "LINK one.value next.b\n"
    "\n"
    "NODE store SetVar var=beats\n"
    "LINK beat.exec store.exec\n"
    "LINK next.result store.value\n"
    "\n"
    "NODE zero ConstFloat value=0.0\n"
    "NODE move SetFieldVec3 field=CLocal.position\n"
    "PIN move exec in exec\n"
    "PIN move entity in int\n"
    "PIN move x in float\n"
    "PIN move y in float\n"
    "PIN move z in float\n"
    "PIN move then out exec\n"
    "PIN move success out bool\n"
    "LINK store.then move.exec\n"
    "LINK ent.value move.entity\n"
    // A SECOND GetVar, not another read of `next`. A pull subtree is re-evaluated wherever it is
    // consumed rather than cached across the exec boundary, so wiring `next.result` here would
    // re-run the Add AFTER the SetVar and write count+1 -- which is exactly what it did, and is
    // the same off-by-one AN_FPTarget's own reporting comment warns about in the other
    // direction. Reading the variable back is the only expression that means "what was stored".
    "NODE report GetVar var=beats\n"
    "LINK report.value move.x\n"
    "LINK zero.value move.y\n"
    "LINK zero.value move.z\n";

int main() {
    AVER_INFO("AnimNotifyGraphTest");

    g_dir = (std::filesystem::temp_directory_path() / "aver-anim-notify-e2e").string();
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);

    // ---- the fixtures: a one-second clip with ONE marker in the middle, and the graph above.
    {
        fmt::OcAnimation a;
        a.duration = 1.0f;
        a.flags = fmt::kOcAnimLoop;
        // A track, because a clip with none is a clip nothing would ever play. It animates a bone
        // this test never poses -- there is no skeleton here at all, which is itself the point: a
        // notify is an event on a clock, not a contribution to a pose.
        fmt::OcTrack t;
        t.boneIndex = 0;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 1.0f};
        t.values = {0, 0, 0,  10, 0, 0};
        a.tracks.push_back(t);
        // 0.5 rather than 0 or 1: a marker at either end is reachable by the wrap logic as well as
        // by an ordinary step, and this test is about the WIRE, not about which interval branch ran.
        a.notifies.push_back({0.5f, "OnBeat"});

        std::string why;
        check(fmt::saveOcAnim(g_dir + "/beat.ocanim", a, &why), "the fixture clip writes: " + why);

        std::ofstream f(g_dir + "/probe.ocgraph", std::ios::binary);
        f << kGraphText;
        f.close();
        check(std::filesystem::exists(g_dir + "/probe.ocgraph"), "the fixture graph writes");
    }

    // ---- the scripting host, pointed at the bridge staged beside this executable
    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + "\\Scripting";
    // The sample actor assembly, staged by the same build that staged the bridge. It is what
    // makes AN_TestActor a REGISTERED MANAGED CLASS in this process -- without it the C# half of
    // this test has no actor to spawn, and reports so rather than silently skipping.
    hd.scriptsDir = executableDir() + "\\ActorScripts";
    if (!host.init(hd)) {
        AVER_WARN("AnimNotifyGraphTest: UNAVAILABLE -- {}", host.declineReason());
        AVER_WARN("  (no .NET runtime or no staged bridge; this is the same path the engine itself "
                  "takes on such a machine, and is not a failure)");
        std::filesystem::remove_all(g_dir, ec);
        return 0;
    }
    g_host = &host;

    check(host.graphAvailable(), "the bridge hosts graphs");
    // THE BIND THIS TEST EXISTS FOR. GraphFire is looked up BY NAME at init, so a rename on either
    // side of the boundary silently disables the whole feature with no compile error anywhere.
    check(host.graphFireAvailable(), "and exports GraphFire -- the export the notify wire needs");
    if (!host.graphFireAvailable()) {
        AVER_ERROR("AnimNotifyGraphTest: 1 FAILURES (nothing further can be tested)");
        return 1;
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem& sys = anim::animSystem();
    sys.clear();
    sys.setResolver(&resolvePath, nullptr);
    sys.setNotifySink(&fireIntoGraph, nullptr);

    const scene::Entity e = w.create("beater");
    auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
    check(a != nullptr, "the animator attaches");
    a->clip = kClipId;
    auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
    check(loc != nullptr, "and the entity has the transform the graph will write");
    if (loc) { loc->xf.position.x = -1.0f; }   // a value the graph could not produce

    check(host.graphLoad(static_cast<i32>(e), g_dir + "/probe.ocgraph"),
          "the probe graph compiles and binds to that entity");

    AVER_INFO("one loop of the clip");
    {
        // Ten ticks of 0.1 s: 0 -> 1.0, crossing the 0.5 marker exactly once.
        for (int i = 0; i < 10; ++i) sys.tick(w, 0.1f);
        check(g_delivered == 1, "the sink was called once");
        check(g_handled == 1, "AND A GRAPH RAN -- the marker reached a handler, not just a log line");
        loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
        check(loc && std::fabs(loc->xf.position.x - 1.0f) < 1e-4f,
              "the graph wrote 1 into the entity's X, so the count crossed the boundary intact");
        if (loc) AVER_INFO("  X = {:.3f}", loc->xf.position.x);
    }

    AVER_INFO("three more loops");
    {
        for (int i = 0; i < 30; ++i) sys.tick(w, 0.1f);
        check(g_handled == 4, "four crossings, four handler runs -- one per lap, none lost, none doubled");
        loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
        check(loc && std::fabs(loc->xf.position.x - 4.0f) < 1e-4f,
              "and the graph's own count agrees at 4");
        if (loc) AVER_INFO("  X = {:.3f}", loc->xf.position.x);
    }

    AVER_INFO("an entity whose graph declares no such event");
    {
        // The other half of the contract: firing at a graph that does not handle the name must be a
        // quiet false, not an exception across the ABI and not a crash. Reusing the SAME graph and
        // a marker name it never declares is the cheapest way to reach that branch.
        const scene::Entity e2 = w.create("deaf");
        auto* a2 = static_cast<scene::CAnimator*>(w.addComponent(e2, scene::kComponentAnimator));
        a2->clip = kClipId;
        check(host.graphLoad(static_cast<i32>(e2), g_dir + "/probe.ocgraph"), "a second entity loads it too");
        // Fired by hand rather than through a clip: this is about the refusal path, and writing a
        // second fixture clip to reach it would prove nothing extra.
        check(!host.graphFire(static_cast<i32>(e2), "NoSuchEvent"),
              "an undeclared event reports false rather than throwing across the boundary");
        check(!host.graphFire(999999, "OnBeat"),
              "and so does an entity with no graph at all");
        w.destroy(e2);
        w.flush();
    }

    AVER_INFO("a C# ACTOR receives the same event, through the same router");
    {
        // THE THIRD KIND OF RECEIVER, and the one that used to be told "no live graph" and left it
        // there. A project whose character is written in C# rather than as a graph could not hear a
        // single animation notify, which is not a limitation anybody would have guessed from the
        // feature's description.
        const i32 cls = aver_fw_class_find("AN_TestActor");
        if (cls == 0) {
            AVER_WARN("  SKIPPED -- AN_TestActor is not registered (no ActorScripts staged beside "
                      "this executable); the C# actor path is NOT covered by this run");
        } else {
            const f32 pos[3] = {0.0f, 0.0f, 0.0f};
            const i32 actor = aver_fw_spawn(cls, "beater-cs", pos, nullptr, nullptr);
            check(actor != 0, "a managed actor spawns");

            // Fired by hand rather than through a clip. The clip half is already proved above, and
            // what is unproved HERE is the router's third branch -- reached identically whichever
            // side raised the event.
            check(g_host->graphFire(actor, "OnBeat"),
                  "AN ACTOR HANDLED IT -- OnEvent returned true, so the caller sees a real success");
            check(g_host->graphFire(actor, "OnBeat"), "and again");

            const scene::Entity ae = static_cast<scene::Entity>(actor);
            const auto* al = w.component<scene::CLocal>(ae, scene::kComponentLocal);
            check(al && std::fabs(al->xf.position.x - 2.0f) < 1e-4f,
                  "and the actor's own count reached the native side: X is 2 after two events");
            if (al) AVER_INFO("  X = {:.3f}", al->xf.position.x);

            // The refusal is the other half of the contract: OnEvent returns false for a name it
            // does not know, and that false is what a FireEvent node reads as its own success pin.
            check(!g_host->graphFire(actor, "NotMine"),
                  "an event the actor does not handle reports false rather than a bare true");

            aver_fw_destroy(actor);
            w.flush();
        }
    }

    sys.setNotifySink(nullptr, nullptr);
    host.graphUnload(static_cast<i32>(e));
    w.destroy(e);
    w.flush();
    g_host = nullptr;
    host.shutdown();
    std::filesystem::remove_all(g_dir, ec);

    AVER_INFO(g_failures ? "AnimNotifyGraphTest: {} FAILURES"
                         : "AnimNotifyGraphTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
