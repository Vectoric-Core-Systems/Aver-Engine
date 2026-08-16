#pragma once
// The in-process CLR host's public interface.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::scripting {

// Where the host looks for the bridge and for user scripts. Both are directories.
struct HostDesc {
    // Directory holding Aver.Scripting.Bridge.dll + .runtimeconfig.json and the managed contract
    // assemblies. nethost.dll is NOT here: it is loaded by bare name, so it sits beside the exe.
    std::string bridgeDir;
    // Directory scanned for user script assemblies. May be empty or may not exist.
    std::string scriptsDir;
};

// In-process CLR host. One per process. Declines rather than failing when .NET is unavailable.
class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Starts the runtime, bootstraps the bridge and loads `scriptsDir`. False when unavailable.
    bool init(const HostDesc& desc);

    // Loads a directory of script assemblies additively and returns the live behaviour count.
    // Returns -1 when the host is not ready, which is distinct from an empty directory.
    i32 loadScripts(const std::string& dir);

    // Drains OnShutdown and unloads the collectible context, leaving the runtime up.
    // Returns true when the old context was fully collected; false is not a failure.
    bool unloadScripts();

    // Drives OnUpdate on every live behaviour.
    void update(f32 dt);

    // How many [AverHud] classes the loaded scripts declare.
    i32 hudCount() const;
    // The HUD's display name.
    std::string hudName(i32 index) const;
    // Calls the HUD's Draw(dt) into whatever rect aver_ui_begin_frame last established.
    bool hudDraw(i32 index, f32 dt);

    // Drives OnShutdown, unloads the context and closes the host context. Safe called twice.
    void shutdown();

    // Graph hosting: GraphLoad/GraphTick/GraphUnload, bound OPTIONALLY at init time exactly like the
    // HUD three -- a bridge built before these existed still boots; graphAvailable() is false and
    // every call below is a documented no-op/false rather than a crash.
    bool graphAvailable() const;
    // Loads and compiles the .ocgraph at `path`, binding it to `entity`. False on any failure
    // (missing bridge support, bad path, parse/compile error) -- see Aver.Graph.GraphHost.Load.
    bool graphLoad(i32 entity, const std::string& path);
    // Ticks the graph bound to `entity`, if any. A no-op for an entity with none, or when
    // graphAvailable() is false.
    void graphTick(i32 entity, f32 timeSeconds);
    // Drops the graph bound to `entity`, if any.
    void graphUnload(i32 entity);

    // GRAPH-AS-CLASS: bound OPTIONALLY, exactly like the graph three just above -- a bridge built
    // before these existed still boots; graphClassesAvailable() is false and both calls below are
    // documented no-ops.
    bool graphClassesAvailable() const;
    // Scans `contentDir` recursively for *.ocgraph files and declares one framework class per file
    // that carries a CLASS record, through the same aver_fw_class_declare/set_flags(MANAGED)/seal
    // sequence a C# actor class goes through (see HostBridge.cs's DeclareGraphClasses). Returns the
    // number of classes declared, or 0 when unavailable/the directory has none. Idempotent to call
    // again (e.g. a second project open): aver_fw_class_declare is idempotent by name.
    i32 declareGraphClasses(const std::string& contentDir);
    // Ticks every live graph-class instance once (see HostBridge.cs's GraphTickBoundInstances for the
    // full "why UNGATED on aver_fw_play_state()" reasoning) -- call once a frame from BOTH composition
    // roots, beside their own equivalent of tickProjectGraphs. A no-op when unavailable.
    void tickGraphClassInstances(f32 dt);

    bool ready() const { return ready_; }
    // Behaviours that were discovered, constructed and survived OnStart. Zero is normal.
    i32 behaviourCount() const { return behaviours_; }
    // Why init() declined. Empty once ready.
    const std::string& declineReason() const { return declineReason_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    i32 behaviours_ = 0;
    std::string declineReason_;
};

} // namespace aver::scripting
