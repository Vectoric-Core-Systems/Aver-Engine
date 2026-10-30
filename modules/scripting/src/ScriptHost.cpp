// In-process CoreCLR host: loads hostfxr, binds the managed bridge, drives it.
#include "aver/scripting/ScriptHost.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/scripting/scripting_abi.h"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/extensions/Extensions.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <format>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aver::scripting {

namespace {

// Declared here instead of included: must match nethost.h / hostfxr.h / coreclr_delegates.h.
//
// hostfxr's char_t is the platform's native path character: UTF-16 wchar_t on Windows, UTF-8 char
// everywhere else. AVER_HOST_STR spells a literal in it, and hstring is a string of it, so the code
// below is one text for both.
#if defined(_WIN32)
using char_t = wchar_t;
#define AVER_HOST_STR(s) L##s
constexpr const char_t* kHostSep = L"\\";
#else
using char_t = char;
#define AVER_HOST_STR(s) s
constexpr const char_t* kHostSep = "/";
#endif
using hstring = std::basic_string<char_t>;
using hostfxr_handle = void*;

// Mirrors hostfxr.h's hostfxr_initialize_parameters field for field.
struct hostfxr_initialize_parameters {
    size_t size;
    const char_t* host_path;
    const char_t* dotnet_root;
};

using get_hostfxr_path_fn = int32_t(AVER_SCRIPT_CALL*)(char_t* buffer, size_t* bufferSize, const void* parameters);
using hostfxr_initialize_for_runtime_config_fn =
    int32_t(AVER_SCRIPT_CALL*)(const char_t* runtimeConfigPath, const hostfxr_initialize_parameters* params,
                      hostfxr_handle* hostContext);
using hostfxr_get_runtime_delegate_fn = int32_t(AVER_SCRIPT_CALL*)(hostfxr_handle ctx, int32_t type, void** del);
using hostfxr_close_fn = int32_t(AVER_SCRIPT_CALL*)(hostfxr_handle ctx);
using load_assembly_and_get_function_pointer_fn =
    int32_t(AVER_SCRIPT_CALL*)(const char_t* assemblyPath, const char_t* typeName, const char_t* methodName,
                      const char_t* delegateTypeName, void* reserved, void** del);

// coreclr_delegates.h: hdt_load_assembly_and_get_function_pointer.
constexpr int32_t kHdtLoadAssemblyAndGetFunctionPointer = 5;
// coreclr_delegates.h: the sentinel meaning the method carries [UnmanagedCallersOnly].
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

// True for the three return codes hostfxr counts as success.
constexpr bool hostfxrOk(int32_t rc) { return rc >= 0 && rc <= 2; }

// The bridge's entry points, all [UnmanagedCallersOnly] on the managed side.
using bootstrap_fn     = int32_t(AVER_SCRIPT_CALL*)(const AverScriptHostApi* api);
using loadScripts_fn   = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Dir);
using unloadScripts_fn = int32_t(AVER_SCRIPT_CALL*)(void);
using update_fn        = void(AVER_SCRIPT_CALL*)(float dt);
using shutdown_fn      = void(AVER_SCRIPT_CALL*)(void);
using hud_count_fn     = int32_t(AVER_SCRIPT_CALL*)(void);
using hud_name_fn      = int32_t(AVER_SCRIPT_CALL*)(int32_t, char*, int32_t);
using hud_draw_fn      = int32_t(AVER_SCRIPT_CALL*)(int32_t, float);
using graph_load_fn    = int32_t(AVER_SCRIPT_CALL*)(int32_t entity, const char* utf8Path);
using graph_tick_fn    = void(AVER_SCRIPT_CALL*)(int32_t entity, float timeSeconds);
using graph_unload_fn  = void(AVER_SCRIPT_CALL*)(int32_t entity);
using graph_fire_fn    = int32_t(AVER_SCRIPT_CALL*)(int32_t entity, const char* utf8EventName);
using graph_validate_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Text, char* outErr, int32_t cap);
using graph_set_hits_fn = int32_t(AVER_SCRIPT_CALL*)(int32_t on);
using graph_get_hits_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8GraphName, char* out, int32_t cap, float maxAgeSeconds);
using graph_diagnose_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Text, char* out, int32_t cap);
using graph_debug_set_enabled_fn = int32_t(AVER_SCRIPT_CALL*)(int32_t on);
using graph_debug_set_breakpoints_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Lines);
using graph_debug_take_break_fn = int32_t(AVER_SCRIPT_CALL*)(char* out, int32_t cap);
using graph_debug_get_values_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Graph, char* out, int32_t cap);
using declare_graph_classes_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8ContentDir);
using tick_graph_class_instances_fn = void(AVER_SCRIPT_CALL*)(float dt);
using configure_input_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8SchemePath, const char* utf8SettingsPath);
using editable_list_fn = int32_t(AVER_SCRIPT_CALL*)(int32_t entity, char* out, int32_t cap);
using editable_set_fn = int32_t(AVER_SCRIPT_CALL*)(int32_t entity, const char* utf8Name, const char* utf8Value);
using load_extensions_fn = int32_t(AVER_SCRIPT_CALL*)(const char* utf8Spec);

