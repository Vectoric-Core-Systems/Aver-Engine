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

    // Raises a named event on whatever graph is bound to `entity`, and returns whether anything
    // ran. THE FIRST NATIVE CALLER OF A GRAPH EVENT -- until animation notifies there were none,
    // which is why GraphEvents.cs's own comment records that this seam was investigated and left
    // unbuilt rather than shipped as an export with nothing behind it.
    //
    // False for every ordinary reason as well as every failing one: no scripting host, a bridge
    // predating this export, an entity with no graph, or a graph that declares no such event. A
    // caller cannot tell those apart HERE and should not try -- the managed side logs each with
    // its own message, once per (entity, event), so a footstep fired at a graph that never handles
    // one costs a line rather than a line per frame.
    bool graphFire(i32 entity, const std::string& eventName);

    // Whether the staged bridge exports GraphFire at all. Distinct from graphAvailable(): a bridge
    // built before this existed hosts graphs perfectly well and simply cannot be fired at.
    bool graphFireAvailable() const;

    // VALIDATES .ocgraph TEXT, loading and running nothing. Returns true when the graph is valid;
    // false with `err` set to the first thing wrong with it, in the words the managed validator
    // already uses (they name the offending node and say what to do).
    //
    // TEXT, NOT A PATH, on purpose: the editor validates what is on the CANVAS, unsaved edits and
    // all. Handing over a path would validate the last saved version and quietly disagree with what
    // the author is looking at.
    //
    // Returns true with `err` cleared when validation is unavailable -- refusing to save a graph
    // because the bridge is missing would be worse than not checking it. Ask
    // graphValidateAvailable() when the distinction matters to the caller.
    bool graphValidate(const std::string& text, std::string& err) const;

    // Whether the staged bridge exports GraphValidate. Optional and separate from every group above,
    // for GraphFire's reason: a bridge that predates it hosts, ticks and fires graphs correctly and
    // is only unable to check one.
    bool graphValidateAvailable() const;

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
