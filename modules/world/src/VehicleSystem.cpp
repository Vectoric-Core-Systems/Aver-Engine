#include "aver/world/VehicleSystem.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_vehicle_abi.h"
#endif

namespace aver::world {

namespace {

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

// The ground-plane part of a vector. Heading, bearing and lateral offset are all questions about the road
// surface; a car's pitch on a ramp is not part of any of them.
Vec3 flat(const Vec3& v) { return Vec3{v.x, v.y, 0.0f}; }

} // namespace

// =============================================================================================
// The driver's maths. Pure: nothing below until the VehicleSystem section touches a world or a body.
// =============================================================================================
namespace lanes {

namespace {

// A lane shorter than this has no road to drive on.
constexpr f32 kMinLaneLengthCm = 1.0f;

// A segment shorter than this has no direction to speak of, and is stepped over rather than divided by.
constexpr f32 kMinSegmentCm = 1e-3f;

// The segment of `lane` that arc length `s` falls in. A zero-length segment (a repeated point) has no
// direction, so a hit on one steps back to the last segment that does.
usize segmentAt(const Lane& lane, f32 s) {
    const usize segs = lane.pts.size() - 1;
    usize k = static_cast<usize>(std::upper_bound(lane.cum.begin(), lane.cum.end(), s) - lane.cum.begin());
    k = k == 0 ? 0 : k - 1;
    k = std::min(k, segs - 1);
    while (k > 0 && lane.cum[k + 1] - lane.cum[k] < kMinSegmentCm) --k;
    return k;
}

} // namespace

Network buildNetwork(const fmt::OcLanesData& d, BuildReport* report) {
    Network net;
    BuildReport rep;
    net.lanes.reserve(d.lanes.size());

    // THE FILE'S IDS ARE RESOLVED TO INDICES HERE, once, so the AI never hashes in a frame. A lane that is
    // dropped below simply has no entry, which is what makes a NEXT that names it a dropped link.
    std::unordered_map<i32, i32> indexOfId;
    std::vector<const fmt::OcLane*> source;   // parallel to net.lanes: the file lane each one came from
    source.reserve(d.lanes.size());

    for (const fmt::OcLane& src : d.lanes) {
        if (src.pts.size() < 2) { ++rep.droppedLanes; continue; }
        Lane lane;
        lane.pts = src.pts;
        lane.speedLimit = src.speedLimit;
        lane.cum.assign(src.pts.size(), 0.0f);
        lane.boundsMin = lane.boundsMax = src.pts[0];
        for (usize k = 1; k < src.pts.size(); ++k) {
            const Vec3& p = src.pts[k];
            lane.cum[k] = lane.cum[k - 1] + dist(src.pts[k - 1], p);
            lane.boundsMin = Vec3{std::min(lane.boundsMin.x, p.x), std::min(lane.boundsMin.y, p.y),
                                  std::min(lane.boundsMin.z, p.z)};
            lane.boundsMax = Vec3{std::max(lane.boundsMax.x, p.x), std::max(lane.boundsMax.y, p.y),
                                  std::max(lane.boundsMax.z, p.z)};
        }
        // Written as a negation so a NaN length (a NaN point) is dropped too.
        if (!(lane.length() >= kMinLaneLengthCm)) { ++rep.droppedLanes; continue; }
        indexOfId.emplace(src.id, static_cast<i32>(net.lanes.size()));
        source.push_back(&src);
        net.lanes.push_back(std::move(lane));
    }

    for (usize i = 0; i < net.lanes.size(); ++i) {
        for (const i32 id : source[i]->next) {
            const auto it = indexOfId.find(id);
            if (it == indexOfId.end()) ++rep.droppedLinks;
            else net.lanes[i].next.push_back(it->second);
        }
    }

    if (report) *report = rep;
    return net;
}

Projection project(const Lane& lane, const Vec3& p, f32 sMin, f32 sMax) {
    Projection best;
    best.distance = kInf;
    if (lane.pts.size() < 2) return best;

    const usize segs = lane.pts.size() - 1;
    // The first segment whose END is at or past sMin; cum[k + 1] is segment k's end.
    usize first = static_cast<usize>(std::lower_bound(lane.cum.begin() + 1, lane.cum.end(), sMin) -
                                     (lane.cum.begin() + 1));
    first = std::min(first, segs - 1);

    f32 bestD2 = kInf;
    for (usize k = first; k < segs; ++k) {
        if (k > first && lane.cum[k] > sMax) break;
        const Vec3& a = lane.pts[k];
        const Vec3 ab = lane.pts[k + 1] - a;
        const f32 l2 = ab.sizeSquared();
        if (l2 < kMinSegmentCm * kMinSegmentCm) continue;
        const f32 t = std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f);
        const Vec3 q = a + ab * t;
        const f32 d2 = distSquared(p, q);
        if (d2 < bestD2) {
            bestD2 = d2;
            best.s = lane.cum[k] + t * (lane.cum[k + 1] - lane.cum[k]);
            best.point = q;
            best.tangent = ab * (1.0f / std::sqrt(l2));
        }
    }
    if (!(bestD2 < kInf)) {
        // Every segment in the window was degenerate; the lane's start is the only honest answer.
        best.s = 0.0f;
        best.point = lane.pts.front();
        best.tangent = Vec3{1.0f, 0.0f, 0.0f};
        bestD2 = distSquared(p, best.point);
    }
    best.distance = std::sqrt(bestD2);