// What the bridge's LoadExtensions takes: one line per Ready extension that has a managed half, fields separated by TAB: its name, its actor
// base class, then the full path of each managed.runtime assembly. A Refused extension logs its reason (one line) and is left out.
std::string managedExtensionSpec(const std::string& root) {
    ext::Registry registry;
    if (root.empty()) registry.scanDefault();
    else registry.scan(root);
    std::string spec;
    for (const ext::Extension& e : registry.extensions()) {
        if (!e.ready()) {
            AVER_WARN("[Scripting] extension {} is not loaded: {}", e.name, e.reason);
            continue;
        }
        if (e.managedRuntime.empty()) continue;
        spec += e.name + '\t' + e.actorBase;
        for (const std::string& m : e.managedRuntime) spec += '\t' + e.dir + "/managed/" + m;
        spec += '\n';
    }
    return spec;
}

// UTF-8 to hostfxr's char_t, and back. The identity everywhere but Windows.
hstring toHost(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    hstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
#else
    return s;
#endif
}
std::string fromHost(const char_t* s) {
#if defined(_WIN32)
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
#else
    return s;
#endif
}

// True when the path names an existing file rather than a directory.
bool fileThere(const hstring& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

// nethost, which only finds hostfxr: beside the executable first (the scripting module's CMake stages
// it there from the SDK's app-host pack), then wherever the platform's loader looks for a bare name.
void* loadNethost() {
#if defined(_WIN32)
    const char* leaf = "nethost.dll";
#elif defined(__APPLE__)
    const char* leaf = "libnethost.dylib";
#else
    const char* leaf = "libnethost.so";
#endif
    const std::string beside = aver::executableDir() + AVER_PATH_SEP + leaf;   // LoadLibraryW wants backslashes
    const char* names[] = {beside.c_str(), leaf};
    return aver::loadFirstLibrary(names, 2);
}

// Writes a managed log line into the engine log. Handed to the bridge at bootstrap.
void AVER_SCRIPT_CALL managedLog(int32_t level, const char* utf8Message) {
    const char* m = utf8Message ? utf8Message : "";
    // EVERY level spelled out, and the default kept as Info for genuinely unknown codes only. The
    // default is a real hazard here rather than a tidy fallback: it does not fail, it DOWNGRADES --
    // a script calling Log.Critical through an ABI code this switch did not know would have arrived
    // in the engine log as an ordinary Info line, which is worse than a crash because it looks like
    // it worked. AVER_FATAL is deliberately last and terminates: managed code asking for Fatal gets
    // Fatal, including the crash report.
    switch (level) {
        case AVER_SCRIPT_LOG_TRACE:    AVER_TRACE("{}", m);    break;
        case AVER_SCRIPT_LOG_WARN:     AVER_WARN("{}", m);     break;
        case AVER_SCRIPT_LOG_ERROR:    AVER_ERROR("{}", m);    break;
        case AVER_SCRIPT_LOG_CRITICAL: AVER_CRITICAL("{}", m); break;
        case AVER_SCRIPT_LOG_FATAL:    AVER_FATAL("{}", m);           // does not return
        default:                       AVER_INFO("{}", m);     break;
    }
}

} // namespace

