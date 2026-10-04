// Road vehicles that DRIVE: Jolt wheeled vehicles, steered along a level's .oclanes by a small AI.
//
// WHY THIS IS NOT AN ANIMATION. A traffic car on a clip is a mesh sliding along a spline: it ignores the
// road it is on, the car in front of it and the player in its way. Here the car is a real wheeled body
// (suspension, tyres, gravity, collisions -- modules/physics, physics_vehicle_abi.h) and everything it
// does comes from three pedals and a steering input set once a frame. Trains, boats and flyers are NOT
// vehicles in this sense and keep their animated paths.
//
// TWO HALVES IN ONE FILE, on purpose. The first half (namespace `lanes`) is the driver's MATHS: lane
// projection, look-ahead across a lane boundary, pure-pursuit steering, curvature and leader-gap speed
// limits, the pedals. It is PURE -- Core and Formats only, no scene, no physics -- so tests/world checks
// it with no world and no solver, and a bad number is found there instead of as one car in a city
// driving wrong. The second half (VehicleSystem) is the part that touches the world and the physics ABI,
// and does nothing the maths could have done.
//
// WHERE IT RUNS. The host calls begin() once at Play start (after it has snapshotted the transforms it
// will restore), prePhysics() every frame immediately BEFORE aver_phys_step, postPhysics() right after it
// and end() at Play stop. begin() reads each entity's CURRENT world transform, so a car the user nudged
// in the editor still drives from where it now stands.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcLanes.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace aver::world::lanes {

// ---- The driver's tuning ---------------------------------------------------------------------------
// Centimetres and seconds throughout, which is what .oclanes and the physics ABI already speak, so no
// value here is converted at a boundary. They are TUNING, not measurements: nothing in this file was
// fitted to a recording of a real vehicle.

inline constexpr f32 kLookAheadMinCm = 600.0f;       // pure pursuit never looks closer than 6 m
inline constexpr f32 kLookAheadMaxCm = 3000.0f;      // ... or farther than 30 m
inline constexpr f32 kLookAheadSeconds = 0.45f;      // how far ahead, in seconds of travel, in between

inline constexpr f32 kCurveHorizonCm = 4000.0f;      // the curvature cap looks this far down the route
inline constexpr f32 kCurveSampleCm = 400.0f;        // ... sampling it this often (a lane vertex is ~3.9 m)
inline constexpr f32 kMaxLateralAccelCmS2 = 250.0f;  // 2.5 m/s^2 of cornering: comfortable, not the limit
inline constexpr f32 kCornerSpeedFloorCmS = 250.0f;  // a kink in the polyline must not stop a car dead

inline constexpr f32 kMinGapCm = 300.0f;             // the bumper-to-bumper gap kept at a standstill
inline constexpr f32 kTimeHeadwayS = 1.4f;           // plus this many seconds of the car's own speed
inline constexpr f32 kStoppedLeaderGapCm = 600.0f;   // a STOPPED leader this close means stop
inline constexpr f32 kStoppedSpeedCmS = 50.0f;       // "stopped" is slower than 0.5 m/s

inline constexpr f32 kStandStillTargetCmS = 5.0f;    // a target this low is "hold still", not "drive slowly"
inline constexpr f32 kBrakeOverCmS = 150.0f;         // brake only once 1.5 m/s over the target
inline constexpr f32 kBrakeSpanCmS = 150.0f;         // ... reaching full brake this much further over
inline constexpr f32 kThrottleSpanCmS = 300.0f;      // full throttle at 3 m/s under the target

// A lane as the driver drives it: the file's polyline plus what the maths needs to walk it quickly.
struct Lane {
    std::vector<Vec3> pts;     // engine cm, first = where traffic enters; always >= 2 points
    std::vector<f32> cum;      // arc length from pts[0] to pts[i], cm; same size as pts
    std::vector<i32> next;     // successors as INDICES into Network::lanes (the file's ids are resolved away)
    f32 speedLimit = 0.0f;     // cm/s
    Vec3 boundsMin, boundsMax; // of the points, for rejecting a lane before projecting onto it

    f32 length() const { return cum.empty() ? 0.0f : cum.back(); }
};

struct Network {
    std::vector<Lane> lanes;
};

// What buildNetwork had to throw away, for the caller's one summary line.
struct BuildReport {
    u32 droppedLanes = 0;   // fewer than two points, or no length to drive
    u32 droppedLinks = 0;   // a successor id that names no kept lane
};

// Resolves a parsed file into a Network: arc lengths, bounds, and NEXT ids turned into indices.
// parseOcLanes already refuses most of what this drops; it still drops rather than trusts, because a
// lane list can also be built by hand (a test, a tool) and a bad index here would be a crash in the AI.
Network buildNetwork(const fmt::OcLanesData& d, BuildReport* report = nullptr);

