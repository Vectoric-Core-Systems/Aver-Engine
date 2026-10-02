// Aver.World: the vehicle driver's lane maths -- world::lanes in VehicleSystem.hpp. Exit code = failure count.
//
// PURE MATHS ONLY, and that is the point of it living apart from VehicleSystem: no world, no physics
// world, no body, nothing to settle or step. Each case is a number a driving car depends on -- where on a
// lane a point is, which lane a car belongs to, what is ahead across a lane boundary, how hard a bend is,
// which way the wheel turns -- checked against a value worked out by hand. The vehicle itself (a Jolt
// constraint on a floor, driven along a lane) needs the physics ABI's vehicle half and is not here.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcLanes.hpp"
#include "aver/world/VehicleSystem.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) <= tol) return;
    AVER_ERROR("   FAIL  {}: got {} want {}", what, got, want);
    ++g_failures;
}

static bool isInf(f32 v) { return v == std::numeric_limits<f32>::infinity(); }

static fmt::OcLane lane(i32 id, std::vector<Vec3> pts, std::vector<i32> next = {}) {
    fmt::OcLane l;
    l.id = id;
    l.pts = std::move(pts);
    l.next = std::move(next);
    return l;
}

int main() {
    // ---- buildNetwork -----------------------------------------------------------------------------
    // 10 runs east 1000 cm and flows into 20 (and into 99, which nothing declares); 20 runs north 1000 cm.
    // 30 has one point and 40 has two identical ones: neither is a road.
    fmt::OcLanesData file;
    file.lanes.push_back(lane(10, {{0, 0, 0}, {500, 0, 0}, {1000, 0, 0}}, {20, 99}));
    file.lanes.push_back(lane(20, {{1000, 0, 0}, {1000, 500, 0}, {1000, 1000, 0}}));
    file.lanes.push_back(lane(30, {{0, 0, 0}}));
    file.lanes.push_back(lane(40, {{5, 5, 5}, {5, 5, 5}}));

    lanes::BuildReport report;
    const lanes::Network net = lanes::buildNetwork(file, &report);
    check(net.lanes.size() == 2, "only the two real lanes survive");
    check(report.droppedLanes == 2, "the one-point lane and the zero-length lane are dropped");
    check(report.droppedLinks == 1, "the successor that names no lane is dropped");
    if (net.lanes.size() != 2) {
        AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
        return exitCode(ExitCode::Failed);
    }
    check(net.lanes[0].next.size() == 1 && net.lanes[0].next[0] == 1, "NEXT 20 became the index of lane 20");
    check(net.lanes[1].next.empty(), "lane 20 is a dead end");
    checkNear(net.lanes[0].length(), 1000.0f, 0.01f, "arc length of lane 10");
    checkNear(net.lanes[1].length(), 1000.0f, 0.01f, "arc length of lane 20");
    checkNear(net.lanes[0].speedLimit, 1300.0f, 0.0f, "the speed limit is carried over");

    // ---- projection -------------------------------------------------------------------------------
    {
        const lanes::Projection pr = lanes::project(net.lanes[0], Vec3{300, 40, 5});
        checkNear(pr.s, 300.0f, 0.01f, "a point beside the lane projects to its own arc length");
        checkNear(pr.distance, std::sqrt(40.0f * 40.0f + 5.0f * 5.0f), 0.01f, "distance is 3D");
        checkNear(pr.lateral, 40.0f, 0.01f, "a point on +Y of a lane running +X is to its RIGHT (positive)");
        checkNear(pr.tangent.x, 1.0f, 1e-5f, "the tangent runs along the lane");
        checkNear(pr.point.y, 0.0f, 1e-5f, "the projected point is on the lane");

        const lanes::Projection left = lanes::project(net.lanes[0], Vec3{300, -40, 0});
        checkNear(left.lateral, -40.0f, 0.01f, "a point on -Y is to the left (negative)");

        const lanes::Projection past = lanes::project(net.lanes[0], Vec3{1200, 0, 0});
        checkNear(past.s, 1000.0f, 0.01f, "past the end clamps to the lane's end");
        checkNear(past.distance, 200.0f, 0.01f, "and the distance is to that end");
    }

    // A lane that doubles back passes 60 cm from itself: a U with legs along y = 0 and y = 100.
    {
        fmt::OcLanesData uFile;
        uFile.lanes.push_back(lane(1, {{0, 0, 0}, {1000, 0, 0}, {1000, 100, 0}, {0, 100, 0}}));
        const lanes::Network u = lanes::buildNetwork(uFile);
        checkNear(u.lanes[0].length(), 2100.0f, 0.01f, "the U is 2100 cm long");

        const Vec3 p{500, 40, 0};
        const lanes::Projection whole = lanes::project(u.lanes[0], p);
        checkNear(whole.s, 500.0f, 0.01f, "unwindowed, the nearer leg wins");
        const lanes::Projection ret = lanes::project(u.lanes[0], p, 1100.0f, lanes::kWholeLane);
        checkNear(ret.s, 1600.0f, 0.01f, "windowed to the return leg, it projects onto that leg instead");
        checkNear(ret.distance, 60.0f, 0.01f, "even though it is the farther leg");
        checkNear(ret.lateral, 60.0f, 0.01f, "the return leg runs -X, so the point (at y = 40, under the leg "
                                              "at y = 100) is on its right");
    }

    // ---- pointAt ----------------------------------------------------------------------------------
    {
        Vec3 t;
        const Vec3 mid = lanes::pointAt(net.lanes[0], 250.0f, &t);
        checkNear(mid.x, 250.0f, 0.01f, "pointAt interpolates along a segment");
        checkNear(t.x, 1.0f, 1e-5f, "and reports the direction of travel");
        checkNear(lanes::pointAt(net.lanes[0], 5000.0f).x, 1000.0f, 0.01f, "past the end clamps to the end");
        checkNear(lanes::pointAt(net.lanes[0], -5.0f).x, 0.0f, 0.01f, "before the start clamps to the start");
    }

    // ---- looking ahead across a lane boundary -----------------------------------------------------
    {
        const std::vector<i32> route{0, 1};
        const lanes::RoutePoint inside = lanes::advanceAlong(net, route, 100.0f, 200.0f);
        check(inside.lane == 0 && !inside.clamped, "200 cm from s=100 stays on the first lane");
        checkNear(inside.s, 300.0f, 0.01f, "at s=300");

        const lanes::RoutePoint across = lanes::advanceAlong(net, route, 900.0f, 300.0f);
        check(across.lane == 1 && !across.clamped, "300 cm from s=900 crosses onto the second lane");
        checkNear(across.s, 200.0f, 0.01f, "with the remaining 200 cm carried onto its start");
        checkNear(across.point.x, 1000.0f, 0.01f, "the point is on the second lane, x");
        checkNear(across.point.y, 200.0f, 0.01f, "and y");
        checkNear(across.tangent.y, 1.0f, 1e-5f, "and the direction is the second lane's");

        const lanes::RoutePoint exact = lanes::advanceAlong(net, route, 900.0f, 100.0f);
        check(exact.lane == 0 && !exact.clamped, "landing exactly on a lane's end is still that lane");

        const lanes::RoutePoint beyond = lanes::advanceAlong(net, route, 900.0f, 1500.0f);
        check(beyond.clamped, "past the route's end is flagged");
        checkNear(beyond.point.y, 1000.0f, 0.01f, "and clamps to the end of the last lane");

        checkNear(lanes::routeRemaining(net, route, 900.0f), 1100.0f, 0.01f, "remaining spans both lanes");
        checkNear(lanes::routeRemaining(net, {0}, 250.0f), 750.0f, 0.01f, "remaining on one lane");
        check(lanes::advanceAlong(net, {}, 0.0f, 100.0f).lane == -1, "an empty route has no point");
    }

    // ---- successors -------------------------------------------------------------------------------
    {
        lanes::Lane branch;
        branch.next = {5, 6, 7};
        check(lanes::pickSuccessor(branch, 0) == 5 && lanes::pickSuccessor(branch, 4) == 6 &&
                  lanes::pickSuccessor(branch, 5) == 7,
              "a successor is picked by the random value modulo the count");
        check(lanes::pickSuccessor(net.lanes[1], 123) == -1, "a dead end has no successor");
    }

    // ---- curvature --------------------------------------------------------------------------------
    {
        const f32 R = 2000.0f;
        auto onCircle = [&](f32 deg) { return Vec3{R * std::cos(radians(deg)), R * std::sin(radians(deg)), 0.0f}; };
        checkNear(lanes::circumradiusXY(onCircle(0), onCircle(10), onCircle(20)), R, 1.0f,
                  "three points on a circle give its radius");
        check(isInf(lanes::circumradiusXY(Vec3{0, 0, 0}, Vec3{100, 0, 0}, Vec3{200, 0, 0})),
              "collinear points are a straight (infinite radius)");
        check(isInf(lanes::circumradiusXY(Vec3{0, 0, 0}, Vec3{100, 0, 0}, Vec3{100, 0, 0})),
              "a repeated point is not a bend");
        check(isInf(lanes::circumradiusXY(Vec3{0, 0, 0}, Vec3{100, 0, 7}, Vec3{200, 0, 3})),
              "height differences do not bend the XY radius");

        checkNear(lanes::curvatureSpeedCap(1500.0f), std::sqrt(250.0f * 1500.0f), 0.01f,
                  "cap = sqrt(2.5 m/s^2 * R)");
        checkNear(lanes::curvatureSpeedCap(10.0f), lanes::kCornerSpeedFloorCmS, 0.0f,
                  "a kink does not take the cap below the floor");
        check(isInf(lanes::curvatureSpeedCap(std::numeric_limits<f32>::infinity())), "a straight has no cap");

        // A quarter circle of radius 1500 cm, 60 segments, then a straight lane for comparison.
        fmt::OcLanesData arcFile;
        std::vector<Vec3> arc;
        for (int i = 0; i <= 60; ++i) {
            const f32 a = radians(90.0f * static_cast<f32>(i) / 60.0f);
            arc.push_back(Vec3{1500.0f * std::cos(a), 1500.0f * std::sin(a), 0.0f});
        }
        arcFile.lanes.push_back(lane(1, arc));
        const lanes::Network curved = lanes::buildNetwork(arcFile);
        const f32 r = lanes::minRadiusAhead(curved, {0}, 0.0f, lanes::kCurveHorizonCm, lanes::kCurveSampleCm);
        checkNear(r, 1500.0f, 60.0f, "the tightest radius ahead on a quarter circle is its radius");
        checkNear(lanes::curvatureSpeedCap(r), std::sqrt(250.0f * 1500.0f), 15.0f, "and the speed cap follows it");
        check(isInf(lanes::minRadiusAhead(net, {0}, 0.0f, lanes::kCurveHorizonCm, lanes::kCurveSampleCm)),
              "a straight lane has no radius, and running off its end does not invent one");

        // THE SCAN SEES A CORNER ACROSS A LANE BOUNDARY: east for 1000 cm, then north. From s = 700 the
        // samples (700,0), (1000,100), (1000,500) bend with a radius near 3 m.
        const f32 corner = lanes::minRadiusAhead(net, {0, 1}, 700.0f, lanes::kCurveHorizonCm, lanes::kCurveSampleCm);
        check(corner > 250.0f && corner < 350.0f, "a corner on the next lane is in view before the car reaches it");
    }

    // ---- steering ---------------------------------------------------------------------------------
    {
        const Vec3 origin{0, 0, 0}, east{1, 0, 0}, north{0, 1, 0};
        checkNear(lanes::bearingToTarget(origin, east, Vec3{100, 100, 0}), radians(45.0f), 1e-4f,
                  "a target ahead and toward +Y is positive when facing +X");
        checkNear(lanes::bearingToTarget(origin, east, Vec3{100, -100, 0}), radians(-45.0f), 1e-4f,
                  "toward -Y is negative");
        checkNear(lanes::bearingToTarget(origin, east, Vec3{500, 0, 0}), 0.0f, 1e-6f, "dead ahead is zero");
        checkNear(lanes::bearingToTarget(origin, north, Vec3{-100, 0, 0}), radians(90.0f), 1e-4f,
                  "facing +Y, the vehicle's right is -X");
        checkNear(lanes::bearingToTarget(origin, east, Vec3{500, 0, 900}), 0.0f, 1e-6f,
                  "height does not change a bearing");

        const f32 maxSteer = radians(35.0f);
        checkNear(lanes::pursuitSteer(radians(30.0f), 260.0f, 600.0f, maxSteer), 0.6695f, 0.002f,
                  "atan2(2 W sin(alpha), L) / maxSteer at alpha 30 degrees");
        check(lanes::pursuitSteer(radians(20.0f), 260.0f, 600.0f, maxSteer) > 0.0f, "a target on the right steers right");
        check(lanes::pursuitSteer(radians(-20.0f), 260.0f, 600.0f, maxSteer) < 0.0f, "on the left steers left");
        checkNear(lanes::pursuitSteer(0.0f, 260.0f, 600.0f, maxSteer), 0.0f, 1e-6f, "dead ahead steers straight");
        checkNear(lanes::pursuitSteer(radians(90.0f), 260.0f, 600.0f, maxSteer), 1.0f, 0.0f, "full lock is the limit");
        checkNear(lanes::pursuitSteer(radians(-90.0f), 260.0f, 600.0f, maxSteer), -1.0f, 0.0f, "in both directions");
        checkNear(lanes::pursuitSteer(radians(30.0f), 260.0f, 600.0f, 0.0f), 0.0f, 0.0f, "a car that cannot steer does not");

        checkNear(lanes::lookAheadDistance(0.0f), 600.0f, 0.0f, "stationary: 6 m");
        checkNear(lanes::lookAheadDistance(1000.0f), 1050.0f, 0.01f, "10 m/s: 6 m + 0.45 s of travel");
        checkNear(lanes::lookAheadDistance(100000.0f), 3000.0f, 0.0f, "never past 30 m");
        checkNear(lanes::lookAheadDistance(-500.0f), 600.0f, 0.0f, "reversing does not shorten it");
    }

    // ---- speed policy -----------------------------------------------------------------------------
    {
        checkNear(lanes::headwaySpeed(300.0f), 0.0f, 0.0f, "inside the standstill gap: stop");
        checkNear(lanes::headwaySpeed(100.0f), 0.0f, 0.0f, "well inside it too");
        checkNear(lanes::headwaySpeed(440.0f), 100.0f, 0.01f, "(gap - 3 m) / 1.4 s");
        // At speed v a car is content with a gap of 3 m + 1.4 s * v: the policy must hand back v for it.
        checkNear(lanes::headwaySpeed(300.0f + 1.4f * 800.0f), 800.0f, 0.01f, "the desired gap at 8 m/s gives 8 m/s back");

        checkNear(lanes::gapLimitedSpeed(500.0f, 0.0f), 0.0f, 0.0f, "a stopped leader within 6 m means stop");
        checkNear(lanes::gapLimitedSpeed(700.0f, 0.0f), 400.0f / 1.4f, 0.01f, "a stopped leader beyond 6 m is a gap");
        checkNear(lanes::gapLimitedSpeed(500.0f, 800.0f), 200.0f / 1.4f, 0.01f, "a moving leader at 5 m is a gap, not a stop");

        const lanes::Pedals low = lanes::speedPedals(1000.0f, 1300.0f);
        check(low.forward == 1.0f && low.brake == 0.0f, "3 m/s under the target is full throttle");
        const lanes::Pedals closer = lanes::speedPedals(1250.0f, 1300.0f);
        checkNear(closer.forward, 50.0f / 300.0f, 1e-5f, "closer is proportionally less");
        check(closer.brake == 0.0f, "and no brake");
        const lanes::Pedals coast = lanes::speedPedals(1400.0f, 1300.0f);
        check(coast.forward == 0.0f && coast.brake == 0.0f, "up to 1.5 m/s over the target coasts");
        const lanes::Pedals over = lanes::speedPedals(1500.0f, 1300.0f);
        check(over.forward == 0.0f, "over the target there is no throttle");
        checkNear(over.brake, 50.0f / 150.0f, 1e-5f, "and the brake comes in past 1.5 m/s over");
        checkNear(lanes::speedPedals(1700.0f, 1300.0f).brake, 1.0f, 0.0f, "4 m/s over is full brake");
        const lanes::Pedals hold = lanes::speedPedals(0.0f, 0.0f);
        check(hold.brake == 1.0f && hold.forward == 0.0f, "a target of zero holds the brake at a standstill");
        check(lanes::speedPedals(900.0f, 0.0f).brake == 1.0f, "and brakes a rolling car to it");
    }

    // ---- which lane a car belongs to --------------------------------------------------------------
    {
        // A runs east along y = 0, B runs west along y = 100: two directions of one road.
        fmt::OcLanesData road;
        road.lanes.push_back(lane(1, {{0, 0, 0}, {2000, 0, 0}}));
        road.lanes.push_back(lane(2, {{2000, 100, 0}, {0, 100, 0}}));
        const lanes::Network two = lanes::buildNetwork(road);
        const f32 maxDist = 1200.0f, minCos = 0.5f;
        const Vec3 east{1, 0, 0}, west{-1, 0, 0};

        lanes::LaneHit hit = lanes::nearestLane(two, Vec3{1000, 30, 0}, east, maxDist, minCos);
        check(hit.lane == 0, "a car heading east beside the eastbound lane belongs to it");
        checkNear(hit.proj.s, 1000.0f, 0.01f, "and starts at its own progress");

        hit = lanes::nearestLane(two, Vec3{1000, 70, 0}, east, maxDist, minCos);
        check(hit.lane == 0, "heading east, the NEARER westbound lane is passed over for the one it agrees with");
        hit = lanes::nearestLane(two, Vec3{1000, 70, 0}, west, maxDist, minCos);
        check(hit.lane == 1, "heading west, the same spot belongs to the westbound lane");

        const Vec3 at55{std::cos(radians(55.0f)), std::sin(radians(55.0f)), 0.0f};
        const Vec3 at65{std::cos(radians(65.0f)), std::sin(radians(65.0f)), 0.0f};
        check(lanes::nearestLane(two, Vec3{1000, 30, 0}, at55, maxDist, minCos).lane == 0, "55 degrees off is within 60");
        check(lanes::nearestLane(two, Vec3{1000, 30, 0}, at65, maxDist, minCos).lane == -1, "65 degrees off is not");
        check(lanes::nearestLane(two, Vec3{1000, 30, 0}, Vec3{0, 1, 0}, maxDist, minCos).lane == -1,
              "square across the road belongs to neither lane");
        check(lanes::nearestLane(two, Vec3{1000, 1500, 0}, east, maxDist, minCos).lane == -1, "15 m away is too far");
        check(lanes::nearestLane(two, Vec3{1000, 30, 2000}, east, maxDist, minCos).lane == -1,
              "20 m above the lane (a deck over a street) is too far: distance is 3D");
        check(lanes::nearestLane(lanes::Network{}, Vec3{0, 0, 0}, east, maxDist, minCos).lane == -1,
              "no lanes, no lane");
    }

    // ---- putting a car back on a lane -------------------------------------------------------------
    {
        const Vec3 tangents[] = {{1, 0, 0}, {0, 1, 0}, {-1, 0.5f, 0.2f}, {0.6f, 0, 0.8f}, {0.3f, -0.9f, -0.1f}};
        for (const Vec3& t : tangents) {
            const Quat q = lanes::laneOrientation(t);
            const Vec3 want = t.getSafeNormal();
            const Vec3 got = q.rotate(Vec3{1, 0, 0});
            checkNear(dist(got, want), 0.0f, 1e-4f, "the nose points along the lane, grade included");
            checkNear(q.rotate(Vec3{0, 1, 0}).z, 0.0f, 1e-5f, "with no roll: the right side stays level");
            check(q.rotate(Vec3{0, 0, 1}).z > 0.0f, "and the car is upright");
        }
        const Quat none = lanes::laneOrientation(Vec3{0, 0, 0});
        checkNear(none.w, 1.0f, 0.0f, "a lane with no direction leaves the car as it was");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
