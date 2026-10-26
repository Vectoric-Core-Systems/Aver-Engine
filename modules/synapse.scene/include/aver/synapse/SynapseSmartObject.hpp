#pragma once
// The scene join for smart objects: CSmartObject (an .ocso asset name on any entity), the world-space
// slot transforms, claim/use/release in entity terms, the behaviours run on the user while a slot is
// Occupied, the mirrored blackboard keys and the events. A member of SynapseAi (the Synapse DLL's
// single instance). Pure state machine: SmartObject.hpp.
#include "aver/scene/World.hpp"
#include "aver/synapse/Bt.hpp"
#include "aver/synapse/BtAsset.hpp"
#include "aver/synapse/Eqs.hpp"
#include "aver/synapse/SmartObject.hpp"
#include "aver/synapse/StateTree.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aver::synapse {

class SynapseAi;

// Registered via World::registerComponent. `asset` is a String field (the scene's string pool).
struct CSmartObject {
    i64 asset = 0;       // content-relative .ocso path
    i32 enabled = 1;
    i32 slotsFree = 0;   // read-only runtime mirror
};

class SmartObjectSystem {
public:
    void bind(SynapseAi* ai) { ai_ = ai; }

    u32 registerComponents(scene::World& world);
    u32 componentType() const { return type_; }
    // Adds CSmartObject with its real defaults and sets the asset (see AgentSystem::attach).
    CSmartObject* attach(scene::World& world, scene::Entity e, const std::string& assetPath);
    bool setEnabled(scene::World& world, scene::Entity e, bool enabled);

    // Registers new or changed objects and refreshes slot transforms. Every API call below does this first.
    void sync(scene::World& world);

    SmartObjectRegistry& registry() { return reg_; }
    const SmartObjectRegistry& registry() const { return reg_; }

    i32 slotCount(scene::World& world, scene::Entity e);   // -1 when not a registered object
    bool slotInfo(scene::World& world, scene::Entity e, u32 slot, SoSlotInfo& out);
    bool setSlotEnabled(scene::World& world, scene::Entity e, u32 slot, bool enabled);

    u32 find(scene::World& world, const SoQuery& q, SoSlotRef* out, f32* dist, u32 cap);
    u32 claim(scene::World& world, scene::Entity user, SoSlotRef slot, const TagList& userTags, f32 ttl, SoRefusal* why);
    bool use(scene::World& world, u32 claim);
    bool release(scene::World& world, u32 claim, SoReleaseReason reason);
    bool claimInfo(u32 claim, SoClaimInfo& out) const { return reg_.claimInfo(claim, out); }
    u32 claimsOf(scene::Entity user, u32* out, u32 cap) const { return reg_.claimsOf(user, out, cap); }
    // Why a claim ended, for the last 64 ended claims.
    bool endedReason(u32 claim, SoReleaseReason& out) const;

    // The EQS `slots` generator.
    u32 findSlots(scene::World& world, const EqsGenerator& g, const Vec3& centre, const EqsRunParams& p, EqsItem* out, TagList* tags, u32 cap);

    // Scan, transforms, TTL / cooldown, sweep of dead users, behaviours, mirrored keys, events.
    void tick(scene::World& world, f32 dt);
    void reset();

    // Floating-origin shift (LARGE_WORLD 2.2.5): slot transforms are derived from the owners' world matrices and
    // re-resolve when a revision moves; this makes the next sync() re-resolve every object regardless. Call after
    // World::shiftRoots.
    void rebase();

    // ---- leaf support ----
    u32& leafClaim(i32 subject, const void* node) { return leafClaims_[{subject, node}]; }
    void dropLeafClaim(i32 subject, const void* node) { leafClaims_.erase({subject, node}); }

private:
    struct ObjRec {
        std::string path;
        const SoDef* def = nullptr;
        bool enabled = true;
        u32 rev = ~0u;
    };
    struct ClaimRt {
        scene::Entity user = scene::kInvalidEntity;
        BtRunningState bt;
        StateTreeInstance st;
        const BtRuntimeTree* btTree = nullptr;
        const StateTreeAsset* stAsset = nullptr;
        bool started = false, finished = false;
    };

    const SoDef* defFor(const std::string& path);
    void applyWorld(scene::World& world, scene::Entity e, u32 object);
    void handleReleases(scene::World& world, std::vector<SoRelease>& released);
    void mirror(scene::World& world, scene::Entity user);
    void runBehaviour(scene::World& world, u32 claim, ClaimRt& rt, f32 dt, std::vector<SoRelease>& released);
    void abortBehaviour(u32 claim, ClaimRt& rt);

    SynapseAi* ai_ = nullptr;
    u32 type_ = 0;
    u32 assetField_ = 0;
    SmartObjectRegistry reg_;
    std::map<scene::Entity, ObjRec> objs_;
    std::map<std::string, std::unique_ptr<SoDef>> defs_;
    std::set<std::string> failed_;
    std::map<std::string, std::unique_ptr<BtRuntimeTree>> btCache_;
    std::map<std::string, std::unique_ptr<StateTreeAsset>> stCache_;
    std::map<u32, ClaimRt> rt_;
    std::vector<std::pair<u32, SoReleaseReason>> ended_;
    std::map<std::pair<i32, const void*>, u32> leafClaims_;
};

} // namespace aver::synapse
