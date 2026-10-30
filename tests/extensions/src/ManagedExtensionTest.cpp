// ManagedExtensionTest -- the managed half of an extension reaches the engine through its manifest and nothing else (docs/EXTENSIONS.md,
// docs/plans/fabric/EXTENSION.md section 3.6), with a real .NET runtime in the process and no engine run.
//
// Three fake extensions are written under a scratch Extensions folder, each carrying a real managed assembly the build already stages:
//   ActorExt   Aver.Framework.SampleActor.dll with actorBase = its TestActor: only the types under that base are declared.
//   GoneExt    lists a managed file it does not have: the loader refuses it, ScriptHost logs the reason once and leaves it out.
//   NoBaseExt  Aver.Scripting.SampleBehaviour.dll with an actorBase that no assembly holds: the bridge logs one line and declares nothing.
// ScriptHost::init is pointed at that folder (HostDesc::extensionsDir) with no scripts directory, so any class that exists afterwards came
// through LoadExtensions. Headless: no device, no window, no audio.
//
// UNAVAILABLE IS NOT A FAILURE: a machine with no .NET runtime, or a build without the staged bridge or samples, exits 77 (skipped).
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/extensions/Extensions.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/platform/FileSystem.hpp"
#include "aver/scripting/ScriptHost.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;
namespace fsys = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    ++g_failures;
    AVER_ERROR("   FAIL  {}", what);
}

// Every log line, managed ones included (the bridge writes through the host's log).
static std::vector<std::string> g_lines;
static void capture(void*, LogLevel, std::string_view message) { g_lines.emplace_back(message); }

static int linesWith(const std::string& a, const std::string& b = {}) {
    int n = 0;
    for (const std::string& l : g_lines)
        if (l.find(a) != std::string::npos && (b.empty() || l.find(b) != std::string::npos)) ++n;
    return n;
}

static void put(const fsys::path& path, const std::string& text) {
    std::error_code ec;
    fsys::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f << text;
}

static std::string manifest(const std::string& name, const std::string& assembly, const std::string& actorBase) {
    return "{\n  \"name\": \"" + name + "\",\n  \"version\": \"1.0.0\",\n  \"abi\": " + std::to_string(AVER_EXT_ABI) +
           ",\n  \"engine\": \">=0.7.0 <0.9.0\",\n  \"license\": \"MIT\",\n  \"managed\": { \"runtime\": [\"" + assembly + "\"], \"actorBase\": \"" + actorBase +
           "\" }\n}\n";
}

int main() {
    AVER_INFO("ManagedExtensionTest");
    const fsys::path exe = executableDir();
    const fsys::path actorDll = exe / "ActorScripts" / "Aver.Framework.SampleActor.dll";
    const fsys::path behaviourDll = exe / "SampleScripts" / "Aver.Scripting.SampleBehaviour.dll";
    std::error_code ec;
    if (!fsys::exists(actorDll, ec) || !fsys::exists(behaviourDll, ec)) {
        AVER_WARN("ManagedExtensionTest: UNAVAILABLE -- the sample assemblies are not staged beside this executable");
        return 77;
    }

    const fsys::path root = fsys::path(AVER_MEXT_WORKDIR) / "Extensions";
    fsys::remove_all(root.parent_path(), ec);
    put(root / "ActorExt" / "extension.json", manifest("ActorExt", "Aver.Framework.SampleActor.dll", "Aver.Framework.SampleActor.TestActor"));
    fsys::create_directories(root / "ActorExt" / "managed", ec);
    fsys::copy_file(actorDll, root / "ActorExt" / "managed" / "Aver.Framework.SampleActor.dll", fsys::copy_options::overwrite_existing, ec);
    put(root / "GoneExt" / "extension.json", manifest("GoneExt", "Missing.dll", "Gone.Base"));
    put(root / "NoBaseExt" / "extension.json", manifest("NoBaseExt", "Aver.Scripting.SampleBehaviour.dll", "No.Such.Type"));
    fsys::create_directories(root / "NoBaseExt" / "managed", ec);
    fsys::copy_file(behaviourDll, root / "NoBaseExt" / "managed" / "Aver.Scripting.SampleBehaviour.dll", fsys::copy_options::overwrite_existing, ec);
    check(fsys::exists(root / "ActorExt" / "managed" / "Aver.Framework.SampleActor.dll", ec) &&
              fsys::exists(root / "NoBaseExt" / "managed" / "Aver.Scripting.SampleBehaviour.dll", ec),
          "the fake extensions are staged");

    // What the loader says about the three, before the host reads them.
    {
        ext::Registry registry;
        registry.scan(root.string());
        const ext::Extension* actor = registry.find("ActorExt");
        const ext::Extension* gone = registry.find("GoneExt");
        check(actor && actor->ready() && actor->managedRuntime.size() == 1 && actor->actorBase == "Aver.Framework.SampleActor.TestActor",
              "ActorExt is Ready with its managed assembly and actor base from the manifest");
        check(gone && !gone->ready() && gone->reason.find("Missing.dll") != std::string::npos, "GoneExt is Refused, naming the missing file");
    }

    setLogSink(&capture, nullptr);
    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + AVER_PATH_SEP "Scripting";
    hd.extensionsDir = root.string();
    const bool up = host.init(hd);
    setLogSink(nullptr, nullptr);
    if (!up) {
        AVER_WARN("ManagedExtensionTest: UNAVAILABLE -- {}", host.declineReason());
        return 77;
    }

    AVER_INFO("-- actor classes declared through the manifest --");
    check(aver_fw_class_find("AN_TestActor") != 0, "the actor type under actorBase is declared (AN_TestActor)");
    check(aver_fw_class_find("Sphere") == 0 && aver_fw_class_find("SphereSpawner") == 0 && aver_fw_class_find("AN_Pawn") == 0,
          "the assembly's other actor types, which are not under actorBase, are not");
    check(linesWith("Aver.Framework.SampleActor: declared 1 actor class") == 1, "and the bridge says it declared one");

    AVER_INFO("-- a refused or unusable extension is one log line and is skipped --");
    check(linesWith("GoneExt", "not loaded") == 1 && linesWith("GoneExt", "Missing.dll") == 1, "GoneExt: one line, with the loader's reason");
    check(linesWith("NoBaseExt", "skipped") == 1 && linesWith("NoBaseExt", "No.Such.Type") == 1, "NoBaseExt: one line, naming the base type it lacks");
    check(linesWith("ActorExt", "skipped") == 0 && linesWith("ActorExt", "not loaded") == 0, "ActorExt, which is fine, says nothing");

    host.shutdown();
    if (g_failures) { AVER_ERROR("ManagedExtensionTest: {} check(s) FAILED ===", g_failures); return 1; }
    AVER_INFO("ManagedExtensionTest: all checks passed");
    return 0;
}