// The loaded runtime modules, the host context and the bound bridge entry points.
struct ScriptHost::Impl {
    void* nethost = nullptr;
    void* hostfxr = nullptr;
    hostfxr_handle ctx = nullptr;
    hostfxr_close_fn close = nullptr;
    loadScripts_fn load = nullptr;
    unloadScripts_fn unload = nullptr;
    update_fn update = nullptr;
    shutdown_fn shutdown = nullptr;
    hud_count_fn hudCount = nullptr;
    hud_name_fn  hudName  = nullptr;
    hud_draw_fn  hudDraw  = nullptr;
    graph_load_fn   graphLoad   = nullptr;
    graph_tick_fn   graphTick   = nullptr;
    graph_unload_fn graphUnload = nullptr;
    graph_fire_fn   graphFire   = nullptr;
    graph_validate_fn graphValidate = nullptr;
    graph_set_hits_fn graphSetHits = nullptr;
    graph_get_hits_fn graphGetHits = nullptr;
    graph_diagnose_fn graphDiagnose = nullptr;
    editable_list_fn editableList = nullptr;
    editable_set_fn editableSet = nullptr;
    graph_debug_set_enabled_fn graphDebugSetEnabled = nullptr;
    graph_debug_set_breakpoints_fn graphDebugSetBreakpoints = nullptr;
    graph_debug_take_break_fn graphDebugTakeBreak = nullptr;
    graph_debug_get_values_fn graphDebugGetValues = nullptr;
    declare_graph_classes_fn      declareGraphClasses    = nullptr;
    tick_graph_class_instances_fn tickGraphClassInstances = nullptr;
    configure_input_fn configureInput = nullptr;
    load_extensions_fn loadExtensions = nullptr;
};

ScriptHost::ScriptHost() = default;

// Shuts the runtime down and releases the implementation.
ScriptHost::~ScriptHost() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

