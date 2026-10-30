// ExtensionsTest -- the extension loader (modules/extensions) against fake extension directories and a tiny extension library written in C
// (tests/extensions/testext/aver_test_ext.c, built by CMake; it needs aver_ext.h and nothing else).
//
// What is exercised: a valid extension; every refusal (wrong ABI, engine out of range, missing native / managed / listed file, name that differs
// from the directory, duplicate name, bad JSON, no manifest) with its reason; lazy loading; command dispatch and exit codes; log forwarding to
// the host; the host's directories; the model-import callback round trip with its allocation freed by the host; an extension whose entry refuses
// the host; shutdown on destruction. Headless: no device, no .NET, no engine run.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/extensions/Extensions.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

static i32 hostImport(const AverExtModelArgs* args, AverExtModelResult* out) {
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
    "{\"name\": \"model\", \"summary\": \"import a model\"} ]";

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
    const std::string good = manifest("GoodExt", 1, engine,
        std::string(",\n  \"managed\": { \"runtime\": [\"Good.dll\"], \"actorBase\": \"Good.Node\" }") + kCommands +
        ",\n  \"importers\": [ {\"menu\": \"Import Good...\", \"command\": \"echo\", \"input\": \"folder\", \"detect\": \"good.project\", \"opensResult\": true} ]"
        ",\n  \"runtimeFiles\": [\"native/{platform}/TestExt.*\", \"managed/Good.dll\", \"data/info.json\"]");
    makeExtension(rootA, "GoodExt", good);
    put(rootA + "/GoodExt/managed/Good.dll", "x");
    put(rootA + "/GoodExt/data/info.json", "{}");

    makeExtension(rootA, "OldAbi", manifest("OldAbi", 2, engine, kCommands));
    makeExtension(rootA, "FutureExt", manifest("FutureExt", 1, ">=9.0.0", kCommands));
    makeExtension(rootA, "NoNative", manifest("NoNative", 1, engine, kCommands), false);
    makeExtension(rootA, "NoManaged", manifest("NoManaged", 1, engine, ",\n  \"managed\": { \"runtime\": [\"Gone.dll\"] }"));
    makeExtension(rootA, "NoListed", manifest("NoListed", 1, engine, ",\n  \"runtimeFiles\": [\"data/never.json\"]"));
    makeExtension(rootA, "NoGlob", manifest("NoGlob", 1, engine, ",\n  \"runtimeFiles\": [\"native/{platform}/Nothing.*\"]"));
    makeExtension(rootA, "WrongName", manifest("SomethingElse", 1, engine, kCommands));
    makeExtension(rootA, "BadJson", "{ \"name\": ");
    makeExtension(rootA, "NoEngine", "{ \"name\": \"NoEngine\", \"abi\": 1, \"native\": \"TestExt\" }");
    makeExtension(rootA, "CmdNoNative", "{ \"name\": \"CmdNoNative\", \"abi\": 1, \"engine\": \">=0.8.0 <0.9.0\", " + std::string("\"commands\": [{\"name\": \"zap\"}] }"), false);
    createDirectories(rootA + "/Empty");
    createDirectories(rootA + "/.hidden");
    makeExtension(rootA, "RefuseExt", manifest("RefuseExt", 1, engine, ",\n  \"commands\": [ {\"name\": \"refuse-me\"} ]"), true, kRefuseDll);
    // The same name from a second root.
    makeExtension(rootB, "GoodExt", good);
    put(rootB + "/GoodExt/managed/Good.dll", "x");
    put(rootB + "/GoodExt/data/info.json", "{}");
    makeExtension(rootB, "OtherExt", manifest("OtherExt", 1, engine, ",\n  \"commands\": [ {\"name\": \"other\"} ]"));
    std::string exeDir = kWork + "/A";
    ext::Registry reg;
    ext::HostServices services;
    services.log = hostLog;
    services.importModel = hostImport;
    services.freeModelResult = hostFree;
    services.progress = hostProgress;
    reg.setHostServices(services);
    reg.setEngineVersion("0.8.1");
    reg.setExecutableDir(exeDir);

    // ---- scan ----
    const usize added = reg.scan(rootA);
    check(added == 13, "scan lists every directory but the hidden one (" + std::to_string(added) + ")");
    reg.clear();
    check(reg.extensions().empty(), "clear forgets every entry");
    check(reg.scanDefault() == 13, "scanDefault reads <executable dir>/Extensions");
    const usize second = reg.scan(rootB);
    check(second == 2, "a second root adds its own entries (" + std::to_string(second) + ")");

    const ext::Extension* g = reg.find("GoodExt");
    check(g && g->ready() && g->reason.empty(), "GoodExt is Ready");
    if (g) {
        check(g->displayName == "GoodExt Display" && g->version == "1.2.3" && g->license == "MIT" && g->abi == 1, "its manifest fields are read");
        check(g->source == "https://example.invalid/GoodExt" && g->engineRange == engine && g->nativeName == "TestExt", "source, engine range and native name");
        check(g->managedRuntime.size() == 1 && g->managedRuntime[0] == "Good.dll" && g->actorBase == "Good.Node", "managed runtime and actor base");
        check(g->commands.size() == 5 && g->commands[0].name == "echo" && g->commands[0].summary == "print the arguments" && g->commands[0].usage == "<word>..." &&
                  g->commands[1].summary.empty() && g->commands[1].usage.empty(),
              "commands, summaries and usage");
        check(g->importers.size() == 1 && g->importers[0].menu == "Import Good..." && g->importers[0].command == "echo" && g->importers[0].input == "folder" &&
                  g->importers[0].detect == "good.project" && g->importers[0].opensResult,
              "importers");
        check(g->runtimeFiles.size() == 3, "runtime files");
        check(contains(g->nativePath, std::string("native/") + ext::platformDirName()) && fileExists(g->nativePath), "its native path is this platform's library");
        check(g->dirName == "GoodExt" && contains(g->dir, "GoodExt"), "directory name and path");
    }

    auto refused = [&](const char* name, const char* reasonPart) {
        const ext::Extension* e = reg.find(name);
        check(e && !e->ready() && contains(e->reason, reasonPart), std::string(name) + " is refused: " + (e ? e->reason : "(missing)"));
    };
    refused("OldAbi", "ABI 2");
    refused("FutureExt", ">=9.0.0");
    refused("NoNative", "missing file native/");
    refused("NoManaged", "missing file managed/Gone.dll");
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
    check(hasLine("exe=" + exeDir) && hasLine("ext=" + rootA + "/GoodExt") && hasLine("engine=0.8.1") && hasLine("abi=1 size=" + std::to_string(sizeof(AverExtHost))),
          "the extension is told the executable and extension directories, the engine version and the ABI");
    check(g_progress.size() == 1 && g_progress[0].first == 0.5 && g_progress[0].second == "halfway", "progress reaches the host");

    // ---- model import callback ----
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
        struct Cap { std::vector<std::string> lines; } cap;
        setLogSink([](void* ctx, LogLevel, std::string_view m) { static_cast<Cap*>(ctx)->lines.push_back(std::string(m)); }, &cap);
        code = -1;
        plain.run("model", 5, modelArgs, code, &why);
        const char* infoArgs[] = {"echo", "x"};
        plain.run("echo", 2, infoArgs, code, &why);
        setLogSink(nullptr, nullptr);
        bool refusedImport = false, engineLog = false;
        for (const std::string& l : cap.lines) {
            if (contains(l, "ok=0 error=the host offers no model importer")) refusedImport = true;
            if (l == "x|") engineLog = true;
        }
        check(code == 1 && plain.isLoaded("GoodExt"), "the plain registry loaded its own copy (rootB's, a separate library) and ran echo");
        check(refusedImport, "the default importModel reports that the host offers none");
        check(engineLog, "the default log goes to the engine log");
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
