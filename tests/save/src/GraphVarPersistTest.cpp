// Graph VARs persist ONLY for entities whose class is GameInstance or a subclass -- an explicit,
// later user decision (see FindPersistedGraphHost, HostBridge.cs) narrowing what was originally
// "every entity with a live GraphHost". This test proves the SCOPING directly against the real
// bridge, not the C++ save-format mechanism (SaveWorldTest/SaveActorTest already cover that): a
// GameInstance-classed entity's graph VAR round-trips through aver_fw_graph_var_count/at/set; a
// plain-classed entity's IDENTICAL declared VAR, on an IDENTICAL live GraphHost, is invisible to
// all three -- proving the gate is the class flag, not merely "does this entity have a graph".
//
// The two test classes are declared through the raw native ABI (aver_fw_class_declare +
// aver_fw_class_set_flags) rather than through HostBridge's own DeclareClass path, so the flag
// under test is set explicitly here rather than inherited -- see class_get_flags's own comment in
// framework_abi.h: flags are a per-class stored field, read from the entity's OWN class, never
// walked up the parent chain at query time. AVER_FW_CLASS_GAME_INSTANCE (framework_abi.h) and
// Aver.Framework's ClassFlags.GameInstance are the same 0x0010 bit by construction -- see that
// header's own "pinned to Aver.Framework's ClassFlags" comment.
//
// UNAVAILABLE IS NOT A FAILURE: a machine with no .NET runtime, or a build with no staged bridge,
// reports so and exits 0 -- the same path the engine itself takes.
#include "aver/framework/framework_abi.h"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/scripting/ScriptHost.hpp"

#include <cstring>
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

// One VAR, one no-op OnTick entry -- this test only reads/writes VarStore through the save
// provider ABI, so the graph is never actually ticked and needs no downstream links.
static const char* kGraphSrc =
    "OCGRAPH 1\n"
    "NAME GraphVarPersistFixture\n"
    "DESCRIPTION fixture for GraphVarPersistTest.cpp\n"
    "\n"
    "VAR Score int 0\n"
    "\n"
    "NODE tick OnTick 0 0\n"
    "\n"
    "ENTRY tick OnTick\n";

int main() {
    AVER_INFO("GraphVarPersistTest");

    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir  = executableDir() + "\\Scripting";
    hd.scriptsDir = executableDir() + "\\ActorScripts";
    if (!host.init(hd)) {
        AVER_WARN("GraphVarPersistTest: UNAVAILABLE -- {}", host.declineReason());
        return 0;
    }
    if (!host.graphAvailable()) {
        AVER_WARN("GraphVarPersistTest: UNAVAILABLE -- the staged bridge does not export graph hosting");
        return 0;
    }

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-graphvar-persist").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string graphPath = dir + "/fixture.ocgraph";
    {
        std::ofstream f(graphPath, std::ios::binary);
        f << kGraphSrc;
    }

    // Two classes, identical shape, differing ONLY in the flag under test.
    const i32 giClass = aver_fw_class_declare("AN_GraphVarPersistGI", "GameInstance");
    check(giClass != 0, "a GameInstance-parented class declares");
    aver_fw_class_set_flags(giClass, AVER_FW_CLASS_GAME_INSTANCE);
    check((aver_fw_class_get_flags(giClass) & AVER_FW_CLASS_GAME_INSTANCE) != 0,
          "and carries the GameInstance flag");

    const i32 plainClass = aver_fw_class_declare("AN_GraphVarPersistPlain", "Actor");
    check(plainClass != 0, "a plain Actor-parented class declares");
    check((aver_fw_class_get_flags(plainClass) & AVER_FW_CLASS_GAME_INSTANCE) == 0,
          "and does NOT carry the GameInstance flag");

    const i32 giEntity = aver_fw_spawn_preview(giClass, "GIHolder", nullptr, nullptr, nullptr);
    const i32 plainEntity = aver_fw_spawn_preview(plainClass, "PlainHolder", nullptr, nullptr, nullptr);
    check(giEntity != 0, "the GameInstance entity spawns");
    check(plainEntity != 0, "the plain entity spawns");

    check(host.graphLoad(giEntity, graphPath), "the fixture graph loads onto the GameInstance entity");
    check(host.graphLoad(plainEntity, graphPath),
          "the IDENTICAL fixture graph loads onto the plain entity");

    AVER_INFO("a GameInstance entity's graph VAR is visible to the save provider");
    {
        const i32 n = aver_fw_graph_var_count(giEntity);
        check(n == 1, "aver_fw_graph_var_count reports the one declared VAR");

        char nameBuf[64] = {};
        i32 kind = -1;
        f32 fv = -1.0f;
        i32 iv = -1;
        const i32 gotAt = aver_fw_graph_var_at(giEntity, 0, nameBuf, sizeof(nameBuf), &kind, &fv, &iv);
        check(gotAt != 0, "aver_fw_graph_var_at reports the VAR");
        check(std::strcmp(nameBuf, "Score") == 0, "with its declared name");
        check(kind == AVER_SCENE_KIND_I32, "and its declared kind (I32)");

        const i32 setOk = aver_fw_graph_var_set(giEntity, "Score", AVER_SCENE_KIND_I32, 0.0f, 77);
        check(setOk != 0, "aver_fw_graph_var_set accepts a restore for this entity");

        char nameBuf2[64] = {};
        i32 kind2 = -1;
        f32 fv2 = -1.0f;
        i32 iv2 = -1;
        aver_fw_graph_var_at(giEntity, 0, nameBuf2, sizeof(nameBuf2), &kind2, &fv2, &iv2);
        check(iv2 == 77, "and the write is visible through the SAME read path a save would use");
    }

    AVER_INFO("a plain entity's IDENTICAL graph VAR is invisible to the save provider");
    {
        const i32 n = aver_fw_graph_var_count(plainEntity);
        check(n == 0,
              "aver_fw_graph_var_count reports ZERO -- despite a live GraphHost with a declared VAR, "
              "identical to the GameInstance entity above");

        const i32 setOk = aver_fw_graph_var_set(plainEntity, "Score", AVER_SCENE_KIND_I32, 0.0f, 99);
        check(setOk == 0, "aver_fw_graph_var_set refuses to write into it");
    }

    host.graphUnload(giEntity);
    host.graphUnload(plainEntity);
    aver_fw_destroy_preview(giEntity);
    aver_fw_destroy_preview(plainEntity);
    host.shutdown();
    std::filesystem::remove_all(dir, ec);

    AVER_INFO(g_failures ? "GraphVarPersistTest: {} FAILURES" : "GraphVarPersistTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