    const Vec3 right = Vec3{-best.tangent.y, best.tangent.x, 0.0f}.getSafeNormal();
    best.lateral = dot(flat(p - best.point), right);
    return best;
}

Vec3 pointAt(const Lane& lane, f32 s, Vec3* outTangent) {
    if (lane.pts.size() < 2) {
        if (outTangent) *outTangent = Vec3{1.0f, 0.0f, 0.0f};
        return lane.pts.empty() ? Vec3{} : lane.pts[0];
    }
    const f32 sc = std::clamp(s, 0.0f, lane.length());
    const usize k = segmentAt(lane, sc);
    const Vec3& a = lane.pts[k];
    const Vec3 ab = lane.pts[k + 1] - a;
    const f32 segLen = lane.cum[k + 1] - lane.cum[k];
    const f32 t = segLen > kMinSegmentCm ? std::clamp((sc - lane.cum[k]) / segLen, 0.0f, 1.0f) : 0.0f;
    if (outTangent) {
        const Vec3 dir = ab.getSafeNormal();
        *outTangent = dir.sizeSquared() > 0.0f ? dir : Vec3{1.0f, 0.0f, 0.0f};
    }
    return a + ab * t;
}

LaneHit nearestLane(const Network& net, const Vec3& p, const Vec3& headingXY, f32 maxDistance,
                    f32 minCosHeading) {
    LaneHit best;
    f32 bestDistance = maxDistance;
    for (usize i = 0; i < net.lanes.size(); ++i) {
        const Lane& lane = net.lanes[i];
        // THE BOUNDS FIRST: a city has two thousand lanes and a car is within a dozen metres of a handful.
        if (p.x < lane.boundsMin.x - maxDistance || p.x > lane.boundsMax.x + maxDistance ||
            p.y < lane.boundsMin.y - maxDistance || p.y > lane.boundsMax.y + maxDistance ||
            p.z < lane.boundsMin.z - maxDistance || p.z > lane.boundsMax.z + maxDistance)
            continue;
        const Projection pr = project(lane, p);
        if (pr.distance > bestDistance) continue;
        if (dot(flat(pr.tangent).getSafeNormal(), headingXY) < minCosHeading) continue;
        best.lane = static_cast<i32>(i);
        best.proj = pr;
        bestDistance = pr.distance;
    }
    return best;
}

RoutePoint advanceAlong(const Network& net, const std::vector<i32>& route, f32 s, f32 ahead) {
    RoutePoint rp;
    if (route.empty()) return rp;
    f32 pos = std::max(s + ahead, 0.0f);
    for (usize k = 0; k < route.size(); ++k) {
        const Lane& lane = net.lanes[static_cast<usize>(route[k])];
        const f32 len = lane.length();
        const bool last = k + 1 == route.size();
        if (pos <= len || last) {
            rp.clamped = pos > len;
            rp.s = std::min(pos, len);
            rp.lane = route[k];
            rp.point = pointAt(lane, rp.s, &rp.tangent);
            return rp;
        }
        pos -= len;
    }
    return rp;
}

f32 routeRemaining(const Network& net, const std::vector<i32>& route, f32 s) {
    f32 total = -s;
    for (const i32 lane : route) total += net.lanes[static_cast<usize>(lane)].length();
    return std::max(total, 0.0f);
}

i32 pickSuccessor(const Lane& lane, u32 random) {
    return lane.next.empty() ? -1 : lane.next[random % lane.next.size()];
}

f32 circumradiusXY(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = flat(b - a), bc = flat(c - b), ca = flat(a - c);
    const f32 la = bc.size(), lb = ca.size(), lc = ab.size();
    // Twice the triangle's area. R = abc / (4 * area) = abc / (2 * this).
    const f32 cr = std::fabs(ab.x * -ca.y - ab.y * -ca.x);
    if (la < 1.0f || lb < 1.0f || lc < 1.0f || cr < 1e-3f) return kInf;
    return la * lb * lc / (2.0f * cr);
}

f32 minRadiusAhead(const Network& net, const std::vector<i32>& route, f32 s, f32 horizon, f32 step) {
    f32 tightest = kInf;
    if (route.empty() || !(step > 0.0f)) return tightest;
    const int samples = static_cast<int>(horizon / step);
    Vec3 p0 = advanceAlong(net, route, s, 0.0f).point;
    Vec3 p1 = advanceAlong(net, route, s, step).point;
    for (int i = 2; i <= samples; ++i) {
        const Vec3 p2 = advanceAlong(net, route, s, step * static_cast<f32>(i)).point;
        tightest = std::min(tightest, circumradiusXY(p0, p1, p2));
        p0 = p1;
        p1 = p2;
    }
    return tightest;
}

f32 curvatureSpeedCap(f32 radius) {
    if (!(radius < kInf)) return kInf;
    return std::max(kCornerSpeedFloorCmS, std::sqrt(kMaxLateralAccelCmS2 * radius));
}

f32 lookAheadDistance(f32 speed) {
    return std::clamp(kLookAheadMinCm + kLookAheadSeconds * std::max(speed, 0.0f), kLookAheadMinCm,
                      kLookAheadMaxCm);
}

f32 bearingToTarget(const Vec3& from, const Vec3& forward, const Vec3& target) {
    const Vec3 f = flat(forward).getSafeNormal();
    const Vec3 d = flat(target - from);
    // The vehicle's right is its forward turned a quarter toward +Y: (1, 0) -> (0, 1).
    const Vec3 right{-f.y, f.x, 0.0f};
    return std::atan2(dot(d, right), dot(d, f));
}

f32 pursuitSteer(f32 alpha, f32 wheelbase, f32 lookAhead, f32 maxSteerRad) {
    if (!(maxSteerRad > 0.0f)) return 0.0f;
    const f32 delta = std::atan2(2.0f * wheelbase * std::sin(alpha), lookAhead);
    return std::clamp(delta / maxSteerRad, -1.0f, 1.0f);
}

f32 headwaySpeed(f32 gap) {
    return std::max(0.0f, (gap - kMinGapCm) / kTimeHeadwayS);
}

f32 gapLimitedSpeed(f32 gap, f32 leaderSpeed) {
    if (leaderSpeed < kStoppedSpeedCmS && gap < kStoppedLeaderGapCm) return 0.0f;
    return headwaySpeed(gap);
}

Pedals speedPedals(f32 speed, f32 target) {
    Pedals p;
    if (target <= kStandStillTargetCmS) {
        p.brake = 1.0f;
        return p;
    }
    const f32 err = target - speed;
    if (err > 0.0f) p.forward = std::min(err / kThrottleSpanCmS, 1.0f);
    else if (-err > kBrakeOverCmS) p.brake = std::min((-err - kBrakeOverCmS) / kBrakeSpanCmS, 1.0f);
    return p;
}

Quat laneOrientation(const Vec3& tangent) {
    const Vec3 u = tangent.getSafeNormal();
    if (u.sizeSquared() < 0.5f) return Quat::identity();
    const f32 yaw = std::atan2(u.y, u.x);
    // A positive pitch about +Y turns +X DOWN (x -> (cos, 0, -sin)), so a nose-up grade is a negative pitch.
    const f32 pitch = -std::asin(std::clamp(u.z, -1.0f, 1.0f));
    return (Quat::fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, yaw) *
            Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, pitch)).normalized();
}

} // namespace lanes

} // namespace aver::world