// Starts the CLR, binds the bridge and loads the script directory. False, with a reason, on decline.
bool ScriptHost::init(const HostDesc& desc) {
    if (ready_) return true;
    if (!impl_) impl_ = new Impl();

    const auto decline = [this](std::string why) {
        declineReason_ = std::move(why);
        AVER_WARN("[Scripting] init declined: {}", declineReason_);
        return false;
    };

    const hstring bridgeDir = toHost(desc.bridgeDir);
    const hstring bridgeDll = bridgeDir + kHostSep + AVER_HOST_STR("Aver.Scripting.Bridge.dll");
    const hstring bridgeCfg = bridgeDir + kHostSep + AVER_HOST_STR("Aver.Scripting.Bridge.runtimeconfig.json");

    if (!fileThere(bridgeDll) || !fileThere(bridgeCfg))
        return decline("the managed bridge was not staged next to the executable "
                       "(Aver.Scripting.Bridge.dll / .runtimeconfig.json) — build with the .NET SDK present");

    // Loaded dynamically, and by bare name: a machine with no .NET must still start the editor.
    impl_->nethost = loadNethost();
    if (!impl_->nethost)
        return decline("nethost could not be loaded (" + aver::libraryError() + ") — the .NET runtime is unavailable");

    auto getHostfxrPath = reinterpret_cast<get_hostfxr_path_fn>(aver::librarySymbol(impl_->nethost, "get_hostfxr_path"));
    if (!getHostfxrPath)
        return decline("nethost exports no get_hostfxr_path");

    char_t fxrPath[4096] = {};
    size_t fxrLen = sizeof(fxrPath) / sizeof(fxrPath[0]);
    if (getHostfxrPath(fxrPath, &fxrLen, nullptr) != 0)
        return decline("get_hostfxr_path found no .NET runtime on this machine");

    impl_->hostfxr = aver::loadLibrary(fromHost(fxrPath).c_str());
    if (!impl_->hostfxr)
        return decline("hostfxr could not be loaded (" + aver::libraryError() + ")");

    auto fxrInit = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        aver::librarySymbol(impl_->hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto fxrDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        aver::librarySymbol(impl_->hostfxr, "hostfxr_get_runtime_delegate"));
    impl_->close = reinterpret_cast<hostfxr_close_fn>(aver::librarySymbol(impl_->hostfxr, "hostfxr_close"));
    if (!fxrInit || !fxrDelegate || !impl_->close)
        return decline("hostfxr is missing an expected export");

    int32_t rc = fxrInit(bridgeCfg.c_str(), nullptr, &impl_->ctx);
    if (!hostfxrOk(rc) || !impl_->ctx) {
        impl_->ctx = nullptr;
        return decline(std::format("hostfxr_initialize_for_runtime_config failed (0x{:08X}) — the "
                                   "framework the bridge targets is not installed",
                                   static_cast<uint32_t>(rc)));
    }

    void* raw = nullptr;
    rc = fxrDelegate(impl_->ctx, kHdtLoadAssemblyAndGetFunctionPointer, &raw);
    if (rc != 0 || !raw)
        return decline(std::format("hostfxr_get_runtime_delegate failed (0x{:08X})", static_cast<uint32_t>(rc)));

    auto loadFn = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(raw);
    const char_t* kType = AVER_HOST_STR("Aver.Scripting.Bridge.HostBridge, Aver.Scripting.Bridge");

    const auto bind = [&](const char_t* method, void** out) {
        return loadFn(bridgeDll.c_str(), kType, method, kUnmanagedCallersOnly, nullptr, out) == 0 && *out;
    };

    bootstrap_fn bootstrap = nullptr;
    if (!bind(AVER_HOST_STR("Bootstrap"), reinterpret_cast<void**>(&bootstrap)) ||
        !bind(AVER_HOST_STR("LoadScripts"), reinterpret_cast<void**>(&impl_->load)) ||
        !bind(AVER_HOST_STR("UnloadScripts"), reinterpret_cast<void**>(&impl_->unload)) ||
        !bind(AVER_HOST_STR("Update"), reinterpret_cast<void**>(&impl_->update)) ||
        !bind(AVER_HOST_STR("Shutdown"), reinterpret_cast<void**>(&impl_->shutdown))) {
        impl_->load = nullptr;
        impl_->unload = nullptr;
        impl_->update = nullptr;
        impl_->shutdown = nullptr;
        return decline("the staged Aver.Scripting.Bridge.dll does not export the expected entry "
                       "points — it is from a different engine build");
    }

    // The HUD three are optional: a missing one costs the preview, not the host.
    if (!bind(AVER_HOST_STR("HudCount"), reinterpret_cast<void**>(&impl_->hudCount)) ||
        !bind(AVER_HOST_STR("HudName"),  reinterpret_cast<void**>(&impl_->hudName))  ||
        !bind(AVER_HOST_STR("HudDraw"),  reinterpret_cast<void**>(&impl_->hudDraw))) {
        impl_->hudCount = nullptr;
        impl_->hudName  = nullptr;
        impl_->hudDraw  = nullptr;
        AVER_WARN("[Scripting] the bridge exports no HUD entry points; HUD preview is unavailable");
    }

    // Graph hosting is optional too, same reasoning as the HUD three: a bridge built before
    // GraphLoad/GraphTick/GraphUnload existed still boots, and graphAvailable() just reports false.
    if (!bind(AVER_HOST_STR("GraphLoad"), reinterpret_cast<void**>(&impl_->graphLoad)) ||
        !bind(AVER_HOST_STR("GraphTick"), reinterpret_cast<void**>(&impl_->graphTick)) ||
        !bind(AVER_HOST_STR("GraphUnload"), reinterpret_cast<void**>(&impl_->graphUnload))) {
        impl_->graphLoad = nullptr;
        impl_->graphTick = nullptr;
        impl_->graphUnload = nullptr;
        AVER_WARN("[Scripting] the bridge exports no Graph entry points; graph hosting is unavailable");
    }

    // GraphFire is optional SEPARATELY from the three above, not folded in with them, because it
    // arrived later: a bridge that predates it hosts and ticks graphs correctly and is only unable
    // to be fired at. Folding it into the block above would turn a bridge missing one new export
    // into a bridge with no graph hosting at all.
    if (!bind(AVER_HOST_STR("GraphFire"), reinterpret_cast<void**>(&impl_->graphFire))) {
        impl_->graphFire = nullptr;
        AVER_WARN("[Scripting] the bridge exports no GraphFire; animation notifies will not reach graphs");
    }

    // GraphValidate is optional and SEPARATE again, for GraphFire's reason: a bridge that predates
    // it hosts, ticks and fires graphs correctly and is only unable to check one. The editor asks
    // graphValidateAvailable() and says so rather than refusing to save.
    if (!bind(AVER_HOST_STR("GraphValidate"), reinterpret_cast<void**>(&impl_->graphValidate))) {
        impl_->graphValidate = nullptr;
        AVER_WARN("[Scripting] the bridge exports no GraphValidate; the graph editor cannot check a "
                  "graph before saving it");
    }

    // The node-hit pair, optional together and separate from GraphValidate -- same graceful-degradation
    // rule every group above follows. A bridge predating them hosts and validates graphs perfectly
    // well and is only unable to say which nodes are running.
    if (!bind(AVER_HOST_STR("GraphSetHitRecording"), reinterpret_cast<void**>(&impl_->graphSetHits)) ||
        !bind(AVER_HOST_STR("GraphGetHits"), reinterpret_cast<void**>(&impl_->graphGetHits))) {
        impl_->graphSetHits = nullptr;
        impl_->graphGetHits = nullptr;
        AVER_WARN("[Scripting] the bridge exports no node-hit entry points; the graph editor cannot "
                  "highlight which nodes are executing");
    }

    // EditableList/EditableSet (the details panel's per-instance [Editable] values) are optional together.
    if (!bind(AVER_HOST_STR("EditableList"), reinterpret_cast<void**>(&impl_->editableList)) ||
        !bind(AVER_HOST_STR("EditableSet"), reinterpret_cast<void**>(&impl_->editableSet))) {
        impl_->editableList = nullptr;
        impl_->editableSet = nullptr;
    }
    // GraphDiagnose (the Problems panel) is optional alone: without it the editor keeps validate's first error.
    if (!bind(AVER_HOST_STR("GraphDiagnose"), reinterpret_cast<void**>(&impl_->graphDiagnose))) {
        impl_->graphDiagnose = nullptr;
        AVER_WARN("[Scripting] the bridge exports no GraphDiagnose; the graph editor lists only the first error");
    }

    // The debugger's four, optional together: a bridge predating them hosts graphs and just cannot debug one.
    if (!bind(AVER_HOST_STR("GraphDebugSetEnabled"), reinterpret_cast<void**>(&impl_->graphDebugSetEnabled)) ||
        !bind(AVER_HOST_STR("GraphDebugSetBreakpoints"), reinterpret_cast<void**>(&impl_->graphDebugSetBreakpoints)) ||
        !bind(AVER_HOST_STR("GraphDebugTakeBreak"), reinterpret_cast<void**>(&impl_->graphDebugTakeBreak)) ||
        !bind(AVER_HOST_STR("GraphDebugGetValues"), reinterpret_cast<void**>(&impl_->graphDebugGetValues))) {
        impl_->graphDebugSetEnabled = nullptr;
        impl_->graphDebugSetBreakpoints = nullptr;
        impl_->graphDebugTakeBreak = nullptr;
        impl_->graphDebugGetValues = nullptr;
        AVER_WARN("[Scripting] the bridge exports no graph debugger entry points; breakpoints are unavailable");
    }

    // GRAPH-AS-CLASS is optional too, same reasoning: a bridge built before DeclareGraphClasses/
    // GraphTickBoundInstances existed still boots, and graphClassesAvailable() just reports false.
    if (!bind(AVER_HOST_STR("DeclareGraphClasses"), reinterpret_cast<void**>(&impl_->declareGraphClasses)) ||
        !bind(AVER_HOST_STR("GraphTickBoundInstances"), reinterpret_cast<void**>(&impl_->tickGraphClassInstances))) {
        impl_->declareGraphClasses = nullptr;
        impl_->tickGraphClassInstances = nullptr;
        AVER_WARN("[Scripting] the bridge exports no graph-class entry points; graph-as-class is unavailable");
    }

    // INPUT SCHEME is optional too, same reasoning: a bridge built before ConfigureInput existed
    // still boots, and configureInput() reports -2 (see the header) rather than pretending success.
    if (!bind(AVER_HOST_STR("ConfigureInput"), reinterpret_cast<void**>(&impl_->configureInput))) {
        impl_->configureInput = nullptr;
        AVER_WARN("[Scripting] the bridge exports no ConfigureInput; rebindable input schemes are unavailable");
    }

    // LoadExtensions (the installed extensions' managed halves) is optional too: a bridge built before it existed boots without them.
    if (!bind(AVER_HOST_STR("LoadExtensions"), reinterpret_cast<void**>(&impl_->loadExtensions))) {
        impl_->loadExtensions = nullptr;
        AVER_WARN("[Scripting] the bridge exports no LoadExtensions; extension actor classes are unavailable");
    }

    AverScriptHostApi api{};
    api.structBytes = static_cast<int32_t>(sizeof(AverScriptHostApi));
    api.contractVersion = AVER_SCRIPTING_CONTRACT_VERSION;
    api.log = &managedLog;

    const int32_t brc = bootstrap(&api);
    if (brc != AVER_SCRIPT_OK) {
        impl_->load = nullptr;
        impl_->unload = nullptr;
        impl_->update = nullptr;
        impl_->shutdown = nullptr;
        if (brc == AVER_SCRIPT_ERR_CONTRACT)
            return decline(std::format("the staged Aver.Scripting.Bridge.dll speaks a different host "
                                       "contract than this build (host v{}) — rebuild the managed side",
                                       AVER_SCRIPTING_CONTRACT_VERSION));
        return decline(std::format("the managed bridge failed to bootstrap (code {})", brc));
    }

    ready_ = true;
    declineReason_.clear();

    // The installed extensions' managed halves, before any project script so their types are declared and shared first.
    if (impl_->loadExtensions) {
        const std::string spec = managedExtensionSpec(desc.extensionsDir);
        if (!spec.empty()) impl_->loadExtensions(spec.c_str());
    }

    behaviours_ = desc.scriptsDir.empty() ? 0 : impl_->load(desc.scriptsDir.c_str());
    if (behaviours_ < 0) behaviours_ = 0;

    AVER_INFO("[Scripting] .NET runtime hosted in-process; {} behaviour(s) live", behaviours_);
    return true;
}

