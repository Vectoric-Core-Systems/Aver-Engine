// ExtensionsTest -- the extension loader (modules/extensions) against fake extension directories and a tiny extension library written in C
// (tests/extensions/testext/aver_test_ext.c, built by CMake; it needs aver_ext.h and nothing else).
//
// What is exercised: a valid extension; every refusal (wrong ABI, engine out of range, missing native / managed / listed file, name that differs
// from the directory, duplicate name, bad JSON, no manifest) with its reason; lazy loading; command dispatch and exit codes; log forwarding to
// the host; the host's directories; host services by name and version (the model-import round trip with its allocation freed by the host); a
// second extension that offers an interface and reports through the command protocol; the protocol lines; an extension whose entry refuses the
// host; shutdown on destruction. Headless: no device, no .NET, no engine run.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/extensions/Extensions.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#define AVER_DUP _dup
#define AVER_DUP2 _dup2
#define AVER_CLOSE _close
#define AVER_OPEN(path, flags, mode) _open(path, flags, _S_IREAD | _S_IWRITE)
#define AVER_O_WRITE_NEW (_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY)
#else
#include <unistd.h>
#define AVER_DUP dup
#define AVER_DUP2 dup2
#define AVER_CLOSE close
#define AVER_OPEN(path, flags, mode) open(path, flags, mode)
#define AVER_O_WRITE_NEW (O_WRONLY | O_CREAT | O_TRUNC)
#endif

using namespace aver;
namespace fsys = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---- what the host sees ----

struct Line { int level; std::string text; };
static std::vector<Line> g_lines;
static std::vector<std::pair<double, std::string>> g_progress;
static int g_imports = 0;
static int g_frees = 0;

static void hostLog(int32_t level, const char* msg) { g_lines.push_back({level, msg}); }
static void hostProgress(double f, const char* text) { g_progress.push_back({f, text}); }

struct ImportData {
    std::vector<AverExtModelPart> parts;
    std::vector<std::string> strings;   // the storage the parts and notes point into
    std::vector<const char*> notes;
};

static int32_t hostImport(const AverExtModelArgs* args, AverExtModelResult* out) {
    ++g_imports;
    auto* d = new ImportData();
    const std::string input = args->input;
    out->hostData = d;
    if (input.find("bad") != std::string::npos) {
        d->strings.push_back("not a model file");
        out->ok = 0;
        out->error = d->strings.back().c_str();
        return 1;
    }
    d->strings.reserve(8);
    d->strings.push_back(std::string(args->outSubdir) + "/" + args->base + ".ocmesh");
    d->strings.push_back("Hull");
    d->strings.push_back("dropped a camera");
    d->parts.push_back({d->strings[0].c_str(), d->strings[1].c_str(), 1.5, -2.0, 3.25, 7});
    d->notes.push_back(d->strings[2].c_str());
    out->ok = 1;
    out->error = "";
    out->parts = d->parts.data();
    out->partCount = uint32_t(d->parts.size());
    out->notes = d->notes.data();
    out->noteCount = uint32_t(d->notes.size());
    return 0;
}

static void hostFree(AverExtModelResult* r) {
    ++g_frees;
    delete static_cast<ImportData*>(r->hostData);
    r->hostData = nullptr;
}

static const AverExtImportModelService kImportService = {sizeof(AverExtImportModelService), hostImport, hostFree};

static bool hasLine(const std::string& needle) {
    for (const Line& l : g_lines) if (contains(l.text, needle)) return true;
    return false;
}
static int levelOf(const std::string& needle) {
    for (const Line& l : g_lines) if (contains(l.text, needle)) return l.level;
    return -1;
}

// ---- fake extension directories ----

static const std::string kWork = AVER_EXT_TEST_WORKDIR;
static const std::string kDll = AVER_EXT_TEST_DLL;
static const std::string kRefuseDll = AVER_EXT_TEST_REFUSE_DLL;
static const std::string kDll2 = AVER_EXT_TEST2_DLL;