#if AVER_MODULE_SCENE

namespace aver::world {

#  if AVER_MODULE_PHYSICS

namespace {

// ---- What a preset means physically ----------------------------------------------------------------
// The mesh bounds fix a vehicle's SIZE; these fix everything else about it. Sizes come from the mesh so a
// level's own cars need no authored dimensions, and the numbers here are the ones a car of that kind needs
// to drive plausibly -- not measurements of any real model.
struct Preset {
    const char* name;
    f32 massKg;
    f32 radiusMinCm, radiusMaxCm;   // the wheel radius is 0.17 * height, clamped to this range
    f32 engineNm;
    f32 maxSteerDeg;
    f32 suspensionHz;
    bool frontDriven;               // false = the rear axle drives
};

constexpr Preset kPresets[] = {
    {"car",    1300.0f, 28.0f, 38.0f,  500.0f, 35.0f, 2.0f, false},
    {"van",    1900.0f, 33.0f, 40.0f,  600.0f, 35.0f, 2.0f, true},
    {"truck",  6000.0f, 45.0f, 55.0f, 1500.0f, 30.0f, 1.5f, false},
    {"bus",   11000.0f, 48.0f, 55.0f, 2000.0f, 30.0f, 1.5f, false},
    {"sports", 1200.0f, 30.0f, 35.0f,  700.0f, 35.0f, 2.0f, false},
};

const Preset* findPreset(const std::string& name) {
    for (const Preset& p : kPresets)
        if (name == p.name) return &p;
    return nullptr;
}

// ---- Chassis and wheel layout, as fractions of the mesh bounds -------------------------------------
constexpr f32 kRadiusPerHeight = 0.17f;        // wheel radius = this * height, then clamped by the preset
constexpr f32 kWheelWidthPerRadius = 0.7f;     // a tyre is about 0.7 radii wide (205 mm at a 330 mm radius)
constexpr f32 kChassisHeight = 0.55f;          // the chassis box is this fraction of the mesh's height ...
constexpr f32 kComHeight = 0.35f;              // ... with its centre of mass this fraction up from the origin
constexpr f32 kClearancePerRadius = 0.6f;      // ground clearance under the chassis box
constexpr f32 kAxleInsetRadii = 1.2f;          // each axle sits this many radii in from its end of the body
constexpr f32 kTrackInsetWheelWidths = 0.6f;   // each wheel centre sits this many tyre widths in from the side
constexpr f32 kSuspensionMinCm = 5.0f;         // fully compressed
constexpr f32 kSuspensionMaxCm = 30.0f;        // fully extended; the wheels hang from this far above their rest
constexpr f32 kSuspensionDamping = 0.5f;
constexpr f32 kBrakeDecelMs2 = 9.0f;           // the stop the brakes could hold if the tyres could grip it
constexpr f32 kHandbrakeFactor = 1.5f;         // rear-axle handbrake, as a multiple of the service brake
constexpr f32 kEngineMinRpm = 1000.0f;         // the ABI's own defaults, stated so a change there is not silent
constexpr f32 kEngineMaxRpm = 6000.0f;
// The chassis' up axis is held within this many degrees of the world's, which is what stops an AI car
// ending on its roof. Jolt's limit only resists: a car on its side comes to rest PRESSED AGAINST it, not
// beyond it, so the flipped-car recovery below has to look for a car at the limit (kFlippedBelowUpZ).
constexpr f32 kMaxPitchRollDeg = 60.0f;
constexpr f32 kGravityCmS2 = 981.0f;

// How far a spring of `hz` hertz compresses under the weight it was sized for, g / (2 pi f)^2: 6.2 cm at
// 2 Hz, 11 cm at 1.5. Jolt sizes each spring from an effective mass a little SMALLER than the quarter of
// the car its wheel carries, so the true sag is 15 to 20 percent more than this (see buildVehicle).
f32 springSagCm(f32 hz) {
    const f32 omega = 2.0f * kPi * hz;
    return kGravityCmS2 / (omega * omega);
}

// ---- Driver behaviour the maths does not own --------------------------------------------------------
constexpr f32 kOriginHeightTolerance = 0.05f;  // a mesh's lowest point may be this fraction of its height off its origin
constexpr f32 kAssignMaxCm = 1200.0f;          // a spawn finds a lane within 12 m ...
constexpr f32 kAssignMinCos = 0.5f;            // ... running within 60 degrees of its heading
constexpr f32 kRouteHorizonCm = 7000.0f;       // successors are chosen this far ahead of the car
constexpr usize kMaxRoute = 12;                // lanes in a route; a cap against a ring of one-metre lanes
constexpr f32 kWindowBackCm = 500.0f;          // a car re-projects onto its lane within this far behind ...
constexpr f32 kWindowAheadCm = 4000.0f;        // ... and this far ahead of where it was
constexpr f32 kLaneSwitchSlackCm = 50.0f;      // it steps onto the next lane this close to the end of this one
constexpr int kMaxHops = 3;                    // lanes stepped in one frame; more means lanes shorter than that
constexpr f32 kRouteEndCm = 100.0f;            // at a dead end: reached the end ...
constexpr f32 kDeadEndStopCm = 800.0f;         // ... or slowed to a crawl this close to it
constexpr f32 kDeadEndCrawlCmS = 100.0f;

constexpr f32 kDeadEndRespawnS = 4.0f;         // a stranded car waits this long, then reappears elsewhere
constexpr f32 kStuckS = 8.0f;                  // asking for throttle and not moving this long = stuck
constexpr f32 kStuckThrottle = 0.1f;           // "asking for throttle" means more than this
constexpr f32 kFlippedS = 3.0f;                // on its side or roof this long = flipped
constexpr f32 kFlippedMarginZ = 0.05f;         // "at the pitch/roll limit" leaves this much of up.Z for overshoot
// The chassis' up against world up, under this = lying against the pitch/roll limit = on its side. The
// limit's own cos plus a margin, because a car can only be pushed OVER the limit for a step, never to rest
// there: a threshold below it (it was 0.4 against a limit of 0.5) is one no car ever reaches.
const f32 kFlippedBelowUpZ = std::cos(radians(kMaxPitchRollDeg)) + kFlippedMarginZ;
constexpr f32 kRespawnClearCm = 6000.0f;       // a respawn is at least this far from the focus ...
constexpr f32 kRespawnSpacingCm = 800.0f;      // ... and this far from every other vehicle
constexpr int kRespawnTries = 24;
constexpr f32 kRespawnLiftCm = 10.0f;          // a car is set down this high so no wheel starts in the road

// A vehicle's own random stream: splitmix64, one per vehicle, seeded from the system's seed and the
// vehicle's spawn index. Per-vehicle rather than shared so one car's choices never depend on how many
// times ANOTHER car drew -- a car that is skipped by the LOD must not change where the rest go.
u32 nextRandom(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<u32>(z >> 32);
}

struct Built {
    i32 handle = 0;
    f32 wheelbase = 0.0f;
    f32 maxSteerRad = 0.0f;
};

// Builds one vehicle through the physics ABI at `xf` (origin pose, scale ignored): the chassis box, four
// wheels in the order front-left, front-right, rear-left, rear-right (left is -Y, the ABI's anti-roll bars
// pair them that way), the engine, and the constraint. False if the physics module refused any step.
bool buildVehicle(const Preset& pr, const Vec3& size, const Transform& xf, Built& out) {
    const f32 length = size.x, width = size.y, height = size.z;
    const f32 radius = std::clamp(kRadiusPerHeight * height, pr.radiusMinCm, pr.radiusMaxCm);
    const f32 wheelWidth = kWheelWidthPerRadius * radius;

    const i32 v = aver_phys_vehicle_create(
        0.5f * length, 0.5f * width, 0.5f * kChassisHeight * height, kClearancePerRadius * radius,
        0.0f, 0.0f, kComHeight * height, pr.massKg,
        xf.position.x, xf.position.y, xf.position.z,
        xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w);
    if (!v) return false;

    // The axle and track clamps only matter for a mesh too short or too narrow for the formula; they keep
    // the four wheels a sane rectangle instead of crossed over.
    const f32 axleX = std::max(0.5f * length - kAxleInsetRadii * radius, 0.1f * length);
    const f32 trackY = std::max(0.5f * width - kTrackInsetWheelWidths * wheelWidth, 0.25f * width);
    // THE WHEEL'S ATTACHMENT IS THE TOP OF ITS SUSPENSION TRAVEL, and a loaded car is never hanging from
    // it fully extended. Jolt rests the spring at its MAX length (the spring's constraint value is
    // length - maxLength, VehicleConstraint.cpp), so the car's weight compresses it by the static sag
    // and the wheel centre settles one sag SHORT of full droop: with the attachment at radius + max the
    // origin -- where the mesh's tyres end -- would come to rest that far INSIDE the road. Lowering the
    // attachment by the sag puts the settled wheel centre one radius above the origin instead.
    //
    // THE FORMULA IS EXACT WHEN A WHEEL CARRIES THE MASS JOLT SIZES ITS SPRING FOR, and it does not quite:
    // Jolt's effective mass for the spring (1 / (1/m + the lever terms)) is about 280 kg against the 325 kg
    // a wheel of a 440 cm, 1300 kg saloon carries -- worked by hand, not measured -- so the true sag is
    // about 15 percent more and a settled car sits a centimetre or so under the road. Measure a populated
    // level before tuning it.
    const f32 attachZ = radius + kSuspensionMaxCm - springSagCm(pr.suspensionHz);
    // A full-friction stop shared by four wheels: torque = m a r / 4.
    const f32 brake = pr.massKg * kBrakeDecelMs2 * (radius * 0.01f) * 0.25f;

    struct Wheel { f32 x, y; bool front; };
    const Wheel wheels[4] = {{axleX, -trackY, true}, {axleX, trackY, true},
                             {-axleX, -trackY, false}, {-axleX, trackY, false}};
    for (const Wheel& wh : wheels) {
        const i32 added = aver_phys_vehicle_add_wheel(
            v, wh.x, wh.y, attachZ, radius, wheelWidth, kSuspensionMinCm, kSuspensionMaxCm, pr.suspensionHz,
            kSuspensionDamping, wh.front ? pr.maxSteerDeg : 0.0f, brake,
            wh.front ? 0.0f : kHandbrakeFactor * brake, wh.front == pr.frontDriven ? 1 : 0);
        if (added < 0) { aver_phys_vehicle_destroy(v); return false; }
    }
    aver_phys_vehicle_set_engine(v, pr.engineNm, kEngineMinRpm, kEngineMaxRpm);
    if (!aver_phys_vehicle_finish(v, kMaxPitchRollDeg)) { aver_phys_vehicle_destroy(v); return false; }

    out.handle = v;
    out.wheelbase = 2.0f * axleX;
    out.maxSteerRad = radians(pr.maxSteerDeg);
    return true;
}

enum class Mode : u8 {
    Parked,      // no lane: brakes on, never driven
    Driving,
    Stranded,    // stopped at a dead end, waiting to reappear elsewhere
};

struct Vehicle {
    scene::Entity entity = scene::kInvalidEntity;
    i32 handle = 0;
    // The entity's WORLD scale at begin(): the physics is unscaled, so the size comes from the bounds times
    // this, and the entity keeps it when its transform is written back.
    Vec3 scale{1.0f, 1.0f, 1.0f};
    f32 length = 0.0f;
    f32 wheelbase = 0.0f;
    f32 maxSteerRad = 0.0f;

