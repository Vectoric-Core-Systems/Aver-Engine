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

// ---------------------------------------------------------------- hostfxr, declared here
//
// These come from nethost.h / hostfxr.h / coreclr_delegates.h in the .NET *host pack*, which is
// only present on a machine with the SDK installed. Declaring the four functions we use — instead
// of including those headers — is what lets the engine BUILD on a machine with no .NET at all,
// which is the same property the runtime path is required to have. The surface is tiny, it has
// been stable since .NET Core 3.0, and getting it wrong fails loudly at the first call rather
// than silently.
using char_t = wchar_t;
using hostfxr_handle = void*;

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
// coreclr_delegates.h: the sentinel that says "the method carries [UnmanagedCallersOnly], so
// there is no delegate type to name".
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

// hostfxr_initialize_for_runtime_config returns three distinct successes; everything else is a
// failure HRESULT, which is negative when read as int32_t.
constexpr bool hostfxrOk(int32_t rc) { return rc >= 0 && rc <= 2; }

// The bridge's entry points. All five are [UnmanagedCallersOnly] on the managed side, so these
// are raw function pointers with no delegate marshalling in the way.
using bootstrap_fn     = int32_t(__cdecl*)(const AverScriptHostApi* api);
using loadScripts_fn   = int32_t(__cdecl*)(const char* utf8Dir);
using unloadScripts_fn = int32_t(__cdecl*)(void);
using update_fn        = void(__cdecl*)(float dt);
using shutdown_fn      = void(__cdecl*)(void);

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool fileThere(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The one function the managed side calls back into. Routed through the engine log rather than
// Console: a GUI process has no console, and the editor's Output Log is where anyone will look.
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

struct ScriptHost::Impl {
    HMODULE nethost = nullptr;
    HMODULE hostfxr = nullptr;
    hostfxr_handle ctx = nullptr;
    hostfxr_close_fn close = nullptr;
    loadScripts_fn load = nullptr;
    unloadScripts_fn unload = nullptr;
    update_fn update = nullptr;
    shutdown_fn shutdown = nullptr;
};

ScriptHost::ScriptHost() = default;

ScriptHost::~ScriptHost() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool ScriptHost::init(const HostDesc& desc) {
    if (ready_) return true;
    if (!impl_) impl_ = new Impl();

    // Every failure below takes this shape: record a reason, log ONE line, return false. The
    // caller is expected to carry on — an editor that cannot run scripts is still an editor.
    const auto decline = [this](std::string why) {
        declineReason_ = std::move(why);
        AVER_WARN("[Scripting] init declined: {}", declineReason_);
        return false;
    };

    const std::wstring bridgeDirW = widen(desc.bridgeDir);
    const std::wstring bridgeDll = bridgeDirW + L"\\Aver.Scripting.Bridge.dll";
    const std::wstring bridgeCfg = bridgeDirW + L"\\Aver.Scripting.Bridge.runtimeconfig.json";

    // Checked before the runtime is touched: staging is a build-system fact, and starting a CLR
    // only to discover there is nothing to run is a slower way to reach the same answer.
    if (!fileThere(bridgeDll) || !fileThere(bridgeCfg))
        return decline("the managed bridge was not staged next to the executable "
                       "(Aver.Scripting.Bridge.dll / .runtimeconfig.json) — build with the .NET SDK present");

    // nethost is loaded DYNAMICALLY rather than linked, so a machine with no .NET runtime still
    // starts the editor: a missing import in the executable's table would fail the process at
    // load time, before any of this code could decline. LoadLibraryW with a bare name searches
    // the executable's own directory first, which is where CMake stages it.
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
        // 0x80008096 is FrameworkMissingFailure — the common one, and the one where the message
        // has to name the framework rather than the error, or nobody knows what to install.
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

    // Loading user scripts is deliberately NOT a condition of readiness: a host with no scripts
    // is the normal case for the editor, and a script that fails to load disables itself rather
    // than taking the runtime down with it.
    behaviours_ = desc.scriptsDir.empty() ? 0 : impl_->load(desc.scriptsDir.c_str());
    if (behaviours_ < 0) behaviours_ = 0;

    AVER_INFO("[Scripting] .NET runtime hosted in-process; {} behaviour(s) live", behaviours_);
    return true;
}

i32 ScriptHost::loadScripts(const std::string& dir) {
    if (!ready_ || !impl_ || !impl_->load) return -1;
    const int32_t n = impl_->load(dir.c_str());
    behaviours_ = n < 0 ? 0 : n;
    return behaviours_;
}

bool ScriptHost::unloadScripts() {
    if (!ready_ || !impl_ || !impl_->unload) return false;
    const int32_t collected = impl_->unload();
    behaviours_ = 0;
    return collected != 0;
}

void ScriptHost::update(f32 dt) {
    if (!ready_ || !impl_ || !impl_->update) return;
    impl_->update(dt);
}

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
    // hostfxr is deliberately NOT freed. Closing the host context releases our handle on it, but
    // the CLR itself stays loaded for the life of the process — that is how CoreCLR works, and
    // FreeLibrary on a module with live runtime threads behind it is a crash, not a tidy-up.
    if (impl_->nethost) { ::FreeLibrary(impl_->nethost); impl_->nethost = nullptr; }
}

#else // !_WIN32

// The scripting host is Win32-only today, exactly as Aver.Platform is. Declining is the same
// contract the missing-runtime path uses, so a non-Windows build still links and runs.
struct ScriptHost::Impl {};

ScriptHost::ScriptHost() = default;
ScriptHost::~ScriptHost() { delete impl_; }

bool ScriptHost::init(const HostDesc&) {
    declineReason_ = "the CLR host is implemented for Win32 only";
    AVER_WARN("[Scripting] init declined: {}", declineReason_);
    return false;
}
i32 ScriptHost::loadScripts(const std::string&) { return -1; }
bool ScriptHost::unloadScripts() { return false; }
void ScriptHost::update(f32) {}
void ScriptHost::shutdown() {}

#endif

} // namespace aver::scripting
