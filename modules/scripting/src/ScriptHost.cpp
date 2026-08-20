// In-process CoreCLR host: loads hostfxr, binds the managed bridge, drives it.
#include "aver/scripting/ScriptHost.hpp"
#include "aver/scripting/scripting_abi.h"

#include "aver/core/Log.hpp"

#include <format>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aver::scripting {

#if defined(_WIN32)
namespace {

// Declared here instead of included: must match nethost.h / hostfxr.h / coreclr_delegates.h.
using char_t = wchar_t;
using hostfxr_handle = void*;

// Mirrors hostfxr.h's hostfxr_initialize_parameters field for field.
struct hostfxr_initialize_parameters {
    size_t size;
    const char_t* host_path;
    const char_t* dotnet_root;
};

using get_hostfxr_path_fn = int32_t(__cdecl*)(char_t* buffer, size_t* bufferSize, const void* parameters);
using hostfxr_initialize_for_runtime_config_fn =
    int32_t(__cdecl*)(const char_t* runtimeConfigPath, const hostfxr_initialize_parameters* params,
                      hostfxr_handle* hostContext);
using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(hostfxr_handle ctx, int32_t type, void** del);
using hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle ctx);
using load_assembly_and_get_function_pointer_fn =
    int32_t(__cdecl*)(const char_t* assemblyPath, const char_t* typeName, const char_t* methodName,
                      const char_t* delegateTypeName, void* reserved, void** del);

// coreclr_delegates.h: hdt_load_assembly_and_get_function_pointer.
constexpr int32_t kHdtLoadAssemblyAndGetFunctionPointer = 5;
// coreclr_delegates.h: the sentinel meaning the method carries [UnmanagedCallersOnly].
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

// True for the three return codes hostfxr counts as success.
constexpr bool hostfxrOk(int32_t rc) { return rc >= 0 && rc <= 2; }

// The bridge's entry points, all [UnmanagedCallersOnly] on the managed side.
using bootstrap_fn     = int32_t(__cdecl*)(const AverScriptHostApi* api);
using loadScripts_fn   = int32_t(__cdecl*)(const char* utf8Dir);
using unloadScripts_fn = int32_t(__cdecl*)(void);
using update_fn        = void(__cdecl*)(float dt);
using shutdown_fn      = void(__cdecl*)(void);
using hud_count_fn     = int32_t(__cdecl*)(void);
using hud_name_fn      = int32_t(__cdecl*)(int32_t, char*, int32_t);
using hud_draw_fn      = int32_t(__cdecl*)(int32_t, float);
using graph_load_fn    = int32_t(__cdecl*)(int32_t entity, const char* utf8Path);
using graph_tick_fn    = void(__cdecl*)(int32_t entity, float timeSeconds);
using graph_unload_fn  = void(__cdecl*)(int32_t entity);
using graph_fire_fn    = int32_t(__cdecl*)(int32_t entity, const char* utf8EventName);
using declare_graph_classes_fn = int32_t(__cdecl*)(const char* utf8ContentDir);
using tick_graph_class_instances_fn = void(__cdecl*)(float dt);

// Converts UTF-8 to UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// True when the path names an existing file rather than a directory.
bool fileThere(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Writes a managed log line into the engine log. Handed to the bridge at bootstrap.
void __cdecl managedLog(int32_t level, const char* utf8Message) {
    const char* m = utf8Message ? utf8Message : "";
    switch (level) {
        case AVER_SCRIPT_LOG_TRACE: AVER_TRACE("{}", m); break;
        case AVER_SCRIPT_LOG_WARN:  AVER_WARN("{}", m);  break;
        case AVER_SCRIPT_LOG_ERROR: AVER_ERROR("{}", m); break;
        default:                    AVER_INFO("{}", m);  break;
    }
}

} // namespace

// The loaded runtime modules, the host context and the bound bridge entry points.
struct ScriptHost::Impl {
    HMODULE nethost = nullptr;
    HMODULE hostfxr = nullptr;
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
    declare_graph_classes_fn      declareGraphClasses    = nullptr;
    tick_graph_class_instances_fn tickGraphClassInstances = nullptr;
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

    const std::wstring bridgeDirW = widen(desc.bridgeDir);
    const std::wstring bridgeDll = bridgeDirW + L"\\Aver.Scripting.Bridge.dll";
    const std::wstring bridgeCfg = bridgeDirW + L"\\Aver.Scripting.Bridge.runtimeconfig.json";

    if (!fileThere(bridgeDll) || !fileThere(bridgeCfg))
        return decline("the managed bridge was not staged next to the executable "
                       "(Aver.Scripting.Bridge.dll / .runtimeconfig.json) — build with the .NET SDK present");

    // Loaded dynamically, and by bare name: a machine with no .NET must still start the editor.
    impl_->nethost = ::LoadLibraryW(L"nethost.dll");
    if (!impl_->nethost)
        return decline("nethost.dll could not be loaded — the .NET runtime is unavailable");

    auto getHostfxrPath =
        reinterpret_cast<get_hostfxr_path_fn>(reinterpret_cast<void*>(::GetProcAddress(impl_->nethost, "get_hostfxr_path")));
    if (!getHostfxrPath)
        return decline("nethost.dll exports no get_hostfxr_path");

    wchar_t fxrPath[MAX_PATH * 2] = {};
    size_t fxrLen = sizeof(fxrPath) / sizeof(fxrPath[0]);
    if (getHostfxrPath(fxrPath, &fxrLen, nullptr) != 0)
        return decline("get_hostfxr_path found no .NET runtime on this machine");

    impl_->hostfxr = ::LoadLibraryW(fxrPath);
    if (!impl_->hostfxr)
        return decline("hostfxr could not be loaded");

