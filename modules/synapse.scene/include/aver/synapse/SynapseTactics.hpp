#pragma once
// Cover and squad tactics in a live scene.
//
//  - CSynapseCoverMarker: an authored cover point. The entity's position is where an agent stands;
//    its local +X axis points at the obstacle that protects it. Markers become CoverMap points,
//    kept in step (moved, disabled, removed) every tick with stable ids.
//  - Generated cover: optionally derived from the nav grid's walls (setAutoGenerate), alongside.
//  - CSynapseSquad: membership in a squad. The tick assigns roles and a target slot to every
//    member of a squad that has a target (setSquadTarget).
//  - Reservations: findCover claims a point so no two agents are sent to the same one.
//
// Line of sight and path cost default to the nav grid (a segment through blocked cells, and
// findPath length). A host with physics can replace either with setBlocked / setPathCost.
#include "aver/formats/OcNav.hpp"
#include "aver/scene/World.hpp"
#include "aver/synapse/Cover.hpp"

#include <map>
#include <unordered_map>
#include <vector>

namespace aver::synapse {

// All 4-byte scalars.
struct CSynapseCoverMarker {
    i32 height = 1;                 // CoverHeight: 0 low, 1 high
    f32 arcHalfAngleDeg = 70.0f;
    i32 enabled = 1;
};

struct CSynapseSquad {
    i32 squadId = 0;
    f32 spacingCm = 200.0f;
    // Outputs, written by the tick once the squad has a target.
    i32 role = 0;                   // a SquadRole
    i32 hasSlot = 0;
    f32 slotXCm = 0.0f, slotYCm = 0.0f, slotZCm = 0.0f;
    i32 coverId = 0;                // the point reserved for this member, 0 if none
};

struct SquadParams {
    f32 flankRadiusCm = 1000.0f;
    f32 flankAngleDeg = 70.0f;
    f32 supportBehindCm = 300.0f;
};

// What a cover search needs from its caller; the rest comes from the system's defaults.
struct CoverSearch {
    f32 maxSeekCm = 2500.0f;
    f32 minThreatDistCm = 300.0f;
    f32 maxCloserCm = 400.0f;
    bool requireHigh = false;
    f32 reserveSec = 20.0f;         // <= 0: held until released
};

class TacticsSystem {
public:
    TacticsSystem();

    u32 registerComponents(scene::World& world);   // both components
    u32 markerType() const { return markerType_; }
    u32 squadType() const { return squadType_; }
    CSynapseCoverMarker* attachMarker(scene::World& world, scene::Entity e);
    CSynapseSquad* attachSquad(scene::World& world, scene::Entity e);

    CoverMap& covers() { return covers_; }
    const CoverMap& covers() const { return covers_; }
    CoverReservations& reservations() { return reservations_; }

    // Derive cover from the grid's walls on the next tick (and whenever the grid changes).
    void setAutoGenerate(bool on, const CoverGenParams& p = {}) { autoGen_ = on; genParams_ = p; lastNav_ = nullptr; }
    // Regenerate the grid-derived points on the next tick (after a rebake into the same buffer).
    void regenerate() { lastNav_ = nullptr; }
    void setBlocked(BlockedFn fn, void* user) { blocked_ = fn; blockedUser_ = user; }
    void setPathCost(PathCostFn fn, void* user) { pathCost_ = fn; pathUser_ = user; useGridPath_ = fn == nullptr; }
    void setSquadParams(const SquadParams& p) { squadParams_ = p; }

    // Finds and RESERVES the best cover from `threat` for `seeker`. False when none protects.
    bool findCover(scene::World& world, scene::Entity seeker, const Vec3& threat, const CoverSearch& s,
                   CoverResult& out);
    void releaseCover(scene::Entity seeker);
    // Whether the point the seeker holds still shields it from `threat`.
    bool isCovered(scene::World& world, scene::Entity seeker, const Vec3& threat) const;

    // Drops every point, claim, marker mapping and squad target.
    void reset() { covers_.clear(); reservations_ = {}; markerCover_.clear(); squadTargets_.clear(); lastNav_ = nullptr; }

    void setSquadTarget(i32 squadId, const Vec3& target);
    void clearSquadTarget(i32 squadId);

    void tick(scene::World& world, const fmt::OcNavData* nav, f32 dt);

    // Displacement that restores a member's spacing from its squad-mates, ground plane.
    V2 spacingPush(scene::World& world, scene::Entity e) const;

private:
    u32 markerType_ = 0, squadType_ = 0;
    CoverMap covers_;
    CoverReservations reservations_;
    bool autoGen_ = false;
    CoverGenParams genParams_;
    const fmt::OcNavData* lastNav_ = nullptr;
    const fmt::OcNavData* nav_ = nullptr;
    BlockedFn blocked_ = nullptr; void* blockedUser_ = nullptr;
    PathCostFn pathCost_ = nullptr; void* pathUser_ = nullptr;
    bool useGridPath_ = true;
    SquadParams squadParams_;
    std::unordered_map<scene::Entity, u32> markerCover_;     // marker entity -> CoverPoint id
    std::map<i32, Vec3> squadTargets_;
    std::vector<scene::Entity> scratch_;
    std::vector<u32> owners_;

    void syncMarkers(scene::World& world);
    void tickSquads(scene::World& world);
    static f32 gridPathCost(void* user, V2 from, V2 to);
};

} // namespace aver::synapse
