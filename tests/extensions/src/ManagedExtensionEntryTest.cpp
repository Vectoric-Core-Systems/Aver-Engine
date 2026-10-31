// ManagedExtensionEntryTest -- an extension whose managed entry (IAverExtension, docs/EXTENSIONS.md) cannot start is skipped with one log line,
// with a real .NET runtime in the process and no engine run. ManagedExtensionTest covers the entry that works; this is the other half, in a
// process of its own because two extensions cannot load the same assembly into one host context.
//
//   ThrowExt     Aver.Framework.SampleActor.dll, whose entry throws from Load: skipped, and the actor base it also names declares nothing.
//   BadEntryExt  Aver.Scripting.SampleBehaviour.dll with an entry type that no assembly holds: skipped.
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

static std::string manifest(const std::string& name, const std::string& assembly, const std::string& entry, const std::string& actorBase) {
    return "{\n  \"name\": \"" + name + "\",\n  \"version\": \"1.0.0\",\n  \"abi\": " + std::to_string(AVER_EXT_ABI) +
           ",\n  \"engine\": \">=0.7.0 <0.9.0\",\n  \"license\": \"MIT\",\n  \"managed\": { \"runtime\": [\"" + assembly + "\"], \"entry\": \"" + entry +
           "\", \"actorBase\": \"" + actorBase + "\" }\n}\n";
}

int main() {
    AVER_INFO("ManagedExtensionEntryTest");
    const fsys::path exe = executableDir();
    const fsys::path actorDll = exe / "ActorScripts" / "Aver.Framework.SampleActor.dll";
    const fsys::path behaviourDll = exe / "SampleScripts" / "Aver.Scripting.SampleBehaviour.dll";
    std::error_code ec;
    if (!fsys::exists(actorDll, ec) || !fsys::exists(behaviourDll, ec)) {
        AVER_WARN("ManagedExtensionEntryTest: UNAVAILABLE -- the sample assemblies are not staged beside this executable");
        return 77;
    }

    const fsys::path root = fsys::path(AVER_MEXT_WORKDIR) / "EntryFailures" / "Extensions";
    fsys::remove_all(root.parent_path(), ec);
    put(root / "ThrowExt" / "extension.json",
        manifest("ThrowExt", "Aver.Framework.SampleActor.dll", "Aver.Framework.SampleActor.SampleThrowingEntry", "Aver.Framework.SampleActor.TestActor"));
    fsys::create_directories(root / "ThrowExt" / "managed", ec);
    fsys::copy_file(actorDll, root / "ThrowExt" / "managed" / "Aver.Framework.SampleActor.dll", fsys::copy_options::overwrite_existing, ec);
    put(root / "BadEntryExt" / "extension.json",
        manifest("BadEntryExt", "Aver.Scripting.SampleBehaviour.dll", "No.Such.Entry", "Aver.Scripting.SampleBehaviour.HelloBehaviour"));
    fsys::create_directories(root / "BadEntryExt" / "managed", ec);
    fsys::copy_file(behaviourDll, root / "BadEntryExt" / "managed" / "Aver.Scripting.SampleBehaviour.dll", fsys::copy_options::overwrite_existing, ec);

    setLogSink(&capture, nullptr);
    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + AVER_PATH_SEP "Scripting";
    hd.extensionsDir = root.string();
    const bool up = host.init(hd);
    setLogSink(nullptr, nullptr);
    if (!up) {
        AVER_WARN("ManagedExtensionEntryTest: UNAVAILABLE -- {}", host.declineReason());
        return 77;
    }

    check(linesWith("ThrowExt", "skipped") == 1 && linesWith("ThrowExt", "entry refused to start") == 1,
          "ThrowExt: one line, saying the entry failed and why");
    check(aver_fw_class_find("AN_TestActor") == 0, "and nothing it names was declared, not even the actor base's types");
    check(linesWith("BadEntryExt", "skipped") == 1 && linesWith("BadEntryExt", "No.Such.Entry") == 1, "BadEntryExt: one line, naming the entry type nobody holds");

    host.shutdown();
    if (g_failures) { AVER_ERROR("ManagedExtensionEntryTest: {} check(s) FAILED ===", g_failures); return 1; }
    AVER_INFO("ManagedExtensionEntryTest: all checks passed");
    return 0;
}