// Runs `fn` with the process's stdout (file descriptor 1) going to a file, and returns what was written: the lines an extension's command prints.
static std::string captureStdout(const std::function<void()>& fn) {
    const std::string path = kWork + "/stdout.txt";
    std::fflush(stdout);
    const int saved = AVER_DUP(1);
    const int file = AVER_OPEN(path.c_str(), AVER_O_WRITE_NEW, 0644);
    AVER_DUP2(file, 1);
    AVER_CLOSE(file);
    fn();
    std::fflush(stdout);
    AVER_DUP2(saved, 1);
    AVER_CLOSE(saved);
    std::string text;
    readFileText(path, text);
    return text;
}

static std::vector<ext::CommandMessage> protocolLines(const std::string& text) {
    std::vector<ext::CommandMessage> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const ext::CommandMessage m = ext::parseCommandMessage(std::string_view(text).substr(at, end - at));
        if (m.kind != ext::MessageKind::None) out.push_back(m);
        at = end + 1;
    }
    return out;
}

static void put(const std::string& path, const std::string& text) {
    createDirectories(fsys::path(path).parent_path().string());
    std::ofstream f(path, std::ios::binary);
    f << text;
}

static std::string manifest(const std::string& name, int abi, const std::string& engine, const std::string& extra = {}) {
    return "{\n  \"name\": \"" + name + "\",\n  \"displayName\": \"" + name + " Display\",\n  \"version\": \"1.2.3\",\n  \"abi\": " + std::to_string(abi) +
           ",\n  \"engine\": \"" + engine + "\",\n  \"license\": \"MIT\",\n  \"source\": \"https://example.invalid/" + name + "\",\n  \"native\": \"TestExt\"" + extra +
           "\n}\n";
}

static const char* kCommands =
    ",\n  \"commands\": [ {\"name\": \"echo\", \"summary\": \"print the arguments\", \"usage\": \"<word>...\"},{\"name\": \"exit7\"}, {\"name\": \"log\"}, {\"name\": \"dirs\"}, "
    "{\"name\": \"model\", \"summary\": \"import a model\"}, {\"name\": \"services\"} ]";

// Writes <root>/<dir>/ with a manifest and (when `withDll`) the native library for this platform.
static void makeExtension(const std::string& root, const std::string& dir, const std::string& json, bool withDll = true, const std::string& dllSource = kDll) {
    const std::string base = root + "/" + dir;
    put(base + "/extension.json", json);
    if (withDll) {
        const std::string dest = base + "/native/" + ext::platformDirName() + "/" + ext::nativeFileName("TestExt");
        createDirectories(fsys::path(dest).parent_path().string());
        std::error_code ec;
        fsys::copy_file(fsys::path(dllSource), fsys::path(dest), fsys::copy_options::overwrite_existing, ec);
        if (ec) AVER_ERROR("could not copy {} -> {}: {}", dllSource, dest, ec.message());
    }
}

static void testVersionRanges() {
    check(ext::versionInRange("0.8.0", ">=0.8.0 <0.9.0"), "0.8.0 is in >=0.8.0 <0.9.0");
    check(ext::versionInRange("0.8.5-2", ">=0.8.0 <0.9.0"), "a suffixed 0.8.5-2 is in it too");
    check(!ext::versionInRange("0.9.0", ">=0.8.0 <0.9.0"), "0.9.0 is not");
    check(!ext::versionInRange("0.7.9", ">=0.8.0 <0.9.0"), "0.7.9 is not");
    check(ext::versionInRange("1.2.0", "1.2"), "a bare version means equal");
    check(ext::versionInRange("1.0.0", ">0.5 <=1.0.0"), "> and <= comparators");
    check(!ext::versionInRange("1.0.1", ">0.5 <=1.0.0"), "<= excludes the next patch");
    std::string why;
    const bool parsed = ext::versionInRange("1.0.0", ">=x.y", &why);
    check(!parsed && contains(why, "not understood"), "a range that does not parse says so: " + why);
    check(!ext::versionInRange("1.0.0", "   ", &why) && contains(why, "not understood"), "an empty range does not parse");
    why.clear();
    const bool inside = ext::versionInRange("0.7.0", ">=0.8.0", &why);
    check(!inside && contains(why, "0.7.0"), "an excluded version names itself: " + why);
}