// Loads a directory of script assemblies. Returns the live behaviour count, or -1 when not ready.
i32 ScriptHost::loadScripts(const std::string& dir) {
    if (!ready_ || !impl_ || !impl_->load) return -1;
    const int32_t n = impl_->load(dir.c_str());
    behaviours_ = n < 0 ? 0 : n;
    return behaviours_;
}

// Drains OnShutdown and unloads the collectible context. True when it was fully collected.
bool ScriptHost::unloadScripts() {
    if (!ready_ || !impl_ || !impl_->unload) return false;
    const int32_t collected = impl_->unload();
    behaviours_ = 0;
    return collected != 0;
}

// How many [AverHud] classes the bridge found. Zero when unavailable.
i32 ScriptHost::hudCount() const {
    return (ready_ && impl_ && impl_->hudCount) ? impl_->hudCount() : 0;
}

// The HUD's display name, or empty.
std::string ScriptHost::hudName(i32 index) const {
    if (!ready_ || !impl_ || !impl_->hudName) return {};
    char buf[128] = {};
    const int32_t n = impl_->hudName(index, buf, static_cast<int32_t>(sizeof buf));
    return n > 0 ? std::string(buf, static_cast<usize>(n)) : std::string();
}

// Calls the HUD's Draw(dt). True if it ran.
bool ScriptHost::hudDraw(i32 index, f32 dt) {
    return (ready_ && impl_ && impl_->hudDraw) && impl_->hudDraw(index, dt) == 1;
}

