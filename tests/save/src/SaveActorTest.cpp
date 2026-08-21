// Restoring an ACTOR, with a real .NET runtime in the process.
//
// THE ONE THING THIS EXISTS TO PROVE. aver_fw_spawn is synchronous: spawnActor runs bind ->
// build_models -> beginPlay inline before it returns (FrameworkAbi.cpp:535-580). So a restore that
// spawns an actor and THEN patches its saved fields has already run OnBeginPlay against the CLASS
// DEFAULTS. Nothing crashes, the world looks right afterwards, and every actor that reads its own
// state at BeginPlay -- a door checking whether it is already open, an enemy computing behaviour
// from current health -- silently got the wrong answer.
//
// The fix is to spawn through aver_fw_spawn_preview (no BeginPlay), patch, then dispatch
// aver_fw_dispatch_begin_play. This test fails against the obvious ordering and passes against the
// correct one, which is the only reason it is worth its runtime.
//
// AN.TestActor's OnBeginPlay copies its own X into its own Z-scale, so what it observed is readable
// from here with no extra ABI.
//
// UNAVAILABLE IS NOT A FAILURE: a machine with no .NET runtime, or a build with no staged bridge,
// reports so and exits 0 -- the same path the engine itself takes.
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/save/SaveWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scripting/ScriptHost.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.4f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
}

// ---- the host seams a composition root installs. This test IS one. --------------------------

// Spawns WITHOUT BeginPlay. The whole point; see the file header.
static scene::Entity spawnClass(const char* className, void*) {
    const i32 c = aver_fw_class_find(className);
    if (c == 0) return scene::kInvalidEntity;
    const i32 e = aver_fw_spawn_preview(c, className, nullptr, nullptr, nullptr);
    return e == 0 ? scene::kInvalidEntity : static_cast<scene::Entity>(e);
}

static const char* classOf(scene::Entity e, void*) {
    const i32 c = aver_fw_class_of(static_cast<i32>(e));
    return c == 0 ? nullptr : aver_fw_class_name(c);
}

static void beginPlay(scene::Entity e, void*) {
    aver_fw_dispatch_begin_play(static_cast<i32>(e), AVER_FW_BEGIN_SPAWN);
}

static void destroyActor(scene::Entity e, void*) { aver_fw_destroy(static_cast<i32>(e)); }

static save::Host makeHost() {
    save::Host h;
    h.spawnClass   = &spawnClass;
    h.classOf      = &classOf;
    h.beginPlay    = &beginPlay;
    h.destroyActor = &destroyActor;
    return h;
}

int main() {
    AVER_INFO("SaveActorTest");

    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir  = executableDir() + "\\Scripting";
    hd.scriptsDir = executableDir() + "\\ActorScripts";
    if (!host.init(hd)) {
        AVER_WARN("SaveActorTest: UNAVAILABLE -- {}", host.declineReason());
        return 0;
    }

    const i32 cls = aver_fw_class_find("AN_TestActor");
    if (cls == 0) {
        AVER_WARN("SaveActorTest: UNAVAILABLE -- AN_TestActor is not registered (no ActorScripts "
                  "staged beside this executable); the actor path is NOT covered by this run");
        return 0;
    }

    scene::World& w = scene::World::instance();

    AVER_INFO("an actor's OnBeginPlay observes the RESTORED world, not its class defaults");
    {
        // Spawn normally: OnBeginPlay runs immediately, sees X = 0, and records 0 into Z-scale.
        const i32 spawned = aver_fw_spawn(cls, "Door", nullptr, nullptr, nullptr);
        check(spawned != 0, "an actor spawns");
        const scene::Entity e = static_cast<scene::Entity>(spawned);

        auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
        check(loc != nullptr, "it has a transform");
        if (loc) {
            checkNear(loc->xf.scale.z, 0.0f, 1e-4f,
                      "at spawn it observed X = 0, because that is what it was");
            // Now the player moves it. This is the state a save exists to keep.
            loc->xf.position.x = 42.0f;
            loc->xf.scale.z = -1.0f;    // a value neither a default nor a correct observation
        }

        save::CaptureOptions co;
        co.host = makeHost();
        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, co, &why), "the world captures: " + why);

        bool sawClass = false;
        for (const fmt::OcSaveEntity& en : snap.entities)
            if (en.className == "AN_TestActor") sawClass = true;
        check(sawClass, "and the actor is recorded BY CLASS NAME -- a class handle is process-local");

        // Through a real file, atomically, so the write path is exercised too.
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-save-actor").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/slot0.ocsave";
        check(fmt::saveOcSave(path, snap, &why), "it writes to disk: " + why);

        fmt::OcSaveData back;
        check(fmt::loadOcSave(path, back, &why), "and loads back: " + why);

        save::RestoreOptions ro;
        ro.host = makeHost();
        check(save::restore(back, w, ro, &why), "it restores: " + why);

        const scene::Entity r = w.find("Door");
        check(r != scene::kInvalidEntity, "the actor is back");
        check(r != e, "as a NEW entity -- the old one was destroyed through the framework");
        check(aver_fw_class_of(static_cast<i32>(r)) == cls,
              "and it is a real instance of its class again, not a plain entity");

        const auto* rl = w.component<scene::CLocal>(r, scene::kComponentLocal);
        check(rl != nullptr, "it has a transform");
        if (rl) {
            checkNear(rl->xf.position.x, 42.0f, 1e-3f, "its saved position came back");
            // THE ASSERTION THIS FILE EXISTS FOR. 42 means BeginPlay ran AFTER the fields were
            // patched. 0 means it ran at spawn, against the class default -- which is what the
            // obvious ordering produces, and what looks fine until a door opens itself twice.
            checkNear(rl->xf.scale.z, 42.0f, 1e-3f,
                      "its OnBeginPlay observed 42, not the class default 0");
            // THE ASSERTION THAT ACTUALLY DISCRIMINATES, and the first version of this test did
            // not have it. Checking the observed value ALONE passes against the naive ordering
            // too: spawn dispatches BeginPlay (observing 0), the patch lands, and restore
            // dispatches BeginPlay a second time (observing 42) -- which overwrites the evidence.
            // Only the COUNT tells them apart. Verified by reverting the spawner to aver_fw_spawn
            // and watching this line, and only this line, fail.
            checkNear(rl->xf.scale.y, 1.0f, 1e-3f,
                      "AND IT BEGAN PLAY EXACTLY ONCE -- 2 means it was spawned with BeginPlay and "
                      "then dispatched again, so its first OnBeginPlay ran against class defaults");
        }

        aver_fw_destroy(static_cast<i32>(r));
        w.flush();
        std::filesystem::remove_all(dir, ec);
    }

    host.shutdown();
    AVER_INFO(g_failures ? "SaveActorTest: {} FAILURES" : "SaveActorTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