int main() {
    AVER_INFO("ExtensionsTest");
    testVersionRanges();

    std::error_code ec;
    fsys::remove_all(fsys::path(kWork), ec);   // this test's own scratch folder under the build tree
    const std::string rootA = kWork + "/A/Extensions";
    const std::string rootB = kWork + "/B/Extensions";
    createDirectories(rootA);
    createDirectories(rootB);

    const std::string engine = ">=0.8.0 <0.9.0";
    const int abi = AVER_EXT_ABI;
    const std::string good = manifest("GoodExt", abi, engine,
        std::string(",\n  \"managed\": { \"runtime\": [\"Good.dll\"], \"entry\": \"Good.Entry\", \"actorBase\": \"Good.Node\" }") + kCommands +
        ",\n  \"importers\": [ {\"id\": \"good\", \"menu\": \"Import Good...\", \"command\": \"echo\", \"tooltip\": \"Tip\", \"description\": \"Imports good things.\","
        " \"input\": {\"kind\": \"file\", \"patterns\": [\"*.good\", \"*.fine\"]}, \"outputDefault\": \"{input}_out\","
        " \"options\": [ {\"flag\": \"--all\", \"label\": \"All of it\", \"default\": true}, {\"flag\": \"--dry\", \"label\": \"Dry\"} ]},"
        " {\"menu\": \"Import Folder...\", \"command\": \"echo\", \"input\": {\"detect\": \"good.project\"}} ]"
        ",\n  \"runtimeFiles\": [\"native/{platform}/TestExt.*\", \"managed/Good.dll\", \"data/info.json\"]");
    makeExtension(rootA, "GoodExt", good);
    put(rootA + "/GoodExt/managed/Good.dll", "x");
    put(rootA + "/GoodExt/data/info.json", "{}");

    makeExtension(rootA, "OldAbi", manifest("OldAbi", 1, engine, kCommands));
    makeExtension(rootA, "FutureAbi", manifest("FutureAbi", abi + 1, engine, kCommands));
    makeExtension(rootA, "FutureExt", manifest("FutureExt", abi, ">=9.0.0", kCommands));
    makeExtension(rootA, "NoNative", manifest("NoNative", abi, engine, kCommands), false);
    makeExtension(rootA, "NoManaged", manifest("NoManaged", abi, engine, ",\n  \"managed\": { \"runtime\": [\"Gone.dll\"] }"));
    makeExtension(rootA, "EntryNoRuntime", manifest("EntryNoRuntime", abi, engine, ",\n  \"managed\": { \"entry\": \"Some.Entry\" }"));
    makeExtension(rootA, "BadInputKind", manifest("BadInputKind", abi, engine,
        ",\n  \"importers\": [ {\"menu\": \"Import...\", \"command\": \"echo\", \"input\": {\"kind\": \"stream\"}} ]"));
    makeExtension(rootA, "NoListed", manifest("NoListed", abi, engine, ",\n  \"runtimeFiles\": [\"data/never.json\"]"));
    makeExtension(rootA, "NoGlob", manifest("NoGlob", abi, engine, ",\n  \"runtimeFiles\": [\"native/{platform}/Nothing.*\"]"));
    makeExtension(rootA, "WrongName", manifest("SomethingElse", abi, engine, kCommands));
    makeExtension(rootA, "BadJson", "{ \"name\": ");
    makeExtension(rootA, "NoEngine", "{ \"name\": \"NoEngine\", \"abi\": " + std::to_string(abi) + ", \"native\": \"TestExt\" }");
    makeExtension(rootA, "CmdNoNative", "{ \"name\": \"CmdNoNative\", \"abi\": " + std::to_string(abi) + ", \"engine\": \">=0.8.0 <0.9.0\", " + std::string("\"commands\": [{\"name\": \"zap\"}] }"), false);
    createDirectories(rootA + "/Empty");
    createDirectories(rootA + "/.hidden");
    makeExtension(rootA, "RefuseExt", manifest("RefuseExt", abi, engine, ",\n  \"commands\": [ {\"name\": \"refuse-me\"} ]"), true, kRefuseDll);
    // The second test extension: a host service, an interface and the command protocol.
    makeExtension(rootA, "ToolExt",
        "{ \"name\": \"ToolExt\", \"version\": \"2.0.0\", \"abi\": " + std::to_string(abi) + ", \"engine\": \"" + engine + "\", \"native\": \"TestExt2\", "
        "\"commands\": [ {\"name\": \"convert\"}, {\"name\": \"fail\"}, {\"name\": \"noresult\"} ] }", false);
    {
        const std::string dest = rootA + "/ToolExt/native/" + ext::platformDirName() + "/" + ext::nativeFileName("TestExt2");
        createDirectories(fsys::path(dest).parent_path().string());
        std::error_code cec;
        fsys::copy_file(fsys::path(kDll2), fsys::path(dest), fsys::copy_options::overwrite_existing, cec);
        if (cec) AVER_ERROR("could not copy {} -> {}: {}", kDll2, dest, cec.message());
    }
    // The same name from a second root.
    makeExtension(rootB, "GoodExt", good);
    put(rootB + "/GoodExt/managed/Good.dll", "x");
    put(rootB + "/GoodExt/data/info.json", "{}");
    makeExtension(rootB, "OtherExt", manifest("OtherExt", abi, engine, ",\n  \"commands\": [ {\"name\": \"other\"} ]"));
    std::string exeDir = kWork + "/A";
    ext::Registry reg;
    ext::HostServices services;
    services.log = hostLog;
    services.progress = hostProgress;
    reg.setHostServices(services);
    reg.setEngineVersion("0.8.1");
    reg.setExecutableDir(exeDir);

    // The host's services, by name and version (the process-wide registry an extension reaches through AverExtHost::getService).
    ext::ServiceRegistry& registry = ext::ServiceRegistry::instance();
    registry.clear();
    check(!registry.add("NotAName", 1, &kImportService) && !registry.add("single", 1, &kImportService) && !registry.add("a..b", 1, &kImportService) &&
              !registry.add(AVER_EXT_SERVICE_IMPORT_MODEL, 0, &kImportService) && !registry.add(AVER_EXT_SERVICE_IMPORT_MODEL, 1, nullptr),
          "a service needs a valid name, a version above 0 and a table");
    check(registry.add(AVER_EXT_SERVICE_IMPORT_MODEL, 1, &kImportService) && !registry.add(AVER_EXT_SERVICE_IMPORT_MODEL, 1, &kImportService),
          "a name and version are added once");
    check(registry.find(AVER_EXT_SERVICE_IMPORT_MODEL, 1) == &kImportService && registry.find(AVER_EXT_SERVICE_IMPORT_MODEL, 2) == nullptr &&
              registry.find("aver.nothing.here", 1) == nullptr,
          "find answers the exact name and version only");
    check(ext::isValidInterfaceName("aver.asset.import-model") && ext::isValidInterfaceName("com.example.x2") && !ext::isValidInterfaceName("Aver.asset") &&
              !ext::isValidInterfaceName("aver.-x") && !ext::isValidInterfaceName("aver.x.") && !ext::isValidInterfaceName(".aver.x") &&
              !ext::isValidInterfaceName("aver") && !ext::isValidInterfaceName("aver.x_y"),
          "the naming rule: lower-case [a-z0-9-] segments joined by '.', at least two");

    // ---- scan ----
    const usize added = reg.scan(rootA);
    check(added == 17, "scan lists every directory but the hidden one (" + std::to_string(added) + ")");
    reg.clear();
    check(reg.extensions().empty(), "clear forgets every entry");
    check(reg.scanDefault() == 17, "scanDefault reads <executable dir>/Extensions");
    const usize second = reg.scan(rootB);
    check(second == 2, "a second root adds its own entries (" + std::to_string(second) + ")");

    const ext::Extension* g = reg.find("GoodExt");
    check(g && g->ready() && g->reason.empty(), "GoodExt is Ready");
    if (g) {
        check(g->displayName == "GoodExt Display" && g->version == "1.2.3" && g->license == "MIT" && g->abi == AVER_EXT_ABI, "its manifest fields are read");
        check(g->source == "https://example.invalid/GoodExt" && g->engineRange == engine && g->nativeName == "TestExt", "source, engine range and native name");
        check(g->managedRuntime.size() == 1 && g->managedRuntime[0] == "Good.dll" && g->managedEntry == "Good.Entry" && g->actorBase == "Good.Node",
              "managed runtime, entry type and actor base");
        check(g->commands.size() == 6 && g->commands[0].name == "echo" && g->commands[0].summary == "print the arguments" && g->commands[0].usage == "<word>..." &&
                  g->commands[1].summary.empty() && g->commands[1].usage.empty(),
              "commands, summaries and usage");
        check(g->importers.size() == 2, "two importers");
        if (g->importers.size() == 2) {
            const ext::ImporterInfo& a = g->importers[0];
            const ext::ImporterInfo& b = g->importers[1];
            check(a.id == "good" && a.menu == "Import Good..." && a.command == "echo" && a.tooltip == "Tip" && a.description == "Imports good things." &&
                      a.input.kind == "file" && a.input.patterns == std::vector<std::string>({"*.good", "*.fine"}) && a.input.detect.empty() &&
                      a.outputDefault == "{input}_out" && a.options.size() == 2 && a.options[0].flag == "--all" && a.options[0].label == "All of it" &&
                      a.options[0].onByDefault && !a.options[1].onByDefault,
                  "an importer: id, menu, command, wording, input kind and patterns, output default and options");
            check(b.id == "echo" && b.input.kind == "folder" && b.input.detect == "good.project" && b.input.patterns.empty() && b.outputDefault.empty(),
                  "another importer: the id defaults to the command, the input to a folder, the rest to empty");
        }
        check(g->runtimeFiles.size() == 3, "runtime files");
        check(contains(g->nativePath, std::string("native/") + ext::platformDirName()) && fileExists(g->nativePath), "its native path is this platform's library");
        check(g->dirName == "GoodExt" && contains(g->dir, "GoodExt"), "directory name and path");
    }

    auto refused = [&](const char* name, const std::string& reasonPart) {
        const ext::Extension* e = reg.find(name);
        check(e && !e->ready() && contains(e->reason, reasonPart), std::string(name) + " is refused: " + (e ? e->reason : "(missing)"));
    };
    refused("OldAbi", "ABI 1, which this engine no longer loads");
    refused("OldAbi", "needs ABI " + std::to_string(AVER_EXT_ABI));
    refused("FutureAbi", "this engine supports ABI " + std::to_string(AVER_EXT_ABI));
    refused("FutureExt", ">=9.0.0");
    refused("NoNative", "missing file native/");
    refused("NoManaged", "missing file managed/Gone.dll");
    refused("EntryNoRuntime", "managed.entry");
    refused("BadInputKind", "input.kind");
    refused("NoListed", "missing file data/never.json");
    refused("NoGlob", "Nothing.*");
    refused("NoEngine", "no engine range");
    refused("CmdNoNative", "names no native library");
    refused("BadJson", "not valid JSON");
    refused("Empty", "no extension.json");
    {
        const ext::Extension* w = nullptr;
        for (const ext::Extension& e : reg.extensions()) if (e.dirName == "WrongName") w = &e;
        check(w && !w->ready() && contains(w->reason, "does not match the directory"), "WrongName is refused: " + (w ? w->reason : "(missing)"));
    }
    {
        usize good2 = 0;
        const ext::Extension* dup = nullptr;
        for (const ext::Extension& e : reg.extensions()) if (e.dirName == "GoodExt") { ++good2; if (!e.ready()) dup = &e; }
        check(good2 == 2 && dup && contains(dup->reason, "already listed"), "the second GoodExt is refused as a duplicate: " + (dup ? dup->reason : "(none)"));
    }
    check(reg.find("OtherExt") && reg.find("OtherExt")->ready(), "OtherExt from the second root is Ready");
    check(reg.find(".hidden") == nullptr, "a hidden directory is skipped");

    // ---- commands are found only on Ready extensions ----
    check(reg.findCommand("echo") && reg.findCommand("echo")->name == "GoodExt", "echo belongs to GoodExt");
    check(reg.findCommand("other") && reg.findCommand("other")->name == "OtherExt", "other belongs to OtherExt");
    check(reg.findCommand("zap") == nullptr, "a refused extension's command is not offered");
    check(reg.findCommand("nope") == nullptr, "an unknown command is not found");
    check(!reg.isLoaded("GoodExt"), "scanning loads nothing (lazy)");

    // ---- dispatch ----
    g_lines.clear();
    const char* echoArgs[] = {"echo", "one", "two words", "three"};
    int code = -1;
    std::string why;
    const bool ranEcho = reg.run("echo", 4, echoArgs, code, &why);
    check(ranEcho && code == 3, "echo runs and returns its argument count (" + std::to_string(code) + ")");
    check(reg.isLoaded("GoodExt"), "the first command loads the library");
    check(hasLine("test extension loaded") && hasLine("one|two words|three|"), "the extension logged its load and its arguments");
    const char* exitArgs[] = {"exit7"};
    check(reg.run("exit7", 1, exitArgs, code, &why) && code == 7, "the extension's exit code comes back");
    code = -1;
    const bool ranZap = reg.run("zap", 1, exitArgs, code, &why);
    check(!ranZap && contains(why, "zap"), "an unknown command does not run: " + why);

    // ---- log forwarding ----
    g_lines.clear();
    const char* logArgs[] = {"log"};
    reg.run("log", 1, logArgs, code, &why);
    check(levelOf("level-trace") == AVER_EXT_LOG_TRACE && levelOf("level-info") == AVER_EXT_LOG_INFO && levelOf("level-warn") == AVER_EXT_LOG_WARN &&
              levelOf("level-error") == AVER_EXT_LOG_ERROR,
          "log lines reach the host with their levels");

    // ---- the host's directories and progress ----
    g_lines.clear();
    g_progress.clear();
    const char* dirArgs[] = {"dirs"};
    reg.run("dirs", 1, dirArgs, code, &why);
    check(hasLine("exe=" + exeDir) && hasLine("ext=" + rootA + "/GoodExt") && hasLine("engine=0.8.1") &&
              hasLine("abi=" + std::to_string(AVER_EXT_ABI) + " size=" + std::to_string(sizeof(AverExtHost))),
          "the extension is told the executable and extension directories, the engine version and the ABI");
    check(g_progress.size() == 1 && g_progress[0].first == 0.5 && g_progress[0].second == "halfway", "progress reaches the host");

    // ---- services ----
    g_lines.clear();
    const char* serviceArgs[] = {"services"};
    reg.run("services", 1, serviceArgs, code, &why);
    check(hasLine("services import=1 import2=0 unknown=0"), "the extension finds the service at version 1, not at version 2, and not an unknown one");

    // ---- model import through the service ----
    g_lines.clear();
    g_imports = g_frees = 0;
    const char* modelArgs[] = {"model", "C:/in/hull.gltf", "C:/proj/Content", "Meshes/Hull", "Hull"};
    check(reg.run("model", 5, modelArgs, code, &why) && code == 0, "the model command ran and the import succeeded");
    check(g_imports == 1 && g_frees == 1, "the host imported once and freed the result once");
    check(hasLine("model rc=0 ok=1 error= parts=1 notes=1"), "the extension saw ok, no error, one part and one note");
    check(hasLine("part Meshes/Hull/Hull.ocmesh|Hull|1.5|-2|3.25|7"), "the part's asset, name, position and node arrive intact");
    check(hasLine("note dropped a camera"), "and the note");
    g_lines.clear();
    const char* badArgs[] = {"model", "C:/in/bad.gltf", "C:/proj/Content", "Meshes/Bad", "Bad"};
    check(reg.run("model", 5, badArgs, code, &why) && code == 1, "a failed import is a non-zero exit");
    check(hasLine("ok=0 error=not a model file parts=0 notes=0") && g_frees == 2, "and its error text arrives, freed by the host");

    // ---- no services: the defaults ----
    {
        ext::Registry plain;
        plain.setEngineVersion("0.8.1");
        plain.scan(rootB);
        registry.clear();   // the host offers nothing now
        struct Cap { std::vector<std::string> lines; } cap;
        setLogSink([](void* ctx, LogLevel, std::string_view m) { static_cast<Cap*>(ctx)->lines.push_back(std::string(m)); }, &cap);
        code = -1;
        plain.run("model", 5, modelArgs, code, &why);
        const int modelCode = code;
        const char* infoArgs[] = {"echo", "x"};
        plain.run("echo", 2, infoArgs, code, &why);
        setLogSink(nullptr, nullptr);
        bool noService = false, engineLog = false;
        for (const std::string& l : cap.lines) {
            if (contains(l, "the host offers no import-model service")) noService = true;
            if (l == "x|") engineLog = true;
        }
        check(modelCode == 3 && plain.isLoaded("GoodExt"), "the plain registry loaded its own copy (rootB's, a separate library) and ran model");
        check(noService, "getService answers null for a service nobody added, and the extension says so");
        check(engineLog, "the default log goes to the engine log");
        registry.add(AVER_EXT_SERVICE_IMPORT_MODEL, 1, &kImportService);
    }

    // ---- a second extension: a service, an interface and the command protocol ----
    {
        g_lines.clear();
        g_imports = g_frees = 0;
        const char* convertArgs[] = {"convert", "C:/in/hull.gltf", "C:/proj/Content", "Meshes/Hull", "Hull"};
        std::vector<ext::CommandMessage> lines;
        const std::string out = captureStdout([&] { code = -1; reg.run("convert", 5, convertArgs, code, &why); });
        lines = protocolLines(out);
        check(code == 0 && g_imports == 1 && g_frees == 1, "convert ran, imported through the host's service and freed the result (" + std::to_string(code) + ")");
        check(lines.size() == 4 && lines[0].kind == ext::MessageKind::Progress && lines[0].fraction == 0.0 && lines[0].text == "importing" &&
                  lines[1].fraction == 0.5 && lines[2].fraction == 1.0 && lines[2].text == "done",
              "it printed three progress lines: " + std::to_string(lines.size()) + " protocol line(s) in all");
        if (lines.size() == 4) {
            const ext::CommandMessage& r = lines[3];
            check(r.kind == ext::MessageKind::Result && r.ok && r.open == "C:/out/Game/Game.ocproject" && r.report == "C:/out/Report.md" && r.warnings == 1 && r.errors == 0 &&
                      r.message == "1 part(s), first at source index 7",
                  "and one result line: ok, the file to open, the report, one warning (the dropped camera) and the message: " + r.message);
        }

        const char* badConvert[] = {"convert", "C:/in/bad.gltf", "C:/proj/Content", "Meshes/Bad", "Bad"};
        lines = protocolLines(captureStdout([&] { reg.run("convert", 5, badConvert, code, &why); }));
        check(code == 1 && !lines.empty() && lines.back().kind == ext::MessageKind::Result && !lines.back().ok && lines.back().errors == 1 &&
                  contains(lines.back().message, "not a model file"),
              "a failed import ends in a result line with ok false and the reason");

        const char* failArgs[] = {"fail"};
        lines = protocolLines(captureStdout([&] { reg.run("fail", 1, failArgs, code, &why); }));
        check(code == 0 && lines.size() == 1 && !lines[0].ok && lines[0].warnings == 1 && lines[0].errors == 2 && lines[0].message == "it failed on purpose",
              "a result line carries its counts and message");
        const char* noResultArgs[] = {"noresult"};
        lines = protocolLines(captureStdout([&] { reg.run("noresult", 1, noResultArgs, code, &why); }));
        check(lines.size() == 1 && lines[0].kind == ext::MessageKind::Progress && lines[0].fraction == 0.25, "a command can print progress and no result");

        // The interface the extension offers.
        const void* greeter = reg.interfaceOf("ToolExt", "com.example.greeter", 1, &why);
        struct Greeter { uint32_t size; const char* (*greet)(const char*); };
        const Greeter* g1 = static_cast<const Greeter*>(greeter);
        check(g1 && g1->size == sizeof(Greeter) && std::string(g1->greet("Aver")) == "hello, Aver", "getInterface hands out the extension's table, which works");
        check(reg.interfaceOf("ToolExt", "com.example.greeter", 2) == nullptr && reg.interfaceOf("ToolExt", "com.example.other", 1) == nullptr,
              "and null for another version or name");
        check(reg.interfaceOf("GoodExt", "com.example.greeter", 1) == nullptr, "an extension that offers no getInterface offers nothing");
        std::string noWhy;
        const void* none = reg.interfaceOf("NoSuchExt", "com.example.greeter", 1, &noWhy);
        check(none == nullptr && contains(noWhy, "NoSuchExt"), "an unknown extension is named in the reason: " + noWhy);
    }

    // ---- the protocol lines ----
    {
        ext::CommandMessage r;
        r.kind = ext::MessageKind::Result;
        r.ok = true;
        r.open = "C:\\out\\a \"b\".ocproject";
        r.report = "r.md";
        r.warnings = 3;
        r.message = "line one\nline two";
        const std::string text = ext::formatCommandMessage(r);
        const ext::CommandMessage back = ext::parseCommandMessage(text);
        check(back.kind == ext::MessageKind::Result && back.ok && back.open == r.open && back.report == "r.md" && back.warnings == 3 && back.errors == 0 &&
                  back.message == r.message && text.find('\n') == std::string::npos,
              "a result line round-trips with escapes and stays on one line: " + text);
        ext::CommandMessage p;
        p.kind = ext::MessageKind::Progress;
        p.fraction = 0.25;
        p.text = "reading";
        const ext::CommandMessage pb = ext::parseCommandMessage(ext::formatCommandMessage(p));
        check(pb.kind == ext::MessageKind::Progress && pb.fraction == 0.25 && pb.text == "reading", "a progress line round-trips");
        check(ext::parseCommandMessage("{\"aver\":\"progress\",\"text\":\"x\"}").fraction == -1.0 && ext::parseCommandMessage("{\"aver\":\"progress\",\"fraction\":7}").fraction == 1.0,
              "progress without a fraction is unknown (-1), and a fraction above 1 is 1");
        check(ext::parseCommandMessage("{\"aver\":\"future-kind\"}").kind == ext::MessageKind::None && ext::parseCommandMessage("[INFO ] hello").kind == ext::MessageKind::None &&
                  ext::parseCommandMessage("{\"schemaVersion\":1}").kind == ext::MessageKind::None && ext::parseCommandMessage("{broken").kind == ext::MessageKind::None &&
                  ext::parseCommandMessage("").kind == ext::MessageKind::None,
              "anything that is not a known protocol line is ignored");
        const ext::CommandMessage extra = ext::parseCommandMessage("{\"aver\":\"result\",\"ok\":true,\"surprise\":[1,2]}\r");
        check(extra.kind == ext::MessageKind::Result && extra.ok && extra.warnings == 0, "an unknown key is ignored, a missing count is 0, and a trailing CR is trimmed");
    }

    // ---- an extension whose entry refuses the host ----
    {
        const ext::Extension* r = reg.find("RefuseExt");
        check(r && r->ready(), "RefuseExt passes its manifest checks");
        code = -1;
        const char* a[] = {"refuse-me"};
        const bool ranRefuse = reg.run("refuse-me", 1, a, code, &why);
        check(!ranRefuse && contains(why, "does not support"), "but its entry refuses the host, so it does not run: " + why);
        r = reg.find("RefuseExt");
        check(r && !r->ready() && contains(r->reason, "does not support"), "and it is Refused with that reason afterwards");
        check(reg.findCommand("refuse-me") == nullptr, "its command is no longer offered");
    }

    // ---- a project's EXTENSION lines against what is installed ----
    {
        fmt::ProjectDesc project;
        project.name = "Demo";
        project.extensions = {{"GoodExt", "1.2.3"}, {"OldAbi", "1.0.0"}, {"Absent", "2.0.0"}, {"Bare", ""}};
        const std::vector<std::string> missing = ext::missingProjectExtensions(project, reg);
        check(missing.size() == 3, "an installed Ready extension is satisfied; a refused one, an absent one and an unversioned absent one are reported");
        check(missing.size() == 3 && contains(missing[0], "OldAbi 1.0.0") && contains(missing[0], "refused") && contains(missing[0], "Demo"),
              "the refused one names the project, the extension, its version and that it was refused");
        check(missing.size() == 3 && contains(missing[1], "Absent 2.0.0") && contains(missing[1], "not installed"), "the absent one is named with its version and 'not installed'");
        check(missing.size() == 3 && contains(missing[2], "extension Bare,") && !contains(missing[2], "Bare "), "a line without a version names only the extension");
        g_lines.clear();
        const usize warned = ext::warnMissingProjectExtensions(project, reg);
        check(warned == 3, "the warning call returns how many are missing");
        fmt::ProjectDesc none;
        none.name = "Plain";
        check(ext::missingProjectExtensions(none, reg).empty() && ext::warnMissingProjectExtensions(none, reg) == 0, "a project that names none reports none");
    }

    // ---- shutdown on destruction ----
    g_lines.clear();
    reg.clear();
    check(hasLine("test extension shutdown"), "clearing the registry shuts the loaded extension down");
    check(!reg.isLoaded("GoodExt"), "and unloads it");

    AVER_INFO("==================================================");
    AVER_INFO("ExtensionsTest done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