// Drives OnUpdate on every live behaviour.
void ScriptHost::update(f32 dt) {
    if (!ready_ || !impl_ || !impl_->update) return;
    impl_->update(dt);
}

// Whether the staged bridge exports GraphLoad/GraphTick -- see the optional-bind block in init().
bool ScriptHost::graphAvailable() const {
    return ready_ && impl_ && impl_->graphLoad && impl_->graphTick;
}

// Loads and compiles an .ocgraph, binding it to `entity`. False when unavailable or on any failure
// GraphHost.Load reports (bad path, parse error, compile error, unsupported PARAM shape).
bool ScriptHost::graphLoad(i32 entity, const std::string& path) {
    if (!graphAvailable()) return false;
    return impl_->graphLoad(entity, path.c_str()) != 0;
}

// Ticks the graph bound to `entity`. A no-op for an entity with none, or when unavailable.
void ScriptHost::graphTick(i32 entity, f32 timeSeconds) {
    if (!graphAvailable()) return;
    impl_->graphTick(entity, timeSeconds);
}

// Drops the graph bound to `entity`, if any.
void ScriptHost::graphUnload(i32 entity) {
    if (!ready_ || !impl_ || !impl_->graphUnload) return;
    impl_->graphUnload(entity);
}

// Whether the staged bridge exports GraphFire -- see its own optional-bind block in init().
bool ScriptHost::graphFireAvailable() const {
    return ready_ && impl_ && impl_->graphFire;
}

