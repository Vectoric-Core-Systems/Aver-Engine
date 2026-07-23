#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver::scripting {

// Where the host looks for the two things it needs. Both are directories, not files, so a caller
// never has to know the bridge assembly's name or the runtimeconfig's.
struct HostDesc {
    // Directory holding Aver.Scripting.Bridge.dll + .runtimeconfig.json (and the managed contract
    // assemblies it depends on). Step 11 stages these in <exe>/Scripting rather than the executable's
    // own directory, because the managed Aver.Framework/Aver.Scene DLLs would otherwise collide by file
    // name with the native DLLs beside the exe. nethost.dll is NOT here: it is loaded by bare name and
    // so must sit next to the executable, which is where CMake stages it.
    std::string bridgeDir;
    // Directory scanned for user script assemblies. May be empty or may not exist: a host with
    // no scripts is the normal case for the editor and must not be an error.
    std::string scriptsDir;
};

// In-process CLR host. One per process in practice — hostfxr will happily hand back an already
// initialised runtime, but the bridge keeps a single collectible load context and a single
// behaviour list, so a second ScriptHost would be talking to the first one's state.
//
// Posture: this subsystem DECLINES. If the .NET runtime is absent, if nethost or hostfxr cannot
// be loaded, if the bridge assembly was not staged, or if the contract version disagrees, init()
// logs once, records a reason and returns false — and the editor runs exactly as it does with no
// scripting at all. Mirrors VoxiRenderer::init. This is not negotiable: the engine must never
// fail to start because scripting is unavailable.
class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Starts the runtime, bootstraps the bridge and loads whatever is in `scriptsDir`.
    // Returns false having logged exactly one line when scripting is unavailable.
    bool init(const HostDesc& desc);

    // Loads (or re-loads) a directory of script assemblies into the collectible context and
    // returns the number of live behaviours. Additive: it does NOT replace what is already
    // loaded, so a caller swapping one set of scripts for another must unloadScripts() first.
    // Returns -1 when the host is not ready, which is different from a directory with nothing
    // in it — the editor surfaces the two differently.
    i32 loadScripts(const std::string& dir);

    // Drains OnShutdown on every live behaviour and unloads the collectible context, leaving the
    // runtime up and the bridge bootstrapped. This is the drain half of hot reload; the caller
    // rebuilds and calls loadScripts() again.
    //
    // Returns true when the old context was fully collected. FALSE IS NOT A FAILURE: unloading in
    // .NET is a request satisfied only once every reference is dropped and a GC has run, so a
    // false means the old assemblies are still resident, not that anything went wrong. Reloading
    // works either way — assemblies are loaded from memory streams, so nothing on disk is locked.
    bool unloadScripts();

    // Drives OnUpdate on every live behaviour. Safe (and free) after a declined init.
    void update(f32 dt);

    // Drives OnShutdown, unloads the collectible context and closes the host context.
    // Safe after a declined init, and safe called twice.
    void shutdown();

    bool ready() const { return ready_; }
    // Behaviours that were discovered, constructed and survived OnStart. Zero is normal.
    i32 behaviourCount() const { return behaviours_; }
    // Why init() declined, for the editor to surface. Empty once ready.
    const std::string& declineReason() const { return declineReason_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    i32 behaviours_ = 0;
    std::string declineReason_;
};

} // namespace aver::scripting