    // The pose postPhysics last read, which is what the driver reasons from.
    Vec3 pos;
    Quat rot;
    f32 speed = 0.0f;           // signed, along the chassis' forward, cm/s
    // The pose the entity was last given, so a vehicle that has not moved writes nothing.
    Vec3 writtenPos;
    Quat writtenRot;

    Mode mode = Mode::Parked;
    std::vector<i32> route;     // route[0] is the lane being driven
    f32 s = 0.0f;               // progress along route[0], monotone
    f32 lastForward = 0.0f;     // the throttle the driver last asked for, for stuck detection
    f32 strandedFor = 0.0f;
    f32 stuckFor = 0.0f;
    f32 flippedFor = 0.0f;
    f32 pendingDt = 0.0f;       // time since the driver last ran, which is longer than a frame under the LOD
    u64 rng = 0;
};

// One vehicle's place on the road for the frame's leader lookups.
struct Occ {
    i32 lane;
    f32 s;
    u32 veh;
};

bool laneThenS(const Occ& a, const Occ& b) {
    return a.lane != b.lane ? a.lane < b.lane : a.s < b.s;
}

struct Leader {
    bool found = false;
    f32 gap = 0.0f;       // bumper to bumper, cm
    f32 speed = 0.0f;     // the leader's own, cm/s
};

// A scale that is 1 up to the noise of decomposing a rotation matrix is 1: writing 1.0000001 back would
// change a transform that never needed to change.
f32 snapToOne(f32 v) { return std::fabs(v - 1.0f) < 1e-4f ? 1.0f : v; }

bool samePose(const Vec3& pa, const Quat& qa, const Vec3& pb, const Quat& qb) {
    return pa.x == pb.x && pa.y == pb.y && pa.z == pb.z &&
           qa.x == qb.x && qa.y == qb.y && qa.z == qb.z && qa.w == qb.w;
}

bool finitePose(const Vec3& p, const Quat& q) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

// `worldXf` (an entity's world transform) expressed as the local transform its parent chain needs.
Transform localFor(scene::World& w, scene::Entity e, const Transform& worldXf) {
    const scene::Entity parent = w.parent(e);
    if (parent == scene::kInvalidEntity) return worldXf;
    // Row-vector matrices: world = local * parentWorld, so local = world * parentWorld^-1.
    return transformFromMatrix(worldXf.toMatrix() * w.worldMatrix(parent).inverse());
}

} // namespace

struct VehicleSystem::Impl {
    lanes::Network net;
    std::vector<Vehicle> vehicles;
    std::vector<Occ> occ;       // every driving vehicle's (lane, s), sorted; rebuilt every frame
    u32 frame = 0;

