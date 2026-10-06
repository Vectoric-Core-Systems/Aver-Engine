#pragma once
// The join between the pure BT evaluator (Aver.Synapse) and a live scene::World: CSynapseBehavior,
// BtSystem's per-entity tick, and the seven built-in Condition/Action names every project gets for
// free (registerBuiltinBehaviors) on top of Aver.Synapse's own BtRegistry, which a project may
// still add its own named entries to afterward.
//
// Plain aver::synapse namespace, NOT aver::synapse::scene -- see SynapseAgent.hpp's own header
// comment for why a namespace segment literally named "scene" would shadow the sibling namespace
// aver::scene for every bare `scene::X` reference in this file.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcBt.hpp"
#include "aver/scene/World.hpp"
#include "aver/synapse/Blackboard.hpp"
#include "aver/synapse/Bt.hpp"
#include "aver/synapse/BtAsset.hpp"
#include "aver/synapse/Hearing.hpp"
#include "aver/synapse/SynapsePerception.hpp"   // reuses its NotifyFn -- see setNotifySink's own comment

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::synapse {

// Registered via World::registerComponent, all scalars -- the RUNNING state (which composite node
// is mid-Sequence, Cooldown timers, an Action's own scratch float) is NOT part of this component,
// for the identical reason CSynapseAgent keeps its own path out of itself: BtRunningState holds
// std::unordered_maps, which the field-table reflection API (Fields.hpp) has no FieldKind for. It
// lives in BtSystem's own side table instead, keyed by the full entity handle and pruned every
// tick -- see AgentSystem::paths_'s own comment for why "full handle, pruned" and not "index, kept
// forever" is the only safe choice.
struct CSynapseBehavior {
    u64 treeAssetId = 0;   // 0 = no tree assigned; BtSystem::loadTree returns the id to store here
    u64 teamId      = 0;   // SharedBlackboards::idOf(team name); 0 = the default shared board
    i32 lastStatus  = 0;   // a BtStatus, from the most recent tick -- readable for debugging/queries
};

// What the editor's live debugger reads for one entity. Pointers are valid until the next tick.
struct BtDebugView {
    u64                   treeId = 0;
    const BtRuntimeTree*  tree = nullptr;
    const BtRunningState* state = nullptr;
    const Blackboard*     board = nullptr;
};

// Mirrors a listener's strongest hearing memory onto its blackboard under reserved keys (defined on
// the fly with these types when the tree does not declare them): HasHeard (Bool), HeardPosition
// (Vec3), HeardLevel (Float), HeardTag (Int), HeardSource (Entity), HeardConfidence (Float, at the
// time it was heard). A forgotten memory that is the mirrored one clears HasHeard. Install with
// hearingSystem().setMemorySink(&btSystem().hearingSink()).
class BtHearingSink final : public IHearingMemorySink {
public:
    void onHeard(u32 listener, const HeardMemory& m) override;
    void onForgotten(u32 listener, const HeardMemory& m) override;
};

class BtSystem {
public:
    IHearingMemorySink& hearingSink() { return hearingSink_; }

    // Registers CSynapseBehavior, idempotently. Call before spawning anything that will carry it.
    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }

    // Adds CSynapseBehavior to `e` with its REAL defaults applied -- see AgentSystem::attach's own
    // comment (SynapseAgent.hpp) for why a bare world.addComponent alone is not enough.
    CSynapseBehavior* attach(scene::World& world, scene::Entity e);

    // Loads and caches a .ocbt by path (keyed by fnv1a64 of the path, the same identity scheme
    // .ocworld placements already use). Returns the id to store on
    // CSynapseBehavior::treeAssetId, or 0 on a failed load (logged once, here, at the WARN level --
    // not cached as a permanent failure, so a path that starts resolving later -- content still
    // syncing, say -- is retried on the next attach rather than staying broken for the process
    // lifetime). Safe to call more than once for the same path; the second call returns the
    // already-cached id without touching disk again.
    u64 loadTree(const std::string& path);

    // Re-reads `path` after an edit and swaps it in; entities using it restart their tree and re-bind
    // their blackboard schema. The old tree stays if the file does not load. False on a failed load.
    bool reloadTree(const std::string& path);
    // Registers an in-memory tree under `name` (tests, tools). Returns its id, like loadTree.
    u64 registerTree(const std::string& name, const BtAsset& asset);
    const BtRuntimeTree* findTree(u64 id) const;

    // ---- blackboards ------------------------------------------------------------------------------
    // One board per entity, created on first use by the tick or by `create`. Agent-scope keys live on
    // it; Shared-scope keys live on the team board named by CSynapseBehavior::teamId.
    Blackboard* blackboard(scene::Entity e, bool create = false);
    // Puts `e` on a team (any name; "" = the default board). False without a CSynapseBehavior on `e`.
    bool setTeam(scene::World& world, scene::Entity e, std::string_view team);
    SharedBlackboards& teams() { return teams_; }
    const std::string* teamName(u64 teamId) const;
    // Drops every entity board and team board (tests, level unload).
    void clearBlackboards();

    // ---- live debugging ---------------------------------------------------------------------------
    // Records per-node results for this one entity (the editor's debug target); invalid = none.
    void watch(scene::Entity e) { watched_ = e; }
    scene::Entity watched() const { return watched_; }
    bool debugView(scene::Entity e, BtDebugView& out) const;
    // Entities whose CSynapseBehavior points at `treeId` (0 = every entity that has a tree).
    std::vector<scene::Entity> entitiesUsing(scene::World& world, u64 treeId) const;

    // Installed onto every BtSystem-owned "FireEvent" action -- see NotifyFn's own comment
    // (SynapsePerception.hpp) for why this is the SAME function signature a composition root
    // already installs onto anim::animSystem() and synapse::perceptionSystem(), and why installing
    // the identical function here too (rather than inventing a fourth copy) is correct, not a
    // shortcut: its body was never anim- or perception-specific.
    void setNotifySink(NotifyFn fn, void* user) { notify_ = fn; notifyUser_ = user; }
    // Fires a named event on `e`'s own graph through the installed sink, or does nothing (returns
    // false) if none is installed -- the built-in "FireEvent" action's whole implementation.
    bool fireNotify(scene::Entity e, const char* name) const;

    // Advances every CSynapseBehavior entity's tree by one tick. An entity with treeAssetId == 0,
    // or one whose id never resolved to a loaded tree, is silently skipped -- "no tree assigned
    // yet" is not an error, the same tolerance AgentSystem's own Requested-with-no-grid state has.
    void tick(scene::World& world, f32 dt);

    // The registry every Condition/Action leaf is resolved through. registerBuiltinBehaviors
    // (below) populates the seven built-ins into THIS; a project registers its own here too.
    BtRegistry& registry() { return registry_; }

    // How many entities currently hold live Running state. Exists so a test can assert the side
    // table does not leak once a behaviour resolves or its entity is destroyed, without reaching
    // into private state -- the same reason AgentSystem::activePathCount() exists.
    usize runningCount() const { return running_.size(); }

private:
    u32 type_ = 0;
    BtRegistry registry_;
    std::unordered_map<u64, BtRuntimeTree> trees_;
    struct BoardRec {
        Blackboard board;
        u64 boundTree = 0;
        u64 team = ~0ull;   // ~0 = not yet joined to a team board
    };
    // teams_ is declared BEFORE boards_ so entity boards (which unregister from their team board in
    // their destructor) are destroyed first.
    SharedBlackboards teams_;
    std::unordered_map<scene::Entity, std::unique_ptr<BoardRec>> boards_;
    std::unordered_map<u64, std::string> teamNames_;
    scene::Entity watched_ = scene::kInvalidEntity;
    BtHearingSink hearingSink_;
    // KEYED BY THE FULL ENTITY HANDLE, pruned every tick -- AgentSystem::paths_'s own fix, applied
    // here for the identical reason.
    std::unordered_map<scene::Entity, BtRunningState> running_;
    NotifyFn notify_ = nullptr;
    void* notifyUser_ = nullptr;

    void prune(scene::World& world);
    BoardRec& boardRec(scene::Entity e);
    void bindBoard(scene::Entity e, BoardRec& rec, u64 team, u64 treeId, const BtRuntimeTree* tree);
    void syncPerception(scene::World& world, scene::Entity e, Blackboard& board);
};

