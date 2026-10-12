// The managed seam with a real .NET runtime in the process: after bridge boot the dispatch function is
// installed (HostBridge.SetupManagedActors -> ManagedAi.Startup), the sample actor assembly's
// [AverAiAction] / [AverEqsTest] methods were scanned at load (ManagedAi.ScanAssembly), a .ocst whose task
// is the managed action runs to Succeeded, a custom EQS test filters items, and unloading the scripts
// clears everything (ManagedAi.Shutdown) so the leaves fail rather than call a dead delegate.
//
// UNAVAILABLE IS NOT A FAILURE: a machine with no .NET runtime, or a build without the staged bridge or
// the sample assembly, reports so and exits 0 -- the same path the engine itself takes.
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/platform/FileSystem.hpp"
#include "aver/scene/World.hpp"
#include "aver/scripting/ScriptHost.hpp"
#include "aver/synapse/Bt.hpp"
#include "aver/synapse/StateTree.hpp"
#include "aver/synapse/synapse_ai_abi.h"
#include "aver/synapse/synapse_query_abi.h"

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

int main() {
    AVER_INFO("ManagedAiClrTest");

    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir  = executableDir() + AVER_PATH_SEP "Scripting";
    hd.scriptsDir = executableDir() + AVER_PATH_SEP "ActorScripts";
    if (!host.init(hd)) {
        AVER_WARN("ManagedAiClrTest: UNAVAILABLE -- {}", host.declineReason());
        return 0;
    }
    if (aver_syn_managed_installed() != 1) {
        AVER_WARN("ManagedAiClrTest: UNAVAILABLE -- the staged bridge predates ManagedAi.Startup");
        host.shutdown();
        return 0;
    }
    check(aver_syn_ai_register() == 1, "the Synapse components register");

    // Probe: the sample assembly's action is already registered, so registering its name again is refused.
    if (aver_syn_managed_register(AVER_SYN_MANAGED_ACTION, "SampleAiStep") != 0) {
        AVER_WARN("ManagedAiClrTest: UNAVAILABLE -- the sample actor assembly is not staged (no SampleAiStep)");
        host.shutdown();
        return 0;
    }
    check(true, "the dispatch is installed and the sample assembly's attributed methods were scanned");

    // The leaves land in the registry the host hands to registerBehaviors.
    synapse::BtRegistry reg;
    aver_syn_ai_register_behaviors(&reg);
    check(reg.findAction("SampleAiStep") != nullptr && reg.findCondition("SampleAiAlways") != nullptr, "[AverAiAction] and [AverAiCondition] became BT / StateTree leaves");

    fmt::OcBtNode cond;
    cond.kind = fmt::OcBtNodeKind::Condition;
    cond.name = "SampleAiAlways";
    const auto* ce = reg.findCondition("SampleAiAlways");
    check(ce && ce->fn(1, cond, ce->user), "the managed Condition answers true");

    // A state tree whose task is the managed action runs to Succeeded.
    synapse::StateTreeAsset tree;
    std::string why;
    check(synapse::parseStateTree("OCST 1\nONFINISH stop\nSTATE A\n  TASK SampleAiStep\nEND\n", "clr.ocst", tree, &why), "parse the tree: " + why);
    synapse::StateTreeInstance inst;
    synapse::StStatus status = synapse::StStatus::Running;
    int ticks = 0;
    while (status == synapse::StStatus::Running && ticks < 50) {
        status = synapse::tickStateTree(tree, reg, 1, 0.01f, inst, nullptr);
        ++ticks;
    }
    check(status == synapse::StStatus::Succeeded && ticks >= 2 && ticks <= 4, "a .ocst task naming the managed action runs to Success (Running first, then Success)");

    // A custom EQS test filters items.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-managed-ai-clr";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir / "clr.oceqs", std::ios::binary);
        f << "OCEQS 1\nOPTION\n  GENERATOR points list=\"0,0,10;0,0,60;0,0,90\"\n  TEST custom name=SampleAiHeight purpose=filter min=50\n";
    }
    aver_syn_set_content_root(dir.generic_string().c_str());
    const int32_t id = aver_syn_eqs_load("clr.oceqs");
    const int32_t querier = static_cast<int32_t>(scene::World::instance().create("ClrQuerier"));
    int32_t h = aver_syn_eqs_run(id, querier, 0, 0, 0, 0, 0, nullptr);
    check(id > 0 && h > 0 && aver_syn_eqs_count(h) == 2, "[AverEqsTest] filters: the items at z 60 and 90 pass min=50");
    aver_syn_eqs_release(h);

    // Unloading the scripts clears the managed names.
    host.unloadScripts();
    check(aver_syn_managed_installed() == 0, "unloading the scripts removes the dispatch (ManagedAi.Shutdown)");
    const auto* act = reg.findAction("SampleAiStep");
    fmt::OcBtNode node;
    node.kind = fmt::OcBtNodeKind::Action;
    node.name = "SampleAiStep";
    f32 elapsed = 0.0f;
    check(act && act->fn(1, node, 0.01f, elapsed, act->user) == synapse::BtStatus::Failure, "...and the leaf fails instead of calling a dead delegate");
    h = aver_syn_eqs_run(id, querier, 0, 0, 0, 0, 0, nullptr);
    check(h > 0 && aver_syn_eqs_count(h) == 0, "...and the custom EQS test rejects every item");
    aver_syn_eqs_release(h);

    host.shutdown();
    if (g_failures == 0) AVER_INFO("ManagedAiClrTest: all passed");
    return g_failures;
}