// A point's nearest position on one lane.
struct Projection {
    f32 s = 0.0f;           // arc length of that position, cm from the lane's start
    f32 distance = 0.0f;    // 3D distance from the query point to it, cm
    f32 lateral = 0.0f;     // signed XY offset: + when the point is to the RIGHT of the direction of travel
    Vec3 point;             // the position itself
    Vec3 tangent;           // unit direction of travel there
};

inline constexpr f32 kWholeLane = std::numeric_limits<f32>::max();

// The nearest point on `lane` to `p`, looking only at the part of the lane whose arc length is in
// [sMin, sMax] (a segment the window touches is tested whole). A car re-projects onto a WINDOW around
// where it was, not the whole lane: a lane that doubles back (a ramp loop, a U-turn) passes within metres
// of itself, and the nearest point overall can be on the leg the car is not driving.
Projection project(const Lane& lane, const Vec3& p, f32 sMin = 0.0f, f32 sMax = kWholeLane);

// The position at arc length `s` (clamped to the lane) and, if asked, the unit direction of travel there.
Vec3 pointAt(const Lane& lane, f32 s, Vec3* outTangent = nullptr);

struct LaneHit {
    i32 lane = -1;          // index into Network::lanes, -1 for none
    Projection proj;
};

// The nearest lane to `p` within `maxDistance` cm whose direction of travel at the nearest point agrees
// with `headingXY` (a unit vector in the ground plane): cos(angle) >= minCosHeading. A lane carrying
// traffic the OTHER way past a car is closer than its own lane about as often as not, so direction is
// part of the question, not a tie-break after it.
LaneHit nearestLane(const Network& net, const Vec3& p, const Vec3& headingXY, f32 maxDistance,
                    f32 minCosHeading);

// A ROUTE is the lane being driven followed by the lanes chosen to come after it. The driver chooses its
// successors AHEAD of arriving (so what it looks at and what it then drives are the same road), and these
// walk that list.
struct RoutePoint {
    Vec3 point;
    Vec3 tangent;            // unit direction of travel there
    i32 lane = -1;           // the lane (index) the point is on; -1 for an empty route
    f32 s = 0.0f;            // arc length on that lane
    bool clamped = false;    // the route ended first: this is the end of its last lane
};

// The point `ahead` cm further along `route` from arc length `s` on route[0]. THE LANE BOUNDARY COSTS
// NOTHING: a successor's first point is (near) the previous lane's last, so the walk carries the distance
// it has left straight onto the next lane's start.
RoutePoint advanceAlong(const Network& net, const std::vector<i32>& route, f32 s, f32 ahead);

// Cm of road from arc length `s` on route[0] to the end of the route's last lane.
f32 routeRemaining(const Network& net, const std::vector<i32>& route, f32 s);

// One of `lane`'s successors chosen by `random` (any u32), or -1 at a dead end.
i32 pickSuccessor(const Lane& lane, u32 random);

// Radius (cm) of the circle through three points, in the ground plane; infinity when they are collinear
// or two coincide (a route clamped at a dead end repeats its last point).
f32 circumradiusXY(const Vec3& a, const Vec3& b, const Vec3& c);

// The tightest turn radius over the next `horizon` cm of route, sampled every `step` cm; infinity when
// the way ahead is straight or too short to judge.
f32 minRadiusAhead(const Network& net, const std::vector<i32>& route, f32 s, f32 horizon, f32 step);

// The speed (cm/s) at which a turn of `radius` cm needs kMaxLateralAccelCmS2: sqrt(a * R), never below
// kCornerSpeedFloorCmS. Infinity for a straight.
f32 curvatureSpeedCap(f32 radius);

// How far ahead pure pursuit aims at `speed` cm/s: clamp(6 m + 0.45 s * speed, 6 m, 30 m).
f32 lookAheadDistance(f32 speed);

// The bearing of `target` from `from`, in the ground plane, measured from `forward` and positive toward
// the vehicle's RIGHT (engine +Y when it faces +X). Radians, in (-pi, pi].
f32 bearingToTarget(const Vec3& from, const Vec3& forward, const Vec3& target);

// Pure pursuit: the steering input in [-1, 1] (+1 = full lock toward the right) that curves toward a
// point `alpha` radians off the nose, `lookAhead` cm away, for a car of `wheelbase` cm whose full lock is
// `maxSteerRad`. steer = atan2(2 W sin(alpha), L) / maxSteer.
f32 pursuitSteer(f32 alpha, f32 wheelbase, f32 lookAhead, f32 maxSteerRad);

// The speed (cm/s) that keeps the time-headway gap behind something `gap` cm ahead: the gap at which a
// car at speed v is content is kMinGapCm + kTimeHeadwayS * v, solved for v. Zero inside kMinGapCm.
f32 headwaySpeed(f32 gap);