// Raises `eventName` on the graph bound to `entity`. False when unavailable, when the entity has
// no graph, or when the graph declares no such event.
bool ScriptHost::graphFire(i32 entity, const std::string& eventName) {
    if (!graphFireAvailable() || eventName.empty()) return false;
    return impl_->graphFire(entity, eventName.c_str()) != 0;
}

bool ScriptHost::graphValidateAvailable() const {
    return ready_ && impl_ && impl_->graphValidate;
}

// Validates .ocgraph TEXT. True when valid, or when validation is unavailable -- see the header for
// why an absent bridge must not read as an invalid graph.
bool ScriptHost::graphValidate(const std::string& text, std::string& err) const {
    err.clear();
    if (!graphValidateAvailable()) return true;
    // A FIXED BUFFER, not a two-call size-then-fill: every message this can return is one sentence
    // written by hand in Graph.Validate/OcGraphParser, and the managed side truncates to fit rather
    // than failing. 2 KB is roughly four times the longest of them.
    char buf[2048] = {};
    const int32_t rc = impl_->graphValidate(text.c_str(), buf, static_cast<int32_t>(sizeof buf));
    if (rc == 0) return true;
    err = buf[0] ? buf : "the graph is not valid";
    // rc < 0 is a bad argument (a null buffer, or text that did not marshal) rather than an invalid
    // graph. Reported as invalid anyway, because the caller wanted to know whether it is safe to
    // save and the honest answer is "this could not be checked".
    return false;
}

bool ScriptHost::graphHitsAvailable() const {
    return ready_ && impl_ && impl_->graphSetHits && impl_->graphGetHits;
}

void ScriptHost::graphSetHitRecording(bool on) const {
    if (!graphHitsAvailable()) return;
    impl_->graphSetHits(on ? 1 : 0);
}