    void rebuildOccupancy();
    Leader leaderOf(usize i) const;
    void extendRoute(Vehicle& v) const;
    void trackProgress(Vehicle& v) const;
    void placeOnLane(Vehicle& v, i32 lane, f32 s) const;
    bool recover(Vehicle& v, f32 dt) const;
    bool occupied(const Vec3& at, usize self) const;
    bool respawn(Vehicle& v, usize self, const Vec3& focus) const;
    void drive(usize i, f32 dt, const Vec3& focus);
};

void VehicleSystem::Impl::rebuildOccupancy() {
    occ.clear();
    for (usize i = 0; i < vehicles.size(); ++i) {
        const Vehicle& v = vehicles[i];
        if (v.mode == Mode::Parked || v.route.empty()) continue;
        occ.push_back(Occ{v.route.front(), v.s, static_cast<u32>(i)});
    }
    std::sort(occ.begin(), occ.end(), [](const Occ& a, const Occ& b) {
        if (a.lane != b.lane) return a.lane < b.lane;
        if (a.s != b.s) return a.s < b.s;
        return a.veh < b.veh;
    });
}

Leader VehicleSystem::Impl::leaderOf(usize i) const {
    const Vehicle& me = vehicles[i];
    Leader out;
    if (me.route.empty()) return out;
    const i32 lane = me.route.front();

    auto report = [&](const Occ& o, f32 centreDistance) {
        const Vehicle& other = vehicles[o.veh];
        out.found = true;
        out.gap = centreDistance - 0.5f * (me.length + other.length);
        out.speed = other.speed;
    };

    // THE NEAREST AHEAD ON THIS LANE: the first entry past my own progress. My own entry (from the start
    // of the frame) is skipped by index, because a recovery can have moved my s back behind it.
    auto it = std::upper_bound(occ.begin(), occ.end(), Occ{lane, me.s, 0u}, laneThenS);
    while (it != occ.end() && it->lane == lane && it->veh == i) ++it;
    if (it != occ.end() && it->lane == lane) {
        report(*it, it->s - me.s);
        return out;
    }

    // ... else the nearest on the lane it is about to drive onto: that lane's first entry.
    if (me.route.size() > 1) {
        const i32 nextLane = me.route[1];
        auto it2 = std::lower_bound(occ.begin(), occ.end(), nextLane,
                                    [](const Occ& o, i32 l) { return o.lane < l; });
        while (it2 != occ.end() && it2->lane == nextLane && it2->veh == i) ++it2;
        if (it2 != occ.end() && it2->lane == nextLane)
            report(*it2, (net.lanes[static_cast<usize>(lane)].length() - me.s) + it2->s);
    }
    return out;
}

void VehicleSystem::Impl::extendRoute(Vehicle& v) const {
    while (v.route.size() < kMaxRoute && lanes::routeRemaining(net, v.route, v.s) < kRouteHorizonCm) {
        const lanes::Lane& last = net.lanes[static_cast<usize>(v.route.back())];
        // A DEAD END DRAWS NOTHING. This runs on every driver run while a car is within the horizon of the
        // end of its route, and a draw per run would make the car's stream position depend on how many
        // frames the approach took -- so the same seed would send a stranded car to a different lane on a
        // slower machine. Every other draw is an event (one per successor chosen).
        if (last.next.empty()) break;
        v.route.push_back(lanes::pickSuccessor(last, nextRandom(v.rng)));
    }
}

void VehicleSystem::Impl::trackProgress(Vehicle& v) const {
    const lanes::Lane* lane = &net.lanes[static_cast<usize>(v.route.front())];
    lanes::Projection pr = lanes::project(*lane, v.pos, v.s - kWindowBackCm, v.s + kWindowAheadCm);

    // STEP ONTO THE NEXT LANE once the car has run to the end of this one. The next lane is re-projected
    // from its own start, so a lane that begins a little behind the car's nose costs nothing.
    for (int hop = 0; hop < kMaxHops && v.route.size() > 1 && pr.s >= lane->length() - kLaneSwitchSlackCm;
         ++hop) {
        v.route.erase(v.route.begin());
        v.s = 0.0f;
        lane = &net.lanes[static_cast<usize>(v.route.front())];
        pr = lanes::project(*lane, v.pos, 0.0f, kWindowAheadCm);
    }
    // MONOTONE: a shove, a bounce or a bend in the polyline must never wind the car's progress back.
    v.s = std::max(v.s, pr.s);
    extendRoute(v);
}

void VehicleSystem::Impl::placeOnLane(Vehicle& v, i32 lane, f32 s) const {
    Vec3 tangent;
    Vec3 at = lanes::pointAt(net.lanes[static_cast<usize>(lane)], s, &tangent);
    at.z += kRespawnLiftCm;
    const Quat q = lanes::laneOrientation(tangent);
    aver_phys_vehicle_set_pose(v.handle, at.x, at.y, at.z, q.x, q.y, q.z, q.w);
    // set_pose zeroes the velocity; the brake keeps it zero through the frame it settles.
    aver_phys_vehicle_set_input(v.handle, 0.0f, 0.0f, 1.0f, 0.0f);
    v.pos = at;
    v.rot = q;
    v.speed = 0.0f;
    v.lastForward = 0.0f;
    v.stuckFor = v.flippedFor = v.strandedFor = 0.0f;
}

bool VehicleSystem::Impl::recover(Vehicle& v, f32 dt) const {
    const f32 upZ = v.rot.rotate(Vec3{0.0f, 0.0f, 1.0f}).z;
    v.flippedFor = upZ < kFlippedBelowUpZ ? v.flippedFor + dt : 0.0f;
    // STUCK IS ASKING FOR THROTTLE AND NOT MOVING. A car held back by a leader asks for none, so a queue
    // is never mistaken for a jam.
    const bool pushing = v.lastForward > kStuckThrottle && std::fabs(v.speed) < lanes::kStoppedSpeedCmS;
    v.stuckFor = pushing ? v.stuckFor + dt : 0.0f;
    if (v.flippedFor < kFlippedS && v.stuckFor < kStuckS) return false;
    placeOnLane(v, v.route.front(), v.s);
    return true;
}

bool VehicleSystem::Impl::occupied(const Vec3& at, usize self) const {
    for (usize i = 0; i < vehicles.size(); ++i)
        if (i != self && distSquared(vehicles[i].pos, at) < kRespawnSpacingCm * kRespawnSpacingCm) return true;
    return false;
}

bool VehicleSystem::Impl::respawn(Vehicle& v, usize self, const Vec3& focus) const {
    if (net.lanes.empty()) return false;
    for (int attempt = 0; attempt < kRespawnTries; ++attempt) {
        const i32 lane = static_cast<i32>(nextRandom(v.rng) % net.lanes.size());
        const Vec3 start = net.lanes[static_cast<usize>(lane)].pts.front();
        if (dist(start, focus) < kRespawnClearCm || occupied(start, self)) continue;
        v.route.assign(1, lane);
        v.s = 0.0f;
        extendRoute(v);
        placeOnLane(v, lane, 0.0f);
        v.mode = Mode::Driving;
        return true;
    }
    // No clear lane start this time: the car stays stranded and tries again on its next driver run.
    return false;
}

void VehicleSystem::Impl::drive(usize i, f32 dt, const Vec3& focus) {
    Vehicle& v = vehicles[i];
    if (v.mode == Mode::Parked || v.route.empty()) return;

    if (v.mode == Mode::Stranded) {
        aver_phys_vehicle_set_input(v.handle, 0.0f, 0.0f, 1.0f, 0.0f);
        v.strandedFor += dt;
        if (v.strandedFor >= kDeadEndRespawnS) respawn(v, i, focus);
        return;
    }

    if (recover(v, dt)) return;
    trackProgress(v);

    const lanes::Lane& lane = net.lanes[static_cast<usize>(v.route.front())];
    // MEASURED TO THE NOSE, NOT THE ORIGIN. The origin is the middle of the car, so a distance from it
    // lets a bus (half-length 6 m) stop with its bumper three metres PAST the end of the lane. The leader
    // gap below subtracts half of each car's length for the same reason.
    const f32 remaining = lanes::routeRemaining(net, v.route, v.s) - 0.5f * v.length;

    // A DEAD END is a wall the car must stop short of, not a place it drives off the end of.
    const bool deadEnd = net.lanes[static_cast<usize>(v.route.back())].next.empty();
    if (deadEnd && (remaining < kRouteEndCm ||
                    (remaining < kDeadEndStopCm && std::fabs(v.speed) < kDeadEndCrawlCmS))) {
        v.mode = Mode::Stranded;
        v.strandedFor = 0.0f;
        aver_phys_vehicle_set_input(v.handle, 0.0f, 0.0f, 1.0f, 0.0f);
        return;
    }

    // TARGET SPEED: the slowest of what the lane allows, what the bends ahead allow, and what the car in
    // front allows.
    f32 target = lane.speedLimit;
    target = std::min(target, lanes::curvatureSpeedCap(lanes::minRadiusAhead(
                                  net, v.route, v.s, lanes::kCurveHorizonCm, lanes::kCurveSampleCm)));
    const Leader leader = leaderOf(i);
    if (leader.found) target = std::min(target, lanes::gapLimitedSpeed(leader.gap, leader.speed));
    if (deadEnd) target = std::min(target, lanes::headwaySpeed(remaining));
    const lanes::Pedals pedals = lanes::speedPedals(v.speed, target);

    // PURE PURSUIT from the REAR AXLE, which is the point the formula is derived for: the origin is the
    // middle of the car, half a wheelbase ahead of it.
    const Vec3 forward = flat(v.rot.rotate(Vec3{1.0f, 0.0f, 0.0f})).getSafeNormal();
    const Vec3 rear = v.pos - forward * (0.5f * v.wheelbase);
    const f32 lookAhead = lanes::lookAheadDistance(v.speed);
    const lanes::RoutePoint aim = lanes::advanceAlong(net, v.route, v.s, lookAhead);
    const f32 alpha = lanes::bearingToTarget(rear, forward, aim.point);
    const f32 steer = lanes::pursuitSteer(alpha, v.wheelbase, lookAhead, v.maxSteerRad);

    aver_phys_vehicle_set_input(v.handle, pedals.forward, steer, pedals.brake, 0.0f);
    v.lastForward = pedals.forward;
}

VehicleSystem::VehicleSystem() : impl_(std::make_unique<Impl>()) {}
VehicleSystem::~VehicleSystem() = default;

void VehicleSystem::begin(scene::World& w, const std::vector<VehicleSpawn>& spawns,
                          const fmt::OcLanesData* lanesData, u32 seed) {
    end();
    Impl& m = *impl_;
    m.frame = 0;
    if (spawns.empty()) return;

    if (!aver_phys_ready()) {
        AVER_WARN("[Vehicles] {} vehicle placements but the physics world is not running; they stay "
                  "where they were placed", spawns.size());
        return;
    }

    lanes::BuildReport report;
    if (lanesData) m.net = lanes::buildNetwork(*lanesData, &report);
    if (report.droppedLanes || report.droppedLinks)
        AVER_WARN("[Vehicles] lane data: {} unusable lanes and {} successor links dropped",
                  report.droppedLanes, report.droppedLinks);

    u32 driving = 0, parked = 0, offCentre = 0, offGround = 0, unknownPreset = 0;
    u32 skippedEntity = 0, skippedBounds = 0, skippedPhysics = 0;
    m.vehicles.reserve(spawns.size());

    for (usize index = 0; index < spawns.size(); ++index) {
        const VehicleSpawn& sp = spawns[index];
        if (!w.valid(sp.entity)) { ++skippedEntity; continue; }

        const Preset* preset = findPreset(sp.preset);
        if (!preset) { ++unknownPreset; preset = &kPresets[0]; }

        const Transform xf = transformFromMatrix(w.worldMatrix(sp.entity));
        if (!finitePose(xf.position, xf.rotation)) { ++skippedEntity; continue; }
        const Vec3 extent = sp.boundsMax - sp.boundsMin;
        const Vec3 size{std::fabs(xf.scale.x) * extent.x, std::fabs(xf.scale.y) * extent.y,
                        std::fabs(xf.scale.z) * extent.z};
        // Written as a negation so a NaN bound is skipped too.
        if (!(size.x > 1.0f && size.y > 1.0f && size.z > 1.0f)) { ++skippedBounds; continue; }

        // The chassis box is centred on the origin whatever the mesh does, so a mesh whose bounds sit well
        // off it drives with its collision off to one side of the body. Counted, not corrected.
        const Vec3 centre = (sp.boundsMin + sp.boundsMax) * 0.5f;
        if (std::fabs(centre.x) > 0.15f * extent.x || std::fabs(centre.y) > 0.15f * extent.y) ++offCentre;
        // AND THE ORIGIN IS THE BOTTOM OF THE CAR: the wheels hang `radius` above it and the chassis box is
        // lifted from it, so a mesh exported with its origin at mid-height or at the roof is built for a
        // car that is not there, and floats or sinks by tens of centimetres -- far more visible than the
        // sideways case above, so it is counted the same way.
        if (std::fabs(sp.boundsMin.z) > kOriginHeightTolerance * extent.z) ++offGround;

        Built built;
        if (!buildVehicle(*preset, size, xf, built)) { ++skippedPhysics; continue; }
        // So a ray or a contact that hits the chassis can name the entity, as every level body does.
        aver_phys_set_entity(aver_phys_vehicle_body(built.handle), static_cast<i32>(sp.entity));

        Vehicle v;
        v.entity = sp.entity;
        v.handle = built.handle;
        v.scale = Vec3{snapToOne(xf.scale.x), snapToOne(xf.scale.y), snapToOne(xf.scale.z)};
        v.length = size.x;
        v.wheelbase = built.wheelbase;
        v.maxSteerRad = built.maxSteerRad;
        v.pos = v.writtenPos = xf.position;
        v.rot = v.writtenRot = xf.rotation;
        v.rng = (static_cast<u64>(seed) << 32) ^ static_cast<u64>(index);

        const Vec3 heading = flat(xf.rotation.rotate(Vec3{1.0f, 0.0f, 0.0f})).getSafeNormal();
        const lanes::LaneHit hit = lanes::nearestLane(m.net, xf.position, heading, kAssignMaxCm, kAssignMinCos);
        if (hit.lane >= 0) {
            v.mode = Mode::Driving;
            v.route.assign(1, hit.lane);
            v.s = hit.proj.s;
            m.extendRoute(v);
            ++driving;
        } else {
            // A PARKED CAR IS STILL PHYSICS: it settles on its suspension and a car can run into it.
            aver_phys_vehicle_set_input(v.handle, 0.0f, 0.0f, 1.0f, 1.0f);
            ++parked;
        }
        m.vehicles.push_back(std::move(v));
    }

    AVER_INFO("[Vehicles] {} vehicles: {} driving on {} lanes, {} parked (no lane within {:.0f} m heading the "
              "same way)", m.vehicles.size(), driving, m.net.lanes.size(), parked, kAssignMaxCm * 0.01f);
    if (skippedEntity || skippedBounds || skippedPhysics)
        AVER_WARN("[Vehicles] {} placements skipped: {} with a stale entity or a bad transform, {} with "
                  "degenerate mesh bounds, {} that the physics module refused -- one still in the level stays "
                  "where it was placed, WITHOUT collision (a vehicle placement is never given a static collider)",
                  skippedEntity + skippedBounds + skippedPhysics, skippedEntity, skippedBounds, skippedPhysics);
    if (unknownPreset)
        AVER_WARN("[Vehicles] {} placements name a preset this build does not know; built as 'car'",
                  unknownPreset);
    if (offCentre)
        AVER_WARN("[Vehicles] {} meshes have their bounds centred well off the origin; the chassis box is "
                  "centred on it, so their collision sits off to one side of the visible body", offCentre);
    if (offGround)
        AVER_WARN("[Vehicles] {} meshes have their lowest point more than {:.0f}% of their height off the "
                  "origin; the wheels and chassis assume the origin is the BOTTOM of the car, so these float "
                  "or sink by that much", offGround, kOriginHeightTolerance * 100.0f);
}

void VehicleSystem::prePhysics(f32 dt, const Vec3& focus) {
    Impl& m = *impl_;
    if (m.vehicles.empty() || !(dt > 0.0f)) return;
    ++m.frame;
    m.rebuildOccupancy();

    for (usize i = 0; i < m.vehicles.size(); ++i) {
        Vehicle& v = m.vehicles[i];
        if (v.mode == Mode::Parked) continue;
        v.pendingDt += dt;
        // THE LOD: far from the focus the driver runs every kVehicleFarInterval-th frame and the vehicle
        // coasts on its last input in between. Staggered by index so the far fleet does not all run on the
        // same frame, which would turn a steady cost into a spike.
        const bool distant = distSquared(v.pos, focus) > kVehicleFarCm * kVehicleFarCm;
        if (distant && (m.frame + static_cast<u32>(i)) % kVehicleFarInterval != 0) continue;
        const f32 elapsed = v.pendingDt;
        v.pendingDt = 0.0f;
        m.drive(i, elapsed, focus);
    }
}

void VehicleSystem::postPhysics(scene::World& w) {
    Impl& m = *impl_;
    u32 reaped = 0;
    for (Vehicle& v : m.vehicles) {
        // AN ENTITY THAT DIED DURING PLAY TAKES ITS CAR WITH IT. A graph or script can destroy a car's
        // entity, and left alone the body would go on driving, colliding with the player and holding its
        // place in the lane queues -- an invisible, solid ghost that raycasts and contacts still name
        // by a dead handle.
        if (!w.valid(v.entity)) {
            aver_phys_vehicle_destroy(v.handle);
            v.handle = 0;
            ++reaped;
            continue;
        }
        f32 p[3], q[4];
        if (!aver_phys_vehicle_pose(v.handle, p, q)) continue;
        const Vec3 pos{p[0], p[1], p[2]};
        const Quat rot = Quat{q[0], q[1], q[2], q[3]}.normalized();
        // A pose Jolt blew up on must not reach the scene, where it would spread to every child and every
        // matrix composed from it.
        if (!finitePose(pos, rot)) continue;

        v.pos = pos;
        v.rot = rot;
        v.speed = aver_phys_vehicle_forward_speed(v.handle);

        if (samePose(pos, rot, v.writtenPos, v.writtenRot)) continue;
        Transform xf;
        xf.position = pos;
        xf.rotation = rot;
        xf.scale = v.scale;
        w.setLocalTransform(v.entity, localFor(w, v.entity, xf));
        v.writtenPos = pos;
        v.writtenRot = rot;
    }
    if (reaped == 0) return;
    // Compacted here, where nothing holds an index: the occupancy list is rebuilt from scratch at the top
    // of every prePhysics, and the LOD stagger only moves a far car's turn by a frame.
    m.vehicles.erase(std::remove_if(m.vehicles.begin(), m.vehicles.end(),
                                    [](const Vehicle& v) { return v.handle == 0; }),
                     m.vehicles.end());
    AVER_INFO("[Vehicles] {} vehicle(s) removed: their entities were destroyed during play ({} left)",
              reaped, m.vehicles.size());
}

void VehicleSystem::end() {
    Impl& m = *impl_;
    // aver_phys_shutdown destroys every vehicle itself, so there is nothing to destroy once the world is gone.
    if (aver_phys_ready())
        for (const Vehicle& v : m.vehicles) aver_phys_vehicle_destroy(v.handle);
    m.vehicles.clear();
    m.occ.clear();
    m.net = lanes::Network{};
}

usize VehicleSystem::count() const { return impl_->vehicles.size(); }

std::vector<scene::Entity> VehicleSystem::entities() const {
    std::vector<scene::Entity> out;
    out.reserve(impl_->vehicles.size());
    for (const Vehicle& v : impl_->vehicles) out.push_back(v.entity);
    return out;
}

#  else // !AVER_MODULE_PHYSICS

// A tree built without physics still links and runs: there is nothing to drive, so every call is inert.
struct VehicleSystem::Impl {};

VehicleSystem::VehicleSystem() : impl_(std::make_unique<Impl>()) {}
VehicleSystem::~VehicleSystem() = default;

void VehicleSystem::begin(scene::World&, const std::vector<VehicleSpawn>& spawns, const fmt::OcLanesData*, u32) {
    if (!spawns.empty())
        AVER_WARN("[Vehicles] built without the physics module: {} vehicle placements stay where they were "
                  "placed", spawns.size());
}
void VehicleSystem::prePhysics(f32, const Vec3&) {}
void VehicleSystem::postPhysics(scene::World&) {}
void VehicleSystem::end() {}
usize VehicleSystem::count() const { return 0; }
std::vector<scene::Entity> VehicleSystem::entities() const { return {}; }

#  endif // AVER_MODULE_PHYSICS

} // namespace aver::world

#endif // AVER_MODULE_SCENE
