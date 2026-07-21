#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver::scripting {

// Where the host looks for the two things it needs. Both are directories, not files, so a caller
// never has to know the bridge assembly's name or the runtimeconfig's.
struct HostDesc {
    // Directory holding Aver.Scripting.Bridge.dll + .runtimeconfig.json + nethost.dll.
    // Normally the executable's own directory.
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