    auto fxrInit = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        reinterpret_cast<void*>(::GetProcAddress(impl_->hostfxr, "hostfxr_initialize_for_runtime_config")));
    auto fxrDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        reinterpret_cast<void*>(::GetProcAddress(impl_->hostfxr, "hostfxr_get_runtime_delegate")));
    impl_->close = reinterpret_cast<hostfxr_close_fn>(
        reinterpret_cast<void*>(::GetProcAddress(impl_->hostfxr, "hostfxr_close")));
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
    const wchar_t* kType = L"Aver.Scripting.Bridge.HostBridge, Aver.Scripting.Bridge";

    const auto bind = [&](const wchar_t* method, void** out) {
        return loadFn(bridgeDll.c_str(), kType, method, kUnmanagedCallersOnly, nullptr, out) == 0 && *out;
    };

    bootstrap_fn bootstrap = nullptr;
    if (!bind(L"Bootstrap", reinterpret_cast<void**>(&bootstrap)) ||
        !bind(L"LoadScripts", reinterpret_cast<void**>(&impl_->load)) ||
        !bind(L"UnloadScripts", reinterpret_cast<void**>(&impl_->unload)) ||
        !bind(L"Update", reinterpret_cast<void**>(&impl_->update)) ||
        !bind(L"Shutdown", reinterpret_cast<void**>(&impl_->shutdown))) {
        impl_->load = nullptr;
        impl_->unload = nullptr;
        impl_->update = nullptr;
        impl_->shutdown = nullptr;
        return decline("the staged Aver.Scripting.Bridge.dll does not export the expected entry "
                       "points — it is from a different engine build");
    }

    // The HUD three are optional: a missing one costs the preview, not the host.
    if (!bind(L"HudCount", reinterpret_cast<void**>(&impl_->hudCount)) ||
        !bind(L"HudName",  reinterpret_cast<void**>(&impl_->hudName))  ||
        !bind(L"HudDraw",  reinterpret_cast<void**>(&impl_->hudDraw))) {
        impl_->hudCount = nullptr;
        impl_->hudName  = nullptr;
        impl_->hudDraw  = nullptr;
        AVER_WARN("[Scripting] the bridge exports no HUD entry points; HUD preview is unavailable");
    }

    // Graph hosting is optional too, same reasoning as the HUD three: a bridge built before
    // GraphLoad/GraphTick/GraphUnload existed still boots, and graphAvailable() just reports false.
    if (!bind(L"GraphLoad", reinterpret_cast<void**>(&impl_->graphLoad)) ||
        !bind(L"GraphTick", reinterpret_cast<void**>(&impl_->graphTick)) ||
        !bind(L"GraphUnload", reinterpret_cast<void**>(&impl_->graphUnload))) {
        impl_->graphLoad = nullptr;
        impl_->graphTick = nullptr;
        impl_->graphUnload = nullptr;
        AVER_WARN("[Scripting] the bridge exports no Graph entry points; graph hosting is unavailable");
    }

    // GraphFire is optional SEPARATELY from the three above, not folded in with them, because it
    // arrived later: a bridge that predates it hosts and ticks graphs correctly and is only unable
    // to be fired at. Folding it into the block above would turn a bridge missing one new export
    // into a bridge with no graph hosting at all.
    if (!bind(L"GraphFire", reinterpret_cast<void**>(&impl_->graphFire))) {
        impl_->graphFire = nullptr;
        AVER_WARN("[Scripting] the bridge exports no GraphFire; animation notifies will not reach graphs");
    }

    // GRAPH-AS-CLASS is optional too, same reasoning: a bridge built before DeclareGraphClasses/
    // GraphTickBoundInstances existed still boots, and graphClassesAvailable() just reports false.
    if (!bind(L"DeclareGraphClasses", reinterpret_cast<void**>(&impl_->declareGraphClasses)) ||
        !bind(L"GraphTickBoundInstances", reinterpret_cast<void**>(&impl_->tickGraphClassInstances))) {
        impl_->declareGraphClasses = nullptr;
        impl_->tickGraphClassInstances = nullptr;
        AVER_WARN("[Scripting] the bridge exports no graph-class entry points; graph-as-class is unavailable");
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
    if (impl_->nethost) { ::FreeLibrary(impl_->nethost); impl_->nethost = nullptr; }
}

#else // !_WIN32

// No CLR host off Win32; every entry point declines the way the missing-runtime path does.
struct ScriptHost::Impl {};

ScriptHost::ScriptHost() = default;
ScriptHost::~ScriptHost() { delete impl_; }

// Always declines: the CLR host is Win32-only.
bool ScriptHost::init(const HostDesc&) {
    declineReason_ = "the CLR host is implemented for Win32 only";
    AVER_WARN("[Scripting] init declined: {}", declineReason_);
    return false;
}
i32 ScriptHost::loadScripts(const std::string&) { return -1; }
bool ScriptHost::unloadScripts() { return false; }
void ScriptHost::update(f32) {}
void ScriptHost::shutdown() {}
bool ScriptHost::graphAvailable() const { return false; }
bool ScriptHost::graphLoad(i32, const std::string&) { return false; }
void ScriptHost::graphTick(i32, f32) {}
void ScriptHost::graphUnload(i32) {}
bool ScriptHost::graphFireAvailable() const { return false; }
bool ScriptHost::graphFire(i32, const std::string&) { return false; }
bool ScriptHost::graphClassesAvailable() const { return false; }
i32  ScriptHost::declareGraphClasses(const std::string&) { return 0; }
void ScriptHost::tickGraphClassInstances(f32) {}

#endif

} // namespace aver::scripting