// headwaySpeed, plus the rule that a leader STOPPED within kStoppedLeaderGapCm means stop outright --
// creeping up on a stopped car at (gap - 3 m) / 1.4 s would otherwise never quite arrive. `leaderSpeed`
// is the leader's own speed along the road, cm/s.
f32 gapLimitedSpeed(f32 gap, f32 leaderSpeed);

struct Pedals {
    f32 forward = 0.0f;    // throttle 0..1
    f32 brake = 0.0f;      // 0..1
};

// A P controller on the speed error. Throttle below the target; nothing within kBrakeOverCmS above it;
// brake beyond that. A target at or below kStandStillTargetCmS is "stand still": brake, whatever the
// speed, so a car held at a gap does not creep on the engine's idle.
Pedals speedPedals(f32 speed, f32 target);

// The chassis orientation for a car sitting on a road whose direction of travel is `tangent`: yaw to
// the heading and pitch to the grade, no roll. Used to put a car back on its lane.
Quat laneOrientation(const Vec3& tangent);

} // namespace aver::world::lanes

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

namespace aver::world {

// Beyond this distance from the focus a vehicle's DRIVER runs only every kVehicleFarInterval-th frame
// and the vehicle keeps its last input between runs. See VehicleSystem's own comment for what that does
// and does not save.
inline constexpr f32 kVehicleFarCm = 30000.0f;
inline constexpr u32 kVehicleFarInterval = 4;

// One placement that asked to be a vehicle: its entity, the preset name from the `vehicle <preset>`
// token (car | van | truck | bus | sports), and its mesh's LOCAL bounds in centimetres, which is where
// the physical size comes from.
struct VehicleSpawn {
    scene::Entity entity = scene::kInvalidEntity;
    std::string preset;
    Vec3 boundsMin, boundsMax;
};

// COST, STATED AS WHAT IT IS. Each vehicle is one Jolt wheeled-vehicle constraint, which casts one probe
// per wheel on every fixed 1/60 s physics step; that is the cost that scales with the vehicle count and
// the LOD below does NOT reduce it (a far vehicle is still simulated every step). What the LOD thins is
// the DRIVER -- projection, look-ahead, the curvature scan, the leader lookup -- which is a few hundred
// flops per run plus one sort of the lane-occupancy list per frame. NO TIMING FOR EITHER HAS BEEN
// MEASURED: the 300 m and every-fourth-frame figures are tuning, chosen so the cost of the part this
// module owns is bounded by the number of vehicles NEAR the player; measure a populated level before
// trusting that they are in the right place, and before spending effort on the physics side of the bill.
class VehicleSystem {
public:
    VehicleSystem();
    ~VehicleSystem();
    // PINNED: it owns physics handles that name this object's own state. A host that needs to move one
    // holds it behind a pointer.
    VehicleSystem(const VehicleSystem&) = delete;
    VehicleSystem& operator=(const VehicleSystem&) = delete;

    // Builds one physics vehicle per spawn at its entity's CURRENT world transform, sized from the mesh
    // bounds (scaled by the entity's world scale) and the preset, and assigns each to the nearest lane
    // whose direction agrees with its heading (within 60 degrees, within 12 m) -- so a car the user moved
    // in the editor still finds a lane. A spawn with no lane near it PARKS: physics, brakes on. `lanes`
    // may be null or empty, and then every vehicle parks. `seed` makes every random choice (which way at
    // a junction, where a stranded car reappears) repeatable. Ends whatever a previous begin() built.
    //
    // A spawn that cannot be built (a stale entity, degenerate bounds, the physics module refusing)
    // is skipped and counted in the one summary line this logs; it never fails the rest.
    void begin(scene::World& w, const std::vector<VehicleSpawn>& spawns, const fmt::OcLanesData* lanes,
               u32 seed);

    // The driver: computes and sets every vehicle's input. Call every frame before aver_phys_step.
    // `focus` (the camera, or the player) decides which vehicles are near enough to drive every frame.
    void prePhysics(f32 dt, const Vec3& focus);

    // Writes each vehicle entity's transform from its physics pose. Call after aver_phys_step and before
    // the world's flush. A vehicle whose pose has not changed since the last write (asleep, parked) writes
    // nothing, so a parked fleet does not dirty a transform per car per frame. A vehicle whose ENTITY has
    // been destroyed since begin() is destroyed with it and leaves count() and entities(): an invisible
    // car must not go on driving, colliding and holding a place in its lane's queue.
    void postPhysics(scene::World& w);

    // Destroys every vehicle. The host restores the entities' transforms itself.
    void end();

    usize count() const;
    // The vehicle entities, in spawn order, for PlayMobility seeding (a moving entity not seeded costs a
    // GI rebuild the first time it moves).
    std::vector<scene::Entity> entities() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