void ScriptHost::graphNodeHits(const std::string& graphName, f32 maxAgeSeconds,
                               std::vector<std::pair<std::string, f32>>& out) const {
    out.clear();
    if (!graphHitsAvailable() || graphName.empty()) return;
    // A FIXED BUFFER, and the managed side truncates at a separator rather than mid-entry, so a graph
    // with more recent hits than fit loses whole entries instead of producing a node id nothing on the
    // canvas matches. 8 KB is a few hundred nodes, well past any graph a person is reading.
    char buf[8192] = {};
    const int32_t n = impl_->graphGetHits(graphName.c_str(), buf, static_cast<int32_t>(sizeof buf), maxAgeSeconds);
    if (n <= 0) return;

    // "nodeId:age;nodeId:age". Parsed here rather than handed over as a string so every caller does
    // not re-derive the format.
    std::string_view all(buf, static_cast<usize>(n));
    usize pos = 0;
    while (pos < all.size()) {
        const usize semi = all.find(';', pos);
        const std::string_view entry = all.substr(pos, semi == std::string_view::npos ? all.size() - pos : semi - pos);
        const usize colon = entry.rfind(':');
        if (colon != std::string_view::npos && colon > 0) {
            f32 age = 0.0f;
            // A node id can itself contain a colon in principle, so the LAST one separates the age.
            const std::string ageText(entry.substr(colon + 1));
            try { age = std::stof(ageText); } catch (...) { age = 0.0f; }
            out.emplace_back(std::string(entry.substr(0, colon)), age);
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
}

bool ScriptHost::graphDiagnoseAvailable() const {
    return ready_ && impl_ && impl_->graphDiagnose;
}

// Every problem in .ocgraph text. A 64 KB buffer on the heap: the wire is one line per problem and the
// managed side cuts at a whole line, so a graph with hundreds of problems loses the tail, not a half line.
bool ScriptHost::graphDiagnose(const std::string& text, std::string& wire) const {
    wire.clear();
    if (!graphDiagnoseAvailable()) return false;
    std::vector<char> buf(64 * 1024, 0);
    const int32_t n = impl_->graphDiagnose(text.c_str(), buf.data(), static_cast<int32_t>(buf.size()));
    if (n < 0) return false;
    wire.assign(buf.data(), static_cast<usize>(n));
    return true;
}

bool ScriptHost::editablesAvailable() const {
    return ready_ && impl_ && impl_->editableList && impl_->editableSet;
}

std::string ScriptHost::editableList(i32 entity) const {
    if (!editablesAvailable()) return {};
    std::vector<char> buf(16 * 1024, 0);
    const int32_t n = impl_->editableList(entity, buf.data(), static_cast<int32_t>(buf.size()));
    return n > 0 ? std::string(buf.data(), static_cast<usize>(n)) : std::string();
}

bool ScriptHost::editableSet(i32 entity, const std::string& name, const std::string& value) const {
    return editablesAvailable() && impl_->editableSet(entity, name.c_str(), value.c_str()) == 1;
}

bool ScriptHost::graphDebugAvailable() const {
    return ready_ && impl_ && impl_->graphDebugSetEnabled && impl_->graphDebugSetBreakpoints &&
           impl_->graphDebugTakeBreak && impl_->graphDebugGetValues;
}

void ScriptHost::graphDebugSetEnabled(bool on) const {
    if (!graphDebugAvailable()) return;
    impl_->graphDebugSetEnabled(on ? 1 : 0);
}

void ScriptHost::graphDebugSetBreakpoints(const std::string& lines) const {
    if (!graphDebugAvailable()) return;
    impl_->graphDebugSetBreakpoints(lines.c_str());
}

bool ScriptHost::graphDebugTakeBreak(std::string& graphAndNode) const {
    graphAndNode.clear();
    if (!graphDebugAvailable()) return false;
    char buf[1024] = {};
    const int32_t n = impl_->graphDebugTakeBreak(buf, static_cast<int32_t>(sizeof buf));
    if (n <= 0) return false;
    graphAndNode.assign(buf, static_cast<usize>(n));
    return true;
}

std::string ScriptHost::graphDebugValues(const std::string& graphName) const {
    if (!graphDebugAvailable() || graphName.empty()) return {};
    // 32 KB: a node.pin=value; entry is a few dozen bytes, so this is several hundred wires; the managed
    // side cuts at an entry boundary when a graph has more.
    std::vector<char> buf(32 * 1024, 0);
    const int32_t n = impl_->graphDebugGetValues(graphName.c_str(), buf.data(), static_cast<int32_t>(buf.size()));
    return n > 0 ? std::string(buf.data(), static_cast<usize>(n)) : std::string();
}

// Whether the staged bridge exports the graph-class entry points -- see the optional-bind block in init().
bool ScriptHost::graphClassesAvailable() const {
    return ready_ && impl_ && impl_->declareGraphClasses && impl_->tickGraphClassInstances;
}

// Declares one framework class per CLASS-bearing .ocgraph under `contentDir`. 0 when unavailable or
// the directory has none.
i32 ScriptHost::declareGraphClasses(const std::string& contentDir) {
    if (!graphClassesAvailable()) return 0;
    return impl_->declareGraphClasses(contentDir.c_str());
}

// Ticks every live graph-class instance once. A no-op when unavailable.
void ScriptHost::tickGraphClassInstances(f32 dt) {
    if (!graphClassesAvailable()) return;
    impl_->tickGraphClassInstances(dt);
}

// Opens the settings store and loads (or, for an empty `schemePath`, unloads) the project's input
// scheme. See the header for the full return convention; -2 means the staged bridge predates this
// export rather than that anything failed to parse.
i32 ScriptHost::configureInput(const std::string& schemePath, const std::string& settingsPath) {
    if (!ready_ || !impl_ || !impl_->configureInput) return -2;
    return impl_->configureInput(schemePath.c_str(), settingsPath.c_str());
}

// Drains the behaviours, unloads the context and closes the host context. Safe twice.
void ScriptHost::shutdown() {
    if (!impl_) return;
    if (ready_ && impl_->shutdown) impl_->shutdown();
    ready_ = false;
    behaviours_ = 0;
    impl_->load = nullptr;
    impl_->unload = nullptr;
    impl_->update = nullptr;
    impl_->shutdown = nullptr;
    if (impl_->ctx && impl_->close) impl_->close(impl_->ctx);
    impl_->ctx = nullptr;
    // hostfxr is never freed: CoreCLR stays loaded for the life of the process.
    if (impl_->nethost) { aver::freeLibrary(impl_->nethost); impl_->nethost = nullptr; }
}

} // namespace aver::scripting