// ---- scalar relay for graph nodes and C# --------------------------------------------------------------
// One function shaped for the framework's blackboard provider (framework_blackboard_abi.h); the
// composition root installs it: aver_fw_set_blackboard_provider(&synapse::blackboardRelay, nullptr).
// Types are 1 + BbType (0 = none). `key` carries the team name for kBbRelaySetTeam.
enum BbRelayOp : i32 {
    kBbRelayType = 0,     // returns the key's type code, 0 when undefined
    kBbRelayGet = 1,      // `type` = wanted type (0 = stored); fills i / f[0..2] / text; returns 1 on success
    kBbRelaySet = 2,      // `type` = type of the supplied value; defines the key (Agent scope) when missing
    kBbRelayReset = 3,
    kBbRelayDefine = 4,   // `type` = key type, *i = scope (0 agent, 1 shared)
    kBbRelaySetTeam = 5,
};
i32 blackboardRelay(i32 op, i32 entity, const char* key, i32 type, i64* i, f32* f, char* text,
                    i32 textCap, void* user);

// The process-global system, matching anim::animSystem() / synapse::agentSystem() /
// synapse::perceptionSystem().
BtSystem& btSystem();

// Registers the built-in Conditions/Actions into `system`'s own registry() (plus the blackboard leaves
// BbCompare / BbSet / BbClear, see BtRegistry::registerBlackboardLeaves) --
//   Conditions: HasTarget, CanSeeTarget, DistanceToTargetLess (params[0] = threshold, cm)
//   Actions:    MoveTo (params[0..2] = world goal), Wait (params[0] = duration, sec), LookAt,
//               FireEvent (stringParam = the event name to raise)
// All five that read perception/pathing state do so through synapse::agentSystem() /
// synapse::perceptionSystem() directly -- the same singletons a graph node's own native relay
// reaches -- rather than through a `user` pointer, so there is nothing a composition root needs to
// pass in beyond calling this function once, after both of those systems' own registerComponents.
// A project's OWN named conditions/actions register into the SAME registry() afterward; nothing
// here assumes these seven are the only entries that will ever exist.
void registerBuiltinBehaviors(BtSystem& system);

} // namespace aver::synapse
